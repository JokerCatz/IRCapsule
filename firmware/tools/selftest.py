"""End-to-end protocol test over USB: record, name, send, rules, persistence.

    IRCAP_PORT=... python3 selftest.py

Safe on a capsule in use:
  * no factory reset, and the TV pairing is never touched;
  * only made-up codes, fed in with `inject` as if the receiver had heard them (no remote needed).
    Sending them does light the emitters, but they belong to no appliance;
  * no rule here presses a TV key;
  * only signals and rules created by this run are deleted; stored data is compared at the end.
Keep real remotes away from the receiver while it runs. It reboots the capsule once."""
import json, sys, time
sys.path.insert(0, __file__.rsplit("/", 1)[0] if "/" in __file__ else ".")
from ircclient import Capsule

fails = 0
def check(name, cond, info=""):
    global fails
    print(("PASS " if cond else "FAIL ") + name + ((" -- " + str(info)[:300]) if (info and not cond) else ""))
    fails += not cond


def nec(addr, cmd):
    word = addr | ((addr ^ 0xFF) << 8) | (cmd << 16) | ((cmd ^ 0xFF) << 24)
    raw = [9000, 4500]
    for i in range(32):
        raw += [560, 1690 if word >> i & 1 else 560]
    return raw + [560]


def long_frame_with_shrinking_marks():
    """Like a dense air-conditioner frame through a VS1838B: marks get shorter, spaces longer by as much."""
    raw = [3400, 1750]
    for bit in range(144):
        lost = min(310, max(0, bit - 25) * 4)
        space = 1300 if (bit * 7 + 3) % 5 < 2 else 450
        raw += [425 - lost, space + lost]
    return raw + [115]


TRIGGER, KEY_B, APPLIANCE = nec(0x5A, 0x11), nec(0x5A, 0x12), nec(0x5A, 0x13)
LONG = long_frame_with_shrinking_marks()
NAMES = ["selftest 觸發", "selftest \"機上盒\"", "selftest 電視", "selftest 冷氣"]

c = Capsule()
before = c.call("list")
info = c.call("info")
layer, dedupe = before["layer"], info["dedupe"]
if info["drafts"] + 4 > 40 or info["ns"] + 4 > info["max"]["sig"] or info["nr"] + 1 > info["max"]["rule"]:
    sys.exit("not enough free room for the test's temporary signals and rule; nothing was changed")
had = {s["id"] for s in before["sig"]}
made_sigs, made_rules = set(), set()


def hear(raw):
    c.drain(0.05)
    c.call("inject", raw=raw)
    frames = [e for e in c.drain(0.6) if e.get("ev") == "frame"]
    ev = frames[-1] if frames else None
    if ev and ev.get("draft") and ev["draft"] not in had:
        made_sigs.add(ev["draft"])
    return ev


def after_trigger(wait=1.5):
    c.drain(0.05)
    c.call("inject", raw=TRIGGER)
    time.sleep(wait)
    ev = c.drain(0.3)
    return [e for e in ev if e.get("ev") == "frame"], [e["m"] for e in ev if e.get("ev") == "log"]


def runs(logs):
    return sum('-> rule "selftest"' in m for m in logs)


