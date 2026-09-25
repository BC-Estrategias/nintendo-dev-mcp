#!/usr/bin/env python3
"""Gera os vetores de teste do NDP v1.

Implementação de referência INDEPENDENTE (só stdlib Python) do que está em
docs/protocol/ndp-v1.md. As implementações em C e TypeScript são verificadas contra os
arquivos gerados aqui:

  docs/protocol/test-vectors/vectors.json     (lido pelos testes TypeScript)
  agent/common/tests/vectors_gen.h            (incluído pelos testes C)

Uso:  python3 docs/protocol/test-vectors/generate.py
"""
import hashlib
import hmac
import json
import os
import re
import struct

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
OUT_JSON = os.path.join(ROOT, "docs", "protocol", "test-vectors", "vectors.json")
OUT_H = os.path.join(ROOT, "agent", "common", "tests", "vectors_gen.h")

# ---------------------------------------------------------------- constantes do protocolo
MAGIC = b"NDP1"
HDR = struct.Struct("<4sBBHIHHI")
assert HDR.size == 20
KIND = {"REQ": 1, "RES": 2, "DATA": 3, "END": 4, "ERR": 5, "EVT": 6}
FLAG_MAC, FLAG_MORE = 1, 2
CMD_HELLO, CMD_PING = 0x0001, 0x0002
STATUS = {
    "OK": 0, "UNSUPPORTED_PROTOCOL": 1, "UNAUTHORIZED": 2, "FORBIDDEN_MODE": 3,
    "PROTECTED_PATH": 4, "NOT_FOUND": 5, "EXISTS": 6, "IO_ERROR": 7, "NO_SPACE": 8,
    "BAD_REQUEST": 9, "BUSY": 10, "TOO_LARGE": 11, "HASH_MISMATCH": 12, "TIMEOUT": 13,
    "UNSUPPORTED_COMMAND": 14, "HELLO_REQUIRED": 15, "BAD_FRAME": 16, "PATH_INVALID": 17,
}
T_DETAIL, T_OSRES = 0x0001, 0x0002
T_PROTO, T_PROTO_MAX, T_BRIDGE, T_NONCE = 0x0010, 0x0011, 0x0012, 0x0013
T_PLATFORM, T_AGENT_VER, T_AUTH, T_MODE, T_MAXFRAME = 0x0014, 0x0015, 0x0016, 0x0017, 0x0018
T_SUP_MIN, T_SUP_MAX, T_PING = 0x0019, 0x001A, 0x0020
T_PAIRED, T_PAIRING_OPEN, T_DEVICE_ID, T_KEY_ID, T_PROOF, T_LABEL = 0x0047, 0x0048, 0x0049, 0x004A, 0x004B, 0x004C
CMD_PAIR, CMD_AUTH = 0x0004, 0x0005
MODES = ["READ_ONLY", "DEVELOPMENT", "FULL"]


def tlv(tag: int, value: bytes) -> bytes:
    return struct.pack("<HH", tag, len(value)) + value


def u16(v): return struct.pack("<H", v)
def u32(v): return struct.pack("<I", v)
def u64(v): return struct.pack("<Q", v)


def frame(kind, request_id, command, status=0, payload=b"", flags=0, mac=None, version=1, magic=MAGIC,
          payload_len=None):
    if mac is not None:
        flags |= FLAG_MAC
    plen = len(payload) if payload_len is None else payload_len
    out = HDR.pack(magic, version, KIND[kind] if isinstance(kind, str) else kind, flags, request_id,
                   command, status, plen) + payload
    if mac is not None:
        out += mac
    return out


def mac_of(key: bytes, counter: int, header_and_payload: bytes) -> bytes:
    return hmac.new(key, u64(counter) + header_and_payload, hashlib.sha256).digest()[:16]


def parse_tlvs(payload: bytes):
    out, i = [], 0
    while i < len(payload):
        tag, ln = struct.unpack_from("<HH", payload, i)  # struct.error se sobrarem < 4 bytes
        if i + 4 + ln > len(payload):
            raise struct.error("tlv length overruns payload")
        out.append((tag, payload[i + 4:i + 4 + ln]))
        i += 4 + ln
    return out


# ---------------------------------------------------------------- paths e política
class PathInvalid(Exception):
    pass


BAD_COMP_CHARS = set(':*?"<>|')
SHORT_NAME = re.compile(r"~[0-9]")


def normalize(b: bytes) -> str:
    if not b or b[0] != 0x2F:
        raise PathInvalid("not absolute")
    if len(b) > 1024:
        raise PathInvalid("too long")
    for c in b:
        if c < 0x20 or c == 0x7F or c == 0x5C:
            raise PathInvalid("control or backslash")
    try:
        s = b.decode("utf-8")  # estrito: recusa overlong, surrogates, > U+10FFFF
    except UnicodeDecodeError:
        raise PathInvalid("bad utf-8")
    if s == "/":
        return "/"
    if s.endswith("/"):
        s = s[:-1]
    comps = s.split("/")[1:]
    for comp in comps:
        if comp == "" or comp in (".", ".."):
            raise PathInvalid("empty/dot component")
        if any(ch in BAD_COMP_CHARS for ch in comp):
            raise PathInvalid("fat-illegal char")
        if comp.endswith(".") or comp.endswith(" "):
            raise PathInvalid("trailing dot/space")
        if SHORT_NAME.search(comp):
            raise PathInvalid("8.3 alias")
        if len(comp.encode("utf-8")) > 255:
            raise PathInvalid("component too long")
    return "/" + "/".join(comps)


def fold(b: bytes) -> bytes:
    return bytes(c + 32 if 0x41 <= c <= 0x5A else c for c in b)


def comps_of(norm: str):
    return [] if norm == "/" else [fold(c.encode("utf-8")) for c in norm.split("/")[1:]]


def inside(path_norm: str, root_norm: str) -> bool:
    r, p = comps_of(root_norm), comps_of(path_norm)
    return p[:len(r)] == r


CONFIGS = {
    "default": {
        "read_roots": ["/"],
        "write_roots": ["/3ds/nintendo-dev-agent"],
        "never_read": ["/3ds/nintendo-dev-agent/config"],
        "never_write": ["/Nintendo 3DS", "/luma", "/boot.firm", "/gm9", "/private",
                        "/3ds/nintendo-dev-agent/config"],
    },
    "custom": {
        "read_roots": ["/roms", "/3ds"],
        "write_roots": ["/roms", "/3ds/tmc3ds", "/cias"],
        "never_read": ["/3ds/nintendo-dev-agent/config"],
        "never_write": ["/Nintendo 3DS", "/luma", "/boot.firm", "/gm9", "/private",
                        "/3ds/nintendo-dev-agent/config"],
    },
    "wide": {  # usuário liberou a raiz inteira: as zonas never_* continuam valendo
        "read_roots": ["/"],
        "write_roots": ["/"],
        "never_read": ["/3ds/nintendo-dev-agent/config"],
        "never_write": ["/Nintendo 3DS", "/luma", "/boot.firm", "/gm9", "/private",
                        "/3ds/nintendo-dev-agent/config"],
    },
}


def policy_check(cfg, mode, op, raw: bytes):
    try:
        p = normalize(raw)
    except PathInvalid:
        return "PATH_INVALID"
    if op == "write" and mode == "READ_ONLY":
        return "FORBIDDEN_MODE"
    never = cfg["never_write" if op == "write" else "never_read"]
    if any(inside(p, n) for n in never):
        return "PROTECTED_PATH"
    roots = cfg["write_roots" if op == "write" else "read_roots"]
    if not any(inside(p, r) for r in roots):
        return "PROTECTED_PATH"
    return "OK"


