/* FS_LIST / FS_STAT / FS_READ against a real temporary directory (POSIX backend + agent core). */
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE 1
#define _DEFAULT_SOURCE 1
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ndp/ndp_access.h"
#include "ndp/ndp_agent.h"
#include "ndp/ndp_names.h"
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

static char g_root[64];
static ndp_posix_fs_ctx g_ctx;
static ndp_fs_ops g_ops;
static ndp_agent g_agent;
static uint8_t g_out[NDP_HEADER_SIZE + 65536];
static uint32_t g_id = 100;

static void fill_pattern(uint8_t *b, size_t n, unsigned seed) {
  size_t i;
  for (i = 0; i < n; i++) b[i] = (uint8_t)((i * 131u + seed + (i >> 8)) & 0xFF);
}

static void write_file(const char *rel, const void *data, size_t n) {
  char p[256];
  FILE *f;
  snprintf(p, sizeof p, "%s%s", g_root, rel);
  f = fopen(p, "wb");
  if (!f) { printf("cannot create %s: %s\n", p, strerror(errno)); exit(2); }
  if (n) fwrite(data, 1, n, f);
  fclose(f);
}

static void make_dir(const char *rel) {
  char p[256];
  snprintf(p, sizeof p, "%s%s", g_root, rel);
  mkdir(p, 0755);
}

/* ---- request helpers ---- */
typedef struct {
  ndp_header h;
  const uint8_t *pl;
  size_t len;
} resp_t;

static resp_t parse(size_t n) {
  resp_t r;
  memset(&r, 0, sizeof r);
  if (n >= NDP_HEADER_SIZE && ndp_header_decode(g_out, &r.h) == NDP_OK) {
    r.pl = g_out + NDP_HEADER_SIZE;
    r.len = r.h.payload_len;
  }
  return r;
}

static resp_t send_req(ndp_agent *a, uint16_t cmd, const uint8_t *tlv, size_t tlv_len) {
  ndp_header h;
  memset(&h, 0, sizeof h);
  h.version = 1; h.kind = NDP_KIND_REQ; h.request_id = ++g_id; h.command = cmd; h.payload_len = (uint32_t)tlv_len;
  return parse(ndp_agent_handle(a, &h, tlv, g_out, sizeof g_out));
}

typedef struct { uint8_t b[512]; ndp_tlv_w w; } treq;
static void treq_init(treq *t) { ndp_tlv_w_init(&t->w, t->b, sizeof t->b); }
static void t_path(treq *t, const char *p) { ndp_tlv_put_str(&t->w, NDP_TAG_PATH, p); }
static resp_t go(ndp_agent *a, uint16_t cmd, treq *t) { return send_req(a, cmd, t->b, t->w.len); }
static resp_t simple(ndp_agent *a, uint16_t cmd, const char *path) {
  treq t; treq_init(&t); t_path(&t, path); return go(a, cmd, &t);
}

static int is_err(resp_t r, int status) { return r.h.kind == NDP_KIND_ERR && r.h.status == status; }
static int is_res(resp_t r) { return r.h.kind == NDP_KIND_RES && r.h.status == 0; }

static uint64_t tlv_u64(resp_t r, uint16_t tag) {
  const uint8_t *v; size_t l;
  return (ndp_tlv_find(r.pl, r.len, tag, &v, &l) && l == 8) ? ndp_tlv_get_u64(v) : (uint64_t)-1;
}
static int tlv_u8(resp_t r, uint16_t tag) {
  const uint8_t *v; size_t l;
  return (ndp_tlv_find(r.pl, r.len, tag, &v, &l) && l == 1) ? v[0] : -1;
}
static long tlv_u32(resp_t r, uint16_t tag) {
  const uint8_t *v; size_t l;
  return (ndp_tlv_find(r.pl, r.len, tag, &v, &l) && l == 4) ? (long)ndp_tlv_get_u32(v) : -1;
}

/* All ENTRY fields of a LIST response: "type:name" appended to `names` ('\n' separated). Returns count. */
static int collect_entries(resp_t r, char *names, size_t cap, int *dirs) {
  size_t i = 0, used = 0;
  int count = 0;
  if (dirs) *dirs = 0;
  names[0] = '\0';
  while (r.len - i >= 4) {
    uint16_t tag = (uint16_t)(r.pl[i] | (r.pl[i + 1] << 8));
    size_t vl = (size_t)r.pl[i + 2] | ((size_t)r.pl[i + 3] << 8);
    if (tag == NDP_TAG_ENTRY && vl >= 9 && used + vl < cap) {
      memcpy(names + used, r.pl + i + 4 + 9, vl - 9);
      used += vl - 9;
      names[used++] = '\n';
      names[used] = '\0';
      if (r.pl[i + 4] == NDP_TYPE_DIR && dirs) (*dirs)++;
      count++;
    }
    i += 4 + vl;
  }
  return count;
}

