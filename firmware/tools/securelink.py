"""Independent client of the capsule's Bluetooth access protocol (see docs/protocol.md), for tests.

    pip install bleak cryptography
"""
import asyncio, base64, hashlib, hmac, json, os
from bleak import BleakClient, BleakScanner
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

RX = "9f1d0002-6b7a-4c5e-8a2b-1c4a9e5d7f10"
TX = "9f1d0003-6b7a-4c5e-8a2b-1c4a9e5d7f10"
PIECE = 600


async def find(prefix="IRCapsule", timeout=15):
    return await BleakScanner.find_device_by_filter(lambda d, a: (a.local_name or d.name or "").startswith(prefix), timeout=timeout)


class Link:
    def __init__(self, dev):
        self.dev = dev
        self.c = None
        self.q = 0
        self.keys = None
        self.plain = b""
        self.wire = b""
        self.msgs = []
        self.wire_lines = []
        self.broken = None
        self.lost = False

    async def open(self, tries=3):
        # Linux pairs by itself when it sees the HID service; right after a dropped link a connect can time out
        for attempt in range(1, tries + 1):
            try:
                self.lost = False
                self.c = BleakClient(self.dev, timeout=20, disconnected_callback=lambda _: setattr(self, "lost", True))
                await self.c.connect()
                await self.c.start_notify(TX, self._rx)
                return self
            except Exception as e:
                print("   connect attempt %d failed: %s %s" % (attempt, type(e).__name__, e), flush=True)
                await self.close()
                if attempt == tries:
                    raise
                await asyncio.sleep(3)

    async def close(self):
        try:
            await self.c.disconnect()
        except Exception:
            pass

    def _rx(self, _, data):
        self.wire += bytes(data)
        while b"\n" in self.wire:
            line, self.wire = self.wire.split(b"\n", 1)
            self.wire_lines.append(line)
            if line.startswith(b"~"):
                try:
                    n = self.rx_n.to_bytes(8, "big")
                    self.plain += AESGCM(self.keys[1]).decrypt(b"\0" * 4 + n, base64.b64decode(line[1:]), None)
                    self.rx_n += 1
                except Exception as e:
                    if not self.broken:
                        self.broken = "%r on wire line %d (%d bytes, expected record %d): %r ... %r" % (
                            e, len(self.wire_lines), len(line), self.rx_n, line[:40], line[-40:])
            else:
                self.plain += line + b"\n"
        while b"\n" in self.plain:
            line, self.plain = self.plain.split(b"\n", 1)
            try:
                self.msgs.append(json.loads(line))
            except ValueError:
                pass

    async def raw(self, line: bytes):
        for i in range(0, len(line), 180):
            await self.c.write_gatt_char(RX, line[i:i + 180], response=True)

    def seal(self, text: bytes) -> bytes:
        ct = AESGCM(self.keys[0]).encrypt(b"\0" * 4 + self.tx_n.to_bytes(8, "big"), text, None)
        self.tx_n += 1
        return base64.b64encode(ct)

    async def wait(self, q, t=10.0):
        for _ in range(int(t / 0.05)):
            for m in self.msgs:
                if m.get("q") == q and "ok" in m:
                    return m
            if self.lost:
                break
            await asyncio.sleep(0.05)
        return None

    async def call(self, cmd, t=10.0, **kw):
        self.q += 1
        text = (json.dumps({"q": self.q, "cmd": cmd, **kw}, separators=(",", ":")) + "\n").encode()
        if self.keys:
            for i in range(0, len(text), PIECE):
                await self.raw(b"~" + self.seal(text[i:i + PIECE]) + b"\n")
        else:
            await self.raw(text)
        return await self.wait(self.q, t)

    async def auth(self, header: str):
        """hello + auth. Returns the final reply (or the hello reply when the capsule is open)."""
        h = await self.call("hello")
        if not h or not h.get("enc"):
            return h
        self.hello = h
        mk = hashlib.pbkdf2_hmac("sha256", header.encode(), bytes.fromhex(h["s"]), h["i"], 32)
        mine = os.urandom(16)
        nonces = mine + bytes.fromhex(h["n"])
        self.keys = tuple(hmac.new(mk, lab + nonces, hashlib.sha256).digest() for lab in (b"c2d", b"d2c"))
        self.tx_n = self.rx_n = 0
        self.q += 1
        proof = self.seal(b"ircapsule").decode()
        self.auth_line = (json.dumps({"q": self.q, "cmd": "auth", "c": mine.hex(), "p": proof}, separators=(",", ":")) + "\n").encode()
        await self.raw(self.auth_line)
        r = await self.wait(self.q)
        if not r or not r.get("ok"):
            self.keys = None
        return r