# ---------------------------------------------------------------- agente de referência (M0)
AGENT_CFG = {
    "platform": "host", "agent_version": "0.1.0", "mode": "READ_ONLY", "auth": "none",
    "max_frame": 65536, "nonce": bytes(range(0xA0, 0xB0)),
}
SUPPORTED_MIN = SUPPORTED_MAX = 1


def err_frame(req, status_name, detail=None, extra=b""):
    p = extra
    if detail is not None:
        p += tlv(T_DETAIL, detail.encode())
    return frame("ERR", req["id"], req["cmd"], STATUS[status_name], p)


def agent_handle(state, req):
    """req: dict(kind, version, id, cmd, payload). Devolve (frame_bytes|None)."""
    if req["version"] != 1:
        return err_frame(req, "UNSUPPORTED_PROTOCOL", "unsupported frame version",
                         tlv(T_SUP_MIN, u16(SUPPORTED_MIN)) + tlv(T_SUP_MAX, u16(SUPPORTED_MAX)))
    if req["kind"] != KIND["REQ"]:
        return err_frame(req, "BAD_REQUEST", "expected REQ")
    fields = {}
    try:
        for tag, val in parse_tlvs(req["payload"]):
            fields.setdefault(tag, val)
    except struct.error:
        return err_frame(req, "BAD_REQUEST", "malformed payload")
    if req["cmd"] == CMD_HELLO:
        if not all(t in fields for t in (T_PROTO, T_PROTO_MAX, T_NONCE)):
            return err_frame(req, "BAD_REQUEST", "missing field")
        if len(fields[T_PROTO]) != 2 or len(fields[T_PROTO_MAX]) != 2 or len(fields[T_NONCE]) != 16:
            return err_frame(req, "BAD_REQUEST", "bad field size")
        lo, hi = struct.unpack("<H", fields[T_PROTO])[0], struct.unpack("<H", fields[T_PROTO_MAX])[0]
        if lo > hi:
            return err_frame(req, "BAD_REQUEST", "protocol_min > protocol_max")
        chosen = min(hi, SUPPORTED_MAX)
        if chosen < max(lo, SUPPORTED_MIN):
            return err_frame(req, "UNSUPPORTED_PROTOCOL", "no common protocol version",
                             tlv(T_SUP_MIN, u16(SUPPORTED_MIN)) + tlv(T_SUP_MAX, u16(SUPPORTED_MAX)))
        state["hello"] = True
        c = AGENT_CFG
        p = (tlv(T_PROTO, u16(chosen)) + tlv(T_PLATFORM, c["platform"].encode()) +
             tlv(T_AGENT_VER, c["agent_version"].encode()) + tlv(T_NONCE, c["nonce"]) +
             tlv(T_AUTH, c["auth"].encode()) + tlv(T_MODE, c["mode"].encode()) +
             tlv(T_MAXFRAME, u32(c["max_frame"])))
        return frame("RES", req["id"], req["cmd"], 0, p)
    if not state.get("hello"):
        return err_frame(req, "HELLO_REQUIRED", "send HELLO first")
    if req["cmd"] == CMD_PING:
        n = fields.get(T_PING)
        if n is None or len(n) != 8:
            return err_frame(req, "BAD_REQUEST", "ping_nonce required")
        return frame("RES", req["id"], req["cmd"], 0, tlv(T_PING, n))
    return err_frame(req, "UNSUPPORTED_COMMAND", "unknown command")


def req_dict(kind, id_, cmd, payload=b"", version=1):
    return {"kind": KIND[kind], "id": id_, "cmd": cmd, "payload": payload, "version": version}



# ---------------------------------------------------------------- pareamento / autenticação (spec §4)
CROCKFORD = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"


def code_encode(code10: bytes) -> str:
    bits = int.from_bytes(code10, "big")
    chars = "".join(CROCKFORD[(bits >> (75 - 5 * i)) & 31] for i in range(16))
    return "-".join(chars[i:i + 4] for i in range(0, 16, 4))


def code_decode(text: str):
    """Devolve os 10 bytes ou None (caractere inválido / tamanho errado)."""
    val = 0
    n = 0
    for ch in text.upper():
        if ch in "- ":
            continue
        ch = {"O": "0", "I": "1", "L": "1"}.get(ch, ch)
        k = CROCKFORD.find(ch)
        if k < 0:
            return None
        val = (val << 5) | k
        n += 1
    return val.to_bytes(10, "big") if n == 16 else None


def derive_psk(code10: bytes) -> bytes:
    return hashlib.sha256(b"NDP-PSK-v1" + code10).digest()


def key_id_of(psk: bytes) -> bytes:
    return hashlib.sha256(psk).digest()[:4]


def proof_of(psk: bytes, label: str, cn: bytes, dn: bytes, extra: bytes = b"") -> bytes:
    return hmac.new(psk, label.encode() + cn + dn + extra, hashlib.sha256).digest()


def session_key_of(psk: bytes, cn: bytes, dn: bytes) -> bytes:
    return hmac.new(psk, b"session" + cn + dn, hashlib.sha256).digest()


def sealed(session, counter, kind, request_id, command, status=0, payload=b"", version=1):
    """Frame com o bit MAC e o MAC (spec §4.5)."""
    unsigned = frame(kind, request_id, command, status, payload, FLAG_MAC, None, version)
    return unsigned + mac_of(session, counter, unsigned)


MAX_KEYS = 4
MAX_FAILS = 5
AUTH_DEVICE_ID = bytes(range(0x50, 0x60))
AUTH_NONCE = bytes(range(0xC0, 0xD0))
BRIDGE_NONCE = bytes(range(0x00, 0x10))