/* Drains an active transfer: concatenates DATA payloads into `dst`. Returns the number of bytes, or -1 on ERR.
 * Reports frame count / END digest through the out-params. */
static long drain(ndp_agent *a, uint8_t *dst, size_t cap, int *frames, uint8_t *digest, int *got_digest, int *status) {
  size_t total = 0;
  int guard = 0, last_more = -1;
  *frames = 0;
  *got_digest = 0;
  if (status) *status = 0;
  while (ndp_agent_streaming(a) && guard++ < 100000) {
    size_t n = ndp_agent_next_frame(a, g_out, sizeof g_out);
    resp_t r = parse(n);
    if (n == 0) return -2;
    if (r.h.kind == NDP_KIND_DATA) {
      int last = !(r.h.flags & NDP_FLAG_MORE);
      CHECK(last_more != 0, "no DATA may follow a DATA frame without MORE");
      last_more = last ? 0 : 1;
      if (total + r.len > cap) return -3;
      memcpy(dst + total, r.pl, r.len);
      total += r.len;
      (*frames)++;
      CHECK(r.h.status == 0 && r.h.command == NDP_CMD_FS_READ, "DATA frame header");
      if (last && ndp_agent_streaming(a) == 0) return -4; /* DATA must be followed by END */
    } else if (r.h.kind == NDP_KIND_END) {
      const uint8_t *v; size_t l;
      CHECK(*frames == 0 || last_more == 0, "the last DATA frame must not carry MORE");
      if (ndp_tlv_find(r.pl, r.len, NDP_TAG_SHA256, &v, &l) && l == 32 && digest) { memcpy(digest, v, 32); *got_digest = 1; }
      return (long)total;
    } else if (r.h.kind == NDP_KIND_ERR) {
      if (status) *status = r.h.status;
      return -1;
    } else return -5;
  }
  return -6;
}

static void new_agent(ndp_agent *a, const ndp_fs_ops *fs) {
  ndp_agent_config cfg;
  treq t; resp_t r;
  memset(&cfg, 0, sizeof cfg);
  cfg.platform = "host"; cfg.agent_version = "t"; cfg.auth = "none"; cfg.mode = NDP_MODE_READ_ONLY;
  cfg.max_frame = 65536; cfg.fs = fs;
  ndp_agent_init(a, &cfg);
  treq_init(&t);
  ndp_tlv_put_u16(&t.w, NDP_TAG_PROTOCOL, 1);
  ndp_tlv_put_u16(&t.w, NDP_TAG_PROTOCOL_MAX, 1);
  { uint8_t n[16] = {0}; ndp_tlv_put(&t.w, NDP_TAG_NONCE, n, 16); }
  r = go(a, NDP_CMD_HELLO, &t);
  CHECK(is_res(r), "hello for test agent");
}

static const uint8_t HELLO_TXT[] = "Hello from Nintendo 3DS\n";
static uint8_t g_big[200000];
static uint8_t g_buf[300000];

static void build_fixture(void) {
  char rel[64];
  int i;
  make_dir("/3ds"); make_dir("/3ds/nintendo-dev-agent"); make_dir("/3ds/nintendo-dev-agent/config");
  make_dir("/dir"); make_dir("/emptydir");
  write_file("/hello.txt", HELLO_TXT, sizeof HELLO_TXT - 1);
  write_file("/empty.txt", "", 0);
  fill_pattern(g_big, sizeof g_big, 7);
  write_file("/big.bin", g_big, sizeof g_big);
  write_file("/3ds/nintendo-dev-agent/agent.log", "log line\n", 9);
  write_file("/3ds/nintendo-dev-agent/config/key", "SECRET", 6);
  for (i = 0; i < 250; i++) { snprintf(rel, sizeof rel, "/dir/f%03d.txt", i); write_file(rel, "x", 1); }
  { /* a name longer than 255 bytes: not addressable, must not be listed */
    char longname[400];
    memset(longname, 'a', 300); longname[300] = '\0';
    snprintf(rel, sizeof rel, "/dir/");
    { char p[512]; snprintf(p, sizeof p, "%s/dir/%s", g_root, longname); FILE *f = fopen(p, "wb"); if (f) { fputc('x', f); fclose(f); } }
  }
  { char link[256], target[256]; snprintf(link, sizeof link, "%s/link.txt", g_root); snprintf(target, sizeof target, "%s/hello.txt", g_root); symlink(target, link); }
}

