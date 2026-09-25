/* Nintendo Dev Agent — 3DS entry point.
 * Wi-Fi/soc bring-up, the UI, and the main loop. Protocol handling and connection management live in
 * the shared code (agent/common, agent/posix). Network start-up follows the sequence proven by ftpd:
 * acInit -> ACU_GetWifiStatus -> memalign(0x1000, 1 MiB) -> socInit -> NDM exclusive+lock. */
#include <3ds.h>

#include <arpa/inet.h>
#include <malloc.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "alog.h"
#include "ndp/ndp_posix_fs.h"
#include "ndp/ndp_server.h"

#define AGENT_PORT NDP_DEFAULT_PORT

/* DEVELOPMENT BUILD: the agent starts with writes ENABLED (only inside /3ds/nintendo-dev-agent, never in
 * /Nintendo 3DS, /luma, /boot.firm...). There is no pairing yet, so any device on the LAN could write
 * there while this mode is on. Switch to NDP_MODE_READ_ONLY before distributing (ARCHITECTURE.md §5).
 * X toggles the mode at run time. */
#define AGENT_START_MODE NDP_MODE_DEVELOPMENT
#define SOC_ALIGN 0x1000
#define SOC_BUFSIZE 0x100000

#define C_RESET "\x1b[0m"
#define C_GREEN "\x1b[32;1m"
#define C_YELLOW "\x1b[33;1m"
#define C_RED "\x1b[31;1m"
#define C_CYAN "\x1b[36;1m"

/* Drawing blocks on vblank (~16-20 ms) and would delay reading the next request, so the UI is
 * redrawn at most every DRAW_MIN_MS (measured on hardware: ~20 ms added per back-to-back request
 * when redrawing on every request). */
#define DRAW_MIN_MS 250
#define DRAW_MAX_MS 1000
#define BOT_COLS 39
#define BOT_ROWS 27

static PrintConsole g_top, g_bot;
static ndp_server g_srv; /* ~135 KB of buffers inside: static, not on the stack */

/* Filesystem: the SD card ("sdmc:"), read-only. Requests that touch the agent's own folder flush the
 * buffered log first, so `ndev cat .../agent.log` shows the current log and not the last flush. */
static ndp_posix_fs_ctx g_fs_ctx;
static ndp_fs_ops g_fs_base, g_fs;

/* Any SD operation slower than this is logged (measured on hardware: mkdir takes ~5.7 s, a small
 * file write ~85 ms; the log shows which call stalls). */
#define SLOW_FS_MS 150
static uint64_t now_ms(void *ctx);
static void slow_check(const char *op, const char *path, uint64_t t0) {
  uint64_t d = now_ms(NULL) - t0;
  if (d >= SLOW_FS_MS) alog("WARN slow %s %lu ms %s", op, (unsigned long)d, path ? path : "");
}

static void flush_if_own_folder(const char *path) {
  static const char own[] = "/3ds/nintendo-dev-agent";
  if (strncmp(path, own, sizeof own - 1) == 0) alog_flush();
}
static int fs_stat(void *ctx, const char *path, ndp_fs_stat *st) {
  uint64_t t0;
  int rc;
  flush_if_own_folder(path);
  t0 = now_ms(NULL);
  rc = g_fs_base.stat(ctx, path, st);
  slow_check("stat", path, t0);
  return rc;
}
static int fs_dir_open(void *ctx, const char *path, void **dir) {
  uint64_t t0;
  int rc;
  flush_if_own_folder(path);
  t0 = now_ms(NULL);
  rc = g_fs_base.dir_open(ctx, path, dir);
  slow_check("dir_open", path, t0);
  return rc;
}
static int fs_file_open(void *ctx, const char *path, void **file) {
  uint64_t t0;
  int rc;
  flush_if_own_folder(path);
  t0 = now_ms(NULL);
  rc = g_fs_base.file_open(ctx, path, file);
  slow_check("file_open", path, t0);
  return rc;
}
static long fs_file_read(void *ctx, void *file, void *buf, size_t n) {
  uint64_t t0 = now_ms(NULL);
  long r = g_fs_base.file_read(ctx, file, buf, n);
  slow_check("file_read", NULL, t0);
  return r;
}
static void fs_file_close(void *ctx, void *file) {
  uint64_t t0 = now_ms(NULL);
  g_fs_base.file_close(ctx, file);
  slow_check("file_close", NULL, t0);
}
static int fs_mkdir(void *ctx, const char *path) {
  uint64_t t0 = now_ms(NULL);
  int rc = g_fs_base.mkdir(ctx, path);
  slow_check("mkdir", path, t0);
  return rc;
}
static int fs_file_create(void *ctx, const char *path, void **file) {
  uint64_t t0 = now_ms(NULL);
  int rc = g_fs_base.file_create(ctx, path, file);
  slow_check("file_create", path, t0);
  return rc;
}
static long fs_file_write(void *ctx, void *file, const void *buf, size_t n) {
  uint64_t t0 = now_ms(NULL);
  long r = g_fs_base.file_write(ctx, file, buf, n);
  slow_check("file_write", NULL, t0);
  return r;
}
static int fs_file_sync(void *ctx, void *file) {
  uint64_t t0 = now_ms(NULL);
  int rc = g_fs_base.file_sync(ctx, file);
  slow_check("file_sync", NULL, t0);
  return rc;
}
static int fs_rename(void *ctx, const char *from, const char *to) {
  uint64_t t0 = now_ms(NULL);
  int rc = g_fs_base.rename(ctx, from, to);
  slow_check("rename", to, t0);
  return rc;
}
static int fs_remove(void *ctx, const char *path) {
  uint64_t t0 = now_ms(NULL);
  int rc = g_fs_base.remove_file(ctx, path);
  slow_check("remove", path, t0);
  return rc;
}