class AuthAgent:
    """Modelo de referência do agent com autenticação (HELLO/PING/PAIR/AUTH)."""

    def __init__(self, keys, pairing_open=False, pairing_code=None, rng_fail=False, auth="required"):
        self.keys = list(keys)  # [(psk, label)]
        self.pairing_open = pairing_open
        self.pairing_code = pairing_code
        self.rng_fail = rng_fail
        self.auth = auth
        self.hello = False
        self.authed = False
        self.session = None
        self.send_ctr = 0
        self.recv_ctr = 0
        self.fails = 0
        self.cn = None
        self.dn = None

    def _reply(self, wire):
        if self.authed:
            h = wire[:20]
            payload = wire[20:]
            kind, rid, cmd, status = h[5], struct.unpack_from("<I", h, 8)[0], struct.unpack_from("<H", h, 12)[0], struct.unpack_from("<H", h, 14)[0]
            wire = sealed(self.session, self.send_ctr, kind, rid, cmd, status, payload)
            self.send_ctr += 1
        return ("reply", wire)

    def handle(self, wire):
        magic, version, kind, flags, rid, cmd, status, plen = HDR.unpack_from(wire, 0)
        payload = wire[20:20 + plen]
        mac = wire[20 + plen:20 + plen + 16] if flags & FLAG_MAC else None
        if self.authed:
            if mac is None:
                return ("close", None)
            if not hmac.compare_digest(mac, mac_of(self.session, self.recv_ctr, wire[:20 + plen])):
                return ("close", None)
            self.recv_ctr += 1
        elif mac is not None:
            return ("close", None)
        req = {"kind": kind, "version": version, "id": rid, "cmd": cmd, "payload": payload}
        return self._dispatch(req)

    def _err(self, req, status, detail, extra=b""):
        return self._reply(err_frame(req, status, detail, extra))

    def _dispatch(self, req):
        if req["version"] != 1:
            return self._err(req, "UNSUPPORTED_PROTOCOL", "unsupported frame version",
                             tlv(T_SUP_MIN, u16(1)) + tlv(T_SUP_MAX, u16(1)))
        if req["kind"] != KIND["REQ"]:
            return self._err(req, "BAD_REQUEST", "expected REQ")
        try:
            fields = {}
            for tag, val in parse_tlvs(req["payload"]):
                fields.setdefault(tag, val)
        except struct.error:
            return self._err(req, "BAD_REQUEST", "malformed payload")
        cmd = req["cmd"]
        if cmd == CMD_HELLO:
            if self.authed:
                return self._err(req, "BAD_REQUEST", "already authenticated")
            if not all(t in fields for t in (T_PROTO, T_PROTO_MAX, T_NONCE)):
                return self._err(req, "BAD_REQUEST", "missing field")
            if len(fields[T_PROTO]) != 2 or len(fields[T_PROTO_MAX]) != 2 or len(fields[T_NONCE]) != 16:
                return self._err(req, "BAD_REQUEST", "bad field size")
            if self.auth == "required" and self.rng_fail:
                return self._err(req, "IO_ERROR", "no secure random")
            self.hello = True
            self.cn = fields[T_NONCE]
            self.dn = AUTH_NONCE
            p = (tlv(T_PROTO, u16(1)) + tlv(T_PLATFORM, b"host") + tlv(T_AGENT_VER, b"0.1.0") +
                 tlv(T_NONCE, self.dn) + tlv(T_AUTH, self.auth.encode()) + tlv(T_MODE, b"READ_ONLY") +
                 tlv(T_MAXFRAME, u32(65536)))
            if self.auth == "required":
                p += (tlv(T_DEVICE_ID, AUTH_DEVICE_ID) + tlv(T_PAIRED, bytes([len(self.keys)])) +
                      tlv(T_PAIRING_OPEN, bytes([1 if self.pairing_open else 0])))
            return self._reply(frame("RES", req["id"], cmd, 0, p))
        if not self.hello:
            return self._err(req, "HELLO_REQUIRED", "send HELLO first")
        if self.auth == "required" and not self.authed:
            if cmd == CMD_PAIR:
                return self._pair(req, fields)
            if cmd == CMD_AUTH:
                return self._auth(req, fields)
            return self._err(req, "UNAUTHORIZED", "authentication required")
        if cmd == CMD_PING:
            n = fields.get(T_PING)
            if n is None or len(n) != 8:
                return self._err(req, "BAD_REQUEST", "ping_nonce required")
            return self._reply(frame("RES", req["id"], cmd, 0, tlv(T_PING, n)))
        if cmd in (CMD_PAIR, CMD_AUTH) and self.auth == "required":
            return self._err(req, "BAD_REQUEST", "already authenticated")
        return self._err(req, "UNSUPPORTED_COMMAND", "unknown command")

    def _fail(self, req, detail):
        self.fails += 1
        if self.fails >= MAX_FAILS:
            self.pairing_open = False
            return ("close", None)
        return self._err(req, "UNAUTHORIZED", detail)

    def _pair(self, req, fields):
        label, proof = fields.get(T_LABEL), fields.get(T_PROOF)
        if label is None or proof is None or len(proof) != 32 or len(label) > 15 or len(label) == 0:
            return self._err(req, "BAD_REQUEST", "label and proof required")
        if not self.pairing_open:
            return self._fail(req, "pairing is not open")
        psk = derive_psk(self.pairing_code)
        if not hmac.compare_digest(proof, proof_of(psk, "pair", self.cn, self.dn, label)):
            return self._fail(req, "wrong pairing code")
        if len(self.keys) >= MAX_KEYS:
            return self._err(req, "NO_SPACE", "pairing storage is full")
        self.keys.append((psk, label.decode()))
        self.pairing_open = False
        return self._reply(frame("RES", req["id"], req["cmd"], 0, tlv(T_KEY_ID, key_id_of(psk))))

    def _auth(self, req, fields):
        kid, proof = fields.get(T_KEY_ID), fields.get(T_PROOF)
        if kid is None or proof is None or len(kid) != 4 or len(proof) != 32:
            return self._err(req, "BAD_REQUEST", "key_id and proof required")
        for psk, _label in self.keys:
            if key_id_of(psk) == kid:
                if hmac.compare_digest(proof, proof_of(psk, "auth", self.cn, self.dn)):
                    self.authed = True
                    self.session = session_key_of(psk, self.cn, self.dn)
                    self.send_ctr = 0
                    self.recv_ctr = 0
                    return self._reply(frame("RES", req["id"], req["cmd"], 0, b""))
                return self._fail(req, "authentication failed")
        return self._fail(req, "unknown key")


