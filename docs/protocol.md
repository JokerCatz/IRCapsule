# Device protocol

The capsule speaks one protocol over two transports. Every message is a single line of JSON
terminated by `\n`.

| Transport | How |
|---|---|
| USB serial | The C3's USB-Serial/JTAG port, 115200 8N1 (the rate is nominal). Opening the port **reboots the board**; wait about 2 s, then ping until it answers. |
| Bluetooth LE | Service `9f1d0001-6b7a-4c5e-8a2b-1c4a9e5d7f10` (not advertised: find the device by name). Write lines to `…0002` (write, with or without response). Subscribe to `…0003` (notify) for replies and events. Lines are split into chunks of at most MTU−3 bytes; reassemble on `\n`. One web client at a time: the newest accepted subscriber takes over and the previous one is disconnected. With an access header set the link is encrypted (below). The same device also offers a BLE HID remote for a TV. |

## Access header over Bluetooth

Optional, off by default. With a header set, a new link gets nothing until it proves it knows the header: no
events, and every other line is answered with `locked`. The header itself is never sent. UART is not affected.

1. `{"q":1,"cmd":"hello"}` (plain) → `{"q":1,"ok":true,"enc":1,"s":<16-byte salt, hex>,"i":20000,"n":<16-byte nonce, hex>}`.
   `enc` 0 means the capsule is open: carry on in plain text. The salt changes when the header is set.
   A `hello` on a connection that was already accepted (a reloaded page reusing it) locks it again while a header is set.
2. Master key = `PBKDF2-HMAC-SHA256(header, salt, i, 32)`. Session keys =
   `HMAC-SHA256(master, label ‖ client nonce (16) ‖ capsule nonce (16))` with label `c2d` (to the capsule) and `d2c`.
   Cipher: AES-256-GCM, 16-byte tag, no additional data. IV = four zero bytes, then an 8-byte big-endian counter,
   one per direction, starting at 0.
3. `{"q":2,"cmd":"auth","c":<client nonce, hex>,"p":<base64 of record 0: "ircapsule">}` (plain).
   The answer is record 0 of `d2c` containing `{"q":2,"ok":true}\n`. Wrong: plain `bad header`, the link is
   closed, and for 8 s every `auth` gets `wait`. A `hello` allows one `auth`. A link not accepted within 30 s is closed.
4. From then on every wire line is `~` + base64(ciphertext ‖ tag). A record holds a piece (at most 600 bytes) of
   the ordinary newline-delimited stream, in either direction. A record that fails to open (altered, repeated,
   out of order) closes the link.

Limits: this keeps out those who only have the public page or recordings without the header. A weak header can be
guessed offline from a recorded handshake; whoever learns the header can also decrypt earlier recordings.
Setting a header over a still-open Bluetooth link sends it in the clear once. Advertising, connecting, pairing
and jamming are not prevented. The capsule-to-TV link is separate.

`firmware/tools/securelink.py` is a reference client.

## Requests and replies

```
→ {"q":12,"cmd":"get","id":3}
← {"q":12,"ok":true,"id":3,"name":"TV power","raw":[9000,4500,...]}
← {"q":12,"ok":false,"err":"no such signal"}
```

`q` is an integer chosen by the client and echoed in the reply. Commands are executed one at a time,
in order. A client must write a whole line before starting the next one (BLE chunks of two lines
must not interleave).

## Events (device → client, no `q`)

| Event | Fields |
|---|---|
| `frame` | `seq`, `t` (ms since boot), `gap` (ms since previous frame), `n`, `p` (protocol guess), `m` (rule id that fired, 0 if none), `sig` (stored signal this frame matches, 0 if none), `draft` (draft created or counted by this frame), `rec` (recording on). USB also includes `raw`; over BLE fetch it with `frame`. |
| `log` | `t`, `m` (text) |

## Objects

* **Signal**: `id`, `name`, `kind` (0 appliance command, 1 remote key; a UI hint), `st` (0 draft, 1 active, 2 disabled), `hits` (times a draft was heard), `dev` (device id, 0 none), `khz` (carrier), `n` (entries), `p` (protocol guess), raw timings in microseconds alternating mark, space, mark… Stored in flash, up to 1200 entries.
* **Device**: `id`, `name`. A label to group signals.
* **Rule**: `id`, `name`, `layer`, `on`, `trig` (signal id), `act`: a flat list of pairs `type,value` where type 0 = send signal `value`, 1 = wait `value` ms, 2 = switch to layer `value`, 3 = press a TV key: bit 15 = keyboard page (else consumer), bit 14 = long press (1 s), bits 0–9 = code. A TV key step is skipped (and logged) while no TV is connected.

A **draft** is a recorded signal that has no name yet. Drafts cannot be sent, used as a trigger or
used in a rule. Naming a draft and setting `state` to 1 makes it usable.

