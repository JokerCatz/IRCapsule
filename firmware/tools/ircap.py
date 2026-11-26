"""Host helper for the IR capsule.

  ircap.py list   <captures.jsonl>
  ircap.py show   <captures.jsonl> <index>          # normalised timings
  ircap.py play   <captures.jsonl> <index> [khz]    # send over USB serial (default 38 kHz)
  ircap.py ping

Set IRCAP_PORT to the board's serial device (a /dev/serial/by-id/... path is stable).
The port is opened with DTR/RTS low so the board is not reset.
"""
import json, os, statistics, sys, time

PORT = os.environ.get("IRCAP_PORT")  # e.g. /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_<MAC>-if00


def load(path):
    with open(path) as f:
        return [json.loads(l) for l in f if l.strip().startswith("{")]


def normalise(raw):
    """Undo receiver mark shrinkage.

    A VS1838B on long, dense frames (air conditioners) outputs marks that get shorter while the
    spaces grow by the same amount, so mark+space (the bit period) stays right. Rebuild every
    mark at the canonical length and give the difference back to the space. Frames whose marks do
    not drift (NEC, TV remotes) are returned unchanged.
    """
    r = list(raw)
    marks = [r[k] for k in range(2, len(r) - 1, 2) if r[k] < 1500]
    if len(marks) < 24:
        return r
    canon = statistics.median(marks[:20])
    tail = statistics.mean(marks[-max(8, len(marks) // 4):])
    if tail > 0.85 * canon:
        return r
    out = r[:2]
    for k in range(2, len(r) - 1, 2):
        m, s = r[k], r[k + 1]
        if m < 1500:
            out += [int(round(canon)), max(int(round(s + m - canon)), 100)]
        else:
            out += [m, s]
    if len(r) % 2:
        out.append(int(round(canon)) if r[-1] < 1500 else r[-1])
    return out


def capsule():
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from ircclient import Capsule
    return Capsule(PORT)


def main():
    if len(sys.argv) < 2:
        print(__doc__); return
    cmd = sys.argv[1]
    if cmd == "ping":
        print(capsule().call("ping")); return
    fr = load(sys.argv[2])
    if cmd == "list":
        for i, f in enumerate(fr):
            print(f'{i:>3}  t={f["t"]:>8}  gap={f["gap"]:>6}  n={f["n"]:>4}  {f["p"]}  [{f.get("label","")}]')
    elif cmd in ("show", "play"):
        idx = int(sys.argv[3]); raw = fr[idx]["raw"]; norm = normalise(raw)
        if cmd == "show":
            print("changed" if norm != raw else "unchanged", len(norm), "entries")
            print(norm); return
        khz = int(sys.argv[4]) if len(sys.argv) > 4 else 38
        print(f"playing #{idx} ({fr[idx]['p']}), {len(norm)} entries @ {khz} kHz")
        print(capsule().call("play", khz=khz, raw=norm))


if __name__ == "__main__":
    main()
