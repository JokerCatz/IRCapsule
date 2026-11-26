# Device protocol

The capsule speaks one protocol over two transports. Every message is a single line of JSON
terminated by `\n`.

| Transport | How |
|---|---|
| USB serial | The C3's USB-Serial/JTAG port, 115200 8N1 (the rate is nominal). Opening the port **reboots the board**; wait about 2 s, then ping until it answers. |
| Bluetooth LE | Service `9f1d0001-6b7a-4c5e-8a2b-1c4a9e5d7f10`. Write lines to `…0002` (write, with or without response). Subscribe to `…0003` (notify) for replies and events; writes are accepted only from the subscribed link, and the latest subscriber wins. Lines are split into chunks of at most MTU−3 bytes; reassemble on `\n`. The config service needs no pairing. The same device also offers a BLE HID remote for a TV (see below), so some hosts, Linux among them, pair when they connect. |

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
* **Rule**: `id`, `name`, `layer`, `on`, `trig` (signal id), `act`: a flat list of pairs `type,value` where type 0 = send signal `value`, 1 = wait `value` ms, 2 = switch to layer `value`.

A **draft** is a recorded signal that has no name yet. Drafts cannot be sent, used as a trigger or
used in a rule. Naming a draft and setting `state` to 1 makes it usable.

## Commands

| `cmd` | Arguments | Reply fields |
|---|---|---|
| `ping` | | |
| `info` | | `fw`, `name`, `heap`, `up`, `layer`, `rec`, `ns`, `nr`, `nd`, `drafts`, `dedupe`, `mtu`, `nvs{used,free,total}`, `max{…}` |
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
| `tv` | `op`: `status` (default), `pair`, `cancel`, `forget` | `paired`, `connected`, `window` (seconds left). `pair` opens a 120 s window: a peer that pairs anew during it becomes the TV. Logs start with `tv:`. |
| `key` | `k`: `up` `down` `left` `right` `ok` `back` `volup` `voldown` `mute`; `ok2` / `back2` are alternative codes | Press and release on the paired TV. Errors: `unknown key`, `TV not connected`, `send failed`. `ok` = keyboard Enter, `ok2` = consumer Menu Pick, `back` = consumer AC Back, `back2` = keyboard Escape. |
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
four times still matches. A rule fires at most once per `dedupe` ms.

**Normalisation:** on long frames a VS1838B shortens marks and lengthens the following spaces by the
same amount. When the marks in the last quarter average below 85 % of the first twenty, every mark is
rebuilt at the canonical length and the difference moved back into its space.