def build_auth_vectors():
    out = {}
    codes = [bytes(range(10)), bytes([0xFF] * 10), bytes.fromhex("0123456789abcdef0123"), bytes(10)]
    out["kdf"] = []
    for c in codes:
        psk = derive_psk(c)
        out["kdf"].append({
            "code_hex": hx(c), "code_text": code_encode(c), "psk_hex": hx(psk), "key_id_hex": hx(key_id_of(psk)),
            "cn_hex": hx(BRIDGE_NONCE), "dn_hex": hx(AUTH_NONCE),
            "pair_proof_hex": hx(proof_of(psk, "pair", BRIDGE_NONCE, AUTH_NONCE, b"my-mac")),
            "auth_proof_hex": hx(proof_of(psk, "auth", BRIDGE_NONCE, AUTH_NONCE)),
            "session_key_hex": hx(session_key_of(psk, BRIDGE_NONCE, AUTH_NONCE)),
            "label": "my-mac",
        })
    c0 = bytes(range(10))
    t0 = code_encode(c0)
    out["code_decode"] = [
        {"text": t0, "code_hex": hx(c0)},
        {"text": t0.lower(), "code_hex": hx(c0)},
        {"text": t0.replace("-", " "), "code_hex": hx(c0)},
        {"text": t0.replace("-", ""), "code_hex": hx(c0)},
        {"text": "0000-0000-0000-000O", "code_hex": hx(bytes(10))},   # O -> 0
        {"text": "0000-0000-0000-000I", "code_hex": hx(code_decode("0000-0000-0000-0001"))},  # I -> 1
        {"text": "0000-0000-0000-000L", "code_hex": hx(code_decode("0000-0000-0000-0001"))},  # L -> 1
        {"text": "0000-0000-0000-000U", "code_hex": None},              # U não existe no alfabeto
        {"text": "0000-0000-0000-000", "code_hex": None},               # curto
        {"text": "0000-0000-0000-00000", "code_hex": None},             # longo
        {"text": "", "code_hex": None},
    ]

    K1 = derive_psk(bytes(range(10)))
    K2 = derive_psk(bytes(range(20, 30)))
    KID1 = key_id_of(K1)
    sk1 = session_key_of(K1, BRIDGE_NONCE, AUTH_NONCE)
    code_c = bytes(range(100, 110))
    psk_c = derive_psk(code_c)

    def hello_wire(i=1, nonce=BRIDGE_NONCE):
        return frame("REQ", i, CMD_HELLO, 0, tlv(T_PROTO, u16(1)) + tlv(T_PROTO_MAX, u16(1)) + tlv(T_BRIDGE, b"ndev/0.2.0") + tlv(T_NONCE, nonce))

    def ping_payload(n): return tlv(T_PING, u64(n))
    def auth_wire(i, kid, proof): return frame("REQ", i, CMD_AUTH, 0, tlv(T_KEY_ID, kid) + tlv(T_PROOF, proof))
    def pair_wire(i, label, proof): return frame("REQ", i, CMD_PAIR, 0, tlv(T_LABEL, label) + tlv(T_PROOF, proof))
    good_auth = proof_of(K1, "auth", BRIDGE_NONCE, AUTH_NONCE)

    def dlg(name, cfg, steps_fn):
        agent = AuthAgent(**cfg["agent"])
        steps = []
        for wire in steps_fn():
            kind, resp = agent.handle(wire)
            steps.append({"request_hex": hx(wire), "response_hex": hx(resp) if resp is not None else None, "close": kind == "close"})
            if kind == "close":
                break  # the connection is gone: later steps are meaningless
        return {
            "name": name,
            "config": {"keys": [{"psk_hex": hx(p), "label": l} for p, l in cfg["agent"]["keys"]],
                       "pairing_open": cfg["agent"].get("pairing_open", False),
                       "pairing_code_hex": hx(cfg["agent"]["pairing_code"]) if cfg["agent"].get("pairing_code") else None,
                       "rng_fail": cfg["agent"].get("rng_fail", False)},
            "steps": steps,
            "store_after": [{"key_id_hex": hx(key_id_of(p)), "psk_hex": hx(p), "label": l} for p, l in agent.keys],
            "pairing_open_after": agent.pairing_open,
        }

    def A(**kw): return {"agent": kw}
    dialogues = []
    dialogues.append(dlg("unpaired_console", A(keys=[]), lambda: [
        hello_wire(1), frame("REQ", 2, CMD_PING, 0, ping_payload(1)), auth_wire(3, b"\x01\x02\x03\x04", bytes(32)),
        pair_wire(4, b"my-mac", bytes(32))]))
    dialogues.append(dlg("pair_success", A(keys=[], pairing_open=True, pairing_code=code_c), lambda: [
        hello_wire(1), pair_wire(2, b"my-mac", proof_of(psk_c, "pair", BRIDGE_NONCE, AUTH_NONCE, b"my-mac")),
        pair_wire(3, b"again", proof_of(psk_c, "pair", BRIDGE_NONCE, AUTH_NONCE, b"again"))]))
    dialogues.append(dlg("pair_wrong_code_then_ok", A(keys=[], pairing_open=True, pairing_code=code_c), lambda: [
        hello_wire(1), pair_wire(2, b"my-mac", bytes(32)),
        pair_wire(3, b"my-mac", proof_of(derive_psk(bytes(10)), "pair", BRIDGE_NONCE, AUTH_NONCE, b"my-mac")),
        pair_wire(4, b"my-mac", proof_of(psk_c, "pair", BRIDGE_NONCE, AUTH_NONCE, b"my-mac"))]))
    dialogues.append(dlg("pair_five_failures_close", A(keys=[], pairing_open=True, pairing_code=code_c), lambda: [
        hello_wire(1)] + [pair_wire(2 + i, b"x", bytes(32)) for i in range(5)] + [hello_wire(9)]))
    dialogues.append(dlg("pair_proof_bound_to_label", A(keys=[], pairing_open=True, pairing_code=code_c), lambda: [
        hello_wire(1), pair_wire(2, b"other", proof_of(psk_c, "pair", BRIDGE_NONCE, AUTH_NONCE, b"my-mac"))]))
    dialogues.append(dlg("pair_storage_full", A(keys=[(derive_psk(bytes([i] * 10)), f"k{i}") for i in range(4)], pairing_open=True, pairing_code=code_c), lambda: [
        hello_wire(1), pair_wire(2, b"my-mac", proof_of(psk_c, "pair", BRIDGE_NONCE, AUTH_NONCE, b"my-mac"))]))
    dialogues.append(dlg("pair_bad_fields", A(keys=[], pairing_open=True, pairing_code=code_c), lambda: [
        hello_wire(1), frame("REQ", 2, CMD_PAIR, 0, tlv(T_LABEL, b"x")), frame("REQ", 3, CMD_PAIR, 0, tlv(T_LABEL, b"x" * 16) + tlv(T_PROOF, bytes(32)))]))
    dialogues.append(dlg("auth_flow", A(keys=[(K1, "my-mac"), (K2, "other")]), lambda: [
        hello_wire(1), auth_wire(2, KID1, good_auth),
        sealed(sk1, 0, "REQ", 3, CMD_PING, 0, ping_payload(7)),
        sealed(sk1, 1, "REQ", 4, CMD_PING, 0, ping_payload(8)),
        sealed(sk1, 2, "REQ", 5, 0x7777, 0, b""),
        sealed(sk1, 3, "REQ", 6, CMD_HELLO, 0, tlv(T_PROTO, u16(1)) + tlv(T_PROTO_MAX, u16(1)) + tlv(T_NONCE, bytes(16)))]))
    dialogues.append(dlg("auth_second_key", A(keys=[(K1, "my-mac"), (K2, "other")]), lambda: [
        hello_wire(1), auth_wire(2, key_id_of(K2), proof_of(K2, "auth", BRIDGE_NONCE, AUTH_NONCE)),
        sealed(session_key_of(K2, BRIDGE_NONCE, AUTH_NONCE), 0, "REQ", 3, CMD_PING, 0, ping_payload(1))]))
    dialogues.append(dlg("auth_wrong_proof", A(keys=[(K1, "my-mac")]), lambda: [
        hello_wire(1), auth_wire(2, KID1, bytes(32)), auth_wire(3, KID1, good_auth)]))
    dialogues.append(dlg("auth_unknown_key", A(keys=[(K1, "my-mac")]), lambda: [
        hello_wire(1), auth_wire(2, b"\xde\xad\xbe\xef", good_auth)]))
    dialogues.append(dlg("auth_five_failures_close", A(keys=[(K1, "my-mac")]), lambda: [
        hello_wire(1)] + [auth_wire(2 + i, KID1, bytes(32)) for i in range(5)]))
    dialogues.append(dlg("auth_missing_mac_after_auth", A(keys=[(K1, "my-mac")]), lambda: [
        hello_wire(1), auth_wire(2, KID1, good_auth), frame("REQ", 3, CMD_PING, 0, ping_payload(1))]))
    dialogues.append(dlg("auth_bad_mac", A(keys=[(K1, "my-mac")]), lambda: [
        hello_wire(1), auth_wire(2, KID1, good_auth),
        sealed(sk1, 0, "REQ", 3, CMD_PING, 0, ping_payload(1))[:-1] + b"\x00"]))
    dialogues.append(dlg("auth_replayed_counter", A(keys=[(K1, "my-mac")]), lambda: [
        hello_wire(1), auth_wire(2, KID1, good_auth),
        sealed(sk1, 0, "REQ", 3, CMD_PING, 0, ping_payload(1)),
        sealed(sk1, 0, "REQ", 4, CMD_PING, 0, ping_payload(1))]))
    dialogues.append(dlg("auth_counter_skipped", A(keys=[(K1, "my-mac")]), lambda: [
        hello_wire(1), auth_wire(2, KID1, good_auth), sealed(sk1, 1, "REQ", 3, CMD_PING, 0, ping_payload(1))]))
    dialogues.append(dlg("auth_wrong_session_key", A(keys=[(K1, "my-mac")]), lambda: [
        hello_wire(1), auth_wire(2, KID1, good_auth), sealed(bytes(32), 0, "REQ", 3, CMD_PING, 0, ping_payload(1))]))
    dialogues.append(dlg("auth_mac_before_auth", A(keys=[(K1, "my-mac")]), lambda: [
        hello_wire(1), sealed(sk1, 0, "REQ", 2, CMD_PING, 0, ping_payload(1))]))
    dialogues.append(dlg("auth_ping_before_hello", A(keys=[(K1, "my-mac")]), lambda: [
        frame("REQ", 1, CMD_AUTH, 0, tlv(T_KEY_ID, KID1) + tlv(T_PROOF, good_auth))]))
    dialogues.append(dlg("auth_rng_failure", A(keys=[(K1, "my-mac")], rng_fail=True), lambda: [hello_wire(1)]))
    out["dialogues"] = dialogues
    out["constants"] = {"device_id_hex": hx(AUTH_DEVICE_ID), "device_nonce_hex": hx(AUTH_NONCE), "bridge_nonce_hex": hx(BRIDGE_NONCE)}
    return out

