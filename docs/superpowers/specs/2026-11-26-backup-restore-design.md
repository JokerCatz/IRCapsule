# Backup and restore (approved)

**Why:** there is a factory reset, but the existing backup was incomplete and "import" only appended.

**Goals:** a backup holds everything portable; restoring it replaces what is on the capsule.
**Non-goals:** the TV pairing (secret keys, tied to the board; restore leaves it alone), draft press counters
(the firmware cannot write them).

**Approaches:** web page only, using existing commands (chosen: no reflash; a dropped link can leave a partial
restore, fixed by importing the same file again) or an atomic restore in firmware (needs staging; not worth it).

**Design** (`web/index.html`)
- Export, format version 2: devices, all signals including drafts (with protocol label and waveform), rules,
  settings `dedupe`, `layer`, `name` (empty when the default name is in use).
- Import: validate the whole file; download the current data; ask to confirm with both counts; stop recording;
  delete rules, signals, devices; write devices, signals, rules, settings; read back and compare.
- Anything wrong before the confirm leaves the capsule untouched. A failure after it stays on screen.
- Version 1 files are accepted: no settings (current ones are kept), no drafts.

**Steps:** browser test first (demo mode), implement, then the same flow on the real board through the bridge.
