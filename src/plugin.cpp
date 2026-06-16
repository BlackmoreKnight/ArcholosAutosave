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

// ----------------------------- runtime state -------------------------------
static unsigned int g_lastSaveTime = 0;      // ms timestamp of last (auto or manual) save
static bool         g_saving       = false;  // a save is in progress
static int          g_nextSlot     = 12;     // next slot in the rotation (persisted)
static int          g_counter      = 0;      // incrementing save-name number (persisted)

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
  g_counter      = zoptions->ReadInt ( sec, "Counter", 0 );
  g_nextSlot     = zoptions->ReadInt ( sec, "NextSlot", g_slotMin );

  if ( mins < 1 ) mins = 1;                                  // never hammer the disk
  if ( grace < 0 ) grace = 0;
  if ( g_slotMin < 1 ) g_slotMin = 12;                       // slot 0 == "current"; never use it
  if ( g_slotMax < g_slotMin ) g_slotMax = g_slotMin;
  if ( g_nextSlot < g_slotMin || g_nextSlot > g_slotMax ) g_nextSlot = g_slotMin;

  g_intervalMs = mins * 60 * 1000;
  g_graceMs    = grace * 1000;

  char buf[200];
  std::snprintf( buf, sizeof( buf ),
                 "[Autosave] config: enabled=%d interval=%dmin slots=%d-%d next=%d counter=%d onlyWhenSafe=%d grace=%ds",
                 (int)g_enabled, mins, g_slotMin, g_slotMax, g_nextSlot, g_counter, (int)g_onlyWhenSafe, grace );
  Log( buf );
}

