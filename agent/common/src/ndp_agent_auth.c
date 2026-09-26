/* PAIR / AUTH commands, HELLO auth fields and frame sealing (spec §4). */
#include <stdio.h>
#include <string.h>

#include "ndp/ndp_pair2.h"
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

/* ---- pairing by number comparison (spec §4.8) ---- */
static void p2_reset(ndp_pairing *p) {
  p->p2_stage = NDP_P2_IDLE;
  memset(p->p2_secret, 0, sizeof p->p2_secret);
  memset(p->p2_psk, 0, sizeof p->p2_psk);
  p->p2_sas = 0;
}

static ndp_pairing *p2_window(ndp_agent *a, const ndp_header *req, uint8_t *out, size_t cap, size_t *err) {
  if (!a->cfg.pairing || !a->cfg.pairing->active || !a->cfg.keys) { *err = fail(a, req, out, cap, "pairing is not open"); return NULL; }
  return a->cfg.pairing;
}

size_t ndp_agent_pair_begin(ndp_agent *a, const ndp_header *req, const uint8_t *pl, uint8_t *out, size_t cap) {
  const uint8_t *label, *commit;
  size_t ll, lc, err = 0;
  ndp_pairing *p;
  ndp_tlv_w w;
  if (!ndp_tlv_find(pl, req->payload_len, NDP_TAG_LABEL, &label, &ll) ||
      !ndp_tlv_find(pl, req->payload_len, NDP_TAG_PAIR_COMMIT, &commit, &lc) || lc != 32 || ll == 0 || ll > NDP_LABEL_MAX)
    return ndp_agent_error(out, cap, req, NDP_ST_BAD_REQUEST, "label and commit required", 0);
  p = p2_window(a, req, out, cap, &err);
  if (!p) return err;
  if (p->p2_attempts >= NDP_P2_MAX_ATTEMPTS) { p->active = 0; return ndp_agent_error(out, cap, req, NDP_ST_UNAUTHORIZED, "too many pairing attempts", 0); }
  if ((p->p2_stage == NDP_P2_COMMITTED || p->p2_stage == NDP_P2_WAITING) && p->p2_owner != a->cfg.conn_id)
    return ndp_agent_error(out, cap, req, NDP_ST_BUSY, "another pairing request is waiting on the console", 0);
  if (!a->cfg.random_bytes || a->cfg.random_bytes(a->cfg.random_ctx, p->p2_secret, 32) != 0 ||
      a->cfg.random_bytes(a->cfg.random_ctx, p->p2_nonce_c, NDP_PAIR2_NONCE) != 0) {
    p2_reset(p);
    return ndp_agent_error(out, cap, req, NDP_ST_IO_ERROR, "no secure random source", 0);
  }
  ndp_x25519_public(p->p2_pub_c, p->p2_secret);
  memcpy(p->p2_commit, commit, 32);
  memcpy(p->p2_label, label, ll);
  p->p2_label[ll] = '\0';
  snprintf(p->p2_peer, sizeof p->p2_peer, "%s", a->cfg.peer ? a->cfg.peer : "");
  p->p2_owner = a->cfg.conn_id;
  p->p2_stage = NDP_P2_COMMITTED;
  p->p2_deadline_ms = 0;
  memset(p->p2_psk, 0, sizeof p->p2_psk);
  if (cap < NDP_HEADER_SIZE) return 0;
  ndp_tlv_w_init(&w, out + NDP_HEADER_SIZE, cap - NDP_HEADER_SIZE);
  ndp_tlv_put(&w, NDP_TAG_PAIR_PUB, p->p2_pub_c, 32);
  ndp_tlv_put(&w, NDP_TAG_PAIR_NONCE, p->p2_nonce_c, NDP_PAIR2_NONCE);
  return ndp_agent_finish(out, cap, req, NDP_KIND_RES, NDP_OK, &w);
}

