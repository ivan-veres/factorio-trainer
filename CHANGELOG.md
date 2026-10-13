# Changelog

All notable changes to this project are listed here. Versions follow [semantic versioning](https://semver.org).

## [0.2.0] - 2026-10-13

Fixes from a two-pass code review. Tested on a live game (Factorio 2.1.14, Space Exploration 0.7.61):
every command, except the Ctrl+Shift+F3 hotkey and the high beep of Ctrl+Shift+F4.

### Changed

- `fill`, `empty`, Ctrl+Shift+F1, `ore set`, `oil` and Ctrl+Shift+F3 now check and write inside the
  end-of-tick stop, like inventory edits. While the game is paused they report "Nothing changed: the
  game ran no tick (paused?)" instead of writing.
- Ctrl+Shift+F4 beeps high only when something went into the chest (a refused item or a full chest
  now beep low).
- `pdbq` exits 1 when a query shows nothing and 2 on an unknown command or a missing or too long
  `FACTORIO_EXE`.

### Fixed

- After a capture timed out, a thread created during cleanup could be left with the hardware
  breakpoint set after the debugger detached, which would crash the game when it ran that
  instruction.
- Fluid writes (`fill`, `empty`, F1) ran while the game's fluid system was updating the same segment,
  so a half-written fluid could break an invariant the game checks fatally.
- `ore set` / `oil` could write into a tile that a miner had just exhausted and the game had freed.
- `set`, item placement (`give`, `chest fill`, `train fill`, `swap`, `chest balance`) and fluid wagons
  used slot addresses, bar, filters and capacities read up to a tick before the edit; they are now
  read again inside the stop, and the inventory and owner are re-checked by vtable there.
- Fluid wagons topped up during the wait kept the temperature of the fluid in them.
- The re-sync stub could be freed two instructions before its last jump; it is now freed only once
  the thread that ran it is stopped outside it. Large block edits (over about 2,000 re-sync calls) no
  longer overflow it: it is sized for the edit before the game stops.
- Output printed while the game was stopped could block (text selected in the console window) and
  keep the game frozen; it is now held until the game resumes.
- `chest clear ,` (only commas) emptied the whole chest; it is now refused. Same for `train clear ,`.
- `oil` with an enormous target converted out of range; it now names the highest valid target.
- A write crossing a 4 KB page left the page cache stale.

## [0.1.0] - 2026-10-13

First public release.

- Items: `inv`, `chest` (chests, cargo wagons, cars, spidertrons), `give`, `clear`, `swap`, `set`,
  `topup`, `find`; item ids written `#123`; slots `22` and ranges `22-90`.
- Chests: `chest fill` / `chest clear` for one chest, its block (`all`) or chosen slots; `chest add` /
  `list` / `reset` and `chest balance` to even out an item over picked chests or a block.
- Trains: `train fill` / `train clear` for every cargo and fluid wagon of the open train.
- Fluids: `tank`, `fill <fluid>`, `empty`; Ore: `ore`, `ore set`, `ore default`, `oil <tgt%>`.
- Hotkeys Ctrl+Shift+F1 to F4.
- Every offset looked up by name in `factorio.pdb`; edits run with the game stopped at the end of a
  tick, then the game's own re-sync (logistic totals, chest icons, inserters, signals).

[0.2.0]: https://github.com/ivan-veres/factorio-trainer/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/ivan-veres/factorio-trainer/releases/tag/v0.1.0
