# Archolos Autosave (Union plugin)

Engine-level autosave for **The Chronicles of Myrtana: Archolos** (Gothic 2 NotR / Union + GD3D11 build).

## Why this instead of the Ninja/Daedalus Autosave patch

The script patches (`Autosave.vdf` + `Toolkit.vdf`) inject a **second** copy of
Ikarus/LeGo plus **vanilla** save hooks into Archolos' already-modified engine.
Duplicate symbols + the wrong save routine cause an **access-violation crash**.

This plugin runs as native C++ inside the engine. It triggers a save by calling
the engine's own full-save routine (`CGameManager::Write_Savegame`) — the same
code path the in-game save menu uses — so Archolos' own save scripts run and the
save format stays correct. It touches **none** of the script layer, so the
duplicate-library crash cannot happen.

## Install

1. Copy `bin/ArcholosAutosave.dll` (or your own build) into:
   `...\TheChroniclesOfMyrtana\System\Autorun\`
   (Union auto-loads every DLL in `System\Autorun`. No ini edit needed.)
2. Add this block to `System\Gothic.ini` (optional — defaults are used if absent).
   The section is `[AUTOSAVE_PLUGIN]`, kept distinct from any leftover `[AUTOSAVE]`
   section (e.g. from the old Autosave Ninja patch) so they never collide:

   ```ini
   [AUTOSAVE_PLUGIN]
   Enabled=1
   IntervalMinutes=5
   SlotMin=12
   SlotMax=15
   OnlyWhenSafe=1
   GraceSeconds=30
   ; Counter and NextSlot are written automatically by the plugin.
   ```

   Autosaves rotate across slots `SlotMin..SlotMax` and are named
   `Autosave 1`, `Autosave 2`, … with an ever-incrementing counter, so the
   newest autosave always has the highest number. Each save regenerates its own
   metadata (name/world/time), so it never inherits the loaded game's name.

3. Launch the game. On start you'll see `[Autosave] plugin loaded.` in zSpy /
   the Union console.

## Settings

| Key | Default | Meaning |
|-----|---------|---------|
| `Enabled` | 1 | Master on/off. |
| `IntervalMinutes` | 5 | Minutes between autosaves (min 1). A manual/quick save resets this clock. |
| `SlotMin` | 12 | First slot in the rotation → `Saves\savegame12`. |
| `SlotMax` | 15 | Last slot in the rotation. Autosaves cycle SlotMin→SlotMax→SlotMin… |
| `OnlyWhenSafe` | 1 | Skip while dead/unconscious, weapon drawn (combat), jumping/climbing/swimming, in inventory/looting, mid-dialog. Re-tries each frame until it's safe. |
| `GraceSeconds` | 30 | Wait this long after a load before the first autosave. |
| `SettleSeconds` | 3 | After an unsafe state (dialogue/combat) clears, wait this long of continuous safe play before saving. Stops a save that was deferred by a dialogue from firing during the dialogue's exit transition — which on reload would resume the dialogue. |
| `Counter` | (auto) | Incrementing save-name number. Written by the plugin; survives restarts. |
| `NextSlot` | (auto) | Which slot the next autosave uses. Written by the plugin. |

## Status: complete

Each autosave (every `IntervalMinutes`, in a safe moment) writes a full save via
the engine's own `CGameManager::Write_Savegame` (the same call the in-game save
menu uses — so the game's normal save progress bar appears), then:
- rotates across slots `SlotMin..SlotMax` (12–15),
- names it `Autosave N` with an ever-incrementing counter + the real date,
- writes a **fresh thumbnail** of the current screen, and
- shows correctly and is **loadable in the same session** (no restart needed).

It never saves mid-dialogue or mid-combat — such saves are deferred, and once the
state clears it waits `SettleSeconds` of continuous safe play before saving so the
captured state has fully settled (otherwise a reload could resume the dialogue).

A low-volume log is written to **`<game root>\ArcholosAutosave.log`** (the game
changes its working dir to the root after startup, so it's there, NOT in
`System\`). It records `world entered`, each `saving slot N`, and throttled
`due but skipped: <reason>` notes — handy for confirming it runs.

### How each piece is done (maintenance notes)
- **Save:** `gameMan->Write_Savegame(slot)` (`CGameManager::Write_Savegame`, the
  G2 full save). It serializes the live world **and** script/parser state, so
  quest stages / Daedalus globals are preserved, and it shows the game's own save
  progress bar (no custom notice needed). ⚠️ Do **not** use
  `oCGame::WriteSavegame(slot,0)` here — that's the Gothic 1 Addon path; on G2 it
  writes only the world and leaves `SAVEDAT`/`SCRPTSAVE` copied from the stale
  `current\` snapshot, silently losing quest progress.
- **Name/area/time/playtime + thumbnail:** after the world save, the plugin uses
  the engine's own post-save path (the same one zUtilities uses) — no on-disk file
  patching. It calls `oCSavegameManager::GetSavegame(slot)` to get the info
  object, sets `m_Name` / `m_WorldName` / `m_TimeDay` / `m_TimeHour` / `m_TimeMin`
  / `m_PlayTimeSeconds`, captures a **fresh** thumbnail via
  `zrenderer->CreateTextureConvert()` + `Vid_GetFrontBufferCopy()` →
  `info->UpdateThumbPic(convert)` (then `delete` the convert — `UpdateThumbPic`
  shrinks it to 256×256, so reusing a static one overflows the next full-screen
  capture and crashes), then writes it all back with
  `mgr->SetAndWriteSavegame(slot, info)`. This also refreshes the in-session slot
  list, so the save is visible and loadable without a restart (no `Reinit` needed).
  Live values come from `oCWorldTimer` (time), `gameMan->playTime` (play time),
  and `GetGameWorld()->GetWorldName()` (area, title-cased before `_` to match the
  menu's display form).
- **Never save mid-dialogue/combat:** `IsSafeToSave()` gates on the dialogue
  manager's conversation partner and the player's weapon mode (plus body state,
  inventory, etc.). ⚠️ The dialogue check reads `oCInformationManager`'s instance
  + init-guard at their fixed addresses — it must **not** call
  `oCInformationManager::GetInformationManager()`, which is a lazy singleton whose
  first call constructs the manager (and its views). Calling that per-frame made
  our hook build the views before the renderer was ready, so every new dialogue
  box flashed at the screen origin for a frame.
- **Settle delay:** the unsafe state is tracked every frame; after it clears the
  save waits `SettleSeconds` of continuous safe play, so a save deferred by a
  dialogue doesn't fire in the exit transition (which would resume the dialogue on
  load).

## How it works (notes for future maintenance)

- This Union build only dispatches the `Game_Entry` callback (not `Game_Loop`/
  `Game_Init`), so the plugin installs its own per-frame detour from `Game_Entry`.
- The detour targets **`zCWorld::AdvanceClock` @ 0x006260E0** (the simulation
  tick). The render path (`oCGame::Render`) is owned by GD3D11 and a hook there
  never fires.
- Must use **`Hook_Detours`** — `Hook_Auto` silently fails to patch on this build.
- "In game" is gated on the global `player` pointer (`oCGame::m_bWorldEntered`
  reads 0 mid-play here). Dialogue is detected from `oCInformationManager`'s
  conversation partner, read at its fixed address **without** calling
  `GetInformationManager()` (that lazy singleton's first call constructs the
  manager's views — doing so from the hook glitched dialogue-box positioning).

## Build from source

The plugin (`src/plugin.cpp`) builds against the **Union SDK / Gothic API**, which
is a separate dependency (not vendored here):

1. Clone the SDK template:
   `git clone https://github.com/Gratt-5r2/UnionProject`
2. Copy `src/plugin.cpp` over `UnionProject/UnionProject/Plugin/plugin.cpp`.
3. Build the **`G2A Release | Win32`** configuration (Gothic 2 NotR, 32-bit):
   ```
   MSBuild UnionProject/UnionProject/UnionProject.vcxproj "/p:Configuration=G2A Release;Platform=Win32"
   ```
   Toolset: VS2022 Build Tools, MSVC v143, Windows 10 SDK, **x86**.
4. Output `G2A Release/UnionProject.dll` → rename to `ArcholosAutosave.dll`.

## License & credits

Licensed under **GPL-3.0** (see [LICENSE](LICENSE)) because it is built against the
GPL-licensed Union SDK / Gothic API by **Gratt-5r2**
(<https://github.com/Gratt-5r2/UnionProject>). Thanks to the Union team and the
Gothic modding community.