# ---------------------------------------------------------------- geração
def hx(b: bytes) -> str:
    return b.hex()


def build():
    v = {"spec": "ndp-v1", "constants": {"status": STATUS, "kind": KIND}}

    # SHA-256 / HMAC
    sha_msgs = [b"", b"abc", b"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", b"a" * 1000,
                bytes(range(256))]
    v["sha256"] = [{"msg_hex": hx(m), "digest_hex": hashlib.sha256(m).hexdigest()} for m in sha_msgs]
    hm = [(b"\x0b" * 20, b"Hi There"), (b"Jefe", b"what do ya want for nothing?"),
          (b"\xaa" * 20, b"\xdd" * 50), (b"\xaa" * 131, b"Test Using Larger Than Block-Size Key - Hash Key First"),
          (bytes(range(32)), b"")]
    v["hmac_sha256"] = [{"key_hex": hx(k), "msg_hex": hx(m),
                         "mac_hex": hmac.new(k, m, hashlib.sha256).hexdigest()} for k, m in hm]

    # frames
    hello_payload = (tlv(T_PROTO, u16(1)) + tlv(T_PROTO_MAX, u16(1)) + tlv(T_BRIDGE, b"ndev/0.1.0") +
                     tlv(T_NONCE, bytes(range(16))))
    key = bytes(range(32))
    mac_hdr_payload = None
    frames = []

    def add(name, kind, id_, cmd, status=0, payload=b"", flags=0, mac=None):
        w = frame(kind, id_, cmd, status, payload, flags, mac)
        frames.append({"name": name, "kind": KIND[kind], "flags": flags | (FLAG_MAC if mac else 0),
                       "request_id": id_, "command": cmd, "status": status,
                       "payload_hex": hx(payload), "mac_hex": hx(mac) if mac else None,
                       "wire_hex": hx(w)})

    add("hello_req", "REQ", 1, CMD_HELLO, payload=hello_payload)
    add("ping_req", "REQ", 0xDEADBEEF, CMD_PING, payload=tlv(T_PING, u64(0x1122334455667788)))
    add("empty_payload", "END", 7, 0x0022)
    add("data_more", "DATA", 9, 0x0022, payload=bytes(range(200)), flags=FLAG_MORE)
    add("err_frame", "ERR", 3, 0x0022, status=STATUS["NOT_FOUND"], payload=tlv(T_DETAIL, b"no such file"))
    hdr_for_mac = frame("REQ", 5, CMD_PING, 0, tlv(T_PING, u64(42)), FLAG_MAC)  # sem MAC ainda
    hp = hdr_for_mac  # header(com flag MAC) + payload
    m = mac_of(key, 5, hp)
    add("ping_req_with_mac", "REQ", 5, CMD_PING, payload=tlv(T_PING, u64(42)), mac=m)
    v["frames"] = frames
    v["mac"] = [{"name": "ping_req_with_mac", "key_hex": hx(key), "counter": 5,
                 "header_and_payload_hex": hx(hp), "mac_hex": hx(m)},
                {"name": "counter_zero", "key_hex": hx(key), "counter": 0,
                 "header_and_payload_hex": hx(hp), "mac_hex": hx(mac_of(key, 0, hp))},
                {"name": "counter_2_32", "key_hex": hx(key), "counter": 1 << 32,
                 "header_and_payload_hex": hx(hp), "mac_hex": hx(mac_of(key, 1 << 32, hp))}]

    # erros de decoder
    v["frame_errors"] = [
        {"name": "bad_magic", "wire_hex": hx(frame("REQ", 1, 1, magic=b"NDP2")), "max_payload": 65536,
         "error": "BAD_FRAME"},
        {"name": "bad_flags", "wire_hex": hx(frame("REQ", 1, 1, flags=0x0004)), "max_payload": 65536,
         "error": "BAD_FRAME"},
        {"name": "too_large_header_only", "wire_hex": hx(frame("DATA", 1, 0x22, payload=b"", payload_len=70000)),
         "max_payload": 65536, "error": "TOO_LARGE"},
        {"name": "too_large_small_limit", "wire_hex": hx(frame("DATA", 1, 0x22, payload=b"x" * 33)),
         "max_payload": 32, "error": "TOO_LARGE"},
    ]

    # paths
    path_cases = [
        b"/", b"/3ds", b"/3ds/", b"/3ds/nintendo-dev-agent/test.txt", b"/Nintendo 3DS/abc",
        b"/a b/c d", "/café/日本".encode(), b"/a/b/c/",
        b"", b"3ds", b"//", b"/a//b", b"/a//", b"/.", b"/..", b"/a/./b", b"/a/../b", b"/a/..",
        b"/a\\b", b"/a\x00b", b"/a\x01b", b"/a\x7fb", b"/a:b", b"/a*b", b"/a?b", b'/a"b', b"/a<b",
        b"/a>b", b"/a|b", b"/a.", b"/a /b", b"/a/b.", b"/NINTEN~1", b"/a~1b", b"/a~b", b"/tilde~",
        b"/\xc0\x80", b"/\xed\xa0\x80", b"/\xf4\x90\x80\x80", b"/\xff", b"/" + b"a" * 255, b"/" + b"a" * 256,
        b"/" + b"/".join([b"a" * 100] * 11),  # > 1024 bytes
    ]
    paths = []
    for raw in path_cases:
        try:
            paths.append({"input_hex": hx(raw), "ok": True, "normalized": normalize(raw)})
        except PathInvalid:
            paths.append({"input_hex": hx(raw), "ok": False, "normalized": None})
    v["paths"] = paths

    prefix_cases = [("/luma", "/luma"), ("/luma/x", "/luma"), ("/lumax", "/luma"), ("/LUMA/x", "/luma"),
                    ("/luma", "/luma/x"), ("/", "/"), ("/a", "/"), ("/", "/a"), ("/a/b", "/a/b"),
                    ("/a/b/c", "/a/b"), ("/a/bc", "/a/b"), ("/Nintendo 3DS/x", "/nintendo 3ds"),
                    ("/café", "/café"), ("/CAFé", "/café"), ("/cafÉ", "/café")]
    v["prefix"] = [{"path": p, "root": r, "inside": inside(p, r)} for p, r in prefix_cases]

    v["policy_configs"] = CONFIGS
    pol = []
    rows = [
        ("default", "READ_ONLY", "read", b"/"), ("default", "READ_ONLY", "read", b"/3ds/x"),
        ("default", "READ_ONLY", "write", b"/3ds/nintendo-dev-agent/a.txt"),
        ("default", "DEVELOPMENT", "write", b"/3ds/nintendo-dev-agent/a.txt"),
        ("default", "DEVELOPMENT", "write", b"/3ds/nintendo-dev-agent"),
        ("default", "DEVELOPMENT", "write", b"/3ds/nintendo-dev-agent/inbox/x.3dsx"),
        ("default", "DEVELOPMENT", "write", b"/3DS/NINTENDO-DEV-AGENT/A.TXT"),
        ("default", "DEVELOPMENT", "write", b"/3ds/tmc3ds/tmc3ds.3dsx"),
        ("default", "DEVELOPMENT", "write", b"/3ds"),
        ("default", "DEVELOPMENT", "write", b"/3ds/nintendo-dev-agent/config/key"),
        ("default", "DEVELOPMENT", "write", b"/3ds/nintendo-dev-agent/CONFIG"),
        ("default", "DEVELOPMENT", "read", b"/3ds/nintendo-dev-agent/config/key"),
        ("default", "FULL", "read", b"/3ds/nintendo-dev-agent/config"),
        ("default", "DEVELOPMENT", "write", b"/luma/payloads/x.firm"),
        ("default", "DEVELOPMENT", "write", b"/boot.firm"),
        ("default", "DEVELOPMENT", "write", b"/a/../luma/x"),
        ("default", "DEVELOPMENT", "write", b"/3ds/nintendo-dev-agent/../../luma/x"),
        ("default", "DEVELOPMENT", "write", b"/NINTEN~1/x"),
        ("default", "DEVELOPMENT", "read", b"relative"),
        ("default", "READ_ONLY", "write", b"bad\\path"),
        ("custom", "DEVELOPMENT", "write", b"/roms/gba/game.gba"),
        ("custom", "DEVELOPMENT", "write", b"/cias/x.cia"),
        ("custom", "DEVELOPMENT", "write", b"/3ds/tmc3ds/tmc3ds.3dsx"),
        ("custom", "DEVELOPMENT", "write", b"/3ds/other/x"),
        ("custom", "DEVELOPMENT", "read", b"/3ds/other/x"),
        ("custom", "DEVELOPMENT", "read", b"/cias/x.cia"),
        ("custom", "DEVELOPMENT", "read", b"/roms"),
        ("wide", "DEVELOPMENT", "write", b"/anything/x"),
        ("wide", "DEVELOPMENT", "write", b"/luma/x"),
        ("wide", "DEVELOPMENT", "write", b"/Nintendo 3DS/ID0/x"),
        ("wide", "FULL", "write", b"/private/x"),
        ("wide", "DEVELOPMENT", "write", b"/3ds/nintendo-dev-agent/config"),
        ("wide", "DEVELOPMENT", "read", b"/3ds/nintendo-dev-agent/config"),
        ("wide", "READ_ONLY", "read", b"/Nintendo 3DS/x"),
    ]
    for cfg, mode, op, raw in rows:
        st = policy_check(CONFIGS[cfg], mode, op, raw)
        pol.append({"config": cfg, "mode": mode, "op": op, "path_hex": hx(raw), "status": st,
                    "status_code": STATUS[st]})
    v["policy"] = pol

    # diálogos do agente (M0: HELLO/PING)
    def dlg(name, steps):
        state, out = {}, []
        for req in steps:
            wire = frame(req["kind"], req["id"], req["cmd"], 0, req["payload"], version=req["version"])
            resp = agent_handle(state, req)
            out.append({"request_hex": hx(wire), "response_hex": hx(resp)})
        return {"name": name, "steps": out}

    H = lambda id_=1, lo=1, hi=1, nonce=bytes(range(16)): req_dict(
        "REQ", id_, CMD_HELLO,
        tlv(T_PROTO, u16(lo)) + tlv(T_PROTO_MAX, u16(hi)) + tlv(T_BRIDGE, b"ndev/0.1.0") + tlv(T_NONCE, nonce))
    P = lambda id_, n=0x1122334455667788: req_dict("REQ", id_, CMD_PING, tlv(T_PING, u64(n)))
    dialogues = [
        dlg("hello_then_ping", [H(1), P(2), P(3, 0)]),
        dlg("hello_range_includes_v1", [H(1, 1, 5)]),
        dlg("hello_unsupported", [H(1, 2, 3)]),
        dlg("ping_before_hello", [P(1)]),
        dlg("hello_missing_fields", [req_dict("REQ", 1, CMD_HELLO, tlv(T_PROTO, u16(1)))]),
        dlg("hello_min_gt_max", [H(1, 3, 2)]),
        dlg("hello_bad_nonce_len", [H(1, 1, 1, b"short")]),
        dlg("hello_version2_frame", [req_dict("REQ", 1, CMD_HELLO, H()["payload"], version=2)]),
        dlg("unknown_command", [H(1), req_dict("REQ", 2, 0x7777)]),
        dlg("ping_missing_nonce", [H(1), req_dict("REQ", 2, CMD_PING)]),
        dlg("ping_bad_nonce_len", [H(1), req_dict("REQ", 2, CMD_PING, tlv(T_PING, b"\x01\x02\x03\x04"))]),
        dlg("non_req_kind", [H(1), req_dict("DATA", 2, CMD_PING, b"raw")]),
        dlg("hello_twice", [H(1), P(2), H(3), P(4)]),
        dlg("malformed_tlv_overrun", [H(1), req_dict("REQ", 2, CMD_PING, tlv(T_PING, u64(1))[:-3])]),
        dlg("malformed_tlv_trailing", [H(1), req_dict("REQ", 2, CMD_PING, tlv(T_PING, u64(1)) + b"\x01\x02")]),
        dlg("malformed_tlv_unknown_cmd", [H(1), req_dict("REQ", 2, 0x7777, b"\x01")]),
        dlg("unknown_tlv_ignored", [req_dict("REQ", 1, CMD_HELLO, H()["payload"] + tlv(0x7F00, b"xyz"))]),
    ]
    v["agent_config"] = {"platform": AGENT_CFG["platform"], "agent_version": AGENT_CFG["agent_version"],
                         "mode": AGENT_CFG["mode"], "auth": AGENT_CFG["auth"],
                         "max_frame": AGENT_CFG["max_frame"], "nonce_hex": hx(AGENT_CFG["nonce"])}
    v["dialogues"] = dialogues
    v["auth"] = build_auth_vectors()
    return v


