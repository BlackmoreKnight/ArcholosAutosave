#define _CRT_SECURE_NO_WARNINGS   // allow fopen/localtime for the diagnostic log
// Archolos engine-level Autosave  -  Union plugin
// ------------------------------------------------------------------
// Why a Union (C++) plugin instead of a Ninja/Daedalus script patch:
//   The script-based Autosave/Toolkit patches inject a SECOND copy of
//   Ikarus/LeGo plus VANILLA save hooks into Archolos' already-modified
//   engine -> duplicate symbols + wrong save routine -> access violation.
//   This plugin never touches the script layer.  It triggers a save by
//   calling the engine's own oCGame::WriteSavegame(), i.e. the exact same
//   code path the in-game save menu uses, so Archolos' custom save
//   scripts run normally and the save format stays correct.
// ------------------------------------------------------------------
#include "plugin.h"
#include "UnionAfx.h"

#include <cstdio>
#include <ctime>
#include <string>

using namespace Gothic_II_Addon;
using namespace Common;

// Diagnostic logger -> System\ArcholosAutosave.log (CWD is the game's System dir).
// Independent of zSpy/Union console so we get on-disk evidence of what ran.
void PluginLog( const char* msg ) {
  FILE* f = std::fopen( "ArcholosAutosave.log", "a" );
  if ( !f )
    return;
  std::time_t t = std::time( 0 );
  std::tm* lt = std::localtime( &t );
  char ts[32] = "";
  if ( lt )
    std::strftime( ts, sizeof( ts ), "%H:%M:%S", lt );
  std::fprintf( f, "[%s] %s\n", ts, msg );
  std::fclose( f );
}

// ----------------------------- configuration (Gothic.ini [AUTOSAVE_PLUGIN]) -
static bool g_enabled      = true;
static int  g_intervalMs   = 5 * 60 * 1000;  // time between autosaves (5 min)
static int  g_slotMin      = 12;             // rotate autosaves across slots 12..15
static int  g_slotMax      = 15;
static bool g_onlyWhenSafe = true;           // skip while in combat / busy
static int  g_graceMs      = 30 * 1000;      // delay after a load before first autosave
static int  g_settleMs     = 3 * 1000;       // wait this long after an unsafe state clears

// ----------------------------- runtime state -------------------------------
static unsigned int g_lastSaveTime   = 0;    // ms timestamp of last (auto or manual) save
static bool         g_saving         = false;// a save is in progress
static int          g_nextSlot       = 12;   // next slot in the rotation (persisted)
static int          g_counter        = 0;    // incrementing save-name number (persisted)
static unsigned int g_lastUnsafeTime = 0;    // last frame we were in an unsafe state

static const char* g_skipReason = "";

static void Log( const char* msg ) {
  cmd.Print( msg );
  endl( cmd );
  PluginLog( msg );
}

static void LoadConfig() {
  if ( !zoptions )
    return;

  zSTRING sec = "AUTOSAVE_PLUGIN";   // distinct section; NOT the defunct Ninja [AUTOSAVE]
  g_enabled      = zoptions->ReadBool( sec, "Enabled", 1 ) != 0;
  int mins       = zoptions->ReadInt ( sec, "IntervalMinutes", 5 );
  g_slotMin      = zoptions->ReadInt ( sec, "SlotMin", 12 );
  g_slotMax      = zoptions->ReadInt ( sec, "SlotMax", 15 );
  g_onlyWhenSafe = zoptions->ReadBool( sec, "OnlyWhenSafe", 1 ) != 0;
  int grace      = zoptions->ReadInt ( sec, "GraceSeconds", 30 );
  int settle     = zoptions->ReadInt ( sec, "SettleSeconds", 3 );
  g_counter      = zoptions->ReadInt ( sec, "Counter", 0 );
  g_nextSlot     = zoptions->ReadInt ( sec, "NextSlot", g_slotMin );

  if ( mins < 1 ) mins = 1;                                  // never hammer the disk
  if ( grace < 0 ) grace = 0;
  if ( settle < 0 ) settle = 0;
  if ( g_slotMin < 1 ) g_slotMin = 12;                       // slot 0 == "current"; never use it
  if ( g_slotMax < g_slotMin ) g_slotMax = g_slotMin;
  if ( g_nextSlot < g_slotMin || g_nextSlot > g_slotMax ) g_nextSlot = g_slotMin;

  g_intervalMs = mins * 60 * 1000;
  g_graceMs    = grace * 1000;
  g_settleMs   = settle * 1000;

  char buf[224];
  std::snprintf( buf, sizeof( buf ),
                 "[Autosave] config: enabled=%d interval=%dmin slots=%d-%d next=%d counter=%d onlyWhenSafe=%d grace=%ds settle=%ds",
                 (int)g_enabled, mins, g_slotMin, g_slotMax, g_nextSlot, g_counter, (int)g_onlyWhenSafe, grace, settle );
  Log( buf );
}

