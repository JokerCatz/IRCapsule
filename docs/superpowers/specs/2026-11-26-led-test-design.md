# LED test mode (approved)

**Why:** after soldering, check with a phone camera that all 8 IR LEDs light up.

**Goals:** start/stop from Bluetooth or USB and from the web page. Blink 0.5 s on, 0.5 s off.
Stop by itself after 60 s.
**Non-goals:** lighting single LEDs (all strings share GPIO5), measuring current or brightness.

**Approaches**
- *Firmware `ledtest` command (chosen):* works from any client, brighter (no carrier), low traffic.
- *Web page repeats `play`:* no firmware change, but dimmer (38 kHz, 1/3 duty), chatty, web only.

**Design**
- Command `ledtest` with `on` (0/1, default 1). Added to `docs/protocol.md`.
- No 38 kHz carrier. The off half and the auto stop keep average current and heat low.
- Frames heard while an LED pulse is on (plus the usual guard time) are ignored.
- Rules do not fire during the test (added after review: they would run late).
- Other sends wait until the test ends; `ledtest` off ends it at once.
- Web page: button on the Device tab, English and zh-TW, also works in demo mode.
