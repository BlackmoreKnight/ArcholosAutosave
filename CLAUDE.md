# CLAUDE.md — project guide for Claude Code

Context for working on **Archolos Autosave**, a Union (C++) engine plugin that adds
a rotating autosave to *The Chronicles of Myrtana: Archolos* (Gothic 2 NotR, on the
Union + GD3D11 engine build).

## Repo layout
- `src/plugin.cpp` — the entire plugin (single translation unit).
- `bin/ArcholosAutosave.dll` — prebuilt 32-bit DLL (commit a rebuilt copy when `src` changes).
- `README.md` — user-facing install/config + maintenance notes.
- `LICENSE` — GPL-3.0 (the plugin links the GPL Union SDK headers).

## Build
The plugin builds against the **Union SDK / Gothic API** (NOT vendored here):
1. `git clone https://github.com/Gratt-5r2/UnionProject`
2. Copy `src/plugin.cpp` over `UnionProject/UnionProject/Plugin/plugin.cpp`.
3. Build **`G2A Release | Win32`** (Gothic 2 NotR, 32-bit) with VS2022 Build Tools / MSVC v143:
   ```
   MSBuild UnionProject/UnionProject/UnionProject.vcxproj "/p:Configuration=G2A Release;Platform=Win32"
   ```
   Output `G2A Release/UnionProject.dll` → rename to `ArcholosAutosave.dll`.
- Add `#define _CRT_SECURE_NO_WARNINGS` at the top (warnings are errors; `fopen`/`localtime` are used).
- Verify the result is 32-bit (PE machine `0x014C`); Gothic2.exe is x86.

## Test loop (no unit tests — verify in-game)
1. Copy the DLL to `<game>\System\Autorun\` (Union auto-loads it).
2. Set `[AUTOSAVE_PLUGIN] IntervalMinutes=1` in `System\Gothic.ini` for fast iteration.
3. Launch, load a save, stand in a calm spot ~40s.
4. Read the diagnostic log at **`<game root>\ArcholosAutosave.log`** — NOT `System\` (the game
   chdir's to the root after startup, so relative `fopen` lands there).
5. Restore `IntervalMinutes=5` when done.

## Engine gotchas (hard-won — do not relearn the hard way)
- **Only `Game_Entry` is dispatched** on this build. `Game_Init`/`Game_Loop`/`Game_LoadEnd_*`
  never fire, so the plugin installs its own per-frame hook from `Game_Entry`.
- **Hook target = `zCWorld::AdvanceClock` @ 0x006260E0** (the simulation tick). GD3D11 owns the
  render path, so a hook on `oCGame::Render` never executes.
- **Use `UnionCore::Hook_Detours`** — `Hook_Auto` silently fails to patch on this build (verified
  by reading the target bytes: with Detours an `E9` jmp appears, with Auto it doesn't).
- **"In game" = global `player != null`**; `oCGame::m_bWorldEntered` reads 0 mid-play here.
- **Save = `gameMan->Write_Savegame(slot)`** (`CGameManager::Write_Savegame`, the G2 full
  save). Serializes the live world AND script/parser state, so quest stages/Daedalus globals
  are preserved, and it shows the game's own save progress bar. **Do NOT use
  `oCGame::WriteSavegame(slot,0)`** — that's the Gothic 1 Addon path; on G2 it writes only the
  world and leaves `SAVEDAT`/`SCRPTSAVE` copied from the stale `current\` snapshot, silently
  losing quest progress (verified: those files matched `current\` byte-for-byte).
  `SetAndWriteSavegame` writes metadata only — never use it for the world.
- **The save copies `SAVEINFO`+`THUMB` from the `Saves\current\` folder** (frozen at load),
  so a fresh save inherits the loaded game's name/area/time/thumbnail. There is NO in-memory
  "current info" object to set (verified: localInfo / infoList[0] / infoList[slot] are not it).
  Fix metadata by patching the ASCII `SAVEINFO.SAV` on disk after the save.
- **Live metadata sources** (`oCSavegameInfo::Init()` is empty on a bare instance):
  time = `ogame->GetWorldTimer()` `GetDay()/GetTime()`; play time = `gameMan->playTime`
  (`CGameManager*` @ 0x008C2958, field +0x90); area = `ogame->GetGameWorld()->GetWorldName()`
  (returns RAW `ARCHOLOS_MAINLAND`; title-case the part before `_` → `Archolos`).
- **Thumbnail**: `zrenderer->Vid_GetFrontBufferCopy` (works under GD3D11) → downscale to 256×256
  → RGB565 → write a ZTEX (`THUMB.SAV` = 36-byte ZTEX header, format 8 = R5G6B5, 256×256).
- **In-session menu refresh = `oCSavegameManager::Reinit()`** (rescans disk). `ReloadResources`
  refreshes only the thumbnail texture, not name/date/existence.
- **Progress bar**: `Write_Savegame` shows the game's own save progress bar, so no custom
  on-screen notice is needed. (Historical note: `oCGame::WriteSavegame` did NOT show it, which
  is why an earlier build drew a `screen->Print` "Autosaving…" notice with a ~250ms pre-save
  deferral; both were removed once the correct save call restored the native bar.)

## Why a C++ plugin (not a Ninja/Daedalus patch)
The script patches (`Autosave.vdf` + `Toolkit.vdf`) inject a second Ikarus/LeGo + vanilla save
hooks into Archolos' modified engine → duplicate symbols + wrong save routine → access violation.
This plugin touches none of the script layer.

## Conventions
- Match the existing style in `plugin.cpp` (2-space indent, brace-on-same-line, terse comments
  explaining *why*).
- Engine addresses/signatures come from the Gothic_II_Addon API in the Union SDK; keep the
  `0x...` literals commented with what they are.
- Keep the diagnostic logging low-volume; it's how in-game behavior is observed.