static void test_stat(void) {
  resp_t r = simple(&g_agent, NDP_CMD_FS_STAT, "/hello.txt");
  CHECK(is_res(r) && tlv_u8(r, NDP_TAG_TYPE) == NDP_TYPE_FILE && tlv_u64(r, NDP_TAG_SIZE) == sizeof HELLO_TXT - 1, "stat file");
  CHECK(tlv_u64(r, NDP_TAG_MTIME) != (uint64_t)-1 && tlv_u64(r, NDP_TAG_MTIME) > 1000000000ull, "stat mtime is a plausible unix time");
  r = simple(&g_agent, NDP_CMD_FS_STAT, "/dir");
  CHECK(is_res(r) && tlv_u8(r, NDP_TAG_TYPE) == NDP_TYPE_DIR, "stat dir");
  r = simple(&g_agent, NDP_CMD_FS_STAT, "/");
  CHECK(is_res(r) && tlv_u8(r, NDP_TAG_TYPE) == NDP_TYPE_DIR, "stat root");
  r = simple(&g_agent, NDP_CMD_FS_STAT, "/nope.txt");
  CHECK(is_err(r, NDP_ST_NOT_FOUND), "stat missing (got kind %d status %d)", r.h.kind, r.h.status);
  r = simple(&g_agent, NDP_CMD_FS_STAT, "/link.txt");
  CHECK(is_err(r, NDP_ST_NOT_FOUND), "symlink is invisible (status %d)", r.h.status);
  r = simple(&g_agent, NDP_CMD_FS_STAT, "relative/path");
  CHECK(is_err(r, NDP_ST_PATH_INVALID), "relative path");
  r = simple(&g_agent, NDP_CMD_FS_STAT, "/a/../hello.txt");
  CHECK(is_err(r, NDP_ST_PATH_INVALID), "dot-dot");
  r = simple(&g_agent, NDP_CMD_FS_STAT, "/3ds/nintendo-dev-agent/config/key");
  CHECK(is_err(r, NDP_ST_PROTECTED_PATH), "never_read zone");
  r = simple(&g_agent, NDP_CMD_FS_STAT, "/3DS/NINTENDO-DEV-AGENT/CONFIG");
  CHECK(is_err(r, NDP_ST_PROTECTED_PATH), "never_read zone, other case");
  { treq t; treq_init(&t); r = go(&g_agent, NDP_CMD_FS_STAT, &t); CHECK(is_err(r, NDP_ST_BAD_REQUEST), "missing path"); }
}

static void test_list(void) {
  static char names[64 * 1024];
  char *p;
  int n, dirs, page, total = 0, pages = 0;
  long next;
  resp_t r = simple(&g_agent, NDP_CMD_FS_LIST, "/");
  CHECK(is_res(r), "list root");
  n = collect_entries(r, names, sizeof names, &dirs);
  CHECK(n == 6 && dirs == 3, "root has 6 entries incl. 3 dirs (the symlink is hidden), got %d / %d dirs", n, dirs);
  CHECK(strstr(names, "hello.txt\n") && strstr(names, "big.bin\n") && strstr(names, "3ds\n") && strstr(names, "emptydir\n"), "root names");
  CHECK(!strstr(names, ".\n") && !strstr(names, "..\n"), "no dot entries");
  CHECK(!strstr(names, "link.txt"), "symlink not listed");
  CHECK(tlv_u8(r, NDP_TAG_LIST_MORE) == 0, "root list complete");
  r = simple(&g_agent, NDP_CMD_FS_LIST, "/emptydir");
  CHECK(is_res(r) && collect_entries(r, names, sizeof names, NULL) == 0 && tlv_u8(r, NDP_TAG_LIST_MORE) == 0, "empty dir");

  /* paging over 250 files (the 300-byte name must be skipped) */
  next = 0;
  for (page = 0; page < 10; page++) {
    treq t; treq_init(&t); t_path(&t, "/dir"); ndp_tlv_put_u32(&t.w, NDP_TAG_CURSOR, (uint32_t)next);
    r = go(&g_agent, NDP_CMD_FS_LIST, &t);
    CHECK(is_res(r), "page %d", page);
    n = collect_entries(r, names, sizeof names, NULL);
    total += n; pages++;
    CHECK(n <= NDP_LIST_PAGE_MAX, "page size cap");
    next = tlv_u32(r, NDP_TAG_NEXT_CURSOR);
    if (tlv_u8(r, NDP_TAG_LIST_MORE) == 0) break;
    CHECK(n == NDP_LIST_PAGE_MAX, "a non-final page is full");
  }
  CHECK(total == 250 && pages == 3, "250 entries in 3 pages, got %d in %d", total, pages);
  /* cursor beyond the end */
  { treq t; treq_init(&t); t_path(&t, "/dir"); ndp_tlv_put_u32(&t.w, NDP_TAG_CURSOR, 9999);
    r = go(&g_agent, NDP_CMD_FS_LIST, &t);
    CHECK(is_res(r) && collect_entries(r, names, sizeof names, NULL) == 0 && tlv_u8(r, NDP_TAG_LIST_MORE) == 0, "cursor past end"); }
  /* re-requesting the same cursor gives the same page (reopen + skip path) */
  { char a1[16 * 1024], a2[16 * 1024];
    treq t; treq_init(&t); t_path(&t, "/dir"); ndp_tlv_put_u32(&t.w, NDP_TAG_CURSOR, 100);
    r = go(&g_agent, NDP_CMD_FS_LIST, &t); collect_entries(r, a1, sizeof a1, NULL);
    r = go(&g_agent, NDP_CMD_FS_LIST, &t); collect_entries(r, a2, sizeof a2, NULL);
    CHECK(strcmp(a1, a2) == 0 && strlen(a1) > 0, "same cursor, same page"); }
  p = strstr(names, "f0"); (void)p;
  r = simple(&g_agent, NDP_CMD_FS_LIST, "/hello.txt");
  CHECK(is_err(r, NDP_ST_BAD_REQUEST), "list a file");
  r = simple(&g_agent, NDP_CMD_FS_LIST, "/3ds/nintendo-dev-agent/config");
  CHECK(is_err(r, NDP_ST_PROTECTED_PATH), "list protected dir");
  r = simple(&g_agent, NDP_CMD_FS_LIST, "/nope");
  CHECK(is_err(r, NDP_ST_NOT_FOUND), "list missing dir");
  r = simple(&g_agent, NDP_CMD_FS_LIST, "/3ds/nintendo-dev-agent");
  n = collect_entries(r, names, sizeof names, NULL);
  CHECK(is_res(r) && strstr(names, "agent.log\n") && strstr(names, "config\n"), "agent folder lists its log and config dir");
}

