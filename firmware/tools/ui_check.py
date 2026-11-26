"""Check remote-layout storage over USB. Uses only empty slots and empties them again.

    IRCAP_PORT=... python3 ui_check.py
"""
import base64, os, sys, time
sys.path.insert(0, __file__.rsplit("/", 1)[0] if "/" in __file__ else ".")
from ircclient import Capsule

fails = 0
def check(name, cond, info=""):
    global fails
    print(("PASS " if cond else "FAIL ") + name + ((" -- " + str(info)[:300]) if (info and not cond) else ""))
    fails += not cond

b64 = lambda b: base64.b64encode(b).decode()
c = Capsule()
r = c.call("ui_list"); check("list", r["ok"] and r["max"] == 8 and r["cap"] == 4096, r)
used = {s["slot"] for s in r.get("slots", [])}
free = [i for i in range(1, 9) if i not in used]
if len(free) < 2:
    sys.exit("needs two empty slots; nothing was changed")
a, b = free[0], free[1]
try:
    full = os.urandom(4096)
    r = c.call("ui_put", slot=a, data=b64(full)); check("put 4096 bytes", r["ok"], r)
    r = c.call("ui_get", slot=a); check("get returns the same bytes", r["ok"] and base64.b64decode(r["data"]) == full, r.get("err"))
    for name, data, err in (("4097 bytes", b64(os.urandom(4097)), "too big"), ("8000 bytes", b64(os.urandom(8000)), "too big"),
                            ("not base64", "!!!!", "bad data"), ("broken padding", "QUJD=QUJ", "bad data"), ("empty", "", "bad data")):
        r = c.call("ui_put", slot=a, data=data); check("refused: " + name, not r["ok"] and r["err"] == err, r)
    r = c.call("ui_get", slot=a); check("a refused put keeps the old content", base64.b64decode(r["data"]) == full)
    for bad in (0, 9, -1):
        for cmd, kw in (("ui_get", {}), ("ui_del", {}), ("ui_put", {"data": "QUJD"})):
            r = c.call(cmd, slot=bad, **kw); check("%s slot %d" % (cmd, bad), not r["ok"] and r["err"] == "bad slot", r)
    r = c.call("ui_put", slot=b, data=b64(b"{\"v\":1}")); check("second slot", r["ok"], r)
    sizes = {s["slot"]: s["size"] for s in c.call("ui_list")["slots"]}
    check("list shows sizes", sizes.get(a) == 4096 and sizes.get(b) == 7, sizes)
    c.call("reboot"); c.close(); time.sleep(2); c = Capsule()
    r = c.call("ui_get", slot=a); check("survives a reboot", r["ok"] and base64.b64decode(r["data"]) == full)
finally:
    c.call("ui_del", slot=a); c.call("ui_del", slot=b)
r = c.call("ui_get", slot=a); check("deleted slot is empty", not r["ok"] and r["err"] == "no such remote", r)
check("other slots untouched", {s["slot"] for s in c.call("ui_list")["slots"]} == used)
print("\nSOME FAILED" if fails else "\nALL PASS")
sys.exit(1 if fails else 0)
