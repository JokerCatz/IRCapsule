"""Development bridge: exposes the board's USB serial port as a WebSocket so the web page can be
tested against real firmware from any browser (including headless ones, which cannot show the
Web Serial / Web Bluetooth device pickers).

    pip install websockets pyserial
    IRCAP_PORT=/dev/serial/by-id/... python3 bridge.py [ws-port]      # default 8765
    open  web/index.html?bridge=ws://127.0.0.1:8765

Opening the serial port resets the board once (USB-Serial/JTAG behaviour)."""
import asyncio, os, sys, threading
import serial
import websockets

PORT = os.environ.get("IRCAP_PORT") or sys.exit("set IRCAP_PORT to the board's serial device")
WS_PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8765

ser = serial.Serial(); ser.port = PORT; ser.baudrate = 115200; ser.timeout = 0.05
ser.dtr = False; ser.rts = False; ser.open()
clients = set()
loop = None

def reader():
    buf = b""
    while True:
        buf += ser.read(4096)
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            line = line.strip()
            if line.startswith(b"{") and loop:
                asyncio.run_coroutine_threadsafe(fanout(line.decode(errors="replace")), loop)

async def fanout(text):
    for ws in list(clients):
        try: await ws.send(text)
        except Exception: clients.discard(ws)

async def handler(ws):
    clients.add(ws)
    try:
        async for msg in ws:
            ser.write((msg.strip() + "\n").encode()); ser.flush()
    finally:
        clients.discard(ws)

async def main():
    global loop
    loop = asyncio.get_running_loop()
    threading.Thread(target=reader, daemon=True).start()
    async with websockets.serve(handler, "127.0.0.1", WS_PORT, max_size=2**22):
        print("bridge listening on ws://127.0.0.1:%d" % WS_PORT, flush=True)
        await asyncio.Future()

asyncio.run(main())
