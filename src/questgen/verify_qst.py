# Usage: python verify_qst.py <target.qst>
# Dumps an lq quest save's data + NPCs using the REAL server struct offsets
# (mirror of lq.c / server.h). Use it to confirm a gen_qst.py output before
# /questload. If base/name look swapped or an item base isn't a real lq_
# template, the server logs `create_item(): could not find item "lq_<name>"`.
#
# Offset crib (o = lq_npc slot base; array starts at file offset 900, slot=2440):
#   base+0 x+40 y+44 dir+48 level+52 mode+56 sprite+60 respawn+64
#   name+68  description+108(160!)  nick+268  greeting+348
#   trigger[k]+604+k*40  reply[k]+804+k*256
#   want_keyID+2084  reward_item+2088  kill_mark+2256  hurt_mark+2260
#   carry_item+2264  carry_gold+2428
#   lq_item = base[40] name[40] description[80] uint keyID(+160)   <- base drives "lq_%s"
import struct, sys
d = open(sys.argv[1], "rb").read()
def s(o, n):
    r = d[o:o + n]; z = r.find(b'\0'); return (r[:z] if z >= 0 else r).decode('latin1')
def i32(o): return struct.unpack_from("<i", d, o)[0]
def u32(o): return struct.unpack_from("<I", d, o)[0]
print("size", len(d), "(expect 1262468)")
print("lq_data: lvl %d-%d entrance (%d,%d) open=%d" % (i32(40), i32(44), i32(892), i32(896), i32(888)))
NPC0, SZ = 900, 2440
for n in range(1, 513):
    b = NPC0 + n * SZ
    base = s(b + 0, 40)
    if not base: continue
    print("[%d] base=%s nick=%s name=%s pos=(%d,%d) lvl=%d mode=%s sprite=%d resp=%d" % (
        n, base, s(b + 268, 40), s(b + 68, 40), i32(b + 40), i32(b + 44), i32(b + 52), chr(d[b + 56]), i32(b + 60), i32(b + 64)))
    g = s(b + 348, 256)
    if g: print("     greeting=%r" % g)
    for k in range(5):
        t, rp = s(b + 604 + k * 40, 40), s(b + 804 + k * 256, 256)
        if t or rp: print("     reply[%d] %r -> %r" % (k, t, rp))
    if u32(b + 2084): print("     want_keyID=%d" % u32(b + 2084))
    rb = s(b + 2088, 40)
    if rb: print("     REWARD item: base=%r name=%r keyID=%d" % (rb, s(b + 2128, 40), u32(b + 2248)))
    km, hm = u32(b + 2256), u32(b + 2260)
    if km or hm: print("     kill_mark=%d hurt_mark=%d" % (km, hm))
    cb = s(b + 2264, 40)
    if cb: print("     CARRY item: base=%r name=%r keyID=%d gold=%d" % (cb, s(b + 2304, 40), u32(b + 2424), i32(b + 2428)))