static u32 *g_soc_buf = NULL;
static bool g_soc_up = false, g_ndm_locked = false, g_ps_ok = false;
static bool g_wifi = false, g_listening = false;
static in_addr_t g_ip = 0;
static char g_err[64] = "";
static uint64_t g_next_soc_try = 0, g_next_listen_try = 0, g_last_check = 0;
static Result g_last_acu = 0;
static bool g_first_check = true;
static uint64_t g_start_ms = 0;
static uint64_t g_last_flush_ms = 0, g_last_req_ms = 0;
static uint32_t g_seen_requests = 0;

static uint64_t now_ms(void *ctx) {
  (void)ctx;
  return svcGetSystemTick() / (SYSCLOCK_ARM11 / 1000);
}

static void srv_log(void *ctx, const char *line) {
  (void)ctx;
  alog("%s", line);
}

static void random_bytes(void *ctx, uint8_t *out, size_t n) {
  static uint32_t x = 0;
  static bool warned = false;
  size_t i;
  (void)ctx;
  if (g_ps_ok && R_SUCCEEDED(PS_GenerateRandomBytes(out, n))) return;
  if (!warned) { alog("WARN: PS random unavailable, weak nonce"); warned = true; }
  if (!x) x = (uint32_t)svcGetSystemTick() | 1u;
  for (i = 0; i < n; i++) {
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    out[i] = (uint8_t)x;
  }
}

static void set_err(const char *fmt, unsigned long v) { snprintf(g_err, sizeof g_err, fmt, v); }

static bool start_soc(uint64_t now) {
  Result r;
  if (now < g_next_soc_try) return false;
  if (!g_soc_buf) g_soc_buf = (u32 *)memalign(SOC_ALIGN, SOC_BUFSIZE);
  if (!g_soc_buf) {
    alog("ERROR: cannot allocate soc buffer");
    snprintf(g_err, sizeof g_err, "no memory for soc buffer");
    g_next_soc_try = now + 5000;
    return false;
  }
  r = socInit(g_soc_buf, SOC_BUFSIZE);
  if (R_FAILED(r)) {
    alog("ERROR: socInit 0x%08lX", (unsigned long)r);
    set_err("socInit failed 0x%08lX", (unsigned long)r);
    free(g_soc_buf);
    g_soc_buf = NULL;
    g_next_soc_try = now + 3000;
    return false;
  }
  g_soc_up = true;
  aptSetSleepAllowed(false);
  r = NDMU_EnterExclusiveState(NDM_EXCLUSIVE_STATE_INFRASTRUCTURE);
  if (R_FAILED(r)) {
    alog("WARN: NDM exclusive state 0x%08lX", (unsigned long)r);
  } else {
    r = NDMU_LockState();
    if (R_FAILED(r)) {
      alog("WARN: NDM lock 0x%08lX", (unsigned long)r);
      NDMU_LeaveExclusiveState();
    } else {
      g_ndm_locked = true;
    }
  }
  alog("Network services ready");
  return true;
}

static void stop_listening(void) {
  ndp_server_close(&g_srv);
  g_listening = false;
}

static bool ip_usable(in_addr_t ip) { return ip != 0 && ip != (in_addr_t)0xFFFFFFFFu; }

