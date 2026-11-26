# IR Capsule

An ESP32-C3 "record and redirect" infrared hub. It learns the signals of your existing remotes
(TV, set-top box, air conditioner, ...), stores them, and replays them on demand. It is meant to sit
in a transparent capsule shell, be configured over Bluetooth LE from a static web page, and be
triggered either from that page or by a spare IR remote that your appliances ignore.

> **Status: early bring-up.** Recording and the replay path work in firmware. Nothing has been
> verified against a real appliance yet, because the transmitter hardware is still being built.
> See [Status](#status).

## How it works

```
spare remote (code your appliances ignore) ──IR──▶ [VS1838B ×1–4] ─┐
                                                                   ▼
                    phone / PC ──BLE (Web Bluetooth)──▶  ESP32-C3 Super Mini
                                                                   │ GPIO5
                                                                   ▼
                                             [ULN2003] ─▶ [IR333C-A ring ×8] ──IR──▶ appliances
```

- **Record:** the receiver captures any remote; the firmware streams each frame as raw mark/space
  timings plus a best-effort protocol guess.
- **Replay:** raw timings are sent with a hardware 38 kHz carrier (ESP32-C3 RMT peripheral), so BLE
  interrupts cannot jitter the carrier.
- **Trigger:** a button in a static Web Bluetooth page, or a "fake" code from a spare remote.
  The device only reacts to codes in its table and ignores its own emissions, so there is no feedback loop.
- No web server, no Wi-Fi, no SD card. Everything stored on the chip's 4 MB flash.

## Status

| Piece | State |
|---|---|
| Record: continuous capture, numbered drafts, press counter, batch naming | Working, tested end to end |
| Signals: saved in flash with name, type, device, active/disabled state | Working, survives reboot |
| Rules: heard signal → list of steps (send, wait, switch layer), enable/disable | Working |
| Web config page (Bluetooth, USB serial, demo) with waveform view, log, backup | Working; tested against real firmware through a bridge and in demo mode. **Web Bluetooth and Web Serial pickers are not exercised by automated tests** |
| Receiver artifact repair for A/C frames | Working |
| RMT transmit with hardware carrier, self-reception guard | Implemented. **Not yet verified on a real appliance** |
| Emitter ring (ULN2003 + 8 LEDs) | Built, being verified (LED test mode, `ledtest`). See [docs/wiring.html](docs/wiring.html) |

## TV remote over Bluetooth

For TV functions that have no infrared code (many Android / Google TV remotes send only Power as infrared),
the capsule pairs with the TV as a Bluetooth remote (HID over GATT): direction keys, OK, Back, Home, volume,
media keys, input source, settings and more, long presses, and any other consumer or keyboard code typed into
the page. **Pairing was confirmed on one Android TV; which keys that TV acts on is still being collected.**
Whether a key works depends on the TV.

1. Open the config page, tab **TV remote**, press **Pair a TV**.
2. Within 120 s, add a Bluetooth accessory on the TV and choose `IRCapsule-XXXX`. Use the TV's general
   accessory list: its pairing screen for its own remote (hold Home + OK) looks for that remote only.
3. The page shows **TV connected**. The pairing survives reboots and a normal `idf.py flash`.

Pair only after pressing the button. If the TV paired earlier, remove the capsule on the TV and start again.
The page and the TV can be connected at the same time. Waking a TV from standby this way should not be relied on.

**Compatibility mode** (off by default). Android picks a key layout by the Bluetooth vendor/product id. Some
keys, such as Input source, are mapped in the layout of Google's reference remote
([`Vendor_0957_Product_0001.kl`](https://android.googlesource.com/platform/frameworks/base/+/main/data/keyboards/Vendor_0957_Product_0001.kl)).
This switch makes the capsule report those ids so you can try that with your own TV. It does not make the
capsule a Google product or certified in any way, and it does not pass a TV's own remote-pairing screen. The
TV has to pair again after switching. No direct "HDMI 1/2/3" key is provided; use Input source and the arrows.

## Backup and restore

**Device** tab. **Export backup** saves signals (drafts included), waveforms, rules, devices and the settings
(name, repeat-ignore time, layer) as one JSON file. **Restore from backup replaces** what is on the capsule
with the file: it is validated first, your current data is downloaded, you are asked to confirm, and the result
is read back and compared. If the link drops in the middle, import the same file again. The TV pairing and
draft press counters are not part of a backup. A changed name applies after a reboot.

## Config page

Hosted with GitHub Pages: <https://jokercatz.github.io/IRCapsule/> ([demo without a device](https://jokercatz.github.io/IRCapsule/?demo=1),
[wiring diagram](https://jokercatz.github.io/IRCapsule/docs/wiring.html)). Bluetooth and USB need Chrome or Edge.
On Linux, Web Bluetooth is off by default: enable `chrome://flags/#enable-experimental-web-platform-features`
and relaunch the browser, or use USB.

## Hardware

| Part | Qty | Notes |
|---|---|---|
| ESP32-C3 Super Mini | 1 | 4 MB flash, on-board LED on GPIO8 |
| VS1838B IR receiver | 1–4 | Power from 3V3, never 5V. Outputs joined on GPIO4 |
| IR333C-A 940 nm LED | 8 (+4 spare) | 4 strings of 2 in series |
| ULN2003APG (DIP-16) | 1 (+1 spare) | Not ULN2004: its inputs need 6–15 V |
| 22 Ω resistor | 4 | One per string |
| 100 µF electrolytic | 1 | On the 5V rail (+ to 5V). Steadies the supply during LED bursts |
| 0.1 µF ceramic (marked 104) | 1 per receiver | VCC to GND at each VS1838B. One more near the ULN2003 is optional |

Full schematic, pin tables, ring layout and pre-power checks: **[docs/wiring.html](docs/wiring.html)**
(open it in a browser). Pins used:

| GPIO | Function |
|---|---|
| 4 | IR receive (VS1838B OUT, idle high) |
| 5 | IR transmit (ULN2003 IN1–IN4) |
| 8 | On-board status LED (not an external part) |

**LED check after soldering:** connect the web page, open the **Device** tab and press **Start LED
test** (protocol command `ledtest`). All emitters blink for 60 s; watch them through a phone camera.
Each string has two LEDs in series, so one bad or reversed LED keeps both of its string dark.

## Build and flash

Requires [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) v6.1.

```sh
cd firmware
idf.py set-target esp32c3
idf.py build
idf.py -p /dev/ttyACM0 flash        # the C3 USB-Serial/JTAG port; no BOOT button needed
```

Prefer a stable `/dev/serial/by-id/...` path if several boards are plugged in. Opening a board's
serial port from esptool or pyserial can reset it, so be careful when sharing a machine with other
projects.

## Web config page

`web/index.html` is a single static file. Serve it over HTTPS (GitHub Pages works) or from
`http://localhost`, because Web Bluetooth needs a secure context.

```sh
cd web && python3 -m http.server 8000      # then open http://localhost:8000
```

Chrome or Edge on Android or desktop. On iPhone use a browser that supports Web Bluetooth, such as Bluefy.
Web Serial (USB) needs desktop Chrome or Edge. Add `?demo=1` to try it with a simulated capsule.

Typical use:

1. **Record**: press *Start recording*, then press keys on any remote pointed at the receiver. Each new
   key becomes a numbered draft with a waveform; pressing the same key again raises its counter.
2. **Name**: type names (or paste a list, one per line, applied in recording order), pick the type and device,
   press *Save named drafts*. Only named signals can be used.
3. **Send**: the *Signals* tab has a *Send* button per appliance command, over Bluetooth or USB.
4. **Rules**: *When I hear* a saved signal (for example a key on a spare remote), *then do* a list of steps.
   Rules and signals can be disabled without deleting them.

## Command line tools

For development. They need [pyserial](https://pypi.org/project/pyserial/) (and `bleak`, `websockets` for two of them).

```sh
export IRCAP_PORT=/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_<MAC>-if00

python3 firmware/tools/selftest.py captures/tv.jsonl   # protocol test over USB (erases stored data!)
python3 firmware/tools/blecheck.py                     # protocol test over Bluetooth
python3 firmware/tools/bridge.py 8765                  # USB <-> WebSocket for testing the web page
python3 firmware/tools/capture.py "$IRCAP_PORT" captures/tv.jsonl 60 tv   # log raw frames to a file
```

The wire protocol is documented in [docs/protocol.md](docs/protocol.md). Opening the serial port reboots
the board, so start tools after plugging in and wait a couple of seconds.

## Repository layout

```
firmware/        ESP-IDF project
  main/ir_rmt.c    RMT capture and send
  main/engine.c    frame ring, drafts, trigger matching, macro executor, log
  main/store.c     flash storage (NVS)
  main/proto.c     command interpreter
  main/ble.c       GATT transport
firmware/tools/  host-side test and development tools (Python)
web/index.html   configuration page (single static file)
docs/wiring.html Schematic and assembly guide (self-contained, open in a browser)
docs/protocol.md Wire protocol
docs/spec.zh-TW.md  Full design spec and discussion (Traditional Chinese)
```

## Known limits

- The VS1838B demodulates, so the carrier frequency of a recorded remote cannot be measured. Replay
  defaults to 38 kHz; other carriers (36, 40, 56 kHz) must be set by hand.
- RMT durations are 15 bits wide, so entries over 32 767 µs are split into pieces on transmit.
- A frame is closed after 30 ms of silence (the hardware idle limit at 1 MHz), so A/C messages with
  longer internal gaps arrive in several frames. Group them by the `gap` field.
- **Air conditioners send their whole panel state in every frame** (mode, temperature, fan, ...). A recorded
  frame therefore sets the appliance to exactly the state it had when recorded. Record one signal per state you
  want (for example "Cool 26 °C", "Off"). Editing individual fields (temperature up/down) is not implemented.
- Free heap after boot is about 85 KB (Bluetooth stack included). Storage holds 96 signals and 48 rules of up to
  1200 entries each; flash is not the limit, RAM is.
- Anyone in Bluetooth range can connect, change settings and press keys on the paired TV: the config page
  needs no pairing or PIN. Pairing is always accepted (Linux hosts require it), but key presses go only to the
  TV chosen during the pairing window, which anyone in range can open.
- Only record and replay remotes for devices you own or are allowed to control.

## License

Not chosen yet.
