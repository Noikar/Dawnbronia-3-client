# Usage: python gen_qst.py <schema.json> <target.qst>
# Regenerates lq_data + lq_npc[] from the schema; keeps the target's existing
# password + lq_door section (byte-exact 1262468-byte Live Quest save).
#
# Offsets mirror the real server structs (lq.c / server.h):
#   struct character.description = LENDESC = 160  (NOT 80)
#   struct item.name = 40, struct item.description = 80
#   struct lq_item  = base[40] name[40] description[80] uint keyID  -> keyID at +160, size 164
#   struct lq_npc   = ... description[160] ... -> sizeof 2440 (verified)
import json, struct, sys
SCHEMA, OUT = sys.argv[1], sys.argv[2]
buf = bytearray(open(OUT, "rb").read()); assert len(buf) == 1262468, len(buf)
q = json.load(open(SCHEMA, encoding="utf-8"))
def pstr(o, s, n):
    b = (s or "").encode("latin1", "replace")[:n - 1]; buf[o:o + n] = b + b"\0" * (n - len(b))
def pi(o, v): struct.pack_into("<i", buf, o, int(v))
def pu(o, v): struct.pack_into("<I", buf, o, int(v) & 0xFFFFFFFF)
for i in range(40, 900): buf[i] = 0
NPC0 = 900; SZ = 2440
for i in range(NPC0, NPC0 + 512 * SZ): buf[i] = 0
# ---- lq_data (file base 40) ----
lv = q.get("level", {}); pi(40, lv.get("min", 1)); pi(44, lv.get("max", 200)); pi(888, 0)
e = q.get("entrance", {}); pi(892, e.get("x", 0)); pi(896, e.get("y", 0))
for rw in q.get("rewards", []):
    m = int(rw.get("mark", 0))
    if 0 <= m < 10: pi(48 + m * 4, rw.get("percent", 0)); pstr(88 + m * 80, rw.get("desc", ""), 80)
# ---- lq_item: base +0, name +40, description +80 (len 80), keyID +160 ----
def item(o, it):
    if not it: return
    pstr(o, it.get("base", ""), 40); pstr(o + 40, it.get("name", ""), 40)
    pstr(o + 80, it.get("description", ""), 80); pu(o + 160, it.get("keyID", 0))
# ---- lq_npc[] ----
slot = 1
for npc in q.get("npcs", []):
    o = NPC0 + slot * SZ
    pstr(o, npc.get("base", ""), 40)
    p = npc.get("pos", {}); pi(o + 40, p.get("x", 0)); pi(o + 44, p.get("y", 0)); pi(o + 48, 0)
    pi(o + 52, npc.get("level", 1)); buf[o + 56] = ord((npc.get("mode", "p") or "p")[0])
    pi(o + 60, npc.get("sprite", 0)); pi(o + 64, npc.get("respawn", 0))
    pstr(o + 68, npc.get("name", ""), 40); pstr(o + 108, npc.get("description", ""), 160)
    pstr(o + 268, npc.get("nick", ""), 40); pstr(o + 348, npc.get("greeting", ""), 256)
    for k, r in enumerate(npc.get("replies", [])[:5]):
        pstr(o + 604 + k * 40, r.get("trigger", ""), 40); pstr(o + 804 + k * 256, r.get("reply", ""), 256)
    pu(o + 2084, (npc.get("want_item") or {}).get("keyID", 0)); item(o + 2088, npc.get("reward_item"))
    pu(o + 2256, npc.get("kill_mark", 0)); pu(o + 2260, npc.get("hurt_mark", 0))
    item(o + 2264, npc.get("item")); pi(o + 2428, npc.get("gold", 0)); slot += 1
open(OUT, "wb").write(buf); print(f"wrote {OUT} ({len(buf)} bytes), NPC slots 1..{slot - 1}")
