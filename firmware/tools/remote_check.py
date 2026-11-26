"""Check the TV remote commands over USB, with no TV connected. Does not change stored signals.

    IRCAP_PORT=... python3 remote_check.py
WARNING: forgets the paired TV, if there is one, and leaves compatibility mode off.
"""
import sys, time
sys.path.insert(0, __file__.rsplit("/", 1)[0] if "/" in __file__ else ".")
from ircclient import Capsule

KEYS = ("up down left right ok ok2 back back2 volup voldown mute up2 down2 left2 right2 home input input2 "
        "settings settings2 menu menu2 search guide info chup chdown playpause stop rew ffwd prev next power "
        "youtube netflix").split()
fails = 0
def check(name, cond, info=""):
    global fails
    if not cond:
        fails += 1; print("FAIL " + name + ((" -- " + str(info)[:300]) if info else ""))
def done(name, before):                      # before = failure count when the group started
    if fails == before: print("PASS " + name)

c = Capsule()
s = fails; r = c.call("tv", op="forget"); check("forget", r["ok"], r)
r = c.call("tv"); check("status", r["ok"] and r["paired"] == 0 and r["connected"] == 0 and r["window"] == 0, r); done("forget and status", s)
s = fails
for k in KEYS:
    r = c.call("key", k=k); check("key " + k, not r["ok"] and r["err"] == "TV not connected", r)
done("all %d key names are known" % len(KEYS), s)
s = fails; r = c.call("key", k="nope"); check("unknown key", not r["ok"] and r["err"] == "unknown key", r); done("unknown key rejected", s)
s = fails
for args in ({"page": "cc", "code": 0x1BB}, {"page": "cc", "code": 1}, {"page": "cc", "code": 0x3FF}, {"page": "kb", "code": 0x65}, {"page": "kb", "code": 255}, {"k": "home", "ms": 1500}):
    r = c.call("key", **args); check("valid %s" % args, not r["ok"] and r["err"] == "TV not connected", r)
done("raw codes and long press accepted", s)
s = fails
for args in ({"page": "cc", "code": 0}, {"page": "cc", "code": 0x400}, {"page": "kb", "code": 256}, {"page": "zz", "code": 1}, {"page": "cc"}, {}):
    r = c.call("key", **args); check("invalid %s" % args, not r["ok"] and r["err"] == "bad key code", r)
for ms in (0, 3001, -1):
    r = c.call("key", k="home", ms=ms); check("bad ms %d" % ms, not r["ok"] and r["err"] == "bad hold time", r)
done("bad codes and hold times rejected", s)
s = fails
r = c.call("tv", op="pair"); check("window opens", r["ok"] and 110 <= r["window"] <= 120, r)
r = c.call("tv", op="cancel"); check("window closes", r["ok"] and r["window"] == 0, r)
r = c.call("tv", op="bogus"); check("unknown op", not r["ok"], r); done("pairing window and unknown op", s)
s = fails
r = c.call("tv"); check("compat off by default", r.get("compat") == 0, r)
r = c.call("tv", op="compat", on=1); check("compat on", r["ok"] and r["compat"] == 1, r)
c.call("reboot"); c.close(); time.sleep(2); c = Capsule()
r = c.call("tv"); check("compat survives a reboot", r.get("compat") == 1, r)
r = c.call("tv", op="compat"); check("compat needs on=0/1", not r["ok"], r)
r = c.call("tv", op="compat", on=0); check("compat off", r["ok"] and r["compat"] == 0, r); done("compatibility mode switches and persists", s)
i = c.call("info"); print("   heap", i["heap"], "min", i["minheap"], "signals", i["ns"])
print("\nSOME FAILED" if fails else "\nALL PASS")
sys.exit(1 if fails else 0)
