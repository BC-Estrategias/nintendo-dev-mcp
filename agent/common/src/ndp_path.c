#include "ndp/ndp_path.h"

#include <string.h>

static int utf8_valid(const uint8_t *s, size_t n) {
  size_t i = 0;
  while (i < n) {
    uint8_t c = s[i];
    size_t extra;
    uint8_t lo = 0x80, hi = 0xBF;
    if (c < 0x80) { i++; continue; }
    if (c >= 0xC2 && c <= 0xDF) extra = 1;
    else if (c == 0xE0) { extra = 2; lo = 0xA0; }
    else if (c == 0xED) { extra = 2; hi = 0x9F; }
    else if (c >= 0xE1 && c <= 0xEF) extra = 2;
    else if (c == 0xF0) { extra = 3; lo = 0x90; }
    else if (c == 0xF4) { extra = 3; hi = 0x8F; }
    else if (c >= 0xF1 && c <= 0xF3) extra = 3;
    else return 0;
    if (n - i <= extra) return 0;
    if (s[i + 1] < lo || s[i + 1] > hi) return 0;
    { size_t k; for (k = 2; k <= extra; k++) if ((s[i + k] & 0xC0) != 0x80) return 0; }
    i += extra + 1;
  }
  return 1;
}

static int bad_component(const uint8_t *c, size_t n) {
  size_t i;
  if (n == 0 || n > NDP_COMPONENT_MAX) return 1;
  if (n == 1 && c[0] == '.') return 1;
  if (n == 2 && c[0] == '.' && c[1] == '.') return 1;
  if (c[n - 1] == '.' || c[n - 1] == ' ') return 1;
  for (i = 0; i < n; i++) {
    switch (c[i]) {
      case ':': case '*': case '?': case '"': case '<': case '>': case '|': return 1;
      case '~': if (i + 1 < n && c[i + 1] >= '0' && c[i + 1] <= '9') return 1; break;
      default: break;
    }
  }
  return 0;
}

int ndp_path_normalize(const uint8_t *in, size_t len, char *out, size_t out_cap) {
  size_t i, start, end;
  if (len == 0 || in[0] != '/' || len > NDP_PATH_MAX || out_cap < len + 1) return NDP_ST_PATH_INVALID;
  for (i = 0; i < len; i++) {
    uint8_t c = in[i];
    if (c < 0x20 || c == 0x7F || c == '\\') return NDP_ST_PATH_INVALID;
  }
  if (!utf8_valid(in, len)) return NDP_ST_PATH_INVALID;
  if (len == 1) { out[0] = '/'; out[1] = '\0'; return NDP_OK; }
  end = len;
  if (in[end - 1] == '/') end--; /* remove one trailing slash */
  /* components are in[1..end) separated by '/' */
  start = 1;
  for (i = 1; i <= end; i++) {
    if (i == end || in[i] == '/') {
      if (bad_component(in + start, i - start)) return NDP_ST_PATH_INVALID;
      start = i + 1;
    }
  }
  memcpy(out, in, end);
  out[end] = '\0';
  return NDP_OK;
}

static char fold(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

int ndp_path_inside(const char *path, const char *root) {
  const char *p = path, *r = root;
  if (r[0] == '/' && r[1] == '\0') return 1;
  while (*r) {
    if (*p != '/') return 0;
    p++; r++;
    while (*r && *r != '/') {
      if (!*p || *p == '/') return 0;
      if (fold(*p) != fold(*r)) return 0;
      p++; r++;
    }
    if (*p && *p != '/') return 0;
  }
  return 1;
}
