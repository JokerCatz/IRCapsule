"""End-to-end protocol test over USB. Uses the `inject` command to feed recorded frames to the
firmware as if the receiver had heard them, so no remote is needed.

    IRCAP_PORT=... python3 selftest.py captures.jsonl
The captures file needs three groups of frames: 9 of a TV, 9 of a set-top box (NEC), 3 of an A/C,
in that order (as recorded for the project), or adjust IDX below.
WARNING: this factory-resets the board's stored data."""
import json, sys, time
sys.path.insert(0, __file__.rsplit("/", 1)[0] if "/" in __file__ else ".")
from ircclient import Capsule

frames = [json.loads(l) for l in open(sys.argv[1]) if l.strip().startswith("{")]
raw = lambda i: frames[i]["raw"]
TV_POWER, STB_A, STB_B, AC_ON = 1, 10, 11, 18      # indexes into the captures file

ok = True
def check(name, cond, info=""):
    global ok
    print(("PASS " if cond else "FAIL ") + name + ((" -- " + str(info)) if (info and not cond) else ""))
    ok &= bool(cond)

c = Capsule()
r = c.call("reset", confirm=1); check("factory reset accepted", r["ok"])
c.close(); time.sleep(1.5); c = Capsule()
r = c.call("info"); check("fresh device is empty", r["ns"] == 0 and r["nr"] == 0, r)
print("   heap", r["heap"], "min", r["minheap"], "nvs free entries", r["nvs"]["free"])

# --- recording: frames become numbered drafts, repeats only count
check("record on", c.call("record", on=1)["ok"])
def hear(i):
    c.call("inject", raw=raw(i)); ev = [e for e in c.drain(0.6) if e.get("ev") == "frame"]
    return ev[-1] if ev else None
e1 = hear(STB_A); e2 = hear(STB_A); e3 = hear(STB_B); e4 = hear(TV_POWER); e5 = hear(AC_ON)
check("first press makes a draft", e1 and e1["draft"] and e1["sig"] == e1["draft"], e1)
check("same key again reuses the draft", e2 and e2["draft"] == e1["draft"], e2)
check("different key makes the next draft", e3 and e3["draft"] > e1["draft"], e3)
check("TV and AC frames are drafted in order", e4 and e5 and e3["draft"] < e4["draft"] < e5["draft"], (e4, e5))
L = c.call("list"); drafts = [s for s in L["sig"] if s["st"] == 0]
check("4 drafts, ascending ids", len(drafts) == 4 and [d["id"] for d in drafts] == sorted(d["id"] for d in drafts), L["sig"])
check("press counter", drafts[0]["hits"] == 2 and drafts[1]["hits"] == 1, drafts)
check("drafts have no name", all(d["name"] == "" for d in drafts))
r = c.call("fire", id=drafts[0]["id"]); check("a draft cannot be fired", not r["ok"], r)
r = c.call("rule_put", name="x", trig=drafts[0]["id"], act=[0, drafts[1]["id"]]); check("rules cannot use drafts", not r["ok"], r)
r = c.call("sig_put", id=drafts[0]["id"], state=1); check("cannot activate without a name", not r["ok"], r)

# --- batch naming
names = ["遙控 A（觸發）", "機上盒 \"電源\"", "電視 電源", "冷氣 開"]
for d, n, k in zip(drafts, names, [1, 0, 0, 0]):
    r = c.call("sig_put", id=d["id"], name=n, kind=k, state=1, khz=38); check("name+activate " + n, r["ok"], r)
c.call("record", on=0)
L = c.call("list"); S = {s["name"]: s for s in L["sig"]}
check("names round-trip incl. CJK and quotes", set(S) == set(names), list(S))
check("all active", all(s["st"] == 1 for s in L["sig"]))
g = c.call("get", id=S["電視 電源"]["id"])
check("stored raw has the recorded length", abs(g["n"] - len(raw(TV_POWER))) <= 0 and len(g["raw"]) == g["n"], (g["n"], len(raw(TV_POWER))))
try:
    ac = c.call("get", id=S["冷氣 開"]["id"])["raw"]
