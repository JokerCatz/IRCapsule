"""Check the Bluetooth access header: UART for setup, this computer's Bluetooth as the phone.
Restores whatever header was set before. Takes about two minutes.

    IRCAP_PORT=... python3 access_check.py [name-prefix]     (needs bleak, cryptography, pyserial)
"""
import asyncio, base64, sys, time
sys.path.insert(0, __file__.rsplit("/", 1)[0] if "/" in __file__ else ".")
from ircclient import Capsule
from securelink import Link, find

SVC = "9f1d0001-6b7a-4c5e-8a2b-1c4a9e5d7f10"
PREFIX = sys.argv[1] if len(sys.argv) > 1 else "IRCapsule"
H1, H2 = "test-header_1.~abcdefgh", "second-header-0123456789"
fails = 0
def check(name, cond, info=""):
    global fails
    print(("PASS " if cond else "FAIL ") + name + ((" -- " + str(info)[:300]) if (info and not cond) else ""), flush=True)
    fails += not cond

c = Capsule()                      # USB; opening it reboots the board
def usb(cmd, **kw):
    return c.call(cmd, **kw)
async def pause(t):                # keep draining USB so the board never blocks on it
    end = time.time() + t
    while time.time() < end:
        c.drain(0.05); await asyncio.sleep(0.15)

async def adv():
    from bleak import BleakScanner
    found = await BleakScanner.discover(timeout=8, return_adv=True)
    return [a for d, a in found.values() if (a.local_name or d.name or "").startswith(PREFIX)]

async def lost(link, t):
    for _ in range(int(t * 5)):
        if link.lost: return True
        await pause(0.2)
    return False

