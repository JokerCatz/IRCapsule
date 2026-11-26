# Custom remotes and IR-to-TV relay (design, awaiting approval)

## Intent
Two needs, one capsule. (1) Control an Android / Google TV over Bluetooth. (2) Record, send and relay
infrared. A press can come from the web page or from a physical IR remote heard by the capsule; it can end
in an IR signal or a TV key. The phone with the web page is the fallback for everything.

Confirmed by the user on their TV: with the ids of Google's reference remote the whole basic remote, Input
source, YouTube, Netflix and Bluetooth power-off work; rewind / fast forward do nothing. IR recording and
re-sending work. Not known: waking the TV from standby.

## Goals
- The capsule always reports the Google-compatible ids (the mode switch goes away).
- Rules can send TV keys.
- Up to 8 custom web remotes stored on the capsule: 3 columns, 1 to 12 rows, per button a label, a colour and
  an action; add, rename, copy, delete, reorder; remap any button.
- A reorganised web page; backups include the remotes.

## Non-goals
Linking web remotes to the physical-remote mapping (decided: separate). Direct "HDMI n" keys. Hold-to-repeat
from a physical remote. Icons, other column counts, voice, app shortcuts beyond what the code list has.
Waking the TV from standby.

## Anti-goals
- An update that wipes or silently changes stored signals or rules, or forces the TV to pair again.
- A button or rule that ends up pointing at a different signal than the one chosen.

## Constraints
- `store_init` resets all data when `sizeof(meta_t)` differs: `meta_t` must not change.
- Command lines are limited to 12288 bytes; `js_str` truncates silently.
- Free heap is about 84 KB: remotes are not kept in RAM.
- The HID report map and GATT table stay as they are (changing them may need re-pairing).
- Public repository: English docs, no personal data. The README must say plainly that the capsule always
  reports the vendor/product ids of Google's reference remote, is not a Google or certified product, and
  should not be shipped as a product like this.

## Approaches
- **A, chosen by the user:** the firmware stores one opaque blob per remote; the web page owns the format.
  New layout features usually need no reflash. The web page validates and handles references.
- B: the firmware understands the layout and cleans references itself. More firmware, a reflash per layout
  feature.

## Firmware
**Ids.** PnP ID is always `0957:0001`. `tv` loses `op: compat` and the `compat` field; the old stored flag is
ignored. A capsule already paired in compatibility mode should stay paired.

**Remote storage.** Slots 1 to 8, NVS namespace `ircap`, keys `ui1`..`ui8`, so factory reset erases them.
At most 4096 bytes per slot, sent as base64 so the bytes pass the line parser unchanged.
- `ui_list` -> `{"slots":[{"slot":1,"size":812},...],"max":8,"cap":4096}` (used slots only)
- `ui_get {slot}` -> `{slot, data}`
- `ui_put {slot, data}` stores or replaces. Errors: `bad slot`, `too big`, `bad data`, `flash write failed`.
  Longer or invalid input is refused, never truncated; a refused put leaves the old content.
- `ui_del {slot}`

**TV key as a rule step.** New action type 3. The 16-bit value: bit 15 = keyboard page (else consumer),
bit 14 = long press (1 s), bits 0-9 = code (keyboard 1-255, consumer 1-1023). `rule_put` validates it.
If the TV is not connected when the step runs, it is logged and skipped and the following steps still run.
The 8-step limit stays. `key` stays as it is, so the page sends keys by page and code.

