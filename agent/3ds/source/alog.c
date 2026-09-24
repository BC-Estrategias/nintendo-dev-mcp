#include "alog.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define LOG_DIR "sdmc:/3ds/nintendo-dev-agent"
#define LOG_PATH LOG_DIR "/agent.log"
#define LOG_OLD LOG_DIR "/agent.log.1"
#define LOG_MAX_BYTES (128 * 1024)

static char g_lines[ALOG_LINES][ALOG_LINE_MAX + 16]; /* + room for the "HH:MM:SS " stamp */
static int g_head = 0;   /* next slot to write */
static int g_count = 0;
static bool g_dirty = false;
static FILE *g_fp = NULL;
static long g_bytes = 0;

static void open_file(void) {
  struct stat st;
  mkdir("sdmc:/3ds", 0777); /* already exists on any Homebrew Launcher setup; ignore errors */
  mkdir(LOG_DIR, 0777);
  if (stat(LOG_PATH, &st) == 0 && st.st_size > LOG_MAX_BYTES) {
    remove(LOG_OLD);
    rename(LOG_PATH, LOG_OLD);
  }
  g_fp = fopen(LOG_PATH, "a");
  g_bytes = (g_fp && stat(LOG_PATH, &st) == 0) ? (long)st.st_size : 0;
}

static void rotate_if_needed(void) {
  if (!g_fp || g_bytes <= LOG_MAX_BYTES) return;
  fclose(g_fp);
  remove(LOG_OLD);
  rename(LOG_PATH, LOG_OLD);
  g_fp = fopen(LOG_PATH, "a");
  g_bytes = 0;
}

void alog_init(void) {
  open_file();
  if (g_fp) {
    fputs("---- agent start ----\n", g_fp);
    fflush(g_fp);
  }
}

void alog_exit(void) {
  if (g_fp) {
    fputs("---- agent exit ----\n", g_fp);
    fclose(g_fp);
    g_fp = NULL;
  }
}

void alog(const char *fmt, ...) {
  char msg[ALOG_LINE_MAX];
  char stamp[16];
  time_t t = time(NULL);
  struct tm *tm = localtime(&t);
  va_list ap;

  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);

  if (tm) strftime(stamp, sizeof stamp, "%H:%M:%S", tm);
  else strcpy(stamp, "--:--:--");

  snprintf(g_lines[g_head], sizeof g_lines[g_head], "%s %s", stamp, msg);
  g_head = (g_head + 1) % ALOG_LINES;
  if (g_count < ALOG_LINES) g_count++;
  g_dirty = true;

  if (g_fp) {
    int n = fprintf(g_fp, "%s %s\n", stamp, msg);
    if (n > 0) g_bytes += n;
    fflush(g_fp);
    rotate_if_needed();
  }
}

const char *alog_get(int index_from_newest) {
  if (index_from_newest < 0 || index_from_newest >= g_count) return NULL;
  return g_lines[(g_head - 1 - index_from_newest + 2 * ALOG_LINES) % ALOG_LINES];
}

bool alog_dirty(void) { return g_dirty; }
void alog_clear_dirty(void) { g_dirty = false; }
