/* Activity log: ring buffer for the bottom screen + rotating file on the SD card. */
#ifndef ALOG_H
#define ALOG_H

#include <stdbool.h>

#define ALOG_LINES 64
#define ALOG_LINE_MAX 96

void alog_init(void);
void alog_exit(void);
void alog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Newest line has index 0. Returns NULL past the end. */
const char *alog_get(int index_from_newest);
/* True when a line was added since the last alog_clear_dirty(). */
bool alog_dirty(void);
void alog_clear_dirty(void);

/* File writes are buffered (a per-line fflush to the SD measured ~7 ms per request, with a 396 ms
 * stall once). The main loop calls alog_flush() when the link is idle. ERROR/WARN lines flush at once. */
bool alog_unflushed(void);
void alog_flush(void);

#endif