// The engine disables the in-game "Save game" menu option whenever saving isn't
// allowed (dialogue, cutscenes, etc.). Checking that flag is the most reliable
// "can I save right now?" gate for dialogue - the same one zUtilities uses, and
// far more robust than watching the conversation view.
static bool GameBlocksSaving() {
  if ( !gameMan || !gameMan->menu_save_savegame )
    return false;
  zSTRING saveMenu = gameMan->menu_save_savegame->name;
  for ( int i = 0; i < zCMenuItem::itemList.GetNum(); ++i ) {
    zCMenuItem* item = zCMenuItem::itemList[i];
    if ( item && item->m_parOnSelAction_S[0] == saveMenu )
      return ( item->m_parItemFlags & ( IT_DISABLED | IT_ONLY_OUT_GAME ) ) != 0;
  }
  return false;
}

// True while a dialogue is active. We must NOT call
// oCInformationManager::GetInformationManager() to find this out: it is a lazy
// (function-static) singleton whose FIRST call runs the constructor, which builds
// the dialogue views. Calling it every frame meant our hook constructed the
// manager on the first in-world frame - before the engine/renderer was ready - so
// the views got bad cached geometry and every new dialogue box flashed at the
// screen origin for a frame. Instead we read the singleton's init guard and
// instance at their fixed addresses, so we only inspect the manager once the
// ENGINE has constructed it, and never trigger construction ourselves.
//   0x00AAC4C4 = function-static init guard (bit 0 set once constructed)
//   0x00AAC458 = the oCInformationManager instance
// (both decoded from GetInformationManager @ 0x0065F790 in this Gothic2.exe.)
static bool InActiveDialogue() {
  if ( ( *(volatile unsigned char*)0x00AAC4C4 & 1 ) == 0 )
    return false;  // manager not constructed yet -> no dialogue has ever started
  return ( (oCInformationManager*)0x00AAC458 )->Npc != 0;
}

// Mirrors the engine's own "can the player save right now?" intent: not mid
// load/level-change, not in dialogue (game-gated), alive, weapon holstered, not
// in a busy body state. When it returns false the caller skips WITHOUT resetting
// the interval timer, so the save (and the next interval) is deferred until the
// player is back in normal play.
static bool IsSafeToSave() {
  if ( !ogame || !player ) {
    g_skipReason = "no ogame/player";
    return false;
  }
  if ( ogame->inLoadSaveGame || ogame->inLevelChange ) {
    g_skipReason = "loading/level-change";
    return false;
  }
  // Cutscene / any state where the game itself disables the save option.
  if ( GameBlocksSaving() ) {
    g_skipReason = "save disabled by game";
    return false;
  }
  // Active dialogue: the information manager's conversation partner is set for
  // the whole conversation. This is the reliable dialogue gate (the menu flag
  // and conversation-view checks missed some dialogues). Always enforced -
  // saving mid-dialogue also crashes the front-buffer capture.
  if ( InActiveDialogue() ) {
    g_skipReason = "in dialogue";
    return false;
  }
  if ( !g_onlyWhenSafe )
    return true;

  if ( player->IsDead() || player->IsUnconscious() ) {
    g_skipReason = "dead/unconscious";
    return false;
  }
  if ( player->GetWeaponMode() != 0 ) {    // any weapon drawn -> in/near combat
    g_skipReason = "weapon drawn";
    return false;
  }

  int bs = player->GetBodyState() & 0x7F;  // strip BS_MOD_* modifier bits (>=128)
  switch ( bs ) {
    case BS_JUMP:
    case BS_CLIMB:
    case BS_FALL:
    case BS_SWIM:
    case BS_DIVE:
    case BS_INVENTORY:
    case BS_ITEMINTERACT:
    case BS_MOBINTERACT:
    case BS_MOBINTERACT_INTERRUPT:
    case BS_STUMBLE:
    case BS_UNCONSCIOUS:
    case BS_DEAD:
    case BS_AIMNEAR:
    case BS_AIMFAR:
    case BS_HIT:
      g_skipReason = "busy body-state";
      return false;
    default:
      break;
  }

  // In a *visible* conversation / choice view -> active dialog, don't save.
  // (array_view_enabled is true whenever the view merely exists; array_view_visible
  //  is what indicates a dialog is actually on screen.)
  if ( ogame->array_view_visible[oCGame::GAME_VIEW_CONVERSATION] ||
       ogame->array_view_visible[oCGame::GAME_VIEW_CHOICE] ) {
    g_skipReason = "in dialog";
    return false;
  }

  g_skipReason = "";
  return true;
}

