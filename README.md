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
| Record: continuous capture, numbered drafts, press counter, batch naming, test-send of a draft | Working |
| Signals: saved in flash with name, type, device, active/disabled state | Working, survives reboot |
| Infrared transmit (RMT hardware carrier, self-reception guard) | Working: the author recorded signals and re-sent them successfully. Not every kind of appliance has been tried |
| Emitter ring (ULN2003 + 8 LEDs) | Built, all LEDs seen through a phone camera (`ledtest`). See [docs/wiring.html](docs/wiring.html) |
| Receiver artifact repair for A/C frames | Working |
| TV remote over Bluetooth | Keys confirmed by the author on one TCL Android TV (see below) |
| Relay rules: heard signal → send IR, wait, switch layer, press a TV key | Tested on the board with injected frames. A rule driving a real TV from a physical remote is **not yet confirmed** |
| Custom web remotes, backup / restore | Working; tested on the board and in the browser |
| Access header and encryption | Tested between the board and both a reference client and the web page (through a test bridge). **Not yet tried from a phone, Bluefy or an NFC tag** |
| Web page | Automated tests run in a headless browser; **the real Web Bluetooth and Web Serial device pickers are not covered by them** |

## TV remote over Bluetooth

For TV functions that have no infrared code, the capsule pairs with an Android / Google TV as a Bluetooth remote
(HID over GATT). Confirmed by the author on one TCL Android TV: directions, OK, Back, Home, Input source, TV
settings, volume, playback keys, YouTube, Netflix and Power (off). Rewind / fast forward did nothing there; other
keys are untested. Waking a TV from standby should not be relied on.

**About the Bluetooth ids:** the capsule always reports the vendor/product id of **Google's reference remote**
(`0957:0001`), because Android picks its key layout
([`Vendor_0957_Product_0001.kl`](https://android.googlesource.com/platform/frameworks/base/+/main/data/keyboards/Vendor_0957_Product_0001.kl))
by it. This project is not a Google product, is not certified by Google, and should not be shipped as a product
with these ids. It does not pass a TV's pairing screen for its own remote: use the general accessory list.

Pairing: page → **Device** → **Pair a TV**, then within 120 s add the capsule on the TV. Pair only after pressing
the button. The pairing survives reboots and a normal `idf.py flash`.

## Custom remotes and relay

* **Remotes** tab: up to 8 remotes stored on the capsule, 3 columns by up to 12 rows. Each button has a label,
  one of 32 colours, and an action: a TV key (short or long press), a saved IR signal, or "run a rule" (a rule
  without a trigger works as a macro). A button can repeat while held, at 150–1000 ms.
* **Relay rules**: a signal heard from a physical remote runs steps: send IR, wait, switch layer, **press a TV
  key**. One press runs a rule once; holding the button does not repeat it.

## Access header (recommended)

Anyone in Bluetooth range can use a capsule without one. Set it on the page (**Device** → **Bluetooth access**) or
over USB. With it, the page proves it knows the header and everything is encrypted (AES-GCM); the header is not
sent. The page shows a URL such as `https://…/#h=<header>&n=<bluetooth name>` for an **NFC tag**: opening it fills
in the header (the part after `#` is never sent to the web server; the browser may keep it in its history). Web
Bluetooth still needs a tap on **Bluetooth** and picking the device. USB never needs the header and can reset it.

What it does not do: whoever has the header or the tag can get in; a short or guessable header can be guessed
from recorded traffic (use the 22 random characters the page offers); a header that leaks later also opens old
recordings; setting it over a still-open Bluetooth link sends it unencrypted that one time; it does not stop
scanning, occupying connections, pairing or jamming. To be less obvious, give the capsule your own Bluetooth name;
the project's service id is not advertised, but the TV remote service has to be.

## Backup and restore

**Device** tab. **Export backup** saves signals (drafts included), waveforms, rules, devices, custom remotes
and the settings (name, repeat-ignore time, layer) as one JSON file. **Restore from backup replaces** what is on the capsule
with the file: it is validated first, your current data is downloaded, you are asked to confirm, and the result
is read back and compared. If the link drops in the middle, import the same file again. The TV pairing, the access
header and draft press counters are not part of a backup. A changed name applies after a reboot.

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

1. **Record** (*IR signals* tab): press *Start recording*, then press keys on any remote pointed at the receiver.
   Each new key becomes a numbered draft with a waveform; pressing the same key again raises its counter.
   *Test send* plays a draft right away.
2. **Name**: type names (or paste a list, one per line, applied in recording order), pick the type and device,
   press *Save named drafts*. Only named signals can be used in buttons and rules.
3. **Send**: from *IR signals*, or from your own buttons on the *Remotes* tab.
4. **Relay rules**: *When I hear* a saved signal (for example a key on a spare remote), *then do* a list of steps.
   Rules and signals can be disabled without deleting them.
5. **Device**: TV pairing, Bluetooth access header, backup, settings, LED test and the device log.

## Command line tools

For development. They need [pyserial](https://pypi.org/project/pyserial/); some also need `bleak`, `websockets`
or `cryptography`.

```sh
export IRCAP_PORT=/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_<MAC>-if00

python3 firmware/tools/selftest.py          # record, name, send, rules, persistence; keeps your data
python3 firmware/tools/relay_check.py       # "one press, one run" and TV-key rule steps
python3 firmware/tools/ui_check.py          # remote layout storage (needs two empty slots)
python3 firmware/tools/ledtest_check.py     # the LED test command
python3 firmware/tools/burst_check.py       # commands sent back to back
python3 firmware/tools/access_check.py      # access header and encryption (USB + this computer's Bluetooth)
python3 firmware/tools/remote_check.py      # TV remote commands. WARNING: forgets the paired TV
python3 firmware/tools/blecheck.py          # Bluetooth transfer (capsule without header, one saved signal)
python3 firmware/tools/bridge.py 8765       # USB <-> WebSocket for testing the web page
python3 firmware/tools/capture.py "$IRCAP_PORT" captures/tv.jsonl 60 tv   # log raw frames to a file
```

Most of these send infrared or change stored data temporarily; read the description at the top of a tool before
running it. A paired TV in range reconnects by itself, so anything that sends TV keys really controls it.
`securelink.py` is a reference client for the encrypted Bluetooth protocol.

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
  main/secure.c    access header: handshake and encryption under the line protocol
  main/remote.c    TV remote (BLE HID)
  main/ui_store.c  storage of the custom web remotes
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
- Free heap after boot is about 73 KB (Bluetooth stack included). Storage holds 96 signals and 48 rules of up to
  1200 entries each; flash is not the limit, RAM is.
- Without an access header, anyone in Bluetooth range can connect, change settings, press keys on the paired TV
  and open the pairing window. Pairing itself is always accepted (Linux hosts require it), but key presses go only
  to the TV chosen during the pairing window.
- Only record and replay remotes for devices you own or are allowed to control.

## License

[MIT](LICENSE).
