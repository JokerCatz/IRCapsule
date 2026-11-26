"""Check TV-key rule steps and "one press, one run" over USB. Creates two signals and rules and removes
only those. Needs no TV (and must run with no TV connected).

    IRCAP_PORT=... python3 relay_check.py
"""
import sys, time
sys.path.insert(0, __file__.rsplit("/", 1)[0] if "/" in __file__ else ".")
from ircclient import Capsule

fails = 0
def check(name, cond, info=""):
    global fails
    print(("PASS " if cond else "FAIL ") + name + ((" -- " + str(info)[:300]) if (info and not cond) else ""))
    fails += not cond

c = Capsule()
def logs(wait):
    return [e["m"] for e in c.drain(wait) if e.get("ev") == "log"]

r = c.call("tv"); check("tv has no compat field", r["ok"] and "compat" not in r, r)
r = c.call("tv", op="compat", on=1); check("compat op is gone", not r["ok"] and r["err"] == "unknown op", r)

nec = [9000, 4500]
word = 0x00FF_5AA5
for i in range(32):
    nec += [560, 1690 if word >> i & 1 else 560]
nec += [560]
# not NEC: another header, 24 bits
other = [4000, 4000]
for i in range(24):
    other += [500, 2000 if (0x35A5C3 >> i) & 1 else 1000]
other += [500]

made_sigs, made_rules = [], []
layer = c.call("layer")["layer"]
try:
    r = c.call("sig_put", name="relay-nec", raw=nec); s_nec = r["id"]; made_sigs.append(s_nec)
    r = c.call("sig_put", name="relay-other", raw=other); s_other = r["id"]; made_sigs.append(s_other)
    r = c.call("rule_put", name="relay-a", trig=s_nec, layer=layer, on=1, act=[3, 0x01BB, 3, 0x4000 | 0x223, 3, 0x8000 | 0x50])
    check("TV key steps accepted", r["ok"], r); ra = r.get("id")
    if ra: made_rules.append(ra)
    for bad in (0, 0x8000 | 0x100, 0x2000, 0x1000 | 5):
        r = c.call("rule_put", id=ra, act=[3, bad])
        check("bad tv key 0x%X rejected" % bad, not r["ok"] and r["err"] == "bad tv key", r)
    got = [x for x in c.call("list")["rule"] if x["id"] == ra][0]["act"]
    check("a rejected step list leaves the rule unchanged", got == [3, 0x01BB, 3, 0x4223, 3, 0x8050], got)

    c.call("rule_put", id=ra, act=[3, 0x00E9, 1, 10, 3, 0x00E2])
    c.drain(0.2); c.call("run", id=ra); lg = logs(1.0)
    check("without a TV both TV steps are skipped and the rule continues", sum("skipped" in m for m in lg) == 2, lg)

    # one press, one run
    def count(name, lg): return sum(m.startswith("trigger") and name in m for m in lg)
    time.sleep(0.6); c.drain(0.1)
    c.call("inject", raw=nec); time.sleep(0.1); c.call("inject", raw=nec); lg = logs(1.0)
    check("two NEC full frames 100 ms apart are two presses", count("relay-nec", lg) == 2, lg)

    rb = c.call("rule_put", name="relay-b", trig=s_other, layer=layer, on=1, act=[1, 10])["id"]; made_rules.append(rb)
    time.sleep(0.6); c.drain(0.1)
    for _ in range(6):
        c.call("inject", raw=other); time.sleep(0.15)
    lg = logs(0.5)
    check("a held button (other format, every 150 ms) runs once", count("relay-other", lg) == 1, lg)
    time.sleep(0.6); c.call("inject", raw=other); lg = logs(1.0)
    check("a new press after a quiet gap runs again", count("relay-other", lg) == 1, lg)
finally:
    for i in made_rules: c.call("rule_del", id=i)
    for i in made_sigs: c.call("sig_del", id=i)

print("\nSOME FAILED" if fails else "\nALL PASS")
sys.exit(1 if fails else 0)