// Title-case the raw world name to the menu's display form, writing into `out`:
// "ARCHOLOS_MAINLAND" -> "Archolos", "SILBACH" -> "Silbach" (part before '_').
static void WorldDisplayName( const char* raw, char* out, int outSize ) {
  int j = 0;
  for ( int i = 0; raw && raw[i] && raw[i] != '_' && j < outSize - 1; ++i, ++j ) {
    char c = raw[i];
    if ( j == 0 ) { if ( c >= 'a' && c <= 'z' ) c -= 32; }   // first letter upper
    else          { if ( c >= 'A' && c <= 'Z' ) c += 32; }   // rest lower
    out[j] = c;
  }
  out[j] = 0;
}

static void DoAutosave() {
  if ( !ogame || !ogame->savegameManager )
    return;

  int slot = g_nextSlot;
  if ( slot < g_slotMin || slot > g_slotMax )
    slot = g_slotMin;

  int num = ++g_counter;
  char name[48];
  std::snprintf( name, sizeof( name ), "Autosave %d", num );

  char buf[96];
  std::snprintf( buf, sizeof( buf ), "[Autosave] saving slot %d as \"%s\"", slot, name );
  Log( buf );

  oCSavegameManager* mgr = ogame->savegameManager;
  if ( !gameMan )
    return;

  // Full G2 save: serializes the live world AND script/parser state (quest
  // stages, Daedalus globals), and shows the game's own save progress bar.
  g_saving = true;
  gameMan->Write_Savegame( slot );
  g_saving = false;

  // Stamp our name + live area/time/playtime + fresh thumbnail onto the slot's
  // info, then persist it. SetAndWriteSavegame rewrites only the SAVEINFO/THUMB
  // (not the world/script data) and registers the slot for the in-game menu, so
  // it shows correctly and loads in the same session. This is the same post-save
  // pattern zUtilities uses - no ASCII file-patching / manual ZTEX / Reinit.
  oCSavegameInfo* info = mgr->GetSavegame( slot );
  if ( info ) {
    info->m_Name = zSTRING( name );
    oCWorld* w = ogame->GetGameWorld();
    if ( w ) {
      char wn[64];
      WorldDisplayName( w->GetWorldName().ToChar(), wn, sizeof( wn ) );
      info->m_WorldName = zSTRING( wn );
    }
    oCWorldTimer* wt = ogame->GetWorldTimer();
    if ( wt ) {
      int h, m;
      info->m_TimeDay = wt->GetDay();
      wt->GetTime( h, m );
      info->m_TimeHour = h;
      info->m_TimeMin  = m;
    }
    info->m_PlayTimeSeconds = gameMan->playTime;
    // Thumbnail: use a FRESH convert each save and delete it. UpdateThumbPic
    // shrinks the convert to thumbnail size, so reusing one would make the next
    // Vid_GetFrontBufferCopy write a full-screen frame into a tiny buffer ->
    // access violation. (Same fresh-convert approach zUtilities uses.)
    if ( zrenderer ) {
      zCTextureConvert* thumb = zrenderer->CreateTextureConvert();
      if ( thumb ) {
        zrenderer->Vid_GetFrontBufferCopy( *thumb );
        info->UpdateThumbPic( thumb );
        delete thumb;
      }
    }
    mgr->SetAndWriteSavegame( slot, info );
  }

  // Advance the rotation and persist counter + next slot to Gothic.ini.
  g_nextSlot = ( slot >= g_slotMax ) ? g_slotMin : slot + 1;
  if ( zoptions ) {
    zSTRING sec = "AUTOSAVE_PLUGIN";
    zoptions->WriteInt( sec, "Counter", g_counter, 0 );
    zoptions->WriteInt( sec, "NextSlot", g_nextSlot, 0 );
  }

  g_lastSaveTime = Timer::GetTime();
}

// ====================== per-frame engine hook ==============================
// On this build Union only dispatches Game_Entry. And GD3D11 owns the render
// path, so a hook on oCGame::Render never fires. Instead we hook the SIMULATION
// path: zCWorld::AdvanceClock(float) @ 0x006260E0, called every frame in-world
// regardless of renderer. thiscall(float) -> __fastcall(self, edx, dt).
typedef void( __fastcall* tAdvanceClockFn )( void* self, void* edx, float dt );
static tAdvanceClockFn Real_AdvanceClock = 0;
static bool            g_configLoaded     = false;
static int             g_prevWorldEntered = 0;

