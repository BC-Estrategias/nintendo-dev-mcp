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
    L.append("#define V_DIALOGUES_N %d\n\n#endif\n" % len(v["dialogues"]))
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
