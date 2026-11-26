# Custom Remotes, TV Relay and Access Header Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Custom web remotes stored on the capsule, IR-to-TV relay, and optional encrypted Bluetooth access.

**Architecture:** The BLE layer gets a transparent record cipher below the line protocol, so the command
handlers do not change. Remotes are opaque blobs; the web page owns their format. TV keys become a rule step.

**Tech Stack:** ESP-IDF v6.1 (NimBLE, PSA crypto, mbedtls base64), single-file `web/index.html` (Web Crypto).

**Spec:** `docs/superpowers/specs/2026-11-26-custom-remotes-design.md`

## Global Constraints
- `sizeof(meta_t)` must not change. HID report map and GATT table must not change.
- 8 remote slots, 4096 bytes each; 3 columns, 1-12 rows; label 24 bytes, remote name 31 bytes.
- Header: 1-64 of `A-Z a-z 0-9 - _ . ~`. UART is plain and unrestricted.
- Docs and code English; web strings English and zh-TW. No MAC, home paths or email in committed files.
- Work on branch `custom-remotes`; push and publish only after the real-board tests pass.
- Tests that forget or replace the TV pairing need the NVS partition saved first and written back after.

## Wire protocol (decided here)
- Master key: `PBKDF2-HMAC-SHA256(header, salt, 20000, 32)`; `salt` = 16 random bytes made when the header is set;
  the capsule stores header, salt and master key.
- `{"cmd":"hello"}` (plain) -> `{"ok":true,"enc":0}` or `{"ok":true,"enc":1,"s":hex32,"i":20000,"n":hex32}`.
  `n` is fresh per link and per hello.
- Session keys: `HMAC-SHA256(master, label || n_client(16) || n_capsule(16))`, label `"c2d"` / `"d2c"`; AES-256-GCM.
- `{"q":q,"cmd":"auth","c":hex32,"p":b64}` (plain); `p` = record 0 of c2d with plaintext `ircapsule`.
  Success: the reply `{"q":q,"ok":true}\n` comes as record 0 of d2c. Failure: plain `{"ok":false,"err":"bad header"}`
  and the link is closed; for 8 s after a failure every `auth` gets `"err":"wait"`.
- After auth each wire line is a record `~` + base64(ciphertext || 16-byte tag). IV = 4 zero bytes + 8-byte
  big-endian per-direction counter (auth used 0, next is 1). A record's plaintext is a piece (at most 600 bytes) of
  the ordinary newline-delimited protocol stream. A record that fails to open closes the link.
- With a header set, any other plain line gets `{"q":q,"ok":false,"err":"locked"}`.
- `access` (accepted link or UART): no `h` -> `{"set":0|1,"h":"..."}`; with `h` sets, `""` clears. Error `bad header`.
  A change made over UART closes the web link; one made over BLE closes every other web link.
- `info` gains `"access":0|1`. `tv` loses `compat`.
- `ui_list` -> `{"slots":[{"slot":1,"size":n}],"max":8,"cap":4096}`; `ui_get{slot}` -> `{"slot","data":b64}`;
  `ui_put{slot,data}`; `ui_del{slot}`. Errors `bad slot`, `no such remote`, `too big`, `bad data`, `flash write failed`.
- Rule action type 3, value: bit15 keyboard page, bit14 long (1000 ms), bits0-9 code.

## Review Focus
- Old cached page with new firmware and no header: works as before.
- New page with old firmware: connects, TV keys and IR work, remotes and header say "update the firmware".
- A second phone with a wrong header while the first is connected: the first is not disturbed.
- Header changed or cleared while a page is connected: no stuck "connected" page; reconnect works.
- A large reply (`get` of 1200 entries, `list`) while frames arrive, encrypted: no counter desync.

---

### Task 1: Firmware: fixed ids, TV-key rule step, one press one run
**Files:** Modify `remote.c`, `remote.h`, `proto.c`, `engine.c`, `store.h`; Create `firmware/tools/relay_check.py`.
**Interfaces:** Produces `ACT_KEY = 3`; `remote_raw()` safe to call from two tasks.
- [ ] Test `relay_check.py` (USB, creates and deletes only its own signal and rules): `tv` has no `compat`;
  `op: compat` -> `unknown op`; `rule_put act=[3,0x01BB]`, `[3,0x4000|0x223]`, `[3,0x8000|0x50]` ok;
  `[3,0]`, `[3,0x8000|0x100]`, `[3,0x2000]` -> `bad tv key`; running a `[3,..,1,10,3,..]` rule with no TV logs
  `skipped` twice; two injected copies of an NEC trigger 100 ms apart fire twice; a non-NEC trigger injected every
  150 ms six times (400 ms window) fires once, and again after 600 ms of silence.