## Commands

| `cmd` | Arguments | Reply fields |
|---|---|---|
| `ping` | | |
| `info` | | `fw`, `name`, `heap`, `up`, `layer`, `rec`, `ns`, `nr`, `nd`, `drafts`, `access` (0/1), `dedupe`, `mtu`, `nvs{used,free,total}`, `max{…}` |
| `list` | | `layer`, `dev[]`, `sig[]` (no raw), `rule[]` |
| `get` | `id` | signal fields plus `raw` |
| `frame` | `seq` (0 = newest of the last 6) | `seq`, `p`, `n`, `raw` |
| `record` | `on` (0/1) | Recording: every new frame heard becomes a draft, repeats raise `hits`, rules do not fire. Ends after 5 min of silence. |
| `sig_put` | `id` (0 = new), `name`, `kind`, `state`, `dev`, `khz`, `proto`; and `raw` **or** `frame` (seq) | `id`. Without `id` and with a name the new signal is active, without a name it is a draft. |
| `sig_del` | `id` | Also removes it from rules. |
| `dev_put` / `dev_del` | `id`, `name` | `id` |
| `rule_put` | `id` (0 = new), `name`, `layer`, `on`, `trig`, `act` | `id`. Rejects drafts. |
| `rule_del` | `id` | |
| `fire` | `id` | Queue a send of an active signal. |
| `run` | `id` | Run a rule's steps now. |
| `play` | `khz`, `raw` | Send raw timings (debug). |
| `tv` | `op`: `status` (default), `pair`, `cancel`, `forget` | `paired`, `connected`, `window` (seconds left). `pair` opens a 120 s window: a peer that pairs anew during it becomes the TV. The capsule always reports the PnP ID of Google's reference remote (`0957:0001`). Logs start with `tv:`. |
| `ui_list` | | `slots[]` of `{slot,size}`, `max` 8, `cap` 4096. Custom web remotes: opaque bytes, the page owns the format. |
| `ui_get` / `ui_put` / `ui_del` | `slot` 1–8; `ui_put` also `data` (base64) | `ui_get` returns `data`. Errors: `bad slot`, `no such remote`, `too big`, `bad data`, `flash write failed`. A refused put keeps the old content; nothing is cut short. |
| `access` | optional `h`: set the access header, `""` clears | `set`, `h`. Header: 1–64 of `A-Z a-z 0-9 - _ . ~` (`bad header`). From UART it closes the web link; from Bluetooth it keeps the asking link and closes the others. |
| `hello` | | `enc` 0 (on an encrypted capsule the handshake below answers it). |
| `key` | `k` (a name), or `page` (`cc` consumer, `kb` keyboard) with `code`; optional `ms` hold time 1–3000 (default 40) | Press and release on the paired TV. Names: `up down left right ok back home input settings voldown mute volup prev rew playpause ffwd next stop menu search guide info chup chdown power youtube netflix`, alternative codes `ok2 back2 input2 settings2 menu2 up2 down2 left2 right2` (the codes are in `firmware/main/remote.c`). `code`: 1–0x3FF consumer, 1–0xFF keyboard. Every press is logged with its code. Errors: `unknown key`, `bad key code`, `bad hold time`, `TV not connected`, `send failed`. |
| `ledtest` | `on` (0/1, default 1) | Hardware check: all IR LEDs blink 0.5 s on, 0.5 s off, without carrier. Ends on `on`=0 or after 60 s (`on`=1 again restarts the 60 s). Other sends wait until it ends, and rules do not fire during it. Logs `led test on` / `led test off`. |
| `layer` | `n` | `layer` |
| `set` | `name` (BLE name, after reboot), `dedupe` (ms) | |
| `log` | | `log[]` of `{t,m}` |
| `inject` | `raw` | Test helper: behave as if the receiver just heard these timings. |
| `reboot` / `reset` | `reset` needs `confirm:1` | `reset` erases all stored data. |

## Matching

A heard frame is normalised (see below) and compared with stored signals entry by entry:
each entry may differ by 25 % or 150 µs, whichever is larger. If lengths differ the shorter must be
at least 20 entries and the common prefix must match, so a frame that repeats its payload three or
four times still matches.

**One press, one run:** a matching frame is a new press only after a quiet gap since the previous matching frame,
so holding a button never re-triggers. The gap is `dedupe` ms, except for NEC, where it is 150 ms (a held NEC
button normally sends short repeat frames, which are ignored; some remotes resend the whole frame every ~108 ms). Up to 8 runs wait in the queue, and
the receiver is deaf while the capsule itself is sending infrared.

**Normalisation:** on long frames a VS1838B shortens marks and lengthens the following spaces by the
same amount. When the marks in the last quarter average below 85 % of the first twenty, every mark is
rebuilt at the canonical length and the difference moved back into its space.
