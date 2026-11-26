"""Read JSON frame lines from the C3 and append them to a JSONL file.
usage: capture.py <port> <out.jsonl> <seconds> [label]
Opening the port reboots a USB-Serial/JTAG board; frames are only heard after it has started.
Tip: recording can also be done on the device itself (web page, Record tab)."""
import json, sys, time, serial

port, out, secs = sys.argv[1], sys.argv[2], float(sys.argv[3])
label = sys.argv[4] if len(sys.argv) > 4 else ""
s = serial.Serial(); s.port = port; s.baudrate = 115200; s.timeout = 0.5
s.dtr = False; s.rts = False; s.open()
end = time.time() + secs
count = 0
with open(out, "a") as f:
    while time.time() < end:
        line = s.readline().decode(errors="replace").strip()
        if not line.startswith("{"):
            continue
        try:
            d = json.loads(line)
        except ValueError:
            continue
        if d.get("ev") != "frame" or "raw" not in d:
            continue
        d["wall"] = time.time(); d["label"] = label
        f.write(json.dumps(d) + "\n"); f.flush()
        count += 1
        print(f'{d["t"]:>8} gap={d["gap"]:>6} n={d["n"]:>4}  {d["p"]}', flush=True)
print(f"done: {count} frames", flush=True)