- [ ] Implement: PnP fixed to compatibility bytes; remove compat code; mutex in key press; `engine.c` keeps
  `s_last_match_us[]` updated on every match; protocol name starting `"NEC "` bypasses the gap.

### Task 2: Firmware: remote storage
**Files:** Create `ui_store.c/.h`; Modify `proto.c`, `CMakeLists.txt`; Create `firmware/tools/ui_check.py`.
**Interfaces:** `esp_err_t ui_put(int slot, const uint8_t*, size_t)`, `ui_get`, `ui_size`, `ui_del`; NVS `ircap`/`ui1..ui8`.
- [ ] Test `ui_check.py` (uses empty slots only, else aborts): put/get round-trip of 4096 random bytes; 4097
  bytes -> `too big` and old content kept; invalid base64 and empty -> `bad data`; slot 0 and 9 -> `bad slot`;
  `ui_list` shows sizes; survives reboot; `ui_del`; `get` of an empty slot -> `no such remote`.
- [ ] Implement.

### Task 3: Firmware: access header and encryption; less recognisable advertising
**Files:** Create `secure.c/.h`; Modify `ble.c`, `proto.c/.h`, `transport.c`, `sdkconfig.defaults` (host stack 6144),
`remote.c` (manufacturer `BLE Remote`); Create `firmware/tools/access_check.py` (UART + bleak + `cryptography`),
an independent client of the wire protocol above.
- [ ] Test: open mode works without hello and `hello` says `enc:0`; set header over UART; scan response has no
  config service UUID; unlocked-needed: plain `info` -> `locked`, no events while injecting frames; wrong header ->
  `bad header` and disconnect, immediate retry -> `wait`; right header: `info`, `list`, a 1200-entry `sig_put` and
  `get`, events during it, all encrypted; replayed record closes the link; altered record closes the link; a
  wrong-header second client cannot exist from one PC, so: unaccepted link is dropped after 30 s (25-40 s);
  header change over BLE keeps that link and `hello` on reconnect has a new salt; clearing over UART closes the
  link; clear at the end (restore the header found at start).
- [ ] Implement.

### Task 4: Web: encrypted link and access settings
**Files:** Modify `web/index.html`.
**Interfaces:** `class SecureChannel { static async open(link, header, hello) ; seal(text) ; async open(line) }`.
- [ ] Test (Playwright, a fake `navigator.bluetooth` implementing the capsule side of the wire protocol with its
  own code): URL `#h=..&n=..` is stored, removed from the address bar, used automatically; wrong header shows the
  input box and a message; right header connects and `list` works; all post-auth bytes on the fake link are `~`
  records; Device tab shows the NFC URL hidden until "show", copy works, generate gives 22 allowed characters,
  short header warns; reminder when none set; "forget on this browser"; old firmware (hello -> unknown cmd) works.
- [ ] Implement. Chooser filters by the remembered/URL name; "show all devices" uses `acceptAllDevices`.

### Task 5: Web: Remotes tab
**Files:** Modify `web/index.html` (MockLink gains `ui_*`).
- [ ] Test: default "TV" remote shown, nothing stored until Save; create, rename, copy, delete, reorder (max 8);
  rows 1-12; edit a cell: label, 32 colours + default, TV key / IR signal / rule, long, repeat interval; move
  swaps; press sends the right command; repeat while held sends several, spaced at least the interval, stops on
  release and never overlaps; missing/draft/disabled targets disabled with reason; TV keys disabled without TV;
  data over 24-byte label rejected; unknown `v` shown unreadable and not overwritten; unsaved-changes prompt;
  zh-TW; no sideways scroll at 360 px.
- [ ] Implement format v1 and the key catalogue of the spec.

### Task 6: Web: four tabs and TV-key rule steps
- [ ] Test: tabs Remotes, IR signals, Relay rules, Device (log inside Device); record/drafts/test send/saved
  signals still work; rule editor adds "Send TV key" step with the catalogue and long press, saved value matches
  the encoding; a rule without trigger is allowed; old "try a code / compatibility" cards gone.
- [ ] Implement.

### Task 7: Backup format 3, docs
- [ ] Test (`spike/testkit/restore_check.js` extended): export has `remotes`; restore rewrites signal and rule
  ids in remotes and read-back matches; dangling refs become 0; unreadable remote on either side refuses with
  nothing changed; v1/v2 keep remotes and the question says buttons will be unavailable; TV rule steps survive.
- [ ] Implement; update `docs/protocol.md`, `README.md` (Google ids always reported; header limits; NFC URL).

### Task 8: Real board
- [ ] With the board connected: save NVS; flash; confirm stored signals and rules unchanged; run Tasks 1-3 checks,
  PC-as-TV test including a rule sending a TV key, web through the bridge; write the NVS partition back; confirm.
- [ ] Final review by a fresh reviewer; one fix pass; merge to main, push, check the live page.