except TimeoutError:
    time.sleep(1); extra = c._pump(1.0)
    print("DEBUG leftover buf", len(c.buf), c.buf[:200], "extra msgs", [ (m.get("q"), m.get("ev")) for m in extra])
    print("DEBUG info", c.call("info")); raise
marks = ac[2::2]
check("A/C marks were restored to ~400us", min(m for m in marks if m < 1500) > 300, min(marks))

# --- direct fire over the link
c.drain(0.1); check("fire active signal", c.call("fire", id=S["電視 電源"]["id"])["ok"])
time.sleep(0.8); lg = [e for e in c.drain(0.3) if e.get("ev") == "log"]
check("fire is logged as sent ok", any("sent" in e["m"] and "ok" in e["m"] for e in lg), lg)

# --- rule: heard trigger -> actions
trig = S["遙控 A（觸發）"]["id"]
r = c.call("rule_put", name="回家", trig=trig, layer=0, act=[0, S["電視 電源"]["id"], 1, 120, 0, S["冷氣 開"]["id"]])
check("rule_put", r["ok"], r); rid = r["id"]
def trigger():
    c.drain(0.05); c.call("inject", raw=raw(STB_A)); time.sleep(1.5)
    ev = c.drain(0.3)
    return [e for e in ev if e.get("ev") == "frame"], [e["m"] for e in ev if e.get("ev") == "log"]
fr, lg = trigger()
check("heard trigger fires the rule", fr and fr[-1]["m"] == rid, fr)
check("rule actions executed", sum("sent" in m for m in lg) == 2, lg)
c.call("rule_put", id=rid, on=0)
fr, lg = trigger()
check("disabled rule does not fire", fr and fr[-1]["m"] == 0 and not any("sent" in m for m in lg), (fr, lg))
c.call("rule_put", id=rid, on=1)
time.sleep(0.5)
c.drain(0.1); c.call("inject", raw=raw(STB_A)); c.call("inject", raw=raw(STB_A)); time.sleep(1.5)
lg = [e["m"] for e in c.drain(0.3) if e.get("ev") == "log"]
check("dedupe: two quick triggers run once", sum("trigger" in m for m in lg) == 1, lg)
c.call("sig_put", id=S["電視 電源"]["id"], state=2)
r = c.call("fire", id=S["電視 電源"]["id"]); check("disabled signal cannot be fired", not r["ok"], r)
c.call("sig_put", id=S["電視 電源"]["id"], state=1)

# --- recording mode suppresses rules
c.call("record", on=1); fr, lg = trigger(); c.call("record", on=0)
check("rules do not fire while recording", fr and fr[-1]["m"] == 0 and not any("trigger" in m for m in lg), (fr, lg))

# --- persistence across reboot
before = c.call("list"); c.call("reboot"); c.close(); time.sleep(1.5); c = Capsule()
after = c.call("list")
check("everything survives a reboot", {k: before[k] for k in ("dev", "sig", "rule")} == {k: after[k] for k in ("dev", "sig", "rule")})
g2 = c.call("get", id=S["冷氣 開"]["id"])["raw"]; check("raw survives a reboot", g2 == ac)

# --- delete cleans references
c.call("sig_del", id=S["電視 電源"]["id"])
rr = [x for x in c.call("list")["rule"] if x["id"] == rid][0]
check("deleting a signal removes it from rules", rr["act"] == [0, S["冷氣 開"]["id"]] or rr["act"] == [1, 120, 0, S["冷氣 開"]["id"]], rr)

r = c.call("info"); print("   heap", r["heap"], "min", r["minheap"], "signals", r["ns"], "nvs free", r["nvs"]["free"])
print("\nALL PASS" if ok else "\nSOME FAILED")
sys.exit(0 if ok else 1)