**One press, one run (decided by the user).** Holding a physical button runs its rule once; pressing it N
times runs it N times. How a press is told from a hold (builder's call):
- NEC: held buttons normally send short repeat frames, already dropped; full frames count as separate presses
  after a 150 ms quiet gap, so a remote that resends them while held does not re-trigger.
- Other formats: a matching frame is a new press only after a quiet gap of at least the ignore-repeats time
  (default 400 ms, adjustable) since the previous matching frame, dropped ones included. So holding never
  re-triggers; two presses closer than the gap count as one.
Limits to document: up to 8 runs wait in the queue; the receiver is deaf while the capsule itself sends IR.
Check with the user's real remotes, including quick repeated presses.

## Web page
Four tabs: **Remotes** (opens first), **IR signals** (record, live list, drafts, saved), **Relay rules**,
**Device** (TV pairing, backup, settings, LED test, log, danger zone). The old "Try a code", compatibility
and "second way" cards go.

**Remotes tab**
```
Remote: [ TV ▾ ]                    [Edit]
┌ Power ─┬ Input ─┬ Home ──┐
├────────┼── Up ──┼────────┤
├ Left ──┼── OK ──┼ Right ─┤
└ Back ──┴─ Down ─┴ AC on ─┘
```
- A button's action: TV key (from the list, or a typed code), short or long; a saved active IR signal; or
  "run a rule" (this is the macro; a rule without a trigger is a macro only the web page runs). Or empty.
- Repeat while held (web only, decided by the user): per button, off by default; if on, holding the button
  sends the action again and again at an adjustable interval (150-1000 ms, default 300). The next send waits
  for the previous reply. Stops on release, on leaving the page or losing the link, and after 20 s at most.
  For TV keys, IR signals and rules alike.
- Colour (decided by the user): no free colour picker. 32 fixed colours in two sets of 16, **dark** (deep
  shade, white text) and **light** (pale shade, black text), over the same hues: red, vermilion, orange,
  amber, yellow, lime, green, teal, cyan, sky, blue, indigo, purple, magenta, brown, gray. Plus "default"
  (no colour). A button looks the same in the page's light and dark theme.
- Edit mode: tap a cell to edit it; "move" then tap a target to swap; add or remove the last row (asks if it
  has buttons); add, rename, copy, delete and reorder remotes. Nothing is written until **Save**; leaving
  with unsaved changes asks.
- A target that was deleted, is a draft or is disabled shows as unavailable with the reason; pressing it
  sends nothing. TV keys are unavailable while no TV is connected; IR buttons still work.
- With no stored remote, a default "TV" remote with keys that work on the tested TV is shown; it is stored
  only on Save.
- The last selected remote is remembered by the browser.

**TV key list** (lives in the web page; the user wants it generous, others may need keys not tried here, and
buttons can be renamed). Based on the layout of Google's remote: the keys of the firmware table, except the alternative codes, plus
the rest of Google's layout: All apps 0x1A2, Notifications 0x9F, Captions 0x61, colour keys 0x69-0x6C,
digits (keyboard 0x1E-0x27). Info becomes 0x1BD (today's 0x60 is not in that layout). Keys not confirmed on a
real TV are marked "untested". Confirmed (the user agreed to this list): arrows, OK,
Back, Home, Input source, TV settings, volume, mute, Play/Pause, Previous, Next, Stop, YouTube, Netflix and
Bluetooth power-off. Rewind / fast forward (skip codes 0xB4 / 0xB3) are marked "did nothing on the
tested TV". The same list serves buttons and rule steps.

**Format v1** (what is stored, UTF-8 JSON, owned by the page):
`{"v":1,"n":"TV","r":4,"c":[{"p":0,"l":"Home","t":"k","k":547,"h":0,"col":22}]}`
`p` position 0..35 (row*3+column), `l` label up to 24 bytes, `t` `k` TV key / `s` signal / `r` rule, `k` the
16-bit key value as in rules without the long bit, or the signal / rule id, `h` long press, `rp` optional repeat interval in ms (absent = no repeat), `col` optional palette index (0-15 dark, 16-31 light, same hue order; absent = default).
`n` up to 31 bytes. Remote order is slot order (reordering rewrites slots). A newer `v` or unreadable data is
shown as such and never overwritten without asking.

**References.** Signal and rule ids come from one counter that only grows, so a deleted id does not come
back in normal use. Factory reset erases the remotes with everything else.

## Access header over Bluetooth: it keys the encryption (the user's choice)
**Why:** keep out neighbours within radio range; people in the room are not the concern. The user types the
header or puts it in a URL (NFC tag), which keeps working until the header is changed.
**Accepted risk (the user's decision):** setting the header may be done over Bluetooth in the clear; someone
listening at that very moment learns it. UART avoids that for whoever wants to. State it in the README.

- Optional. 1-64 characters `A-Z a-z 0-9 - _ . ~`. Not set = plain text and open, as today; the page keeps
  reminding, and offers to generate 22 random characters. Shorter than 16 shows a "can be guessed" warning.
- With a header, it is never sent over the air again. Per connection both sides exchange fresh random
  numbers and derive session keys from header + both numbers (slow derivation, so guessing costs more).
  AES-GCM in both directions with a per-direction message counter: replies, logs and heard frames included.
  Old or altered messages are rejected. The page proves itself with its first encrypted message and the
  capsule answers with one; until then nothing else is accepted or sent to that link.
- Use the platform's crypto (Web Crypto, the ESP-IDF crypto library), no home-made cipher.
- **Any number of phones** can use the same header; nothing about a phone is stored on the capsule. One web
  page is connected at a time, as today: a newly **accepted** link takes over and the old one is disconnected.
  An unaccepted link never pushes out an accepted one; it is dropped after 30 s, wrong tries are slowed down.
- **UART is never restricted and stays plain text.** It can read, set and clear the header.
- It can be set or changed from an accepted BLE link (encrypted then), from an open one (plain, see above) or
  from UART. While none is set anyone in range could set one; UART gets the owner back in.
- Factory reset clears it. It is not in a backup, and restore leaves it alone.
- TV pairing and TV keys work as before; opening the pairing window needs an accepted link.
- Page: `#h=<header>&n=<Bluetooth name>` (fragment, not sent to the web server). The page stores both in the
  browser, removes them from the address bar and connects with them. Missing or wrong header: an input box.
  "Forget on this browser" button. **The Device tab shows the NFC URL** (header hidden until "show" is
  pressed) with a copy button. Web Bluetooth still needs a tap on Connect and picking the device.
- Not covered: anyone holding the header or the tag; recorded traffic if the header leaks later; guessing a
  weak header from recorded traffic; advertising, occupying links, pairing, jamming; the capsule-to-TV link.

**Harder to recognise** (lowers attention, is not access control):
- The Bluetooth name is user-settable (exists today). The page filters the chooser by the name from the URL or
  the remembered one, and offers "show all devices".
- The config service UUID is no longer advertised; the manufacturer string no longer names the project.
- The HID service, appearance, Google-compatible ids and the fixed address stay (needed for the TV).

## Backup (format 3)
Adds `remotes`. Validated like the rest before anything is deleted (count, size, references that are not
already dangling in the file). Restore rewrites signal and rule ids inside the remotes, reads them back and
compares. Already-dangling references stay dangling and must not collide with a newly assigned id (store
them as 0). Version 1 and 2 files have no remotes: current remotes are kept, but since restore gives signals
and rules new ids their IR and rule buttons will show as unavailable; the confirm dialog says so.

**Remotes the page cannot read** (newer `v` or damaged). Export includes their bytes as they are. A restore is
refused, with nothing changed, while the capsule or the file holds one, because its references cannot be
rewritten and could end up on the wrong signal. The message says to use a newer page, or to delete that remote
in the Remotes tab first (for a file: to restore with that newer page).

## Compatibility
New page with old firmware: TV keys work; remote storage and TV rule steps say the firmware must be
updated. Existing signals, rules and the TV pairing are kept by a normal flash.

## Testing
Browser tests (demo mode and through the USB bridge) for editor, storage, rules and backup; USB tests for
the new commands including over-size and invalid input; the PC as TV for TV keys from rules. That last one
replaces the user's TV pairing, so the whole NVS partition is saved first and written back afterwards.
Whether the real TV then reconnects without pairing again is only known when the user checks.

## Phases
1. Firmware (one flash). 2. Web page and backup.

## Left to the builder
Exact colour values of the 32 swatches (text must stay readable), dialog layout, wording, default remote arrangement, internal code structure.
