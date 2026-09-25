/* PAIR / AUTH commands, HELLO auth fields and frame sealing (spec §4). */
#include <string.h>

#include "ndp_agent_internal.h"

/* An authentication attempt failed. After NDP_MAX_AUTH_FAILS the pairing window closes and the
 * connection is dropped. */
static size_t fail(ndp_agent *a, const ndp_header *req, uint8_t *out, size_t cap, const char *detail) {
  if (++a->sec.fails >= NDP_MAX_AUTH_FAILS) {
    if (a->cfg.pairing) a->cfg.pairing->active = 0;
    return NDP_CLOSE;
  }
  return ndp_agent_error(out, cap, req, NDP_ST_UNAUTHORIZED, detail, 0);
}

void ndp_agent_hello_auth_fields(ndp_agent *a, ndp_tlv_w *w) {
  ndp_keystore *ks = a->cfg.keys;
  uint8_t v;
  if (!ks) return;
  if (!ks->has_device_id) { /* first contact: a stable identifier the Bridge can recognize the console by */
    if (a->cfg.random_bytes && a->cfg.random_bytes(a->cfg.random_ctx, ks->device_id, sizeof ks->device_id) == 0)
      ks->has_device_id = 1;
  }
  if (ks->has_device_id) ndp_tlv_put(w, NDP_TAG_DEVICE_ID, ks->device_id, sizeof ks->device_id);
  v = (uint8_t)ks->count;
  ndp_tlv_put(w, NDP_TAG_PAIRED_KEYS, &v, 1);
  v = (uint8_t)(a->cfg.pairing && a->cfg.pairing->active ? 1 : 0);
  ndp_tlv_put(w, NDP_TAG_PAIRING_OPEN, &v, 1);
}

size_t ndp_agent_pair(ndp_agent *a, const ndp_header *req, const uint8_t *pl, uint8_t *out, size_t cap) {
  const uint8_t *label, *proof;
  size_t ll, lp;
  uint8_t psk[32], expect[32], id[4];
  char lab[NDP_LABEL_MAX + 1];
  ndp_tlv_w w;
  int rc;
  if (!ndp_tlv_find(pl, req->payload_len, NDP_TAG_LABEL, &label, &ll) ||
      !ndp_tlv_find(pl, req->payload_len, NDP_TAG_PROOF, &proof, &lp) || lp != 32 || ll == 0 || ll > NDP_LABEL_MAX)
    return ndp_agent_error(out, cap, req, NDP_ST_BAD_REQUEST, "label and proof required", 0);
  if (!a->cfg.pairing || !a->cfg.pairing->active || !a->cfg.keys) return fail(a, req, out, cap, "pairing is not open");
  ndp_derive_psk(a->cfg.pairing->code, psk);
  ndp_proof(psk, "pair", a->sec.cn, a->sec.dn, label, ll, expect);
  if (!ndp_ct_equal(expect, proof, 32)) return fail(a, req, out, cap, "wrong pairing code");
  memcpy(lab, label, ll);
  lab[ll] = '\0';
  rc = ndp_keystore_add(a->cfg.keys, psk, lab);
  if (rc != NDP_OK) return ndp_agent_error(out, cap, req, NDP_ST_NO_SPACE, "pairing storage is full", 0);
  a->cfg.pairing->active = 0; /* the code is single-use */
  memset(a->cfg.pairing->code, 0, sizeof a->cfg.pairing->code);
  if (a->cfg.keys_changed) a->cfg.keys_changed(a->cfg.keys_ctx, a->cfg.keys);
  ndp_key_id(psk, id);
  if (cap < NDP_HEADER_SIZE) return 0;
  ndp_tlv_w_init(&w, out + NDP_HEADER_SIZE, cap - NDP_HEADER_SIZE);
  ndp_tlv_put(&w, NDP_TAG_KEY_ID, id, 4);
  return ndp_agent_finish(out, cap, req, NDP_KIND_RES, NDP_OK, &w);
}

size_t ndp_agent_auth(ndp_agent *a, const ndp_header *req, const uint8_t *pl, uint8_t *out, size_t cap) {
  const uint8_t *kid, *proof;
  size_t lk, lp;
  uint8_t expect[32];
  ndp_tlv_w w;
  int i;
  if (!ndp_tlv_find(pl, req->payload_len, NDP_TAG_KEY_ID, &kid, &lk) ||
      !ndp_tlv_find(pl, req->payload_len, NDP_TAG_PROOF, &proof, &lp) || lk != 4 || lp != 32)
    return ndp_agent_error(out, cap, req, NDP_ST_BAD_REQUEST, "key_id and proof required", 0);
  if (!a->cfg.keys) return fail(a, req, out, cap, "unknown key");
  for (i = 0; i < a->cfg.keys->count; i++) {
    const ndp_paired_key *k = &a->cfg.keys->keys[i];
    if (memcmp(k->key_id, kid, 4) != 0) continue;
    ndp_proof(k->psk, "auth", a->sec.cn, a->sec.dn, NULL, 0, expect);
    if (!ndp_ct_equal(expect, proof, 32)) return fail(a, req, out, cap, "authentication failed");
    ndp_session_key(k->psk, a->sec.cn, a->sec.dn, a->sec.session);
    a->sec.authed = 1;
    a->sec.send_ctr = a->sec.recv_ctr = 0;
    if (cap < NDP_HEADER_SIZE) return 0;
    ndp_tlv_w_init(&w, out + NDP_HEADER_SIZE, cap - NDP_HEADER_SIZE);
    return ndp_agent_finish(out, cap, req, NDP_KIND_RES, NDP_OK, &w); /* sealed by the caller (counter 0) */
  }
  return fail(a, req, out, cap, "unknown key");
}

size_t ndp_agent_seal(ndp_agent *a, uint8_t *frame, size_t len, size_t cap) {
  if (len < NDP_HEADER_SIZE || len + NDP_MAC_SIZE > cap) return 0;
  frame[6] |= (uint8_t)NDP_FLAG_MAC; /* flags are little-endian at offset 6 */
  ndp_frame_mac(a->sec.session, 32, a->sec.send_ctr, frame, frame + NDP_HEADER_SIZE, len - NDP_HEADER_SIZE,
                frame + len);
  a->sec.send_ctr++;
  return len + NDP_MAC_SIZE;
}