async def main():
    before = usb("access")
    check("UART can read the header setting", before["ok"] and "set" in before, before)
    try:
        usb("access", h="")
        for bad in ("a" * 65, "has space", "semi;colon", "中文"):
            r = usb("access", h=bad); check("refused header %r" % bad[:12], not r["ok"] and r["err"] == "bad header", r)
        a = await adv()
        check("advertised", a, a)
        check("config service UUID is not advertised", a and all(SVC not in x.service_uuids for x in a), [x.service_uuids for x in a])
        dev = await find(PREFIX)

        # open mode: old pages work without hello
        l = await Link(dev).open()
        r = await l.call("ping"); check("open: a command works without hello", r and r["ok"], r)
        r = await l.call("hello"); check("open: hello says enc 0", r and r.get("enc") == 0, r)
        r = await l.call("info"); check("info reports no header", r and r.get("access") == 0, r)
        await l.close(); await pause(2)

        r = usb("access", h=H1); check("UART sets a header", r["ok"] and r["set"] == 1, r)
        r = usb("access"); check("UART reads it back", r.get("h") == H1, r)
        check("UART stays plain and unrestricted", usb("info")["ok"] and usb("info")["access"] == 1)

        l = await Link(dev).open()
        r = await l.call("info"); check("locked: info refused", r and not r["ok"] and r["err"] == "locked", r)
        r = await l.call("tv", op="pair"); check("locked: pairing window refused", r and r.get("err") == "locked" and usb("tv")["window"] == 0, r)
        seen = len(l.wire_lines)
        usb("inject", raw=[9000, 4500] + [560, 560] * 32 + [560]); await pause(2)
        check("locked: no events reach the link", len(l.wire_lines) == seen, l.wire_lines[seen:])
        h1 = await l.call("hello"); h2 = await l.call("hello")
        check("hello: salt, iterations and a fresh nonce", h1 and h1["enc"] == 1 and h1["i"] == 20000 and len(h1["s"]) == 32 and len(h1["n"]) == 32 and h1["n"] != h2["n"], (h1, h2))
        r = await l.auth("wrong-header")
        check("wrong header refused", r and r.get("err") == "bad header", r)
        check("and the link is closed", await lost(l, 6))
        l = await Link(dev).open(); r = await l.auth(H1)
        if r and r.get("err") == "wait": pass
        check("an immediate retry is slowed down", r and r.get("err") == "wait", r)
        await l.close(); await pause(4)

        l = await Link(dev).open(); t = time.time(); r = await l.auth(H1)
        check("right header accepted, reply is encrypted", r and r["ok"] and l.wire_lines[-1].startswith(b"~"), (r, l.broken))
        print("   handshake %.1f s" % (time.time() - t))
        mark = len(l.wire_lines)
        r = await l.call("info"); check("encrypted info", r and r["ok"] and r["access"] == 1, (r, l.broken))
        r = await l.call("access"); check("an accepted link can read the header", r and r.get("h") == H1, r)
        big = [9000, 4500] + [560, 1690] * 599                         # 1200 entries
        r = await l.call("sig_put", t=30, name="access-tmp", raw=big); sid = r and r.get("id")
        check("1200-entry write, encrypted", r and r["ok"], (r, l.broken))
        if sid:
            task = asyncio.create_task(l.call("get", t=30, id=sid))
            for _ in range(5):
                usb("inject", raw=[9000, 4500] + [560, 560] * 32 + [560]); await pause(0.1)
            g = await task
            check("1200-entry read while events arrive", g and g.get("raw") == big and not l.broken, l.broken)
            await pause(2)
            check("events arrive encrypted", any(m.get("ev") == "frame" for m in l.msgs), l.broken)
            r = await l.call("sig_del", id=sid); check("still in step afterwards", r and r["ok"] and not l.broken, (r, l.broken))
        check("nothing readable on the wire after auth", all(x.startswith(b"~") for x in l.wire_lines[mark:]))
        sealed = b"~" + l.seal(b'{"q":900,"cmd":"ping"}\n') + b"\n"
        await l.raw(sealed); r = await l.wait(900); check("a record works once", r and r["ok"], r)
        await l.raw(sealed); check("the same record again closes the link", await lost(l, 6))

        await pause(2); l = await Link(dev).open(); await l.auth(H1)
        bad = bytearray(l.seal(b'{"q":901,"cmd":"ping"}\n')); bad[5] = ord("A") if bad[5] != ord("A") else ord("B")
        try: await l.raw(b"~" + bytes(bad) + b"\n")
        except Exception: pass
        check("an altered record closes the link", await lost(l, 6))

        await pause(2); l = await Link(dev).open(); await l.auth(H1)
        old_auth = l.auth_line
        r = await l.call("access", h=H2); check("change over Bluetooth keeps this link", r and r["ok"] and (await l.call("ping")) and not l.lost, r)
        old_salt = l.hello["s"]; await l.close(); await pause(2)
        l = await Link(dev).open(); await l.raw(old_auth); await pause(1.5)
        check("a recorded auth from before cannot be replayed", not any(m.get("ok") and m.get("q") for m in l.msgs), l.msgs)
        await l.close(); await pause(4)
        l = await Link(dev).open(); r = await l.auth(H2)
        check("new header works, with a new salt", r and r["ok"] and l.hello["s"] != old_salt, r)
        r = usb("access", h=""); check("UART clears it", r["ok"] and r["set"] == 0, r)
        check("which closes the web link", await lost(l, 6))

        usb("access", h=H1); await pause(2)
        l = await Link(dev).open(); t = time.time()
        gone = await lost(l, 45); took = time.time() - t
        check("a link that never authenticates is dropped after about 30 s", gone and 25 <= took <= 40, took)
    finally:
        usb("access", h=before.get("h", ""))
        for s in usb("list")["sig"]:
            if s["name"] == "access-tmp": usb("sig_del", id=s["id"])
    check("header setting restored", usb("access").get("h", "") == before.get("h", ""))

asyncio.run(main())
print("\nSOME FAILED" if fails else "\nALL PASS")
sys.exit(1 if fails else 0)
