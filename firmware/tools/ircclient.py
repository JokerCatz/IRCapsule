"""Minimal client for the IR capsule's line-JSON protocol over USB serial.

    from ircclient import Capsule
    c = Capsule(port)            # port=None -> $IRCAP_PORT
    print(c.call("info"))
    c.events                     # events received so far (frames, logs)
"""
import json, os, sys, time


class Capsule:
    def __init__(self, port=None, timeout=8.0):
        import serial
        port = port or os.environ.get("IRCAP_PORT")
        if not port:
            sys.exit("set IRCAP_PORT to the board's serial device")
        self.s = serial.Serial()
        self.s.port = port; self.s.baudrate = 115200; self.s.timeout = 0.05
        self.s.dtr = False; self.s.rts = False        # do not reset the board
        self.s.open()
        self.q = 0
        self.timeout = timeout
        self.events = []
        self.buf = b""
        # Opening the port resets a USB-Serial/JTAG board; wait for it to boot.
        self.wait_boot()

    def close(self):
        self.s.close()

    def wait_boot(self, limit=10.0):
        """The reset caused by opening the port may still be in progress, and old output may be
        buffered, so wait a moment and then ping until the firmware answers."""
        time.sleep(1.0)
        end = time.time() + limit
        while time.time() < end:
            self.s.write(b'{"q":0,"cmd":"ping"}\n'); self.s.flush()
            for m in self._pump(0.4):
                if m.get("q") == 0 and m.get("ok"):
                    return True
        return False

    def _pump(self, wait=0.0):
        end = time.time() + wait
        out = []
        while True:
            chunk = self.s.read(4096)
            if chunk:
                self.buf += chunk
            while b"\n" in self.buf:
                line, self.buf = self.buf.split(b"\n", 1)
                line = line.strip()
                if not line.startswith(b"{"):
                    continue
                try:
                    out.append(json.loads(line))
                except ValueError:
                    pass
            if time.time() >= end:
                return out

    def call(self, cmd, **kw):
        self.q += 1
        msg = {"q": self.q, "cmd": cmd, **kw}
        self.s.write((json.dumps(msg, ensure_ascii=False, separators=(",", ":")) + "\n").encode())
        self.s.flush()
        end = time.time() + self.timeout
        while time.time() < end:
            reply = None
            for m in self._pump(0.1):
                if "ev" in m:
                    self.events.append(m)       # keep every event, even those after the reply
                elif m.get("q") == self.q:
                    reply = m
            if reply is not None:
                return reply
        raise TimeoutError(f"no reply to {cmd}")

    def drain(self, wait=0.5):
        for m in self._pump(wait):
            if "ev" in m:
                self.events.append(m)
        ev, self.events = self.events, []
        return ev
