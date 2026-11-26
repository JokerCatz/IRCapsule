"""A client may send several commands without waiting: none may get lost.  IRCAP_PORT=... python3 burst_check.py"""
import sys, time
sys.path.insert(0, __file__.rsplit("/", 1)[0] if "/" in __file__ else ".")
from ircclient import Capsule
c = Capsule()
bad = 0
for round_ in range(3):
    ids = list(range(1000 * (round_ + 1), 1000 * (round_ + 1) + 12))
    c.s.write(b"".join(b'{"q":%d,"cmd":"%s"}\n' % (q, [b"list", b"info", b"log", b"tv"][q % 4]) for q in ids)); c.s.flush()
    got = {m.get("q") for m in c._pump(4.0) if "ok" in m}
    missing = [q for q in ids if q not in got]
    print(("PASS " if not missing else "FAIL ") + "12 commands in one go, round %d" % (round_ + 1) + ("" if not missing else " -- %d without a reply" % len(missing)))
    bad += bool(missing)
print("\nSOME FAILED" if bad else "\nALL PASS")
sys.exit(1 if bad else 0)
