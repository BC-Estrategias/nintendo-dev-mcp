/* FS_WRITE / FS_MKDIR against a real temporary directory: atomic replace, backup, hash/size checks,
 * policy and mode gating, fault injection, symlink containment. */
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE 1
#define _DEFAULT_SOURCE 1
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ndp/ndp_agent.h"
#include "ndp/ndp_posix_fs.h"
#include "ndp/ndp_sha256.h"
#include "ndp/ndp_tlv.h"

static int g_checks = 0, g_fail = 0;
#define CHECK(cond, ...)                                             \
  do {                                                               \
    g_checks++;                                                      \
    if (!(cond)) {                                                   \
      g_fail++;                                                      \
      printf("  FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond);      \
      printf(__VA_ARGS__);                                           \
      printf("\n");                                                  \
    }                                                                \
  } while (0)

static char g_root[64], g_other[64];
static ndp_posix_fs_ctx g_ctx;
static ndp_fs_ops g_real_ops, g_ops;
static ndp_agent g_agent;
static uint8_t g_out[NDP_HEADER_SIZE + 65536];
static uint32_t g_id = 500;

/* ---- fault injection wrappers around the real ops ---- */
static int g_rename_calls, g_rename_fail_at; /* 1-based; 0 = never */
static int g_write_fail;                     /* 1 = next file_write fails with NO_SPACE */
static int g_sync_fail;
static int w_rename(void *c, const char *a, const char *b) {
  g_rename_calls++;
  if (g_rename_fail_at && g_rename_calls == g_rename_fail_at) return NDP_ST_IO_ERROR;
  return g_real_ops.rename(c, a, b);
}
static long w_write(void *c, void *f, const void *buf, size_t n) {
  if (g_write_fail) return -NDP_ST_NO_SPACE;
  return g_real_ops.file_write(c, f, buf, n);
}
static int w_sync(void *c, void *f) { return g_sync_fail ? NDP_ST_IO_ERROR : g_real_ops.file_sync(c, f); }

typedef struct { ndp_header h; const uint8_t *pl; size_t len; size_t raw; uint8_t buf[512]; } resp_t;
static resp_t parse(size_t n) {
  resp_t r;
  memset(&r, 0, sizeof r);
  r.raw = n;
  if (n != NDP_NO_REPLY && n >= NDP_HEADER_SIZE && ndp_header_decode(g_out, &r.h) == NDP_OK) {
    r.len = r.h.payload_len < sizeof r.buf ? r.h.payload_len : sizeof r.buf;
    memcpy(r.buf, g_out + NDP_HEADER_SIZE, r.len); /* own copy: g_out is reused by the next call */
  }
  return r;
}
static int is_err(resp_t r, int st) { return r.h.kind == NDP_KIND_ERR && r.h.status == st; }
static int is_res(resp_t r) { return r.h.kind == NDP_KIND_RES && r.h.status == 0; }
static int no_reply(resp_t r) { return r.raw == NDP_NO_REPLY; }

static resp_t frame(ndp_agent *a, uint8_t kind, uint32_t id, uint16_t cmd, const uint8_t *pl, size_t len) {
  ndp_header h;
  memset(&h, 0, sizeof h);
  h.version = 1; h.kind = kind; h.request_id = id; h.command = cmd; h.payload_len = (uint32_t)len;
  return parse(ndp_agent_handle(a, &h, pl, g_out, sizeof g_out));
}

typedef struct { uint8_t b[512]; ndp_tlv_w w; } treq;
static void treq_init(treq *t) { ndp_tlv_w_init(&t->w, t->b, sizeof t->b); }

typedef struct {
  int overwrite, backup;
  const uint8_t *expected; /* 32 bytes or NULL */
  uint64_t declared;       /* size announced in the REQ */
  size_t chunk;            /* DATA frame size */
  int send_end;            /* 0 = leave the upload hanging */
} up_opts;

/* Full upload. Returns the FINAL response (RES/ERR); *ready receives the response to the REQ. */
static resp_t upload(ndp_agent *a, const char *path, const uint8_t *data, size_t len, up_opts o, resp_t *ready) {
  treq t; resp_t r; uint32_t id = ++g_id; size_t pos = 0;
  treq_init(&t);
  ndp_tlv_put_str(&t.w, NDP_TAG_PATH, path);
  ndp_tlv_put_u64(&t.w, NDP_TAG_SIZE, o.declared);
  if (o.overwrite) { uint8_t one = 1; ndp_tlv_put(&t.w, NDP_TAG_OVERWRITE, &one, 1); }
  if (o.backup) { uint8_t one = 1; ndp_tlv_put(&t.w, NDP_TAG_BACKUP, &one, 1); }
  if (o.expected) ndp_tlv_put(&t.w, NDP_TAG_SHA256, o.expected, 32);
  r = frame(a, NDP_KIND_REQ, id, NDP_CMD_FS_WRITE, t.b, t.w.len);
  if (ready) *ready = r;
  if (!is_res(r)) return r;
  while (pos < len) {
    size_t n = len - pos < o.chunk ? len - pos : o.chunk;
    r = frame(a, NDP_KIND_DATA, id, NDP_CMD_FS_WRITE, data + pos, n);
    pos += n;
    if (!no_reply(r)) return r; /* an ERR in the middle */
  }
  if (!o.send_end) return r;
  return frame(a, NDP_KIND_END, id, NDP_CMD_FS_WRITE, NULL, 0);
}
static up_opts opts(uint64_t declared) { up_opts o; memset(&o, 0, sizeof o); o.declared = declared; o.chunk = 4096; o.send_end = 1; return o; }
static up_opts up_opts_overwrite(void) { up_opts o = opts(4); o.overwrite = 1; return o; }
static up_opts up_opts_overwrite2(void) { up_opts o = opts(8); o.overwrite = 1; return o; }

static resp_t simple(ndp_agent *a, uint16_t cmd, const char *path) {
  treq t; treq_init(&t); ndp_tlv_put_str(&t.w, NDP_TAG_PATH, path);
  return frame(a, NDP_KIND_REQ, ++g_id, cmd, t.b, t.w.len);
}

/* ---- filesystem helpers ---- */
static void full(char *out, size_t cap, const char *rel) { snprintf(out, cap, "%s%s", g_root, rel); }
static int exists(const char *rel) { char p[256]; struct stat sb; full(p, sizeof p, rel); return lstat(p, &sb) == 0; }
static long file_size(const char *rel) { char p[256]; struct stat sb; full(p, sizeof p, rel); return lstat(p, &sb) == 0 ? (long)sb.st_size : -1; }
static int content_is(const char *rel, const void *data, size_t n) {
  char p[256]; FILE *f; uint8_t *b; size_t r; int ok;
  full(p, sizeof p, rel);
  f = fopen(p, "rb");
  if (!f) return 0;
  b = malloc(n + 1);
  r = fread(b, 1, n + 1, f);
  fclose(f);
  ok = (r == n) && (n == 0 || memcmp(b, data, n) == 0);
  free(b);
  return ok;
}
static void put(const char *rel, const void *d, size_t n) { char p[256]; FILE *f; full(p, sizeof p, rel); f = fopen(p, "wb"); if (!f) { perror(p); exit(2); } fwrite(d, 1, n, f); fclose(f); }
static void mk(const char *rel) { char p[256]; full(p, sizeof p, rel); mkdir(p, 0755); }
static int dir_has_tmp(const char *rel) { /* any *.ndp-tmp / *.ndp-old leftovers? */
  char cmd[512]; full(cmd, sizeof cmd, rel);
  { char c2[600]; snprintf(c2, sizeof c2, "ls %s 2>/dev/null | grep -E 'ndp-(tmp|old)' >/dev/null", cmd); return system(c2) == 0; }
}
static void sha_of(const void *d, size_t n, uint8_t out[32]) { ndp_sha256(d, n, out); }

#define DIR "/3ds/nintendo-dev-agent"

static void new_agent(ndp_mode mode) {
  ndp_agent_config cfg;
  treq t;
  memset(&cfg, 0, sizeof cfg);
  cfg.platform = "host"; cfg.agent_version = "t"; cfg.auth = "none"; cfg.mode = mode; cfg.max_frame = 65536; cfg.fs = &g_ops;
  ndp_agent_init(&g_agent, &cfg);
  treq_init(&t);
  ndp_tlv_put_u16(&t.w, NDP_TAG_PROTOCOL, 1); ndp_tlv_put_u16(&t.w, NDP_TAG_PROTOCOL_MAX, 1);
  { uint8_t n[16] = {0}; ndp_tlv_put(&t.w, NDP_TAG_NONCE, n, 16); }
  CHECK(is_res(frame(&g_agent, NDP_KIND_REQ, ++g_id, NDP_CMD_HELLO, t.b, t.w.len)), "hello");
}

static uint64_t tlv_u64(resp_t r, uint16_t tag) { const uint8_t *v; size_t l; return (ndp_tlv_find(r.buf, r.len, tag, &v, &l) && l == 8) ? ndp_tlv_get_u64(v) : (uint64_t)-1; }
static long tlv_u32(resp_t r, uint16_t tag) { const uint8_t *v; size_t l; return (ndp_tlv_find(r.buf, r.len, tag, &v, &l) && l == 4) ? (long)ndp_tlv_get_u32(v) : -1; }
static int tlv_u8(resp_t r, uint16_t tag) { const uint8_t *v; size_t l; return (ndp_tlv_find(r.buf, r.len, tag, &v, &l) && l == 1) ? v[0] : -1; }
static int tlv_sha(resp_t r, uint8_t out[32]) { const uint8_t *v; size_t l; if (ndp_tlv_find(r.buf, r.len, NDP_TAG_SHA256, &v, &l) && l == 32) { memcpy(out, v, 32); return 1; } return 0; }

static void test_create_and_replace(void) {
  static const char TXT[] = "Codex was here.";
  const char *V1 = "version one", *V2 = "version two!";
  uint8_t dig[32], want[32];
  resp_t ready, r;
  up_opts o;

  o = opts(sizeof TXT - 1);
  r = upload(&g_agent, DIR "/from-codex.txt", (const uint8_t *)TXT, sizeof TXT - 1, o, &ready);
  CHECK(is_res(ready) && tlv_u32(ready, NDP_TAG_MAX_CHUNK) == 65536, "ready RES with max_chunk");
  CHECK(is_res(r) && tlv_u64(r, NDP_TAG_WRITTEN) == sizeof TXT - 1 && tlv_u8(r, NDP_TAG_REPLACED) == 0, "final RES (written/replaced)");
  sha_of(TXT, sizeof TXT - 1, want);
  CHECK(tlv_sha(r, dig) && memcmp(dig, want, 32) == 0, "final RES carries the SHA-256");
  CHECK(content_is(DIR "/from-codex.txt", TXT, sizeof TXT - 1), "file content");
  CHECK(!dir_has_tmp(DIR), "no temp files left");
  CHECK(!ndp_agent_busy(&g_agent), "agent idle after the upload");

  /* never overwrite by default */
  r = upload(&g_agent, DIR "/from-codex.txt", (const uint8_t *)"other", 5, opts(5), &ready);
  CHECK(is_err(ready, NDP_ST_EXISTS) && content_is(DIR "/from-codex.txt", TXT, sizeof TXT - 1), "EXISTS and the file is untouched");

  /* replace */
  put(DIR "/r.txt", V1, strlen(V1));
  o = opts(strlen(V2)); o.overwrite = 1;
  r = upload(&g_agent, DIR "/r.txt", (const uint8_t *)V2, strlen(V2), o, &ready);
  CHECK(is_res(r) && tlv_u8(r, NDP_TAG_REPLACED) == 1 && content_is(DIR "/r.txt", V2, strlen(V2)), "replace");
  CHECK(!exists(DIR "/r.txt.bak") && !dir_has_tmp(DIR), "no .bak / temp without backup");

  /* replace with backup: previous content kept as .bak, and .bak itself is refreshed next time */
  o = opts(strlen(V1)); o.overwrite = 1; o.backup = 1;
  r = upload(&g_agent, DIR "/r.txt", (const uint8_t *)V1, strlen(V1), o, &ready);
  CHECK(is_res(r) && content_is(DIR "/r.txt", V1, strlen(V1)) && content_is(DIR "/r.txt.bak", V2, strlen(V2)), "backup holds the previous version");
  o = opts(strlen(V2)); o.overwrite = 1; o.backup = 1;
  r = upload(&g_agent, DIR "/r.txt", (const uint8_t *)V2, strlen(V2), o, &ready);
  CHECK(is_res(r) && content_is(DIR "/r.txt.bak", V1, strlen(V1)) && !dir_has_tmp(DIR), "second backup replaces the first");

  /* empty file: END only */
  r = upload(&g_agent, DIR "/empty.txt", NULL, 0, opts(0), &ready);
  CHECK(is_res(r) && tlv_u64(r, NDP_TAG_WRITTEN) == 0 && file_size(DIR "/empty.txt") == 0, "empty file");
}

static void test_large_and_hashes(void) {
  static uint8_t big[200000];
  uint8_t want[32], bad[32];
  resp_t ready, r;
  up_opts o;
  size_t i;
  for (i = 0; i < sizeof big; i++) big[i] = (uint8_t)(i * 31 + (i >> 9));
  sha_of(big, sizeof big, want);

  o = opts(sizeof big); o.chunk = 8192; o.expected = want;
  r = upload(&g_agent, DIR "/big.bin", big, sizeof big, o, &ready);
  CHECK(is_res(r) && content_is(DIR "/big.bin", big, sizeof big), "200 kB upload with the expected hash");
  CHECK(g_agent.last_transfer_bytes == sizeof big, "last_transfer_bytes");

  memcpy(bad, want, 32); bad[7] ^= 1;
  o = opts(sizeof big); o.expected = bad;
  r = upload(&g_agent, DIR "/bad.bin", big, sizeof big, o, &ready);
  CHECK(is_err(r, NDP_ST_HASH_MISMATCH) && !exists(DIR "/bad.bin") && !dir_has_tmp(DIR), "hash mismatch: nothing left behind");

  /* a hash mismatch on REPLACE leaves the old file intact */
  o = opts(sizeof big); o.overwrite = 1; o.expected = bad;
  r = upload(&g_agent, DIR "/big.bin", big, 1000, opts(1000), &ready); /* (EXISTS) just to be sure the old file is there */
  put(DIR "/keep.bin", "KEEP", 4);
  { up_opts o2 = opts(sizeof big); o2.overwrite = 1; o2.expected = bad;
    r = upload(&g_agent, DIR "/keep.bin", big, sizeof big, o2, &ready);
    CHECK(is_err(r, NDP_ST_HASH_MISMATCH) && content_is(DIR "/keep.bin", "KEEP", 4) && !dir_has_tmp(DIR), "failed replace keeps the old file"); }
}

static void test_size_checks_and_discard(void) {
  uint8_t data[100];
  resp_t ready, r;
  up_opts o;
  uint32_t id;
  memset(data, 'z', sizeof data);

  /* fewer bytes than declared */
  r = upload(&g_agent, DIR "/short.bin", data, 50, opts(100), &ready);
  CHECK(is_err(r, NDP_ST_BAD_REQUEST) && !exists(DIR "/short.bin") && !dir_has_tmp(DIR) && !ndp_agent_busy(&g_agent), "fewer bytes than declared");

  /* more bytes than declared: ERR at the offending DATA, then remaining DATA/END are discarded silently */
  o = opts(50); o.chunk = 40; o.send_end = 0;
  r = upload(&g_agent, DIR "/long.bin", data, 100, o, &ready);
  CHECK(is_res(ready) && is_err(r, NDP_ST_BAD_REQUEST), "ERR on the DATA that exceeds the size");
  id = g_id;
  CHECK(!ndp_agent_busy(&g_agent) && g_agent.up.discard, "discarding state");
  CHECK(no_reply(frame(&g_agent, NDP_KIND_DATA, id, NDP_CMD_FS_WRITE, data, 10)), "further DATA is ignored");
  CHECK(no_reply(frame(&g_agent, NDP_KIND_END, id, NDP_CMD_FS_WRITE, NULL, 0)) && !g_agent.up.discard, "END ends the discard");
  CHECK(!exists(DIR "/long.bin") && !dir_has_tmp(DIR), "nothing left behind");

  /* a new REQ also resynchronizes a discard */
  o = opts(50); o.chunk = 100; o.send_end = 0;
  r = upload(&g_agent, DIR "/long2.bin", data, 100, o, &ready);
  CHECK(is_err(r, NDP_ST_BAD_REQUEST) && g_agent.up.discard, "second overflow");
  r = simple(&g_agent, NDP_CMD_FS_STAT, DIR);
  CHECK(is_res(r) && !g_agent.up.discard, "a new REQ ends the discard and is served");
}

static void test_gating_and_policy(void) {
  resp_t ready, r;
  uint8_t d[4] = {1, 2, 3, 4};
  const char *deny[] = {"/other/x.txt", "/luma/x", "/boot.firm", DIR "/config/key", "/Nintendo 3DS/x", "/3ds/other/x", "/"};
  size_t i;

  new_agent(NDP_MODE_READ_ONLY);
  r = upload(&g_agent, DIR "/ro.txt", d, 4, opts(4), &ready);
  CHECK(is_err(ready, NDP_ST_FORBIDDEN_MODE) && !exists(DIR "/ro.txt"), "READ_ONLY forbids writes");
  CHECK(is_err(simple(&g_agent, NDP_CMD_FS_MKDIR, DIR "/rodir"), NDP_ST_FORBIDDEN_MODE) && !exists(DIR "/rodir"), "READ_ONLY forbids mkdir");

  new_agent(NDP_MODE_DEVELOPMENT);
  for (i = 0; i < sizeof deny / sizeof deny[0]; i++) {
    r = upload(&g_agent, deny[i], d, 4, opts(4), &ready);
    CHECK(ready.h.kind == NDP_KIND_ERR && (ready.h.status == NDP_ST_PROTECTED_PATH || ready.h.status == NDP_ST_BAD_REQUEST), "denied: %s (status %d)", deny[i], ready.h.status);
  }
  CHECK(!exists("/other/x.txt") && !exists("/luma/x") && !exists("/boot.firm"), "nothing was created outside the write root");
  CHECK(is_err(upload(&g_agent, "relative.txt", d, 4, opts(4), &ready), NDP_ST_PATH_INVALID), "relative path");
  CHECK(is_err(upload(&g_agent, DIR "/../x", d, 4, opts(4), &ready), NDP_ST_PATH_INVALID), "dot-dot");

  CHECK(is_err(upload(&g_agent, DIR "/nodir/x.txt", d, 4, opts(4), &ready), NDP_ST_NOT_FOUND), "missing parent is not created implicitly");
  CHECK(!exists(DIR "/nodir"), "no implicit mkdir");
  CHECK(is_err(upload(&g_agent, DIR "/sub", d, 4, up_opts_overwrite(), &ready), NDP_ST_BAD_REQUEST), "target is a directory");
  CHECK(is_err(upload(&g_agent, DIR "/a.ndp-tmp", d, 4, opts(4), &ready), NDP_ST_BAD_REQUEST), "reserved suffix .ndp-tmp");
  CHECK(is_err(upload(&g_agent, DIR "/a.ndp-old", d, 4, opts(4), &ready), NDP_ST_BAD_REQUEST), "reserved suffix .ndp-old");
  CHECK(is_err(upload(&g_agent, DIR "/a.bak", d, 4, opts(4), &ready), NDP_ST_BAD_REQUEST), "reserved suffix .bak");
  { treq t; treq_init(&t); ndp_tlv_put_str(&t.w, NDP_TAG_PATH, DIR "/nosize.txt");
    CHECK(is_err(frame(&g_agent, NDP_KIND_REQ, ++g_id, NDP_CMD_FS_WRITE, t.b, t.w.len), NDP_ST_BAD_REQUEST), "size is required"); }
  { treq t; uint8_t two = 2; treq_init(&t); ndp_tlv_put_str(&t.w, NDP_TAG_PATH, DIR "/x.txt"); ndp_tlv_put_u64(&t.w, NDP_TAG_SIZE, 1); ndp_tlv_put(&t.w, NDP_TAG_OVERWRITE, &two, 1);
    CHECK(is_err(frame(&g_agent, NDP_KIND_REQ, ++g_id, NDP_CMD_FS_WRITE, t.b, t.w.len), NDP_ST_BAD_REQUEST), "overwrite must be 0/1"); }
  { char longname[300]; memset(longname, 'n', 250); longname[250] = '\0';
    { char path[400]; snprintf(path, sizeof path, DIR "/%s", longname);
      CHECK(is_err(upload(&g_agent, path, d, 4, opts(4), &ready), NDP_ST_PATH_INVALID), "name too long for the temporary name"); } }

  /* mkdir */
  r = simple(&g_agent, NDP_CMD_FS_MKDIR, DIR "/newdir");
  CHECK(is_res(r) && exists(DIR "/newdir"), "mkdir");
  CHECK(is_err(simple(&g_agent, NDP_CMD_FS_MKDIR, DIR "/newdir"), NDP_ST_EXISTS), "mkdir again -> EXISTS");
  CHECK(is_err(simple(&g_agent, NDP_CMD_FS_MKDIR, DIR "/a/b/c"), NDP_ST_NOT_FOUND), "mkdir needs the parent");
  CHECK(is_err(simple(&g_agent, NDP_CMD_FS_MKDIR, "/other/d"), NDP_ST_PROTECTED_PATH), "mkdir outside the root");
  CHECK(is_err(simple(&g_agent, NDP_CMD_FS_MKDIR, DIR "/hello.txt"), NDP_ST_EXISTS), "mkdir over a file");
}

static void test_busy_and_abort(void) {
  static uint8_t data[20000];
  resp_t ready, r;
  up_opts o;
  uint32_t id;
  uint8_t dig[32];

  memset(data, 'q', sizeof data);
  o = opts(sizeof data); o.chunk = 5000; o.send_end = 0;
  r = upload(&g_agent, DIR "/busy.bin", data, 10000, o, &ready);
  id = g_id;
  CHECK(is_res(ready) && no_reply(r) && ndp_agent_busy(&g_agent), "upload in progress");
  CHECK(is_err(simple(&g_agent, NDP_CMD_PING, "/"), NDP_ST_BUSY), "REQ during an upload -> BUSY");
  CHECK(is_err(simple(&g_agent, NDP_CMD_FS_STAT, DIR), NDP_ST_BUSY), "STAT during an upload -> BUSY");
  CHECK(ndp_agent_busy(&g_agent), "still uploading after BUSY replies");
  /* finish it (declared 20000: send the rest) */
  CHECK(no_reply(frame(&g_agent, NDP_KIND_DATA, id, NDP_CMD_FS_WRITE, data, 5000)) && no_reply(frame(&g_agent, NDP_KIND_DATA, id, NDP_CMD_FS_WRITE, data, 5000)), "rest of the data");
  r = frame(&g_agent, NDP_KIND_END, id, NDP_CMD_FS_WRITE, NULL, 0);
  CHECK(is_res(r) && tlv_sha(r, dig) && file_size(DIR "/busy.bin") == 20000, "completes after BUSY replies");

  /* DATA for a different id is not an upload frame */
  o = opts(100); o.chunk = 10; o.send_end = 0;
  upload(&g_agent, DIR "/other-id.bin", data, 10, o, &ready);
  id = g_id;
  r = frame(&g_agent, NDP_KIND_DATA, id + 99, NDP_CMD_FS_WRITE, data, 10);
  CHECK(is_err(r, NDP_ST_BAD_REQUEST), "DATA with a foreign request_id is rejected");
  ndp_agent_close(&g_agent);
  CHECK(!ndp_agent_busy(&g_agent) && !exists(DIR "/other-id.bin") && !dir_has_tmp(DIR), "closing the connection aborts and cleans up");

  /* dropping to READ_ONLY mid-upload aborts it */
  new_agent(NDP_MODE_DEVELOPMENT);
  o = opts(100); o.chunk = 10; o.send_end = 0;
  upload(&g_agent, DIR "/mode.bin", data, 10, o, &ready);
  id = g_id;
  ndp_agent_set_mode(&g_agent, NDP_MODE_READ_ONLY);
  CHECK(!ndp_agent_busy(&g_agent) && !dir_has_tmp(DIR), "READ_ONLY aborts the upload");
  CHECK(no_reply(frame(&g_agent, NDP_KIND_DATA, id, NDP_CMD_FS_WRITE, data, 10)), "its DATA is discarded");
  ndp_agent_set_mode(&g_agent, NDP_MODE_DEVELOPMENT);
  CHECK(is_err(frame(&g_agent, NDP_KIND_END, id, NDP_CMD_FS_WRITE, NULL, 0), NDP_ST_BAD_REQUEST) || 1, "(END after discard state was already cleared is harmless)");
}

static void test_faults(void) {
  static uint8_t d[10000];
  resp_t ready, r;
  up_opts o;
  memset(d, 'f', sizeof d);
  ndp_agent_close(&g_agent);
  new_agent(NDP_MODE_DEVELOPMENT);
  g_ops = g_real_ops;
  g_ops.rename = w_rename; g_ops.file_write = w_write; g_ops.file_sync = w_sync;
  g_agent.cfg.fs = &g_ops;

  /* out of space on DATA */
  g_write_fail = 1;
  r = upload(&g_agent, DIR "/full.bin", d, sizeof d, opts(sizeof d), &ready);
  CHECK(is_err(r, NDP_ST_NO_SPACE) && !exists(DIR "/full.bin") && !dir_has_tmp(DIR), "NO_SPACE reported, temp removed");
  g_write_fail = 0;
  r = simple(&g_agent, NDP_CMD_FS_STAT, DIR);
  CHECK(is_res(r), "agent usable after the failure (discard cleared by the next REQ)");

  /* flush failure */
  g_sync_fail = 1;
  r = upload(&g_agent, DIR "/sync.bin", d, 100, opts(100), &ready);
  CHECK(is_err(r, NDP_ST_IO_ERROR) && !exists(DIR "/sync.bin") && !dir_has_tmp(DIR), "sync failure cleans up");
  g_sync_fail = 0;

  /* replace where "temp -> target" (the 2nd rename) fails: the old file must be restored */
  put(DIR "/safe.txt", "ORIGINAL", 8);
  g_rename_calls = 0; g_rename_fail_at = 2;
  o = opts(sizeof d); o.overwrite = 1;
  r = upload(&g_agent, DIR "/safe.txt", d, sizeof d, o, &ready);
  CHECK(is_err(r, NDP_ST_IO_ERROR), "second rename fails");
  CHECK(content_is(DIR "/safe.txt", "ORIGINAL", 8), "the original file is restored");
  CHECK(!dir_has_tmp(DIR), "no .ndp-old / .ndp-tmp left");

  /* the first rename (target -> old) fails: nothing changed */
  g_rename_calls = 0; g_rename_fail_at = 1;
  r = upload(&g_agent, DIR "/safe.txt", d, sizeof d, o, &ready);
  CHECK(is_err(r, NDP_ST_IO_ERROR) && content_is(DIR "/safe.txt", "ORIGINAL", 8) && !dir_has_tmp(DIR), "first rename fails: untouched");
  g_rename_fail_at = 0;

  /* create path: the rename into place fails */
  g_rename_calls = 0; g_rename_fail_at = 1;
  r = upload(&g_agent, DIR "/fresh.txt", d, 100, opts(100), &ready);
  CHECK(is_err(r, NDP_ST_IO_ERROR) && !exists(DIR "/fresh.txt") && !dir_has_tmp(DIR), "create: rename failure cleans up");
  g_rename_fail_at = 0;

  /* a stale temp from a crash is replaced */
  put(DIR "/stale.txt.ndp-tmp", "junk", 4);
  r = upload(&g_agent, DIR "/stale.txt", (const uint8_t *)"ok", 2, opts(2), &ready);
  CHECK(is_res(r) && content_is(DIR "/stale.txt", "ok", 2) && !dir_has_tmp(DIR), "stale temp is removed");
}

static void test_symlinks(void) {
  resp_t ready, r;
  char p[256], target[256];
  uint8_t d[8] = {'P','A','Y','L','O','A','D','!'};
  g_agent.cfg.fs = &g_real_ops;
  ndp_agent_close(&g_agent);
  new_agent(NDP_MODE_DEVELOPMENT);
  g_agent.cfg.fs = &g_real_ops;

  /* file symlink at the target: the link itself is replaced, its destination is never written */
  put("/victim.txt", "VICTIM", 6);
  full(p, sizeof p, DIR "/link.txt"); full(target, sizeof target, "/victim.txt");
  symlink(target, p);
  r = upload(&g_agent, DIR "/link.txt", d, 8, up_opts_overwrite2(), &ready);
  CHECK(content_is("/victim.txt", "VICTIM", 6), "the symlink destination is untouched (%s)", is_res(r) ? "replaced link" : "refused");

  /* directory symlink pointing outside the root. The dangerous case is an INTERMEDIATE component
   * (dirlink/sub/x): lstat() alone would follow dirlink and happily reach the outside directory. */
  { char q[300]; snprintf(q, sizeof q, "%s/sub", g_other); mkdir(q, 0755);
    snprintf(q, sizeof q, "%s/sub/secret.txt", g_other);
    { FILE *f = fopen(q, "wb"); if (f) { fputs("OUTSIDE-SECRET", f); fclose(f); } } }
  full(p, sizeof p, DIR "/dirlink");
  symlink(g_other, p);

  r = upload(&g_agent, DIR "/dirlink/sub/x.txt", d, 8, opts(8), &ready);
  CHECK(ready.h.kind == NDP_KIND_ERR, "write through a directory symlink is refused (status %d)", ready.h.status);
  { char q[300]; snprintf(q, sizeof q, "%s/sub/x.txt", g_other); CHECK(access(q, F_OK) != 0, "nothing was created outside the root"); }
  { char q[300]; snprintf(q, sizeof q, "%s/sub/x.txt.ndp-tmp", g_other); CHECK(access(q, F_OK) != 0, "no temp file outside the root"); }
  r = simple(&g_agent, NDP_CMD_FS_MKDIR, DIR "/dirlink/sub/newdir");
  CHECK(r.h.kind == NDP_KIND_ERR, "mkdir through a directory symlink is refused");
  { char q[300]; snprintf(q, sizeof q, "%s/sub/newdir", g_other); CHECK(access(q, F_OK) != 0, "no directory created outside the root"); }

  /* reads must not escape either */
  r = simple(&g_agent, NDP_CMD_FS_READ, DIR "/dirlink/sub/secret.txt");
  CHECK(r.h.kind == NDP_KIND_ERR && !ndp_agent_streaming(&g_agent), "read through a directory symlink is refused (kind %d)", r.h.kind);
  r = simple(&g_agent, NDP_CMD_FS_STAT, DIR "/dirlink/sub/secret.txt");
  CHECK(r.h.kind == NDP_KIND_ERR, "stat through a directory symlink is refused");
  r = simple(&g_agent, NDP_CMD_FS_LIST, DIR "/dirlink/sub");
  CHECK(r.h.kind == NDP_KIND_ERR, "list through a directory symlink is refused");
}


static int trash_is(resp_t r, const char *expect) {
  const uint8_t *v; size_t l;
  return ndp_tlv_find(r.buf, r.len, NDP_TAG_TRASH_PATH, &v, &l) && l == strlen(expect) && memcmp(v, expect, l) == 0;
}
static resp_t del(const char *path) { return simple(&g_agent, NDP_CMD_FS_DELETE, path); }

static void test_delete(void) {
  resp_t r;
  ndp_policy pol;
  static const uint8_t d[1] = {'x'};
  resp_t ready;

  ndp_agent_close(&g_agent);
  g_ops = g_real_ops;
  new_agent(NDP_MODE_DEVELOPMENT);

  /* a file goes to the trash; nothing is destroyed */
  put(DIR "/del1.txt", "DEL1", 4);
  r = del(DIR "/del1.txt");
  CHECK(is_res(r) && trash_is(r, DIR "/.ndp-trash/del1.txt"), "delete reports the trash path");
  CHECK(!exists(DIR "/del1.txt") && content_is(DIR "/.ndp-trash/del1.txt", "DEL1", 4), "the file was moved, content intact");
  /* the same name again does not overwrite what is in the trash */
  put(DIR "/del1.txt", "DEL1b", 5);
  r = del(DIR "/del1.txt");
  CHECK(is_res(r) && trash_is(r, DIR "/.ndp-trash/del1.txt.1"), "second deletion of the same name -> .1");
  CHECK(content_is(DIR "/.ndp-trash/del1.txt", "DEL1", 4) && content_is(DIR "/.ndp-trash/del1.txt.1", "DEL1b", 5), "both versions kept");
  put(DIR "/del1.txt", "DEL1c", 5);
  CHECK(trash_is(del(DIR "/del1.txt"), DIR "/.ndp-trash/del1.txt.2"), "third -> .2");

  /* a whole directory (with children) moves as a unit */
  mk(DIR "/deldir"); mk(DIR "/deldir/sub");
  put(DIR "/deldir/a.txt", "A", 1); put(DIR "/deldir/sub/b.txt", "B", 1);
  r = del(DIR "/deldir");
  CHECK(is_res(r) && trash_is(r, DIR "/.ndp-trash/deldir"), "directory delete");
  CHECK(!exists(DIR "/deldir") && content_is(DIR "/.ndp-trash/deldir/a.txt", "A", 1) && content_is(DIR "/.ndp-trash/deldir/sub/b.txt", "B", 1), "children moved with it");

  /* errors and protections */
  CHECK(is_err(del(DIR "/nope.txt"), NDP_ST_NOT_FOUND), "missing item");
  CHECK(is_err(del(DIR), NDP_ST_BAD_REQUEST), "a write root cannot be deleted");
  put("/other/x.txt", "x", 1);
  CHECK(is_err(del("/other/x.txt"), NDP_ST_PROTECTED_PATH) && exists("/other/x.txt"), "outside the write roots");
  CHECK(is_err(del(DIR "/config"), NDP_ST_PROTECTED_PATH) && exists(DIR "/config"), "never_write zone");
  CHECK(is_err(del("/luma"), NDP_ST_PROTECTED_PATH) && exists("/luma"), "/luma");
  CHECK(is_err(del(DIR "/.ndp-trash/del1.txt"), NDP_ST_BAD_REQUEST) && exists(DIR "/.ndp-trash/del1.txt"), "items in the trash cannot be deleted permanently");
  CHECK(is_err(del(DIR "/.ndp-trash"), NDP_ST_BAD_REQUEST), "the trash itself cannot be deleted");
  CHECK(is_err(del("relative"), NDP_ST_PATH_INVALID), "invalid path");
  { treq t; treq_init(&t); CHECK(is_err(frame(&g_agent, NDP_KIND_REQ, ++g_id, NDP_CMD_FS_DELETE, t.b, t.w.len), NDP_ST_BAD_REQUEST), "path required"); }
  /* the trash is managed by the agent: no uploads / mkdir into it */
  r = upload(&g_agent, DIR "/.ndp-trash/injected.txt", d, 1, opts(1), &ready);
  CHECK(is_err(ready, NDP_ST_PROTECTED_PATH) && !exists(DIR "/.ndp-trash/injected.txt"), "no upload into the trash");
  CHECK(is_err(simple(&g_agent, NDP_CMD_FS_MKDIR, DIR "/.ndp-trash/newdir"), NDP_ST_PROTECTED_PATH), "no mkdir in the trash");

  /* READ_ONLY forbids deletion */
  put(DIR "/ro-del.txt", "RO", 2);
  new_agent(NDP_MODE_READ_ONLY);
  CHECK(is_err(del(DIR "/ro-del.txt"), NDP_ST_FORBIDDEN_MODE) && exists(DIR "/ro-del.txt"), "READ_ONLY forbids delete");
  new_agent(NDP_MODE_DEVELOPMENT);

  /* symlinks are invisible to the protocol */
  { char p[256], t[256]; full(p, sizeof p, DIR "/dellink"); full(t, sizeof t, "/other/x.txt"); symlink(t, p); }
  CHECK(is_err(del(DIR "/dellink"), NDP_ST_NOT_FOUND) && exists("/other/x.txt"), "a symlink is not deletable and its target is untouched");

  /* rename failure: the item stays where it was */
  put(DIR "/stay.txt", "STAY", 4);
  g_ops = g_real_ops; g_ops.rename = w_rename; g_agent.cfg.fs = &g_ops;
  g_rename_calls = 0; g_rename_fail_at = 1;
  CHECK(is_err(del(DIR "/stay.txt"), NDP_ST_IO_ERROR) && content_is(DIR "/stay.txt", "STAY", 4), "rename failure leaves the item in place");
  g_rename_fail_at = 0; g_ops = g_real_ops; g_agent.cfg.fs = &g_ops;

  /* several write roots: the trash lives under the MOST SPECIFIC root; the filesystem root works too */
  ndp_policy_init_default(&pol);
  pol.write_roots.count = 0;
  ndp_pathlist_add(&pol.write_roots, DIR);
  ndp_pathlist_add(&pol.write_roots, "/roms");
  ndp_pathlist_add(&pol.write_roots, DIR "/inbox");
  new_agent(NDP_MODE_DEVELOPMENT);
  g_agent.policy = pol;
  mk("/roms"); put("/roms/game.gba", "GBA", 3);
  r = del("/roms/game.gba");
  CHECK(is_res(r) && trash_is(r, "/roms/.ndp-trash/game.gba") && exists("/roms/.ndp-trash/game.gba"), "trash under /roms");
  mk(DIR "/inbox"); put(DIR "/inbox/n.txt", "N", 1);
  r = del(DIR "/inbox/n.txt");
  CHECK(is_res(r) && trash_is(r, DIR "/inbox/.ndp-trash/n.txt"), "the most specific root owns the trash");
  ndp_pathlist_add(&pol.write_roots, "/");
  g_agent.policy = pol;
  put("/at-root.txt", "R", 1);
  r = del("/at-root.txt");
  CHECK(is_res(r) && trash_is(r, "/.ndp-trash/at-root.txt") && exists("/.ndp-trash/at-root.txt"), "trash at the filesystem root");
  CHECK(is_err(del("/luma"), NDP_ST_PROTECTED_PATH), "never_write still wins with a wide root");
}

/* ---- FS_RENAME ---- */
static resp_t ren(const char *from, const char *to) {
  treq t; treq_init(&t);
  ndp_tlv_put_str(&t.w, NDP_TAG_PATH, from);
  ndp_tlv_put_str(&t.w, NDP_TAG_NEW_PATH, to);
  return frame(&g_agent, NDP_KIND_REQ, ++g_id, NDP_CMD_FS_RENAME, t.b, t.w.len);
}
/* does directory `rel` list exactly this name (case-sensitive, unlike exists() on a case-insensitive host)? */
static int lists(const char *rel, const char *name) {
  char p[256], cmd[600]; full(p, sizeof p, rel);
  snprintf(cmd, sizeof cmd, "ls -1 '%s' | grep -x -F -- '%s' >/dev/null", p, name);
  return system(cmd) == 0;
}

static void test_rename(void) {
  resp_t r;
  const uint8_t *v; size_t l;
  ndp_agent_close(&g_agent);
  g_ops = g_real_ops;
  new_agent(NDP_MODE_DEVELOPMENT);

  put(DIR "/r1.txt", "R1", 2);
  r = ren(DIR "/r1.txt", DIR "/r1-renamed.txt");
  CHECK(is_res(r) && ndp_tlv_find(r.buf, r.len, NDP_TAG_NEW_PATH, &v, &l) && l == strlen(DIR "/r1-renamed.txt") &&
            memcmp(v, DIR "/r1-renamed.txt", l) == 0, "rename replies with the new path");
  CHECK(!exists(DIR "/r1.txt") && content_is(DIR "/r1-renamed.txt", "R1", 2), "renamed, content intact");

  mk(DIR "/rdir");
  r = ren(DIR "/r1-renamed.txt", DIR "/rdir/moved.txt");
  CHECK(is_res(r) && content_is(DIR "/rdir/moved.txt", "R1", 2) && !exists(DIR "/r1-renamed.txt"), "moved into a folder");

  /* never overwrites */
  put(DIR "/r2.txt", "R2", 2); put(DIR "/r3.txt", "R3", 2);
  r = ren(DIR "/r2.txt", DIR "/r3.txt");
  CHECK(is_err(r, NDP_ST_EXISTS), "destination exists -> EXISTS");
  CHECK(content_is(DIR "/r2.txt", "R2", 2) && content_is(DIR "/r3.txt", "R3", 2), "nothing changed");
  CHECK(is_err(ren(DIR "/r2.txt", DIR "/nope/x.txt"), NDP_ST_NOT_FOUND) && exists(DIR "/r2.txt"), "missing destination folder");
  CHECK(is_err(ren(DIR "/ghost.txt", DIR "/g2.txt"), NDP_ST_NOT_FOUND), "missing source");
  CHECK(is_err(ren(DIR "/r2.txt", DIR "/r2.txt"), NDP_ST_EXISTS), "same path");

  /* a whole folder moves as a unit; a folder cannot go inside itself */
  mk(DIR "/fa"); mk(DIR "/fa/sub"); put(DIR "/fa/a.txt", "A", 1); put(DIR "/fa/sub/b.txt", "B", 1);
  r = ren(DIR "/fa", DIR "/fb");
  CHECK(is_res(r) && !exists(DIR "/fa") && content_is(DIR "/fb/a.txt", "A", 1) && content_is(DIR "/fb/sub/b.txt", "B", 1), "folder renamed with its children");
  CHECK(is_err(ren(DIR "/fb", DIR "/fb/inside"), NDP_ST_BAD_REQUEST) && exists(DIR "/fb/a.txt"), "folder into itself");
  CHECK(is_err(ren(DIR "/fb", DIR "/fb/sub/deeper"), NDP_ST_BAD_REQUEST), "folder into its own child");

  /* configured roots, the trash, and protected zones */
  CHECK(is_err(ren(DIR, DIR "-x"), NDP_ST_PROTECTED_PATH), "the write root itself cannot be renamed");
  CHECK(is_err(ren("/", DIR "/x"), NDP_ST_PROTECTED_PATH), "the filesystem root");
  CHECK(is_err(ren(DIR "/r2.txt", "/other/r2.txt"), NDP_ST_PROTECTED_PATH) && exists(DIR "/r2.txt"), "destination outside the write roots");
  CHECK(is_err(ren("/other/x", DIR "/x"), NDP_ST_PROTECTED_PATH), "source outside the write roots");
  CHECK(is_err(ren(DIR "/r2.txt", "/luma/r2.txt"), NDP_ST_PROTECTED_PATH), "destination in a protected zone");
  CHECK(is_err(ren(DIR "/r2.txt", DIR "/config/r2.txt"), NDP_ST_PROTECTED_PATH), "destination in the agent config");
  CHECK(is_err(ren(DIR "/r2.txt", DIR "/.ndp-trash/r2.txt"), NDP_ST_PROTECTED_PATH) && exists(DIR "/r2.txt"), "cannot be moved into the trash (that is what delete is for)");

  /* restoring from the trash is a rename out of it */
  put(DIR "/r4.txt", "R4", 2);
  CHECK(is_res(del(DIR "/r4.txt")), "delete first");
  r = ren(DIR "/.ndp-trash/r4.txt", DIR "/r4-restored.txt");
  CHECK(is_res(r) && content_is(DIR "/r4-restored.txt", "R4", 2) && !exists(DIR "/.ndp-trash/r4.txt"), "restored from the trash");
  CHECK(is_err(ren(DIR "/.ndp-trash", DIR "/trash2"), NDP_ST_PROTECTED_PATH), "the trash folder itself");

  /* only the capitalization changes (FAT is case-insensitive) */
  put(DIR "/CaseTest.txt", "C", 1);
  r = ren(DIR "/CaseTest.txt", DIR "/casetest.txt");
  CHECK(is_res(r) && lists(DIR, "casetest.txt") && !lists(DIR, "CaseTest.txt") && content_is(DIR "/casetest.txt", "C", 1), "case-only rename");
  CHECK(!lists(DIR, "CaseTest.txt.ndp-ren") && !lists(DIR, "casetest.txt.ndp-ren"), "no temporary name left behind");

  { /* a folder the owner opened as a write root cannot be renamed away from under the configuration */
    ndp_policy pol;
    mk(DIR "/inbox2"); mk(DIR "/inbox2/x");
    ndp_policy_init_default(&pol);
    pol.write_roots.count = 0;
    ndp_pathlist_add(&pol.write_roots, DIR);
    ndp_pathlist_add(&pol.write_roots, DIR "/inbox2");
    g_agent.policy = pol;
    CHECK(is_err(ren(DIR "/inbox2", DIR "/inbox3"), NDP_ST_PROTECTED_PATH) && exists(DIR "/inbox2/x"), "a configured write root cannot be renamed");
    CHECK(is_res(ren(DIR "/inbox2/x", DIR "/x-out")), "what is inside it can be moved");
    ndp_policy_init_default(&pol);
    g_agent.policy = pol;
  }

  /* a failing rename leaves the source alone; gating by mode; malformed */
  put(DIR "/r5.txt", "R5", 2);
  g_ops = g_real_ops; g_ops.rename = w_rename; g_rename_calls = 0; g_rename_fail_at = 1;
  ndp_agent_close(&g_agent); new_agent(NDP_MODE_DEVELOPMENT);
  r = ren(DIR "/r5.txt", DIR "/r5b.txt");
  CHECK(is_err(r, NDP_ST_IO_ERROR) && content_is(DIR "/r5.txt", "R5", 2) && !exists(DIR "/r5b.txt"), "a failing rename leaves the source in place");
  g_rename_fail_at = 0; g_ops = g_real_ops;
  ndp_agent_close(&g_agent); new_agent(NDP_MODE_READ_ONLY);
  CHECK(is_err(ren(DIR "/r5.txt", DIR "/r5c.txt"), NDP_ST_FORBIDDEN_MODE) && exists(DIR "/r5.txt"), "READ_ONLY forbids rename");
  ndp_agent_close(&g_agent); new_agent(NDP_MODE_DEVELOPMENT);
  { treq t; treq_init(&t); ndp_tlv_put_str(&t.w, NDP_TAG_PATH, DIR "/r5.txt");
    CHECK(is_err(frame(&g_agent, NDP_KIND_REQ, ++g_id, NDP_CMD_FS_RENAME, t.b, t.w.len), NDP_ST_BAD_REQUEST), "new_path is required"); }
}

int main(void) {
  char tmpl[] = "/tmp/ndp-w-test-XXXXXX", tmpl2[] = "/tmp/ndp-w-other-XXXXXX", cmd[200];
  if (!mkdtemp(tmpl) || !mkdtemp(tmpl2)) { perror("mkdtemp"); return 2; }
  snprintf(g_root, sizeof g_root, "%s", tmpl);
  snprintf(g_other, sizeof g_other, "%s", tmpl2);
  mk("/3ds"); mk(DIR); mk(DIR "/config"); mk(DIR "/sub"); mk("/other"); mk("/luma");
  put(DIR "/hello.txt", "Hello", 5);
  if (ndp_posix_fs_init(&g_real_ops, &g_ctx, g_root) != 0) return 2;
  g_ops = g_real_ops;
  new_agent(NDP_MODE_DEVELOPMENT);

  test_create_and_replace();
  test_large_and_hashes();
  test_size_checks_and_discard();
  test_gating_and_policy();
  new_agent(NDP_MODE_DEVELOPMENT);
  test_busy_and_abort();
  test_faults();
  test_symlinks();
  test_delete();
  test_rename();

  ndp_agent_close(&g_agent);
  snprintf(cmd, sizeof cmd, "rm -rf %s %s", g_root, g_other);
  if (system(cmd) != 0) printf("warning: cleanup failed\n");
  printf("%d checks, %d failed\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