# ---------------------------------------------------------------- emissão do header C
def c_bytes(name, b: bytes) -> str:
    body = ", ".join(f"0x{c:02x}" for c in b) if b else "0"
    return f"static const uint8_t {name}[] = {{{body}}};\n"


def c_str(s: str) -> str:
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def emit_h(v) -> str:
    L = ["/* GERADO por docs/protocol/test-vectors/generate.py — NÃO EDITAR. */\n",
         "#ifndef NDP_VECTORS_GEN_H\n#define NDP_VECTORS_GEN_H\n#include <stddef.h>\n#include <stdint.h>\n\n"]
    # sha256
    for i, e in enumerate(v["sha256"]):
        L.append(c_bytes(f"v_sha_msg_{i}", bytes.fromhex(e["msg_hex"])))
        L.append(c_bytes(f"v_sha_dig_{i}", bytes.fromhex(e["digest_hex"])))
    L.append("typedef struct { const uint8_t *msg; size_t msg_len; const uint8_t *digest; } v_sha_t;\n")
    L.append("static const v_sha_t v_sha[] = {\n")
    for i, e in enumerate(v["sha256"]):
        L.append(f"  {{v_sha_msg_{i}, {len(e['msg_hex']) // 2}, v_sha_dig_{i}}},\n")
    L.append("};\n#define V_SHA_N %d\n\n" % len(v["sha256"]))
    # hmac
    for i, e in enumerate(v["hmac_sha256"]):
        L.append(c_bytes(f"v_hm_key_{i}", bytes.fromhex(e["key_hex"])))
        L.append(c_bytes(f"v_hm_msg_{i}", bytes.fromhex(e["msg_hex"])))
        L.append(c_bytes(f"v_hm_mac_{i}", bytes.fromhex(e["mac_hex"])))
    L.append("typedef struct { const uint8_t *key; size_t key_len; const uint8_t *msg; size_t msg_len; "
             "const uint8_t *mac; } v_hmac_t;\n static const v_hmac_t v_hmac[] = {\n")
    for i, e in enumerate(v["hmac_sha256"]):
        L.append(f"  {{v_hm_key_{i}, {len(e['key_hex']) // 2}, v_hm_msg_{i}, {len(e['msg_hex']) // 2}, v_hm_mac_{i}}},\n")
    L.append("};\n#define V_HMAC_N %d\n\n" % len(v["hmac_sha256"]))
    # frames
    for i, e in enumerate(v["frames"]):
        L.append(c_bytes(f"v_fr_pay_{i}", bytes.fromhex(e["payload_hex"])))
        L.append(c_bytes(f"v_fr_wire_{i}", bytes.fromhex(e["wire_hex"])))
        L.append(c_bytes(f"v_fr_mac_{i}", bytes.fromhex(e["mac_hex"] or "")))
    L.append("typedef struct { const char *name; uint8_t kind; uint16_t flags; uint32_t request_id; "
             "uint16_t command; uint16_t status; const uint8_t *payload; size_t payload_len; "
             "const uint8_t *mac; int has_mac; const uint8_t *wire; size_t wire_len; } v_frame_t;\n")
    L.append("static const v_frame_t v_frames[] = {\n")
    for i, e in enumerate(v["frames"]):
        L.append(f"  {{{c_str(e['name'])}, {e['kind']}, 0x{e['flags']:04x}, 0x{e['request_id']:08x}u, "
                 f"0x{e['command']:04x}, {e['status']}, v_fr_pay_{i}, {len(e['payload_hex']) // 2}, "
                 f"v_fr_mac_{i}, {1 if e['mac_hex'] else 0}, v_fr_wire_{i}, {len(e['wire_hex']) // 2}}},\n")
    L.append("};\n#define V_FRAMES_N %d\n\n" % len(v["frames"]))
    # mac
    for i, e in enumerate(v["mac"]):
        L.append(c_bytes(f"v_mac_key_{i}", bytes.fromhex(e["key_hex"])))
        L.append(c_bytes(f"v_mac_hp_{i}", bytes.fromhex(e["header_and_payload_hex"])))
        L.append(c_bytes(f"v_mac_out_{i}", bytes.fromhex(e["mac_hex"])))
    L.append("typedef struct { const uint8_t *key; uint64_t counter; const uint8_t *hp; size_t hp_len; "
             "const uint8_t *mac; } v_mac_t;\nstatic const v_mac_t v_macs[] = {\n")
    for i, e in enumerate(v["mac"]):
        L.append(f"  {{v_mac_key_{i}, {e['counter']}ull, v_mac_hp_{i}, {len(e['header_and_payload_hex']) // 2}, v_mac_out_{i}}},\n")
    L.append("};\n#define V_MACS_N %d\n\n" % len(v["mac"]))
    # frame errors
    for i, e in enumerate(v["frame_errors"]):
        L.append(c_bytes(f"v_fe_wire_{i}", bytes.fromhex(e["wire_hex"])))
    L.append("typedef struct { const char *name; const uint8_t *wire; size_t wire_len; uint32_t max_payload; "
             "int error; } v_ferr_t;\nstatic const v_ferr_t v_ferrs[] = {\n")
    for i, e in enumerate(v["frame_errors"]):
        L.append(f"  {{{c_str(e['name'])}, v_fe_wire_{i}, {len(e['wire_hex']) // 2}, {e['max_payload']}u, "
                 f"{STATUS[e['error']]}}},\n")
    L.append("};\n#define V_FERRS_N %d\n\n" % len(v["frame_errors"]))
    # paths
    for i, e in enumerate(v["paths"]):
        L.append(c_bytes(f"v_pa_in_{i}", bytes.fromhex(e["input_hex"])))
    L.append("typedef struct { const uint8_t *in; size_t in_len; int ok; const char *normalized; } v_path_t;\n"
             "static const v_path_t v_paths[] = {\n")
    for i, e in enumerate(v["paths"]):
        norm = c_str(e["normalized"]) if e["ok"] else "NULL"
        L.append(f"  {{v_pa_in_{i}, {len(e['input_hex']) // 2}, {1 if e['ok'] else 0}, {norm}}},\n")
    L.append("};\n#define V_PATHS_N %d\n\n" % len(v["paths"]))
    # prefix
    L.append("typedef struct { const char *path; const char *root; int inside; } v_prefix_t;\n"
             "static const v_prefix_t v_prefixes[] = {\n")
    for e in v["prefix"]:
        L.append(f"  {{{c_str(e['path'])}, {c_str(e['root'])}, {1 if e['inside'] else 0}}},\n")
    L.append("};\n#define V_PREFIX_N %d\n\n" % len(v["prefix"]))
    # policy
    cfg_names = list(v["policy_configs"].keys())
    L.append("typedef struct { const char *read_roots[8]; const char *write_roots[8]; "
             "const char *never_read[8]; const char *never_write[8]; } v_cfg_t;\n")

    def arr(xs):
        return "{" + ", ".join(c_str(x) for x in xs) + (", NULL" if len(xs) < 8 else "") + "}"
    L.append("static const v_cfg_t v_cfgs[] = {\n")
    for n in cfg_names:
        c = v["policy_configs"][n]
        L.append(f"  {{{arr(c['read_roots'])}, {arr(c['write_roots'])}, {arr(c['never_read'])}, "
                 f"{arr(c['never_write'])}}}, /* {n} */\n")
    L.append("};\n")
    for i, e in enumerate(v["policy"]):
        L.append(c_bytes(f"v_po_path_{i}", bytes.fromhex(e["path_hex"])))
    L.append("typedef struct { int cfg; int mode; int write; const uint8_t *path; size_t path_len; int status; } "
             "v_policy_t;\nstatic const v_policy_t v_policies[] = {\n")
    for i, e in enumerate(v["policy"]):
        L.append(f"  {{{cfg_names.index(e['config'])}, {MODES.index(e['mode'])}, {1 if e['op'] == 'write' else 0}, "
                 f"v_po_path_{i}, {len(e['path_hex']) // 2}, {e['status_code']}}},\n")
    L.append("};\n#define V_POLICY_N %d\n\n" % len(v["policy"]))
    # dialogues
    ac = v["agent_config"]
    L.append(f"#define V_AGENT_PLATFORM {c_str(ac['platform'])}\n#define V_AGENT_VERSION {c_str(ac['agent_version'])}\n"
             f"#define V_AGENT_MAXFRAME {ac['max_frame']}u\n")
    L.append(c_bytes("v_agent_nonce", bytes.fromhex(ac["nonce_hex"])))
    step_rows, dl_rows = [], []
    for di, d in enumerate(v["dialogues"]):
        first = len(step_rows)
        for si, s in enumerate(d["steps"]):
            L.append(c_bytes(f"v_dl_{di}_{si}_req", bytes.fromhex(s["request_hex"])))
            L.append(c_bytes(f"v_dl_{di}_{si}_res", bytes.fromhex(s["response_hex"])))
            step_rows.append(f"  {{v_dl_{di}_{si}_req, {len(s['request_hex']) // 2}, v_dl_{di}_{si}_res, "
                             f"{len(s['response_hex']) // 2}}},\n")
        dl_rows.append(f"  {{{c_str(d['name'])}, {first}, {len(d['steps'])}}},\n")
    L.append("typedef struct { const uint8_t *req; size_t req_len; const uint8_t *res; size_t res_len; } "
             "v_step_t;\nstatic const v_step_t v_steps[] = {\n" + "".join(step_rows) + "};\n")
    L.append("typedef struct { const char *name; int first; int count; } v_dialogue_t;\n"
             "static const v_dialogue_t v_dialogues[] = {\n" + "".join(dl_rows) + "};\n")
    L.append("#define V_DIALOGUES_N %d\n\n" % len(v["dialogues"]))
    L.append(emit_auth(v["auth"]))
    L.append("#endif\n")
    return "".join(L)


