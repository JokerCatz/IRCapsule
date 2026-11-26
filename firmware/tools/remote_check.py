"""Check the TV remote commands over USB, with no TV paired. Does not change stored signals.

    IRCAP_PORT=... python3 remote_check.py
WARNING: forgets the paired TV, if there is one.
"""
import sys
sys.path.insert(0, __file__.rsplit("/", 1)[0] if "/" in __file__ else ".")
from ircclient import Capsule

ok = True
def check(name, cond, info=""):
    global ok
    print(("PASS " if cond else "FAIL ") + name + ((" -- " + str(info)[:300]) if (info and not cond) else ""))
    ok &= bool(cond)

c = Capsule()
r = c.call("tv", op="forget"); check("forget is ok", r["ok"], r)
r = c.call("tv"); check("status: no TV", r["ok"] and r["paired"] == 0 and r["connected"] == 0 and r["window"] == 0, r)
for k in ("up", "down", "left", "right", "ok", "ok2", "back", "back2", "volup", "voldown", "mute"):
    r = c.call("key", k=k); check("key %s says no TV" % k, not r["ok"] and r["err"] == "TV not connected", r)
r = c.call("key", k="nope"); check("unknown key rejected", not r["ok"] and r["err"] == "unknown key", r)
r = c.call("tv", op="pair"); check("pairing window opens", r["ok"] and 110 <= r["window"] <= 120, r)
r = c.call("tv", op="cancel"); check("pairing window closes", r["ok"] and r["window"] == 0, r)
r = c.call("tv", op="bogus"); check("unknown op rejected", not r["ok"], r)
i = c.call("info"); print("   heap", i["heap"], "min", i["minheap"], "signals", i["ns"])
print("\nALL PASS" if ok else "\nSOME FAILED")
sys.exit(0 if ok else 1)