static void OnFrame( oCGame* game ) {
  if ( !game )
    return;

  // Lazy config load once zoptions is available; defaults apply until then.
  if ( !g_configLoaded && zoptions ) {
    LoadConfig();
    g_configLoaded = true;
  }
  if ( !g_enabled )
    return;

  // m_bWorldEntered is unreliable on this build (reads 0 mid-play). Use the
  // global hero pointer instead: non-null only when actually in the world.
  int we = ( player != 0 ) ? 1 : 0;
  if ( we && !g_prevWorldEntered ) {
    // Just entered the world (new game / load / level change): start grace.
    g_lastSaveTime = Timer::GetTime() - (unsigned int)g_intervalMs + (unsigned int)g_graceMs;
    Log( "[Autosave] world entered, monitoring." );
  }
  g_prevWorldEntered = we;
  if ( !we || g_saving )
    return;

  unsigned int now = Timer::GetTime();

  // Track unsafe states EVERY frame (not just when due) so we know how long the
  // player has been continuously safe. This lets us hold the save for a settle
  // window after a dialogue/combat state clears.
  bool safe = IsSafeToSave();
  if ( !safe )
    g_lastUnsafeTime = now;

  if ( now - g_lastSaveTime < (unsigned int)g_intervalMs )
    return;

  if ( !safe ) {
    static unsigned int lastSkip = 0;
    if ( now - lastSkip >= 30000 ) {
      lastSkip = now;
      char buf[96];
      std::snprintf( buf, sizeof( buf ), "[Autosave] due but skipped: %s", g_skipReason );
      PluginLog( buf );
    }
    return;
  }

  // Wait until we've been safe for the settle window. A dialogue/combat state
  // takes a moment to fully tear down; saving the instant the gate clears can
  // capture a still-exiting state that resumes (e.g. re-enters the dialogue) on
  // load. Holding a few seconds lets the game state settle first.
  if ( now - g_lastUnsafeTime < (unsigned int)g_settleMs )
    return;

  // gameMan->Write_Savegame shows the game's own save progress bar, so no custom
  // on-screen notice (or pre-save frame deferral) is needed anymore.
  DoAutosave();
}

void __fastcall Hooked_AdvanceClock( void* self, void* edx, float dt ) {
  Real_AdvanceClock( self, edx, dt );   // run the engine's normal clock advance
  OnFrame( ogame );                     // then our autosave check (self is zCWorld)
}

// ====================== Union lifecycle callbacks ==========================

cexport void Game_Entry() {
  // Game_Entry is the only callback this Union build reliably dispatches, so we
  // install our own per-frame hook here. Must use Hook_Detours (Hook_Auto does
  // not patch on this build). Target: zCWorld::AdvanceClock @ 0x006260E0,
  // the simulation tick (GD3D11 leaves it alone, unlike the render path).
  auto& hk = UnionCore::CreateHook( (void*)0x006260E0,
                                    (tAdvanceClockFn)&Hooked_AdvanceClock,
                                    UnionCore::Hook_Detours );
  Real_AdvanceClock = hk;
  PluginLog( Real_AdvanceClock ? "[Autosave] hook installed."
                               : "[Autosave] HOOK INSTALL FAILED." );
}

cexport void Game_Init() {
  // May not be called on this build; harmless if it is.
  PluginLog( "[Autosave] Game_Init." );
  LoadConfig();
  g_configLoaded = true;
  Log( "[Autosave] plugin loaded." );
}

cexport void Game_Exit() {
}

cexport void Game_PreLoop() {
}

cexport void Game_Loop() {
  // Fallback: if this build ever does dispatch Game_Loop, route through OnFrame
  // too. The interval guard makes a double-call harmless.
  if ( ogame )
    OnFrame( ogame );
}

cexport void Game_PostLoop() {
}

cexport void Game_MenuLoop() {
}

cexport void Game_SaveBegin() {
  g_saving = true;
}

cexport void Game_SaveEnd() {
  g_saving = false;
  // A manual/quick save also resets the autosave clock.
  g_lastSaveTime = Timer::GetTime();
}

static void LoadBegin() {
  g_prevWorldEntered = 0;
}

static void LoadEnd() {
  // Start the grace period: don't autosave the instant a load finishes.
  // (OnFrame also detects this via the player pointer; this is a fast path.)
  g_lastSaveTime = Timer::GetTime() - (unsigned int)g_intervalMs + (unsigned int)g_graceMs;
}

cexport void Game_LoadBegin_NewGame()     { LoadBegin(); }
cexport void Game_LoadEnd_NewGame()       { LoadEnd();   }
cexport void Game_LoadBegin_SaveGame()    { LoadBegin(); }
cexport void Game_LoadEnd_SaveGame()      { LoadEnd();   }
cexport void Game_LoadBegin_ChangeLevel() { LoadBegin(); }
cexport void Game_LoadEnd_ChangeLevel()   { LoadEnd();   }
cexport void Game_LoadBegin_Trigger()     {}
cexport void Game_LoadEnd_Trigger()       {}

cexport void Game_Pause() {
}

cexport void Game_Unpause() {
}

cexport void Game_DefineExternals() {
}

cexport void Game_ApplyOptions() {
  // Re-read config when the player changes options in-game.
  LoadConfig();
}
