"""Check the `ledtest` command over USB. Adds a temporary signal and rule and removes them again. Takes about 80 s.

    IRCAP_PORT=... python3 ledtest_check.py
"""
import sys, time
sys.path.insert(0, __file__.rsplit("/", 1)[0] if "/" in __file__ else ".")
from ircclient import Capsule

ok = True
def check(name, cond, info=""):
    global ok
    print(("PASS " if cond else "FAIL ") + name + ((" -- " + str(info)[:300]) if (info and not cond) else ""))
    ok &= bool(cond)

def logs(c, wait):
    return [e["m"] for e in c.drain(wait) if e.get("ev") == "log"]

c = Capsule()
r = c.call("ledtest", on=0); check("off while idle is ok", r["ok"], r)
c.drain(0.7)
r1 = c.call("ledtest", on=1); r2 = c.call("ledtest", on=1)
check("on twice is ok", r1["ok"] and r2["ok"], (r1, r2))
lg = logs(c, 3.0)
check("one blink job only", lg.count("led test on") == 1, lg)
check("still answers while blinking", c.call("ping")["ok"] and c.call("info")["ok"])
c.drain(0.1); c.call("play", khz=38, raw=[560, 560, 560])
lg = logs(c, 1.0)
check("other sends wait during the test", not any(m.startswith("sent raw") for m in lg), lg)
t = time.time(); r = c.call("ledtest", on=0); check("off is ok", r["ok"], r)
lg = logs(c, 1.5)
sent = [m for m in lg if m.startswith("sent raw")]
check("off ends the test, then the queued send runs",
      "led test off" in lg and sent and sent[0].endswith("ok") and lg.index("led test off") < lg.index(sent[0]), lg)

# a rule whose trigger is heard during the test must not fire (it would run late, after the test)
c.drain(0.5)
frame = [9000, 4500] + [560, 1690] * 32 + [560]
sid = c.call("sig_put", name="ledtest-tmp", raw=frame)["id"]
rid = c.call("rule_put", name="ledtest-tmp", trig=sid, layer=c.call("layer")["layer"], on=1, act=[1, 10])["id"]
try:
    c.drain(0.2); c.call("ledtest", on=1); time.sleep(0.8); c.call("inject", raw=frame)
    time.sleep(0.5); c.call("ledtest", on=0)
    lg = logs(c, 2.0)
    check("rules do not fire during the test", not any(m.startswith("trigger") for m in lg), lg)
    time.sleep(0.5); c.drain(0.1); c.call("inject", raw=frame)
    lg = logs(c, 1.0)
    check("rules fire again after the test", any(m.startswith("trigger") for m in lg), lg)
finally:
    c.call("rule_del", id=rid); c.call("sig_del", id=sid)

c.drain(0.5)
c.call("ledtest", on=1); start = time.time(); ended = None
while time.time() - start < 63 and ended is None:
    if "led test off" in logs(c, 0.2):
        ended = time.time() - start
check("stops by itself after 60 s", ended is not None and 59 <= ended <= 62, ended)
if ended is None:
    c.call("ledtest", on=0)

c.drain(0.5)
c.call("inject", raw=[9000, 4500] + [560, 560] * 32 + [560])
check("reception works after the test", any(e.get("ev") == "frame" for e in c.drain(1.0)))
print("\nALL PASS" if ok else "\nSOME FAILED")
sys.exit(0 if ok else 1)