static void sha_of(const uint8_t *d, size_t n, uint8_t out[32]) { ndp_sha256(d, n, out); }

static void test_read(void) {
  uint8_t dig[32], want[32];
  int frames, got, status;
  long n;
  uint64_t ws;
  resp_t r;
  treq t;

  /* small file, with hash */
  treq_init(&t); t_path(&t, "/hello.txt"); { uint8_t one = 1; ndp_tlv_put(&t.w, NDP_TAG_WANT_HASH, &one, 1); }
  r = go(&g_agent, NDP_CMD_FS_READ, &t);
  CHECK(is_res(r) && tlv_u64(r, NDP_TAG_TOTAL_SIZE) == 24 && tlv_u64(r, NDP_TAG_WILL_SEND) == 24, "read RES");
  CHECK(ndp_agent_streaming(&g_agent), "streaming after RES");
  n = drain(&g_agent, g_buf, sizeof g_buf, &frames, dig, &got, &status);
  CHECK(n == 24 && memcmp(g_buf, HELLO_TXT, 24) == 0 && frames == 1, "hello content (n=%ld frames=%d)", n, frames);
  sha_of(HELLO_TXT, 24, want);
  CHECK(got && memcmp(dig, want, 32) == 0, "sha256 of hello.txt");
  CHECK(!ndp_agent_streaming(&g_agent) && g_agent.last_transfer_bytes == 24, "transfer finished, bytes recorded");

  /* big file in 4096-byte chunks */
  treq_init(&t); t_path(&t, "/big.bin"); ndp_tlv_put_u32(&t.w, NDP_TAG_CHUNK, 4096);
  { uint8_t one = 1; ndp_tlv_put(&t.w, NDP_TAG_WANT_HASH, &one, 1); }
  r = go(&g_agent, NDP_CMD_FS_READ, &t);
  CHECK(is_res(r) && tlv_u64(r, NDP_TAG_WILL_SEND) == sizeof g_big, "big RES");
  n = drain(&g_agent, g_buf, sizeof g_buf, &frames, dig, &got, &status);
  CHECK(n == (long)sizeof g_big && memcmp(g_buf, g_big, sizeof g_big) == 0, "big content (n=%ld)", n);
  CHECK(frames == (int)((sizeof g_big + 4095) / 4096), "49 DATA frames, got %d", frames);
  sha_of(g_big, sizeof g_big, want);
  CHECK(got && memcmp(dig, want, 32) == 0, "sha256 of big.bin");

  /* no hash requested -> END carries none */
  treq_init(&t); t_path(&t, "/hello.txt");
  r = go(&g_agent, NDP_CMD_FS_READ, &t);
  n = drain(&g_agent, g_buf, sizeof g_buf, &frames, dig, &got, &status);
  CHECK(is_res(r) && n == 24 && !got, "END without digest when not requested");

  /* ranges */
  treq_init(&t); t_path(&t, "/big.bin"); ndp_tlv_put_u64(&t.w, NDP_TAG_OFFSET, 1000); ndp_tlv_put_u64(&t.w, NDP_TAG_LENGTH, 5000);
  r = go(&g_agent, NDP_CMD_FS_READ, &t);
  ws = tlv_u64(r, NDP_TAG_WILL_SEND); /* capture before drain(): it reuses g_out */
  n = drain(&g_agent, g_buf, sizeof g_buf, &frames, dig, &got, &status);
  CHECK(is_res(r) && ws == 5000, "offset+length RES (will_send=%llu)", (unsigned long long)ws);
  CHECK(n == 5000, "offset+length byte count (n=%ld)", n);
  CHECK(n == 5000 && memcmp(g_buf, g_big + 1000, 5000) == 0, "offset+length content");
  treq_init(&t); t_path(&t, "/big.bin"); ndp_tlv_put_u64(&t.w, NDP_TAG_OFFSET, sizeof g_big - 10); ndp_tlv_put_u64(&t.w, NDP_TAG_LENGTH, 999999);
  r = go(&g_agent, NDP_CMD_FS_READ, &t);
  n = drain(&g_agent, g_buf, sizeof g_buf, &frames, dig, &got, &status);
  CHECK(is_res(r) && n == 10 && memcmp(g_buf, g_big + sizeof g_big - 10, 10) == 0, "length clamped to the end");
  treq_init(&t); t_path(&t, "/big.bin"); ndp_tlv_put_u64(&t.w, NDP_TAG_OFFSET, sizeof g_big);
  r = go(&g_agent, NDP_CMD_FS_READ, &t);
  ws = tlv_u64(r, NDP_TAG_WILL_SEND);
  n = drain(&g_agent, g_buf, sizeof g_buf, &frames, dig, &got, &status);
  CHECK(is_res(r) && ws == 0 && n == 0 && frames == 0, "offset == size: empty stream");
  treq_init(&t); t_path(&t, "/big.bin"); ndp_tlv_put_u64(&t.w, NDP_TAG_OFFSET, sizeof g_big + 1);
  r = go(&g_agent, NDP_CMD_FS_READ, &t);
  CHECK(is_err(r, NDP_ST_BAD_REQUEST) && !ndp_agent_streaming(&g_agent), "offset beyond end");
  r = simple(&g_agent, NDP_CMD_FS_READ, "/empty.txt");
  ws = tlv_u64(r, NDP_TAG_TOTAL_SIZE);
  n = drain(&g_agent, g_buf, sizeof g_buf, &frames, dig, &got, &status);
  CHECK(is_res(r) && ws == 0 && n == 0, "empty file");

  /* chunk clamping */
  treq_init(&t); t_path(&t, "/big.bin"); ndp_tlv_put_u32(&t.w, NDP_TAG_CHUNK, 1);
  r = go(&g_agent, NDP_CMD_FS_READ, &t);
  n = drain(&g_agent, g_buf, sizeof g_buf, &frames, dig, &got, &status);
  CHECK(n == (long)sizeof g_big && frames == (int)((sizeof g_big + 511) / 512), "chunk=1 clamped to 512 (frames %d)", frames);
  treq_init(&t); t_path(&t, "/big.bin"); ndp_tlv_put_u32(&t.w, NDP_TAG_CHUNK, 4000000);
  r = go(&g_agent, NDP_CMD_FS_READ, &t);
  n = drain(&g_agent, g_buf, sizeof g_buf, &frames, dig, &got, &status);
  CHECK(n == (long)sizeof g_big && frames == 4, "huge chunk clamped to max_frame (frames %d)", frames);

  /* errors */
  r = simple(&g_agent, NDP_CMD_FS_READ, "/dir");
  CHECK(is_err(r, NDP_ST_BAD_REQUEST), "read a directory");
  r = simple(&g_agent, NDP_CMD_FS_READ, "/nope");
  CHECK(is_err(r, NDP_ST_NOT_FOUND), "read missing");
  r = simple(&g_agent, NDP_CMD_FS_READ, "/3ds/nintendo-dev-agent/config/key");
  CHECK(is_err(r, NDP_ST_PROTECTED_PATH), "read protected");
  r = simple(&g_agent, NDP_CMD_FS_READ, "/link.txt");
  CHECK(is_err(r, NDP_ST_NOT_FOUND), "read through a symlink");
  treq_init(&t); t_path(&t, "/hello.txt"); ndp_tlv_put_u16(&t.w, NDP_TAG_OFFSET, 1);
  r = go(&g_agent, NDP_CMD_FS_READ, &t);
  CHECK(is_err(r, NDP_ST_BAD_REQUEST), "offset with the wrong size");
}

