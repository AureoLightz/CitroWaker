# Touch shortcuts

**Development status:** this describes the implemented configuration contract and intended panel behaviour. The latest shortcut panel still needs renderer integration and in-game validation. Editing this file alone does not make the buttons appear. This GitHub repository does not yet include the port source or a downloadable release.

The intended bottom buttons stay **Menu** (left) and **Map zoom** (right). Map zoom cycles the game's normal minimap show/zoom states. These two buttons cannot be replaced with item shortcuts.

The three right-hand item shortcuts have fixed positions. Change their actions in `/3ds/ww3ds/shortcuts.ini`, then restart the game:

```ini
slot1.action = X
slot2.action = Y
slot3.action = Z
```

X/Y/Z use the player's currently equipped item in that game slot. `NONE` leaves a quiet inactive well. Duplicate item assignments are supported. For example, swap the first two shortcuts and leave the third empty:

```ini
slot1.action = Y
slot2.action = X
slot3.action = NONE
```

Physical controller mappings stay in `buttons.ini`. Positions/sizes and footer reassignment are unsupported. The whole file is validated before applying it; a bad entry keeps defaults and writes one `[ui] shortcuts.ini rejected` line. The file is read once at startup, with no per-frame file access.

The parser, input and presentation helpers are host-tested. A minimap-routing backend alone does not make the redesigned shortcut buttons available.