/* Every 500 ms: Wi-Fi state, soc, IP, (re)listen. Returns true when something visible changed. */
static bool service_network(uint64_t now) {
  bool changed = false, up;
  u32 st = 0;
  Result r;
  if (!g_first_check && now - g_last_check < 500) return false;
  g_first_check = false;
  g_last_check = now;

  r = ACU_GetWifiStatus(&st);
  if (r != g_last_acu) { alog("ACU_GetWifiStatus result 0x%08lX", (unsigned long)r); g_last_acu = r; }
  up = R_SUCCEEDED(r) && st != 0;
  if (up != g_wifi) {
    g_wifi = up;
    changed = true;
    if (up) alog("Wi-Fi connected (status %lu)", (unsigned long)st);
    else {
      alog("Wi-Fi lost");
      stop_listening();
      g_ip = 0;
    }
  }
  if (!up) return changed;

  if (!g_soc_up && !start_soc(now)) return true;

  {
    in_addr_t ip = (in_addr_t)gethostid();
    if (!ip_usable(ip)) {
      if (g_listening) { alog("IP address lost"); stop_listening(); changed = true; }
      g_ip = 0;
      snprintf(g_err, sizeof g_err, "waiting for an IP address");
      return changed;
    }
    if (!g_listening || ip != g_ip) {
      int rc;
      if (now < g_next_listen_try) return changed;
      if (g_listening) alog("IP changed, re-listening");
      stop_listening();
      rc = ndp_server_listen(&g_srv, ip, AGENT_PORT);
      changed = true;
      if (rc == 0) {
        struct in_addr ia;
        ia.s_addr = ip;
        g_listening = true;
        g_ip = ip;
        g_err[0] = '\0';
        alog("Listening on %s:%u", inet_ntoa(ia), (unsigned)g_srv.port);
      } else {
        alog("ERROR: listen failed (%d: %s)", rc, strerror(-rc));
        snprintf(g_err, sizeof g_err, "listen failed: %s", strerror(-rc));
        g_next_listen_try = now + 2000;
      }
    }
  }
  return changed;
}

static void hms(char *out, size_t cap, uint64_t ms) {
  unsigned long s = (unsigned long)(ms / 1000);
  snprintf(out, cap, "%02lu:%02lu:%02lu", s / 3600, (s / 60) % 60, s % 60);
}

/* Rows a log line occupies on the bottom console: BOT_COLS, then 2-space-indented continuations. */
static int wrapped_rows(const char *s) {
  int len = (int)strlen(s);
  if (len <= BOT_COLS) return 1;
  return 1 + (len - BOT_COLS + (BOT_COLS - 2) - 1) / (BOT_COLS - 2);
}

static void print_wrapped(const char *s) {
  int len = (int)strlen(s), pos = 0, first = 1;
  while (pos < len || first) {
    int width = first ? BOT_COLS : BOT_COLS - 2;
    if (!first) printf("  ");
    printf("%.*s\n", width, s + pos);
    pos += width;
    first = 0;
  }
}

static void draw(uint64_t now) {
  char up[16];
  int n, i, rows;
  const char *color, *status;

  if (g_listening) { color = C_GREEN; status = "ONLINE"; }
  else if (!g_wifi) { color = C_YELLOW; status = "WAITING FOR Wi-Fi"; }
  else if (g_err[0]) { color = C_RED; status = "NETWORK ERROR"; }
  else { color = C_YELLOW; status = "STARTING NETWORK"; }

  consoleSelect(&g_top);
  consoleClear();
  printf(C_CYAN "Nintendo Dev Agent" C_RESET "   v%s\n", NDP_AGENT_VERSION);
  printf("Protocol %d\n\n", NDP_PROTOCOL_VERSION);
  printf("Status : %s%s" C_RESET "\n", color, status);
  if (g_listening) {
    struct in_addr ia;
    ia.s_addr = g_ip;
    printf("IP     : %s\n", inet_ntoa(ia));
    printf("Port   : %u\n", (unsigned)g_srv.port);
  } else {
    printf("IP     : -\nPort   : %u\n", (unsigned)AGENT_PORT);
  }
  if (g_srv.client_fd >= 0) printf("Bridge : " C_GREEN "CONNECTED" C_RESET " %s\n", g_srv.peer);
  else printf("Bridge : not connected\n");
  if (g_srv.agent_cfg.mode == NDP_MODE_READ_ONLY) printf("Mode   : " C_GREEN "READ_ONLY" C_RESET " (no writes)\n");
  else printf("Mode   : " C_YELLOW "%s" C_RESET " (writes on)\n", ndp_mode_name(g_srv.agent_cfg.mode));
  printf("Write  : /3ds/nintendo-dev-agent\nAuth   : none (dev build)\n");
  hms(up, sizeof up, now - g_start_ms);
  printf("Uptime : %s   Requests: %lu\n", up, (unsigned long)g_srv.requests);
  if (g_err[0]) printf("\n" C_RED "%s" C_RESET "\n", g_err);
  printf("\n\nX = toggle mode    START = Exit\n");

  consoleSelect(&g_bot);
  consoleClear();
  printf(C_CYAN "Recent activity" C_RESET "\n\n");
  rows = 0;
  for (n = 0; alog_get(n); n++) {
    int r = wrapped_rows(alog_get(n));
    if (rows + r > BOT_ROWS) break;
    rows += r;
  }
  for (i = n - 1; i >= 0; i--) print_wrapped(alog_get(i));

  gfxFlushBuffers();
  gfxSwapBuffers();
  gspWaitForVBlank();
}