static void test_busy_abort_shrink(void) {
  uint8_t dig[32];
  int frames, got, status;
  long n;
  resp_t r;
  treq t;

  /* BUSY while a transfer is active, then normal service afterwards */
  r = simple(&g_agent, NDP_CMD_FS_READ, "/big.bin");
  CHECK(is_res(r) && ndp_agent_streaming(&g_agent), "start transfer");
  { ndp_header h = {1, NDP_KIND_REQ, 0, 777, NDP_CMD_PING, 0, 0};
    r = parse(ndp_agent_handle(&g_agent, &h, NULL, g_out, sizeof g_out));
    CHECK(is_err(r, NDP_ST_BUSY), "PING during a transfer -> BUSY"); }
  r = simple(&g_agent, NDP_CMD_FS_STAT, "/hello.txt");
  CHECK(is_err(r, NDP_ST_BUSY), "STAT during a transfer -> BUSY");
  n = drain(&g_agent, g_buf, sizeof g_buf, &frames, dig, &got, &status);
  CHECK(n == (long)sizeof g_big, "the transfer completes after BUSY replies");
  r = simple(&g_agent, NDP_CMD_FS_STAT, "/hello.txt");
  CHECK(is_res(r), "service resumes after the transfer");

  /* connection closed mid-transfer */
  r = simple(&g_agent, NDP_CMD_FS_READ, "/big.bin");
  ndp_agent_next_frame(&g_agent, g_out, sizeof g_out);
  CHECK(ndp_agent_streaming(&g_agent), "mid-transfer");
  ndp_agent_close(&g_agent);
  CHECK(!ndp_agent_streaming(&g_agent), "close aborts the transfer");
  ndp_agent_close(&g_agent); /* idempotent */

  /* file shrinks while it is being read */
  write_file("/shrink.bin", g_big, 100000);
  treq_init(&t); t_path(&t, "/shrink.bin"); ndp_tlv_put_u32(&t.w, NDP_TAG_CHUNK, 4096);
  r = go(&g_agent, NDP_CMD_FS_READ, &t);
  CHECK(is_res(r), "shrink: start");
  ndp_agent_next_frame(&g_agent, g_out, sizeof g_out); /* first chunk */
  { char p[256]; snprintf(p, sizeof p, "%s/shrink.bin", g_root); if (truncate(p, 5000) != 0) perror("truncate"); }
  n = drain(&g_agent, g_buf, sizeof g_buf, &frames, dig, &got, &status);
  CHECK(n == -1 && status == NDP_ST_IO_ERROR && !ndp_agent_streaming(&g_agent), "shrunk file -> ERR IO_ERROR (n=%ld status=%d)", n, status);

  /* an open directory is released on close and by HELLO */
  { treq lt; treq_init(&lt); t_path(&lt, "/dir");
    r = go(&g_agent, NDP_CMD_FS_LIST, &lt);
    CHECK(is_res(r) && g_agent.dir.open, "dir kept open between pages");
    ndp_agent_close(&g_agent);
    CHECK(!g_agent.dir.open, "close releases the directory"); }
}