// Mirrors the engine's own "can the player save right now?" intent:
// not mid-load/level-change, alive, weapon holstered, not in a busy/anim
// body state and not in a dialog view.
static bool IsSafeToSave() {
  if ( !ogame || !player ) {
    g_skipReason = "no ogame/player";
    return false;
  }
  if ( ogame->inLoadSaveGame || ogame->inLevelChange ) {
    g_skipReason = "loading/level-change";
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

// Replace the value of an ASCII-archive "<key>=string:VALUE" line.
static void PatchInfoStr( std::string& s, const char* key, const std::string& val ) {
  std::string k = std::string( "\t" ) + key + "=string:";
  size_t p = s.find( k );
  if ( p == std::string::npos )
    return;
  size_t vs  = p + k.size();
  size_t eol = s.find( '\n', vs );
  if ( eol == std::string::npos )
    eol = s.size();
  s.replace( vs, eol - vs, val );
}

// Replace the value of an ASCII-archive "<key>=int:VALUE" line.
static void PatchInfoInt( std::string& s, const char* key, int val ) {
  std::string k = std::string( "\t" ) + key + "=int:";
  size_t p = s.find( k );
  if ( p == std::string::npos )
    return;
  size_t vs  = p + k.size();
  size_t eol = s.find( '\n', vs );
  if ( eol == std::string::npos )
    eol = s.size();
  char num[16];
  std::snprintf( num, sizeof( num ), "%d", val );
  s.replace( vs, eol - vs, num );
}

// WriteSavegame copies the loaded game's "current" SAVEINFO verbatim, so the new
// slot inherits its name/area/time/playtime. SAVEINFO.SAV is a plain ASCII
// archive, so we overwrite those fields with the LIVE values on disk afterwards.
static void PatchSaveInfo( int slot, const char* title ) {
  // --- gather live metadata ---
  int tDay = -1, tHour = -1, tMin = -1;
  if ( ogame ) {
    oCWorldTimer* wt = ogame->GetWorldTimer();
    if ( wt ) { tDay = wt->GetDay(); wt->GetTime( tHour, tMin ); }
  }
  // Live world (area) name and play time. Init() leaves these empty on a bare
  // instance, so read them straight from the engine.
  std::string worldName, saveDate;
  int playSec = -1;
  if ( ogame ) {
    oCWorld* w = ogame->GetGameWorld();
    if ( w && w->GetWorldName().ToChar() )
      worldName = w->GetWorldName().ToChar();
  }
  if ( gameMan )
    playSec = gameMan->playTime;

  // The raw world name is like "ARCHOLOS_MAINLAND"; the menu shows the display
  // form "Archolos". Approximate that: take the part before '_' and Title-case it
  // ("SILBACH" -> "Silbach", "ARCHOLOS_MAINLAND" -> "Archolos").
  if ( !worldName.empty() ) {
    size_t us = worldName.find( '_' );
    if ( us != std::string::npos )
      worldName = worldName.substr( 0, us );
    for ( size_t i = 0; i < worldName.size(); ++i ) {
      char c = worldName[i];
      if ( i == 0 ) { if ( c >= 'a' && c <= 'z' ) c -= 32; }
      else          { if ( c >= 'A' && c <= 'Z' ) c += 32; }
      worldName[i] = c;
    }
  }

  char lg[224];
  std::snprintf( lg, sizeof( lg ), "[meta] world='%s' day=%d %d:%02d play=%d date='%s'",
                 worldName.c_str(), tDay, tHour, tMin, playSec, saveDate.c_str() );
  PluginLog( lg );

  // real-time fallback for SaveDate
  if ( saveDate.empty() ) {
    char d[48];
    std::time_t t = std::time( 0 );
    std::tm* lt = std::localtime( &t );
    if ( lt ) { std::snprintf( d, sizeof( d ), "%d.%d.%d - %d:%02d",
                 lt->tm_mday, lt->tm_mon + 1, lt->tm_year + 1900, lt->tm_hour, lt->tm_min ); saveDate = d; }
  }

  char path[176];
  std::snprintf( path, sizeof( path ),
                 "Saves_TheChroniclesOfMyrtana\\savegame%d\\SAVEINFO.SAV", slot );
  FILE* f = std::fopen( path, "rb" );
  if ( !f )
    return;
  std::fseek( f, 0, SEEK_END );
  long n = std::ftell( f );
  std::fseek( f, 0, SEEK_SET );
  if ( n <= 0 || n > 100000 ) { std::fclose( f ); return; }
  std::string buf;
  buf.resize( (size_t)n );
  size_t rd = std::fread( &buf[0], 1, (size_t)n, f );
  std::fclose( f );
  buf.resize( rd );

  PatchInfoStr( buf, "Title", title );
  if ( !worldName.empty() ) PatchInfoStr( buf, "WorldName", worldName );
  if ( !saveDate.empty() )  PatchInfoStr( buf, "SaveDate", saveDate );
  if ( tDay  >= 0 ) PatchInfoInt( buf, "TimeDay", tDay );
  if ( tHour >= 0 ) PatchInfoInt( buf, "TimeHour", tHour );
  if ( tMin  >= 0 ) PatchInfoInt( buf, "TimeMin", tMin );
  if ( playSec > 0 ) PatchInfoInt( buf, "PlayTimeSeconds", playSec );

  f = std::fopen( path, "wb" );
  if ( !f )
    return;
  std::fwrite( buf.data(), 1, buf.size(), f );
  std::fclose( f );
}

// Capture the current frame and write it as a 256x256 RGB565 ZTEX (THUMB.SAV
// format) to `path`. Returns true on success. Worst case (wrong source format)
// is a garbled image - it cannot corrupt the savegame.
static bool WriteFreshThumb( const char* path ) {
  if ( !zrenderer )
    return false;
  // Reuse one capture surface for the whole session (avoids per-save alloc/leak).
  static zCTextureConvert* tex = 0;
  if ( !tex )
    tex = zrenderer->CreateTextureConvert();
  if ( !tex )
    return false;

  bool ok = false;
  if ( zrenderer->Vid_GetFrontBufferCopy( *tex ) ) {
    zCTextureInfo ti = tex->GetTextureInfo();
    void* src = 0;
    int pitch = 0;
    tex->Lock( 0 );
    if ( tex->GetTextureBuffer( 0, src, pitch ) && src && ti.sizeX > 0 && ti.sizeY > 0 ) {
      static bool loggedFmt = false;
      if ( !loggedFmt ) {
        loggedFmt = true;
        char fb[96];
        std::snprintf( fb, sizeof( fb ), "[thumb] src fmt=%d %dx%d pitch=%d",
                       (int)ti.texFormat, ti.sizeX, ti.sizeY, pitch );
        PluginLog( fb );
      }

      const int TS = 256;
      static unsigned short out[256 * 256];
      const unsigned char* base = (const unsigned char*)src;
      // Assume a 32-bit source (BGRA/XRGB, the usual front-buffer layout).
      // 16-bit sources fall through harmlessly via the same index math.
      int bpp = ( pitch >= ti.sizeX * 4 ) ? 4 : 2;
      for ( int ty = 0; ty < TS; ++ty ) {
        int sy = ty * ti.sizeY / TS;
        const unsigned char* row = base + (size_t)sy * pitch;
        for ( int tx = 0; tx < TS; ++tx ) {
          int sx = tx * ti.sizeX / TS;
          unsigned r, g, b;
          if ( bpp == 4 ) {
            const unsigned char* px = row + (size_t)sx * 4;
            b = px[0]; g = px[1]; r = px[2];   // BGRA
          } else {
            unsigned short p = *(const unsigned short*)( row + (size_t)sx * 2 );
            r = ( ( p >> 11 ) & 0x1F ) << 3; g = ( ( p >> 5 ) & 0x3F ) << 2; b = ( p & 0x1F ) << 3;
          }
          out[ty * TS + tx] = (unsigned short)( ( ( r >> 3 ) << 11 ) | ( ( g >> 2 ) << 5 ) | ( b >> 3 ) );
        }
      }
      tex->Unlock();

      // ZTEX header: "ZTEX",ver=0,format=8(R5G6B5),w,h,mip=1,refW,refH,avgColor
      unsigned int hdr[9];
      ( (char*)hdr )[0] = 'Z'; ( (char*)hdr )[1] = 'T'; ( (char*)hdr )[2] = 'E'; ( (char*)hdr )[3] = 'X';
      hdr[1] = 0; hdr[2] = 8; hdr[3] = TS; hdr[4] = TS; hdr[5] = 1; hdr[6] = TS; hdr[7] = TS; hdr[8] = 0;

      FILE* f = std::fopen( path, "wb" );
      if ( f ) {
        std::fwrite( hdr, 1, sizeof( hdr ), f );
        std::fwrite( out, 1, sizeof( out ), f );
        std::fclose( f );
        ok = true;
      }
    } else {
      tex->Unlock();
    }
  }
  return ok;
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

  // CGameManager::Write_Savegame is the G2 full save: it serializes the LIVE
  // world AND script/parser state (quest stages, log, Daedalus globals).
  // oCGame::WriteSavegame (the G1A path) only wrote the world and left
  // SAVEDAT/SCRPTSAVE copied from the stale current\ snapshot, so quest
  // progress made since the last load/level-change was lost.
  g_saving = true;
  if ( gameMan )
    gameMan->Write_Savegame( slot );
  g_saving = false;

  // Fix the on-disk SAVEINFO (name/date) and write a fresh thumbnail from the
  // current screen (WriteSavegame copies a stale THUMB from current\, so we
  // overwrite it). Also refresh current\'s thumb. Then reload the slot's cached
  // info so the in-game load menu shows the new name + thumbnail without restart.
  PatchSaveInfo( slot, name );

  char thumbPath[176];
  std::snprintf( thumbPath, sizeof( thumbPath ),
                 "Saves_TheChroniclesOfMyrtana\\savegame%d\\THUMB.SAV", slot );
  bool tok = WriteFreshThumb( thumbPath );
  WriteFreshThumb( "Saves_TheChroniclesOfMyrtana\\current\\THUMB.SAV" );
  char lb[80];
  std::snprintf( lb, sizeof( lb ), "[thumb] slot %d written=%d", slot, (int)tok );
  PluginLog( lb );

  // ReloadResources only refreshed the thumbnail texture, not the name/date or
  // the slot's existence, so the in-session menu still showed stale/unloadable.
  // Reinit re-scans all slots from disk (reading our patched SAVEINFO + fresh
  // THUMB), fully refreshing the manager's list for the in-game menu.
  mgr->Reinit();

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
  if ( now - g_lastSaveTime < (unsigned int)g_intervalMs )
    return;

  if ( !IsSafeToSave() ) {
    static unsigned int lastSkip = 0;
    if ( now - lastSkip >= 30000 ) {
      lastSkip = now;
      char buf[96];
      std::snprintf( buf, sizeof( buf ), "[Autosave] due but skipped: %s", g_skipReason );
      PluginLog( buf );
    }
    return;
  }

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
