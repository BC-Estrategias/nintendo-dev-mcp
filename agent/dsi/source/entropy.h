/* Software entropy pool for the DSi build: the DS/DSi has no hardware RNG (unlike the 3DS's PS
 * service). See agent/dsi/source/entropy.c for the design and its real limits. */
#ifndef NDP_DSI_ENTROPY_H
#define NDP_DSI_ENTROPY_H

#include <stddef.h>
#include <stdint.h>

/* Starts the free-running timers this pool draws jitter from. Call once at startup, before the
 * first entropy_tick(). */
void entropy_init(void);

/* Mixes one sample of timer/scanline jitter into the pool. Call every VBlank (60 Hz): the pool's
 * real strength comes from accumulating this over the session, not from any single sample. */
void entropy_tick(void);

/* ndp_agent_config.random_bytes: extracts n bytes via SHA-256 over the pool and ratchets it
 * forward. Always succeeds (ctx is unused; ndp_agent_config's calling convention returns int for
 * sources that can fail, this one cannot). */
int entropy_random_bytes(void *ctx, uint8_t *out, size_t n);

#endif