static void test_gating(void) {
  ndp_agent a;
  resp_t r;
  ndp_agent_config cfg;
  ndp_header h = {1, NDP_KIND_REQ, 0, 5, NDP_CMD_FS_STAT, 0, 0};
  treq t;

  memset(&cfg, 0, sizeof cfg);
  cfg.platform = "host"; cfg.agent_version = "t"; cfg.auth = "none"; cfg.max_frame = 65536; cfg.fs = &g_ops;
  ndp_agent_init(&a, &cfg);
  treq_init(&t); t_path(&t, "/hello.txt");
  h.payload_len = (uint32_t)t.w.len;
  r = parse(ndp_agent_handle(&a, &h, t.b, g_out, sizeof g_out));
  CHECK(is_err(r, NDP_ST_HELLO_REQUIRED), "FS before HELLO");

  cfg.fs = NULL;
  new_agent(&a, NULL);
  r = parse(ndp_agent_handle(&a, &h, t.b, g_out, sizeof g_out));
  CHECK(is_err(r, NDP_ST_UNSUPPORTED_COMMAND), "FS without a filesystem backend");
}

/* Spec §11.1: with read roots deep in the tree, the directories on the way can be stat'ed and listed, but a
 * listing shows only what leads to a readable folder; everything else stays PROTECTED_PATH. */