def emit_auth(a) -> str:
    L = ["/* ---- pairing / authentication ---- */\n"]
    for i, e in enumerate(a["kdf"]):
        for k in ("code_hex", "psk_hex", "key_id_hex", "cn_hex", "dn_hex", "pair_proof_hex", "auth_proof_hex", "session_key_hex"):
            L.append(c_bytes(f"v_kdf_{i}_{k[:-4]}", bytes.fromhex(e[k])))
    L.append("typedef struct { const uint8_t *code; const char *code_text; const uint8_t *psk; const uint8_t *key_id; "
             "const uint8_t *cn; const uint8_t *dn; const char *label; const uint8_t *pair_proof; const uint8_t *auth_proof; "
             "const uint8_t *session; } v_kdf_t;\nstatic const v_kdf_t v_kdfs[] = {\n")
    for i, e in enumerate(a["kdf"]):
        L.append(f"  {{v_kdf_{i}_code, {c_str(e['code_text'])}, v_kdf_{i}_psk, v_kdf_{i}_key_id, v_kdf_{i}_cn, v_kdf_{i}_dn, "
                 f"{c_str(e['label'])}, v_kdf_{i}_pair_proof, v_kdf_{i}_auth_proof, v_kdf_{i}_session_key}},\n")
    L.append("};\n#define V_KDF_N %d\n\n" % len(a["kdf"]))
    for i, e in enumerate(a["code_decode"]):
        L.append(c_bytes(f"v_cd_{i}", bytes.fromhex(e["code_hex"]) if e["code_hex"] else b""))
    L.append("typedef struct { const char *text; int ok; const uint8_t *code; } v_codedec_t;\nstatic const v_codedec_t v_codedecs[] = {\n")
    for i, e in enumerate(a["code_decode"]):
        L.append(f"  {{{c_str(e['text'])}, {1 if e['code_hex'] else 0}, v_cd_{i}}},\n")
    L.append("};\n#define V_CODEDEC_N %d\n\n" % len(a["code_decode"]))
    c = a["constants"]
    L.append(c_bytes("v_auth_device_id", bytes.fromhex(c["device_id_hex"])))
    L.append(c_bytes("v_auth_device_nonce", bytes.fromhex(c["device_nonce_hex"])))
    step_rows, dl_rows, key_rows = [], [], []
    for di, d in enumerate(a["dialogues"]):
        first, kfirst = len(step_rows), len(key_rows)
        for si, s in enumerate(d["steps"]):
            L.append(c_bytes(f"v_ad_{di}_{si}_req", bytes.fromhex(s["request_hex"])))
            L.append(c_bytes(f"v_ad_{di}_{si}_res", bytes.fromhex(s["response_hex"]) if s["response_hex"] else b""))
            rl = len(s["response_hex"]) // 2 if s["response_hex"] else 0
            step_rows.append(f"  {{v_ad_{di}_{si}_req, {len(s['request_hex']) // 2}, v_ad_{di}_{si}_res, {rl}, {1 if s['close'] else 0}}},\n")
        for ki, k in enumerate(d["config"]["keys"]):
            L.append(c_bytes(f"v_ad_{di}_key{ki}", bytes.fromhex(k["psk_hex"])))
            key_rows.append(f"  {{v_ad_{di}_key{ki}, {c_str(k['label'])}}},\n")
        pc = d["config"]["pairing_code_hex"]
        L.append(c_bytes(f"v_ad_{di}_code", bytes.fromhex(pc) if pc else b""))
        for ki, k in enumerate(d["store_after"]):
            L.append(c_bytes(f"v_ad_{di}_after{ki}", bytes.fromhex(k["psk_hex"])))
        dl_rows.append((di, d, first, kfirst, pc))
    L.append("typedef struct { const uint8_t *req; size_t req_len; const uint8_t *res; size_t res_len; int close; } v_astep_t;\n"
             "static const v_astep_t v_asteps[] = {\n" + "".join(step_rows) + "};\n")
    L.append("typedef struct { const uint8_t *psk; const char *label; } v_akey_t;\nstatic const v_akey_t v_akeys[] = {\n" + "".join(key_rows) + "  {0, 0}};\n")
    L.append("typedef struct { const char *name; int first; int count; int key_first; int key_count; int pairing_open; "
             "const uint8_t *code; int rng_fail; int after_count; int pairing_open_after; } v_adlg_t;\nstatic const v_adlg_t v_adlgs[] = {\n")
    for di, d, first, kfirst, pc in dl_rows:
        L.append(f"  {{{c_str(d['name'])}, {first}, {len(d['steps'])}, {kfirst}, {len(d['config']['keys'])}, "
                 f"{1 if d['config']['pairing_open'] else 0}, v_ad_{di}_code, {1 if d['config']['rng_fail'] else 0}, "
                 f"{len(d['store_after'])}, {1 if d['pairing_open_after'] else 0}}},\n")
    L.append("};\n#define V_ADLG_N %d\n" % len(a["dialogues"]))
    L.append("static const uint8_t *const v_aafter[][5] = {\n")
    for di, d, *_ in dl_rows:
        L.append("  {" + ", ".join(f"v_ad_{di}_after{ki}" for ki in range(len(d["store_after"]))) + (", " if d["store_after"] else "") + "0},\n")
    L.append("};\n")
    return "".join(L)


def main():
    v = build()
    with open(OUT_JSON, "w", encoding="utf-8") as f:
        json.dump(v, f, indent=1, ensure_ascii=False)
        f.write("\n")
    os.makedirs(os.path.dirname(OUT_H), exist_ok=True)
    with open(OUT_H, "w", encoding="utf-8") as f:
        f.write(emit_h(v))
    print(f"ok: {len(v['frames'])} frames, {len(v['paths'])} paths, {len(v['policy'])} policy, "
          f"{len(v['dialogues'])} dialogues")


if __name__ == "__main__":
    main()
