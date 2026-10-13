# Factorio trainer

A standalone Windows console trainer for **Factorio 2.1.14** (tested with Space Exploration 0.7.61),
single player only. It replaces a Cheat Engine table (referred to below as "the CT") so that items,
fluids and ore can be edited without attaching Cheat Engine, and without the `/c` console (which
disables achievements).

## Use

1. Start Factorio, load the save (back it up first).
2. Run `factorio-trainer.exe` (`--window` moves its console to `x y width height` read from
   `trainer-window.txt` next to the exe). It attaches to `factorio.exe`, reads `factorio.pdb`, loads item and
   fluid names from the game and registers the hotkeys.

| Command | What it does |
|---|---|
| `inv` | your character's main inventory, no clicking |
| `chest` | the chest, cargo wagon, car or spidertron whose window is open in game |
| `chest fill [item]` | fill the open chest's free slots (up to its limit bar) with its first item, or the named one |
| `chest clear [items]` | empty the open chest, or remove only the named items |
| `chest fill all [item]` / `chest clear all [items]` | the same for every chest of that kind in its block, in one game stop |
| `chest add` / `chest list` / `chest reset` | pick chests anywhere, one by one: open a chest's window, `chest add`; repeat |
| `chest balance [item]` / `chest balance all [item]` | even out an item (normal quality, no item data) over the picked chests / the open chest's block, in one game stop; a chest with less room gets what fits, nothing is created or lost |
| `train fill [names]` | with a wagon/locomotive window open: fill every cargo and fluid wagon (own item/fluid, or the named ones) |
| `train clear [names]` | empty every wagon of that train (everything, or only the named items/fluids); locomotive fuel stays |
| `inv click` | capture any other inventory by clicking an item in it (car, wagon, ...) |
| `set <slot> <count>` | set a stack count (simple items only; above a stack only in your own inventory) |
| `topup` | top up every simple stack of the listed inventory to its stack size |
| `give <item> <n>` | add n of an item as normal-quality stacks, never over the stack size |
| `clear <item>` | remove every stack of an item from the listed inventory |
| `swap <from> <to>` | replace all of one item with the same number of another (room checked first) |
| `find <text>` | search item and fluid internal names (`find circuit`); an item's id from `find` works wherever an item name does (`chest fill 777`) |
| `tank` | show the fluid network of the tank/pipe whose window is open (read-only) |
| `fill [fluid]` | fill that network; name a fluid when it is empty (`fill water`) |
| `empty` | empty it, like the flush button |
| `ore` | capture: hover an ore tile, or one oil well without a pumpjack (then the whole field is found) |
| `ore set <n>` / `ore default <n>` | set every tile of the captured patch / the amount used by Ctrl+Shift+F3 |
| `oil <tgt%>` | set every well of the captured oil field to that yield (100% = the prototype's `normalAmount`) |
| `names` | reload names (after a mod change) |

Hotkeys (work while the game has focus; high beep = done, low beep = nothing usable / refused):

| Key | Action |
|---|---|
| Ctrl+Shift+F1 | fill the tank or pipe under the cursor |
| Ctrl+Shift+F2 | top up every stack in your main inventory |
| Ctrl+Shift+F3 | set the ore patch under the cursor to `ore default` |
| Ctrl+Shift+F4 | `chest fill` on the open chest |

Ctrl+Shift, not Ctrl+Alt: pressing Alt toggles Factorio's alt-mode icons.

## Build

Visual Studio 18 (Community) with the C++ workload. From WSL or a Windows prompt:

```
build.bat          -> factorio-trainer.exe   (trainer.cpp, dbghelp.lib, user32.lib)
build-pdbq.bat     -> pdbq.exe               (pdbq.cpp, the PDB query tool)
```

It must build clean at `/W4`. A running trainer locks its exe: build to another name
(`sed 's/factorio-trainer.exe/trainer-new.exe/' build.bat > build-test.bat`) and swap it in once
the trainer is closed.

After a game update, first run the offline check:

```
factorio-trainer.exe --layout "C:\path\to\Factorio\bin\x64\factorio.exe"
```

`OK` means every symbol and struct member still resolves by name. Otherwise it lists exactly which
names Wube renamed.

## How it works

**Everything is looked up by name in `factorio.pdb`**, which Wube ships next to `factorio.exe`. The
PDB carries full class layouts with member names (`pdbq type Character` lists them). DbgHelp loads it
in a private session (`pdb::open`). `pdb::sym` resolves globals and functions, and `pdb::off("Type",
"a.b.c")` resolves dotted member paths, including through base classes. There are no hard-coded
addresses. The few hard-coded values are named in the code: MSVC `std::string` and deque layouts, the
`+0x66` click-hook instruction from the CT, and connection flag `0x8`. Everything else updates itself
when fields move.

**Reaching things without clicking:**
- **Main inventory:** `global -> GlobalContext.game -> Game.localPlayer -> Player.controllerManager.controller
  -> CharacterController.character -> Character.inventory`. It falls back to the physical, stashed and
  `controllerBeforePause` controllers when the active one is not your character (remote view, or a
  `PauseController` while the game is paused), accepting only a `CharacterController`.
- **Open window:** the back of `Controller.guiTarget.data` (an MSVC `std::deque` of `GuiTargetData`)
  -> `entity.target` (a union keyed by `openGuiType`; accepted only if it points at an object with a
  named vtable). For a chest that is `ContainerEntity.inventory`; for a cargo wagon, car or spidertron
  the embedded `inventory` / `trunkInventory` found by the entity's class. Either is accepted only if
  `Inventory.owner` points back at the entity. For a tank or pipe, the entity's class is read from its vtable symbol
  (`StorageTank::`vftable'`), then `<Class>.fluidBox.fluidSegment`.

**Captures** (`inv click`, `ore`, hotkey F1/F3) use a one-shot hardware execute breakpoint through
the Windows debug API on the same instructions the CT hooks: `CursorTransferLogic::cursorTransfer+0x66`
with RDI = slot, `FluidSegment::addToDescription` with RCX, `ResourceEntity::addToDescription` with
RCX. The breakpoint is set on every thread, the register is read, everything is cleared and the
debugger detaches (`DebugSetProcessKillOnExit(FALSE)`, so the game survives a trainer crash). Cheat
Engine must not be attached at the same time.

## Why it is built the way it is (the game's own rules)

Every write keeps the invariants the game asserts on. Each one was found by reading the game code
with `pdbq` and `objdump` (disassembly of the exact function, named by the PDB):

- **Inventory indices.** `Game::update` runs `ConsistencyScraper::step` when
  `OtherSettings.consistencyScraper` is on (it is in the author's config). It walks every chunk and entity
  into `Inventory::checkConsistency`, which recomputes `firstNonFullIndex` and `lastNonEmptyIndex` and
  stops the game on a mismatch. That is why the old CT edit "chest slot 10 -> max" crashed minutes
  later. `editInventory` therefore checks first that its model reproduces the cached indices (it
  refuses inventories with other slot limits), edits, and writes both indices back, computed exactly
  like `Inventory::computeFirstNonFullIndex`.
- **Which items `give` may create.** `ItemPrototype::shouldCreateItem` says a stack needs an `Item`
  object (per-item data) when a prototype virtual returns true or the item can spoil. The trainer
  reads that vtable slot out of `shouldCreateItem`'s own `call [rax+disp32]` and accepts only the
  shared `xor al,al; ret` stub. Rails, capsules, modules and plain items pass. Tools (repair packs),
  ammo, armor and spoilables are refused. New slots are written as one 8-byte store: count, id,
  quality = `normal`, cached stack size = min(stack, 255). Filtered slots and limit bars are respected.
  Inventories with their own stack or weight limits are refused.
- **Overstacking only in your own inventory.** `Inventory::canInsertCount` adds `stackSize - count`
  as unsigned 32-bit. A slot above its stack size wraps it and can make the game promise room that
  does not exist. A common habit (bump a player slot, then ctrl+click it into a chest at once) is safe
  because the game's own transfer splits it and recomputes, so `set` allows more than a stack only in
  the character's main inventory. `give`/`swap` always build normal stacks. `swap` is one edit and
  runs only when everything fits under `give`'s rules (bar, filters, the new item's stack size).
- **Fluids.** `FluidBuffer::checkConsistency`: amount <= volume; an empty buffer must have amount 0
  and temperature 0; with a fluid, the temperature must be inside the filter range.
  `FluidSegment::checkConsistency`: the segment filter must match its filtering boxes.
  `FluidBox::checkConsistency`: directly connected boxes that would not mix must share a segment.
  `fill <fluid>` refuses a different fluid already present, a filter for another fluid, and a
  neighbouring segment across a plain two-way connection. It writes `{id, temperature, amount}` in
  one 16-byte store, using the fluid's `defaultTemperature` clamped to the filter. `empty` writes
  all zeros, like the flush button.
- **Edits run with the game stopped, then the game re-syncs.** `editInventory` catches the update
  thread at `Controller::checkSortInventoryAfterUpdate` (end of tick, where the game sorts
  inventories itself) with the one-shot breakpoint. While the whole process is frozen in that debug
  event it re-checks the inventory, edits, and rewrites the indices, so no robot, inserter or the
  scraper sees a half-done edit. Then the thread is detoured through a small stub (`atEndOfTick`) that
  makes the call `Inventory::sort` ends with, `owner->inventorySorted(inventory)` (slot 0x18 of the
  owner's `PostTransferNotifiable` vtable, checked by symbol name), and jumps back. The game caches
  things derived from an inventory and updates them only by deltas from its own transfers (chest
  picture / alt-mode icon, waiting inserters, circuit signals, the logistic network's per-item
  totals); without the call a `give` into a storage chest was missing from the network total, and
  taking the items out by hand later drove it negative. For containers it is
  `ItemFrequencyCounter::setup`, inserter wake-up, `reevaluateSignalsToSend` and
  `LogisticPoint::reInitSupply` (a full recount). A paused game runs no tick: nothing is changed.
- **`train fill`.** From the open wagon or locomotive window the train is walked through
  `RollingStock.connectedToFront/Back` (a wagon can face either way, so each step takes the link that
  is not where it came from). Cargo wagons get `give`'s placement; fluid wagons get their `Fluid` set
  to `cached.fluidCapacity`, and the train's `schedulable.tickOfLastActivity` is stamped with
  `Map.entityTick`, which is all `FluidWagon::setFluid` does besides the write. Locomotives, and the fuel in them, are never touched (verified in game: `train clear rocket-fuel`
  emptied the wagons and left the locomotives' fuel). Every wagon is edited in
  the same end-of-tick stop, and their `inventorySorted` calls run from one stub.
- **`chest fill all` / `chest clear all` and `ore`.** From the open chest (or the hovered ore tile), a
  flood fill collects every entity of the same class and prototype whose centres are at most
  `w + max(1, w/2)` tiles apart (`EntityPrototype.tileGridSizeStored`; 6x6 warehouses: 9; ore: 1, oil
  wells: 12). It reads only the chunks the fill reaches: `Entity.surface` -> `Surface.chunks` ->
  `Chunk.position` and `Chunk.advancedTiles`, each tile's entity list (`AdvancedTile.entities` ->
  `EntityConnector.nextOnTile` -> `entity`). There is no memory scan: removed entities are unlinked
  from those lists even though their freed memory keeps the old bytes. Chests must also pass the owner
  check. All are edited in one end-of-tick stop.
- **Reads while the game is stopped.** During the end-of-tick stop nothing else writes the game's
  memory, so reads come from 4 KB page copies (one `ReadProcessMemory` per page, not per field) and
  writes go through to the game and the copy. That keeps the stop short for large blocks and trains.
- **Picked inventories are re-validated.** A reload or a mined chest frees the inventory and the heap
  reuses the memory, so `inv`/`chest`/`inv click` remember the inventory's and its owner's vtables
  and owner pointer, and every command checks them before touching it.
- **Achievements.** The only writers of `Map.consoleCommandUsed` / `Map.editorUsed` are
  `CommandProcessor::doExecuteCommand`, `CommandProcessor::cheat`, `CommandProcessor::editor`,
  `InputActionHandler::playerJoinGame` and `LuaPlayer::luaSetController`. Memory writes go through
  none of them.

## Known limits

- **Inserters can stay asleep.** Inserters waiting on a chest sleep on its `inputWakeUpList` /
  `outputWakeUpList`, and only the game's transfers wake them. After `chest fill`, drop or take one
  item by hand to wake them.
- **Ore map label lags.** The map's ore label updates after a re-chart or reload; tooltips and miners
  see the new amount at once. Ore 0 is refused (the game cannot clean up a zero tile written from
  outside; use 1).
- **Stale captures.** Captured addresses go stale: inventories after armor/research/sort, fluid
  segments after pipe changes. `inv` and `chest` re-read live every time.
- **Single player only.** Memory edits desync multiplayer.

## Files

- `trainer.cpp`: the trainer.
- `pdbq.cpp`: PDB query tool (set `FACTORIO_EXE` to the full path of `factorio.exe`). `pdbq type <Class>`, `pdbq sym <mask>`, `pdbq data <mask>`,
  `pdbq addr <rva>...` (the function containing an RVA; pair it with
  `objdump -d -M intel --start-address=...` on `factorio.exe` to read a function).
- `build.bat`, `build-pdbq.bat`.

## License

MIT, see `LICENSE`.
