"""Check the BLE transport: connect, subscribe, run commands, compare big replies.
    python3 blecheck.py [name-prefix]      (needs `pip install bleak`)"""
import asyncio, json, sys
from bleak import BleakClient, BleakScanner

SVC = "9f1d0001-6b7a-4c5e-8a2b-1c4a9e5d7f10"
RX  = "9f1d0002-6b7a-4c5e-8a2b-1c4a9e5d7f10"
TX  = "9f1d0003-6b7a-4c5e-8a2b-1c4a9e5d7f10"

async def main(prefix="IRCapsule"):
    dev = await BleakScanner.find_device_by_filter(lambda d, a: (d.name or "").startswith(prefix) or SVC in (a.service_uuids or []), timeout=15)
    if not dev:
        print("FAIL no device found"); return 1
    print("found", dev.name, dev.address)
    buf = bytearray(); msgs = asyncio.Queue()
    def on_tx(_, data):
        buf.extend(data)
        while b"\n" in buf:
            i = buf.index(b"\n"); line = bytes(buf[:i]); del buf[:i + 1]
            try: msgs.put_nowait(json.loads(line))
            except ValueError: print("bad json line", line[:80])
    ok = True
    def check(name, cond, info=""):
        nonlocal ok; ok &= bool(cond); print(("PASS " if cond else "FAIL ") + name + ((" -- " + str(info)[:200]) if not cond and info else ""))
    async with BleakClient(dev) as cli:
        print("mtu", cli.mtu_size)
        await cli.start_notify(TX, on_tx)
        q = 0
        async def call(cmd, **kw):
            nonlocal q; q += 1
            data = (json.dumps({"q": q, "cmd": cmd, **kw}, ensure_ascii=False, separators=(",", ":")) + "\n").encode()
            step = max(20, min(180, cli.mtu_size - 3))
            for i in range(0, len(data), step):
                await cli.write_gatt_char(RX, data[i:i + step], response=True)
            while True:
                m = await asyncio.wait_for(msgs.get(), 15)
                if m.get("q") == q: return m
        r = await call("ping"); check("ping", r["ok"])
        r = await call("info"); check("info", r["ok"] and r["mtu"] >= 23, r); print("   device mtu", r["mtu"], "heap", r["heap"])
        L = await call("list"); check("list", L["ok"] and len(L["sig"]) >= 1, L); print("   signals", [(s["id"], s["name"], s["n"]) for s in L["sig"]])
        big = max(L["sig"], key=lambda s: s["n"])
        g = await call("get", id=big["id"]); check(f"get {big['n']}-entry signal over BLE", g["ok"] and len(g["raw"]) == big["n"], g.get("n"))
        r = await call("fire", id=L["sig"][0]["id"]); check("fire", r["ok"], r)
        # a big write: save a copy of the big signal under a new name through BLE
        r = await call("sig_put", name="ble copy", kind=0, state=1, raw=g["raw"]); check(f"write {len(g['raw'])} entries over BLE", r["ok"], r)
        if r["ok"]:
            g2 = await call("get", id=r["id"]); check("copy reads back identical", g2["raw"] == g["raw"])
            await call("sig_del", id=r["id"])
        lg = await call("log"); check("log", lg["ok"] and len(lg["log"]) > 0)
        # events while connected
        await call("record", on=1); await call("inject", raw=g["raw"][:67] if len(g["raw"]) >= 67 else g["raw"])
        ev = None
        try:
            for _ in range(10):
                m = await asyncio.wait_for(msgs.get(), 3)
                if m.get("ev") == "frame": ev = m; break
        except asyncio.TimeoutError: pass
        await call("record", on=0)
        check("frame event arrives over BLE (no raw)", ev and "raw" not in ev, ev)
        if ev and ev.get("draft"): await call("sig_del", id=ev["draft"])
    print("\nALL PASS" if ok else "\nSOME FAILED"); return 0 if ok else 1

sys.exit(asyncio.run(main(*(sys.argv[1:2]))))
