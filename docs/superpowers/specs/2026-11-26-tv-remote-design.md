# BLE TV remote (requested: finish it all, test from a web page)

**Why:** the TV's own remote sends only Power as infrared; the other keys never reach the IR receiver.
The TV is in another room, so pairing and testing must work from the web page alone.

**Goals:** direction keys, OK, Back, volume, mute from the config page. **Non-goals:** voice, app shortcut
keys, waking from standby, rules that send TV keys (possible next step).

**Approaches:** ESP-IDF `esp_hid` (tracks a single connection, so the web page could take the TV's place) or an
own HID service in `remote.c` (chosen).

**Design**
- Up to 3 links. The web link is the one subscribed to the config TX characteristic.
- Pairing is always accepted. Refusing it was tried first: a Linux host then dropped the whole link.
- A peer becomes the TV only by a new pairing inside the 120 s window. Keys go only to it.
- Bonds are stored in NVS; when storage is full a peer other than the TV is dropped.
