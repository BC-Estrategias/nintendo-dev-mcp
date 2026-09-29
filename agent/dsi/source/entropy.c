/* Software entropy pool for the DSi build.
 *
 * The DS/DSi has no hardware RNG (confirmed against a real, shipped DSi homebrew that needs
 * secure randomness for its own TLS: KirovAir/TwilightBoxart's tls_entropy.c draws from the same
 * free-running timers and LCD line counter used here, with the comment "The DS has no random
 * number generator"). Unlike that project's per-call snapshot, this pool is mixed continuously,
 * every VBlank, for as long as the agent has been running before a nonce is ever needed, and
 * extraction goes through SHA-256 (Hash_DRBG-style) rather than raw xorshift output. This raises
 * the bar from "guessable from a single timer snapshot" to "requires seconds of precise physical
 * timing knowledge" -- it does not reach the 3DS's hardware-backed guarantee (agent/3ds's
 * random_bytes, PS_GenerateRandomBytes). Documented as a known limitation in ARCHITECTURE.md. */
#include <string.h>

#include <nds.h>

#include "ndp/ndp_sha256.h"
#include "entropy.h"

static uint8_t g_pool[32];
static uint64_t g_ticks;
static uint64_t g_calls;

void entropy_init(void) {
  /* Free-running counters at different rates; nothing else in this app uses TIMER0/1. */
  TIMER0_CR = TIMER_ENABLE | TIMER_DIV_1;
  TIMER1_CR = TIMER_ENABLE | TIMER_DIV_64;
}

void entropy_tick(void) {
  uint8_t sample[6];
  uint16_t t0 = TIMER0_DATA, t1 = TIMER1_DATA, vc = REG_VCOUNT;
  g_ticks++;
  memcpy(sample, &t0, 2);
  memcpy(sample + 2, &t1, 2);
  memcpy(sample + 4, &vc, 2);
  /* Cheap per-frame mix: rotate the pool and XOR in this sample plus the tick count. A single
   * sample proves nothing; the pool's strength is the accumulation across many frames. */
  for (int i = 0; i < 32; i++) {
    uint8_t rotated = (uint8_t)((g_pool[i] << 1) | (g_pool[i] >> 7));
    g_pool[i] = (uint8_t)(rotated ^ sample[i % sizeof sample] ^ (uint8_t)(g_ticks >> (8 * (i % 8))));
  }
}

int entropy_random_bytes(void *ctx, uint8_t *out, size_t n) {
  (void)ctx;
  while (n > 0) {
    uint8_t extract_buf[32 + 8 + 6], ratchet_buf[32 + 32], digest[32];
    uint16_t t0 = TIMER0_DATA, t1 = TIMER1_DATA, vc = REG_VCOUNT;
    memcpy(extract_buf, g_pool, 32);
    memcpy(extract_buf + 32, &g_calls, 8);
    memcpy(extract_buf + 40, &t0, 2);
    memcpy(extract_buf + 42, &t1, 2);
    memcpy(extract_buf + 44, &vc, 2);
    g_calls++;

    ndp_sha256(extract_buf, sizeof extract_buf, digest);

    size_t take = n < 32 ? n : 32;
    memcpy(out, digest, take);
    out += take;
    n -= take;

    /* Ratchet: the pool changes after every extraction, so exposing one output does not expose
     * the state that produced (or will produce) any other. */
    memcpy(ratchet_buf, g_pool, 32);
    memcpy(ratchet_buf + 32, digest, 32);
    ndp_sha256(ratchet_buf, sizeof ratchet_buf, g_pool);
  }
  return 0;
}