static void test_traversal(void) {
  char tmpl[] = "/tmp/ndp-trav-test-XXXXXX", cmd[128], saved[64];
  ndp_posix_fs_ctx ctx;
  ndp_fs_ops ops;
  ndp_agent a;
  ndp_agent_config cfg;
  ndp_policy pol;
  ndp_access acc;
  static char names[64 * 1024];
  resp_t r;
  treq t;
  int i;
  if (!mkdtemp(tmpl)) { perror("mkdtemp"); exit(2); }
  strcpy(saved, g_root);
  snprintf(g_root, sizeof g_root, "%s", tmpl);
  make_dir("/roms"); make_dir("/roms/gba"); make_dir("/roms/nds"); make_dir("/luma"); make_dir("/3ds");
  make_dir("/3ds/nintendo-dev-agent"); make_dir("/3ds/other"); make_dir("/pg"); make_dir("/pg/keep"); make_dir("/pg/keep/sub");
  write_file("/roms/gba/game.gba", "GBA", 3); write_file("/roms/nds/x.nds", "NDS", 3); write_file("/roms/note.txt", "n", 1);
  write_file("/luma/secret", "S", 1); write_file("/top.txt", "t", 1); write_file("/3ds/other/o", "o", 1);
  write_file("/3ds/nintendo-dev-agent/agent.log", "log", 3); write_file("/pg/keep/sub/deep.txt", "d", 1);
  for (i = 0; i < 200; i++) { char rel[64]; snprintf(rel, sizeof rel, "/pg/f%03d", i); make_dir(rel); }
  if (ndp_posix_fs_init(&ops, &ctx, g_root) != 0) exit(2);

  ndp_access_init(&acc);
  CHECK(ndp_access_set(&acc, "/roms/gba", NDP_LVL_READ) == NDP_OK && ndp_access_set(&acc, "/pg/keep/sub", NDP_LVL_READ) == NDP_OK, "owner opens two deep folders");
  ndp_access_to_policy(&acc, &pol);
  memset(&cfg, 0, sizeof cfg);
  cfg.platform = "host"; cfg.agent_version = "t"; cfg.auth = "none"; cfg.mode = NDP_MODE_READ_ONLY;
  cfg.max_frame = 65536; cfg.fs = &ops; cfg.policy = &pol;
  ndp_agent_init(&a, &cfg);
  treq_init(&t);
  ndp_tlv_put_u16(&t.w, NDP_TAG_PROTOCOL, 1); ndp_tlv_put_u16(&t.w, NDP_TAG_PROTOCOL_MAX, 1);
  { uint8_t n[16] = {0}; ndp_tlv_put(&t.w, NDP_TAG_NONCE, n, 16); }
  CHECK(is_res(go(&a, NDP_CMD_HELLO, &t)), "hello");

  r = simple(&a, NDP_CMD_FS_LIST, "/");
  CHECK(is_res(r) && collect_entries(r, names, sizeof names, NULL) == 3 && strstr(names, "roms\n") && strstr(names, "3ds\n") && strstr(names, "pg\n"), "root lists only the ways in: got [%s]", names);
  CHECK(!strstr(names, "luma") && !strstr(names, "top.txt"), "root hides luma and files");
  r = simple(&a, NDP_CMD_FS_LIST, "/roms");
  CHECK(is_res(r) && collect_entries(r, names, sizeof names, NULL) == 1 && strstr(names, "gba\n"), "/roms lists only gba: [%s]", names);
  r = simple(&a, NDP_CMD_FS_LIST, "/3ds");
  CHECK(is_res(r) && collect_entries(r, names, sizeof names, NULL) == 1 && strstr(names, "nintendo-dev-agent\n"), "/3ds lists only the workspace: [%s]", names);
  r = simple(&a, NDP_CMD_FS_LIST, "/pg/keep");
  CHECK(is_res(r) && collect_entries(r, names, sizeof names, NULL) == 1 && strstr(names, "sub\n"), "/pg/keep -> sub");
  { /* paging with a filter: 201 raw entries, 1 shown; the cursor must still walk the whole directory */
    long next = 0; int total = 0, pages = 0;
    do {
      treq q; treq_init(&q); t_path(&q, "/pg"); ndp_tlv_put_u32(&q.w, NDP_TAG_CURSOR, (uint32_t)next);
      r = go(&a, NDP_CMD_FS_LIST, &q);
      CHECK(is_res(r), "filtered page");
      total += collect_entries(r, names, sizeof names, NULL); pages++;
      next = tlv_u32(r, NDP_TAG_NEXT_CURSOR);
    } while (tlv_u8(r, NDP_TAG_LIST_MORE) == 1 && pages < 20);
    CHECK(total == 1 && pages >= 3, "filtered paging (scan cap): one visible entry over several short pages (got %d in %d)", total, pages);
  }
  CHECK(is_res(simple(&a, NDP_CMD_FS_STAT, "/")) && is_res(simple(&a, NDP_CMD_FS_STAT, "/roms")), "stat of traversal directories");
  CHECK(is_err(simple(&a, NDP_CMD_FS_STAT, "/roms/note.txt"), NDP_ST_PROTECTED_PATH), "a file next to the way is not readable");
  CHECK(is_err(simple(&a, NDP_CMD_FS_STAT, "/luma"), NDP_ST_PROTECTED_PATH), "luma: no traversal, no stat");
  CHECK(is_err(simple(&a, NDP_CMD_FS_LIST, "/roms/nds"), NDP_ST_PROTECTED_PATH), "sibling of a root: closed");
  CHECK(is_err(simple(&a, NDP_CMD_FS_LIST, "/luma"), NDP_ST_PROTECTED_PATH), "luma listing closed");
  CHECK(is_err(simple(&a, NDP_CMD_FS_READ, "/top.txt"), NDP_ST_PROTECTED_PATH), "reading a file in a traversal dir: closed");
  CHECK(is_err(simple(&a, NDP_CMD_FS_READ, "/roms"), NDP_ST_PROTECTED_PATH), "READ never traverses");
  r = simple(&a, NDP_CMD_FS_LIST, "/roms/gba");
  CHECK(is_res(r) && collect_entries(r, names, sizeof names, NULL) == 1 && strstr(names, "game.gba\n"), "inside a root: normal listing");
  r = simple(&a, NDP_CMD_FS_LIST, "/3ds/nintendo-dev-agent");
  CHECK(is_res(r) && strstr((collect_entries(r, names, sizeof names, NULL), names), "agent.log\n"), "the workspace is always open");

  { /* ACCESS_INFO tells the client what it may touch */
    treq q; const uint8_t *v; size_t l, i2 = 0; int reads = 0, writes = 0;
    treq_init(&q);
    r = go(&a, NDP_CMD_ACCESS_INFO, &q);
    CHECK(is_res(r), "ACCESS_INFO");
    while (r.len - i2 >= 4) {
      uint16_t tag = (uint16_t)(r.pl[i2] | (r.pl[i2 + 1] << 8));
      size_t vl = (size_t)r.pl[i2 + 2] | ((size_t)r.pl[i2 + 3] << 8);
      if (tag == NDP_TAG_READ_ROOT) reads++;
      if (tag == NDP_TAG_WRITE_ROOT) writes++;
      i2 += 4 + vl;
    }
    CHECK(reads == 3 && writes == 1, "ACCESS_INFO lists 3 read roots (workspace + 2) and 1 write root, got %d/%d", reads, writes);
    CHECK(ndp_tlv_find(r.pl, r.len, NDP_TAG_MODE, &v, &l) && l == 9 && memcmp(v, "READ_ONLY", 9) == 0, "ACCESS_INFO mode");
  }

  /* the owner changes the list on the console: it applies from the next request */
  ndp_access_set(&acc, "/roms/nds", NDP_LVL_READ);
  ndp_access_to_policy(&acc, &pol);
  ndp_agent_set_policy(&a, &pol);
  CHECK(is_res(simple(&a, NDP_CMD_FS_LIST, "/roms/nds")), "the newly opened folder is readable at once");
  ndp_access_set(&acc, "/roms/nds", NDP_LVL_NONE);
  ndp_access_to_policy(&acc, &pol);
  ndp_agent_set_policy(&a, &pol);
  CHECK(is_err(simple(&a, NDP_CMD_FS_LIST, "/roms/nds"), NDP_ST_PROTECTED_PATH), "and closed again at once");

  ndp_agent_close(&a);
  snprintf(cmd, sizeof cmd, "rm -rf %s", tmpl);
  if (system(cmd) != 0) printf("warning: cleanup failed for %s\n", tmpl);
  strcpy(g_root, saved);
}

int main(void) {
  char tmpl[] = "/tmp/ndp-fs-test-XXXXXX";
  char cmd[128];
  if (!mkdtemp(tmpl)) { perror("mkdtemp"); return 2; }
  snprintf(g_root, sizeof g_root, "%s", tmpl);
  build_fixture();
  if (ndp_posix_fs_init(&g_ops, &g_ctx, g_root) != 0) return 2;
  new_agent(&g_agent, &g_ops);

  test_stat();
  test_list();
  test_read();
  test_busy_abort_shrink();
  test_gating();
  test_traversal();

  ndp_agent_close(&g_agent);
  snprintf(cmd, sizeof cmd, "rm -rf %s", g_root);
  if (system(cmd) != 0) printf("warning: cleanup failed for %s\n", g_root);
  printf("%d checks, %d failed\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