int main(void) {
  ndp_agent_config cfg;
  ndp_server_platform plat;
  uint64_t last_draw = 0;
  bool draw_pending = false, is_new3ds = false;
  Result r;

  osSetSpeedupEnable(true);
  gfxInitDefault();
  consoleInit(GFX_TOP, &g_top);
  consoleInit(GFX_BOTTOM, &g_bot);
  alog_init();
  g_start_ms = now_ms(NULL);

  alog("Nintendo Dev Agent v%s, protocol %d", NDP_AGENT_VERSION, NDP_PROTOCOL_VERSION);
  if (R_SUCCEEDED(APT_CheckNew3DS(&is_new3ds))) alog("Console: %s", is_new3ds ? "New 3DS family" : "Old 3DS family");
  r = acInit();
  alog("acInit: 0x%08lX", (unsigned long)r);
  r = psInit();
  g_ps_ok = R_SUCCEEDED(r);
  alog("psInit: 0x%08lX", (unsigned long)r);
  r = ndmuInit();
  alog("ndmuInit: 0x%08lX", (unsigned long)r);

  memset(&cfg, 0, sizeof cfg);
  cfg.platform = "3ds";
  cfg.agent_version = NDP_AGENT_VERSION;
  cfg.mode = AGENT_START_MODE;
  cfg.auth = "none";
  cfg.max_frame = NDP_DEFAULT_MAX_FRAME;
  cfg.random_bytes = random_bytes;
  if (ndp_posix_fs_init(&g_fs_base, &g_fs_ctx, "sdmc:") == 0) {
    g_fs = g_fs_base;
    g_fs.stat = fs_stat;
    g_fs.dir_open = fs_dir_open;
    g_fs.file_open = fs_file_open;
    g_fs.file_read = fs_file_read;
    g_fs.file_close = fs_file_close;
    g_fs.mkdir = fs_mkdir;
    g_fs.file_create = fs_file_create;
    g_fs.file_write = fs_file_write;
    g_fs.file_sync = fs_file_sync;
    g_fs.rename = fs_rename;
    g_fs.remove_file = fs_remove;
    cfg.fs = &g_fs; /* default policy: read everything except the agent's config; no writes */
  }
  memset(&plat, 0, sizeof plat);
  plat.now_ms = now_ms;
  plat.log = srv_log;
  ndp_server_init(&g_srv, &plat, &cfg);

  draw(now_ms(NULL));
  while (aptMainLoop()) {
    uint64_t now;
    bool changed;
    hidScanInput();
    if (hidKeysDown() & KEY_START) break;
    if (hidKeysDown() & KEY_X) {
      ndp_mode next = g_srv.agent_cfg.mode == NDP_MODE_READ_ONLY ? NDP_MODE_DEVELOPMENT : NDP_MODE_READ_ONLY;
      ndp_server_set_mode(&g_srv, next);
      alog("Mode -> %s", ndp_mode_name(next));
      draw_pending = true;
    }

    now = now_ms(NULL);
    changed = service_network(now);
    if (g_listening) {
      int s = ndp_server_step(&g_srv, 16);
      if (s < 0) {
        alog("Listener lost; will re-listen");
        stop_listening();
        g_ip = 0;
        g_next_listen_try = now + 1000;
        changed = true;
      } else if (s > 0) {
        changed = true;
      }
    } else {
      svcSleepThread(16 * 1000000LL);
    }

    now = now_ms(NULL);
    /* Flush the buffered SD log only when the link is quiet (or at least every 5 s). */
    if (g_srv.requests != g_seen_requests) { g_seen_requests = g_srv.requests; g_last_req_ms = now; }
    if (alog_unflushed() &&
        ((now - g_last_req_ms >= 250 && now - g_last_flush_ms >= 500) || now - g_last_flush_ms >= 5000)) {
      alog_flush();
      g_last_flush_ms = now;
    }
    if (changed || alog_dirty()) draw_pending = true;
    if ((draw_pending && now - last_draw >= DRAW_MIN_MS) || now - last_draw >= DRAW_MAX_MS) {
      alog_clear_dirty();
      draw(now);
      draw_pending = false;
      last_draw = now;
    }
  }

  alog("Exiting");
  ndp_server_close(&g_srv);
  if (g_ndm_locked) {
    NDMU_UnlockState();
    NDMU_LeaveExclusiveState();
  }
  aptSetSleepAllowed(true);
  if (g_soc_up) socExit();
  free(g_soc_buf);
  if (g_ps_ok) psExit();
  ndmuExit();
  acExit();
  alog_exit();
  gfxExit();
  return 0;
}