try:
    c.call("set", dedupe=400)

    # --- recording: frames become numbered drafts, repeats only count
    check("record on", c.call("record", on=1)["ok"])
    e1, e2, e3, e4, e5 = (hear(x) for x in (TRIGGER, TRIGGER, KEY_B, APPLIANCE, LONG))
    check("first press makes a draft", e1 and e1["draft"] and e1["sig"] == e1["draft"], e1)
    check("same key again reuses the draft", e2 and e2["draft"] == e1["draft"], e2)
    check("different keys make further drafts", e3 and e4 and e5 and e1["draft"] < e3["draft"] < e4["draft"] < e5["draft"], (e3, e4, e5))
    drafts = [s for s in c.call("list")["sig"] if s["id"] in made_sigs]
    check("4 drafts, no names, press counter",
          len(drafts) == 4 and all(d["st"] == 0 and d["name"] == "" for d in drafts) and [d["hits"] for d in drafts] == [2, 1, 1, 1], drafts)
    if len(drafts) != 4:
        raise RuntimeError("recording did not work, stopping here")
    r = c.call("fire", id=drafts[0]["id"]); check("a draft cannot be fired", not r["ok"], r)
    r = c.call("rule_put", name="selftest", trig=drafts[0]["id"], act=[0, drafts[1]["id"]]); check("rules cannot use drafts", not r["ok"], r)
    r = c.call("sig_put", id=drafts[0]["id"], state=1); check("cannot activate without a name", not r["ok"], r)

    # --- naming
    for d, n, k in zip(drafts, NAMES, [1, 0, 0, 0]):
        r = c.call("sig_put", id=d["id"], name=n, kind=k, state=1, khz=38); check("name and activate " + n, r["ok"], r)
    c.call("record", on=0)
    mine = {s["name"]: s for s in c.call("list")["sig"] if s["id"] in made_sigs}
    check("names round-trip incl. CJK and quotes, all active", set(mine) == set(NAMES) and all(s["st"] == 1 for s in mine.values()), list(mine))
    trig, key_b, tv, ac = (mine[n]["id"] for n in NAMES)
    g = c.call("get", id=tv); check("stored waveform is what was heard", g["raw"] == APPLIANCE, g.get("n"))
    stored_long = c.call("get", id=ac)["raw"]
    marks = [m for m in stored_long[2::2] if m < 1500]
    check("shrunken marks of a long frame were restored", len(stored_long) == len(LONG) and min(marks) > 300, min(marks))

    # --- direct send
    c.drain(0.1); check("fire an active signal", c.call("fire", id=tv)["ok"])
    time.sleep(0.8); lg = [e["m"] for e in c.drain(0.3) if e.get("ev") == "log"]
    check("it is logged as sent ok", any(m.startswith("sent") and m.endswith("ok") for m in lg), lg)

    # --- rule: heard trigger -> steps
    r = c.call("rule_put", name="selftest", trig=trig, layer=layer, on=1, act=[0, tv, 1, 120, 0, ac])
    check("rule_put", r["ok"], r); rid = r["id"]; made_rules.add(rid)
    time.sleep(0.5); fr, lg = after_trigger()
    check("heard trigger fires the rule", fr and fr[0]["m"] == rid, fr)
    check("both send steps ran", sum(m.startswith("sent") for m in lg) == 2, lg)
    c.call("rule_put", id=rid, on=0); time.sleep(0.5); fr, lg = after_trigger()
    check("a disabled rule does not fire", fr and fr[0]["m"] == 0 and not runs(lg), (fr, lg))
    c.call("rule_put", id=rid, on=1, act=[1, 10]); time.sleep(0.5)
    c.drain(0.1); c.call("inject", raw=TRIGGER); time.sleep(0.3); c.call("inject", raw=TRIGGER); time.sleep(0.8)
    lg = [e["m"] for e in c.drain(0.3) if e.get("ev") == "log"]
    check("two quick presses run the rule twice (one press, one run)", runs(lg) == 2, lg)
    c.call("sig_put", id=tv, state=2)
    r = c.call("fire", id=tv); check("a disabled signal cannot be fired", not r["ok"], r)
    c.call("sig_put", id=tv, state=1)

    # --- recording suppresses rules
    time.sleep(0.5); c.call("record", on=1); fr, lg = after_trigger(0.8); c.call("record", on=0)
    check("rules do not fire while recording", fr and fr[0]["m"] == 0 and not runs(lg), (fr, lg))

    # --- persistence
    c.call("rule_put", id=rid, act=[0, tv, 1, 120, 0, ac])
    saved = c.call("list"); c.call("reboot"); c.close(); time.sleep(1.5); c = Capsule()
    after = c.call("list")
    check("everything survives a reboot", {k: saved[k] for k in ("dev", "sig", "rule")} == {k: after[k] for k in ("dev", "sig", "rule")})
    check("waveform survives a reboot", c.call("get", id=ac)["raw"] == stored_long)

    # --- delete cleans references
    c.call("sig_del", id=tv); made_sigs.discard(tv)
    rule = [x for x in c.call("list")["rule"] if x["id"] == rid][0]
    check("deleting a signal removes it from rules", rule["act"] == [1, 120, 0, ac], rule)
finally:
    c.call("record", on=0)
    for rid in made_rules:
        c.call("rule_del", id=rid)
    for sid in made_sigs:
        c.call("sig_del", id=sid)
    c.call("set", dedupe=dedupe)

end = c.call("list")
check("stored signals, rules and devices are as before",
      {k: end[k] for k in ("dev", "sig", "rule")} == {k: before[k] for k in ("dev", "sig", "rule")},
      {"signals": [(s["id"], s["name"]) for s in end["sig"]], "rules": [x["name"] for x in end["rule"]]})
r = c.call("info"); print("   heap", r["heap"], "min", r["minheap"])
print("\nSOME FAILED" if fails else "\nALL PASS")
sys.exit(1 if fails else 0)