size_t ndp_agent_pair_reveal(ndp_agent *a, const ndp_header *req, const uint8_t *pl, uint8_t *out, size_t cap) {
  const uint8_t *pub, *nonce;
  size_t lp, ln, err = 0;
  uint8_t expect[32], shared[32];
  ndp_pair2_keys k;
  ndp_pairing *p;
  ndp_tlv_w w;
  if (!ndp_tlv_find(pl, req->payload_len, NDP_TAG_PAIR_PUB, &pub, &lp) || !ndp_tlv_find(pl, req->payload_len, NDP_TAG_PAIR_NONCE, &nonce, &ln) ||
      lp != 32 || ln != NDP_PAIR2_NONCE)
    return ndp_agent_error(out, cap, req, NDP_ST_BAD_REQUEST, "pub and nonce required", 0);
  p = p2_window(a, req, out, cap, &err);
  if (!p) return err;
  if (p->p2_stage != NDP_P2_COMMITTED || p->p2_owner != a->cfg.conn_id)
    return ndp_agent_error(out, cap, req, NDP_ST_BAD_REQUEST, "no pairing request to reveal", 0);
  ndp_pair2_commit(pub, nonce, expect);
  if (!ndp_ct_equal(expect, p->p2_commit, 32)) { /* the client changed its key after seeing ours: never show a number for that */
    p->p2_attempts++;
    p2_reset(p);
    return fail(a, req, out, cap, "commitment mismatch");
  }
  if (ndp_x25519(shared, p->p2_secret, pub) != 0) {
    p->p2_attempts++;
    p2_reset(p);
    return ndp_agent_error(out, cap, req, NDP_ST_BAD_REQUEST, "invalid public key", 0);
  }
  memcpy(p->p2_pub_b, pub, 32);
  memcpy(p->p2_nonce_b, nonce, NDP_PAIR2_NONCE);
  ndp_pair2_derive(shared, p->p2_pub_b, p->p2_pub_c, p->p2_nonce_b, p->p2_nonce_c, (const uint8_t *)p->p2_label, strlen(p->p2_label), &k);
  memcpy(p->p2_psk, k.psk, 32);
  p->p2_sas = k.sas;
  memset(p->p2_secret, 0, sizeof p->p2_secret);
  memset(shared, 0, sizeof shared);
  p->p2_stage = NDP_P2_WAITING;
  p->p2_deadline_ms = 0; /* the server starts the clock when the number is on the screen */
  if (cap < NDP_HEADER_SIZE) return 0;
  ndp_tlv_w_init(&w, out + NDP_HEADER_SIZE, cap - NDP_HEADER_SIZE);
  return ndp_agent_finish(out, cap, req, NDP_KIND_RES, NDP_OK, &w);
}

size_t ndp_agent_pair_poll(ndp_agent *a, const ndp_header *req, const uint8_t *pl, uint8_t *out, size_t cap) {
  ndp_pairing *p = a->cfg.pairing;
  uint8_t state = 0, proof[32];
  ndp_tlv_w w;
  (void)pl;
  if (!p || !a->cfg.keys || p->p2_owner != a->cfg.conn_id || p->p2_stage == NDP_P2_IDLE)
    return ndp_agent_error(out, cap, req, NDP_ST_BAD_REQUEST, "no pairing request", 0);
  if (cap < NDP_HEADER_SIZE) return 0;
  ndp_tlv_w_init(&w, out + NDP_HEADER_SIZE, cap - NDP_HEADER_SIZE);
  switch (p->p2_stage) {
    case NDP_P2_APPROVED: {
      uint8_t id[4];
      state = 1;
      ndp_key_id(p->p2_psk, id);
      ndp_pair2_ok_proof(p->p2_psk, p->p2_pub_b, p->p2_pub_c, proof);
      ndp_tlv_put(&w, NDP_TAG_PAIR_STATE, &state, 1);
      ndp_tlv_put(&w, NDP_TAG_KEY_ID, id, 4);
      ndp_tlv_put(&w, NDP_TAG_PROOF, proof, 32);
      p2_reset(p); /* delivered once */
      break;
    }
    case NDP_P2_DENIED: state = 2; ndp_tlv_put(&w, NDP_TAG_PAIR_STATE, &state, 1); p2_reset(p); break;
    case NDP_P2_FULL: state = 3; ndp_tlv_put(&w, NDP_TAG_PAIR_STATE, &state, 1); p2_reset(p); break;
    default: state = 0; ndp_tlv_put(&w, NDP_TAG_PAIR_STATE, &state, 1); break; /* still waiting for the person */
  }
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
