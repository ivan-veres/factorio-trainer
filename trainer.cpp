// Factorio trainer: a standalone port of factorio-combined_6.CT. Single player only.
//
// Every address and offset is looked up BY NAME in factorio.pdb (shipped next to
// factorio.exe): globals and functions as symbols, struct offsets as class
// members. A game update that only moves fields needs no code change; a renamed
// field fails at startup with its name.
//
// Your character's main inventory is reached without clicking:
//   global -> game -> localPlayer -> controllerManager.controller -> character -> inventory
// Clicked inventories (chests), hovered tanks and hovered ore use a one-shot
// hardware breakpoint through the Windows debug API, then detach.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <dbghelp.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using u64 = uint64_t;
using u32 = uint32_t;
using u16 = uint16_t;
using u8 = uint8_t;

constexpr double FLUID_SCALE = 16777216.0;  // fluid amounts are int64 fixed point, 2^24
constexpr int OIL_REACH = 12;               // max tile gap between wells of one field

// ---------------------------------------------------------------- process
struct Game {
  DWORD pid = 0;
  HANDLE proc = nullptr;
  u64 base = 0;
  u32 size = 0;
  std::wstring exe;
} g;

// While the game is frozen (atEndOfTick) nothing else writes its memory, so small reads are served
// from 4 KB page copies: one ReadProcessMemory per page instead of one per field. Writes go through.
struct PageCache { bool on = false; std::unordered_map<u64, std::vector<uint8_t>> pages; } cache;

const uint8_t* cached(u64 a, size_t len) {
  u64 p = a & ~0xFFFull;
  if (!cache.on || a - p + len > 0x1000) return nullptr;
  auto [it, fresh] = cache.pages.try_emplace(p);
  SIZE_T n = 0;
  if (fresh) {
    it->second.resize(0x1000);
    if (!ReadProcessMemory(g.proc, (LPCVOID)p, it->second.data(), 0x1000, &n) || n != 0x1000) it->second.clear();
  }
  return it->second.empty() ? nullptr : it->second.data() + (a - p);
}

template <class T> bool rd(u64 a, T& v) {
  if (a < 0x10000) return false;
  if (auto c = cached(a, sizeof v)) { std::memcpy(&v, c, sizeof v); return true; }
  SIZE_T n = 0;
  return ReadProcessMemory(g.proc, (LPCVOID)a, &v, sizeof v, &n) && n == sizeof v;
}
template <class T> std::optional<T> rd(u64 a) {
  T v{};
  if (rd(a, v)) return v;
  return std::nullopt;
}
u64 ptr(u64 a) { return rd<u64>(a).value_or(0); }
template <class T> bool wr(u64 a, T v) {
  SIZE_T n = 0;
  if (!WriteProcessMemory(g.proc, (LPVOID)a, &v, sizeof v, &n) || n != sizeof v) return false;
  if (auto c = cached(a, sizeof v)) std::memcpy(const_cast<uint8_t*>(c), &v, sizeof v);
  return true;
}

bool attach() {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  PROCESSENTRY32W pe{sizeof pe};
  for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
    if (_wcsicmp(pe.szExeFile, L"factorio.exe") == 0) { g.pid = pe.th32ProcessID; break; }
  CloseHandle(snap);
  if (!g.pid) { std::puts("factorio.exe is not running."); return false; }

  g.proc = OpenProcess(PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_QUERY_INFORMATION, FALSE, g.pid);
  if (!g.proc) { std::printf("OpenProcess failed (%lu).\n", GetLastError()); return false; }

  snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, g.pid);
  MODULEENTRY32W me{sizeof me};
  for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me))
    if (_wcsicmp(me.szModule, L"factorio.exe") == 0) {
      g.base = (u64)me.modBaseAddr; g.size = me.modBaseSize; g.exe = me.szExePath; break;
    }
  CloseHandle(snap);
  if (!g.base) { std::puts("Could not find the factorio.exe module."); return false; }
  std::printf("Attached to factorio.exe pid %lu\n", g.pid);
  return true;
}

// ---------------------------------------------------------------- pdb
// cvconst.h values (DIA SDK header, not shipped with dbghelp.h)
enum : DWORD { TAG_DATA = 7, TAG_TYPEDEF = 17, TAG_BASECLASS = 18, DATA_MEMBER = 7 };

namespace pdb {
HANDLE h = (HANDLE)(uintptr_t)0xF00D;  // private DbgHelp session, the game is not invaded
std::vector<std::string> missing;

bool open() {
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_FAIL_CRITICAL_ERRORS);
  std::wstring dir = std::filesystem::path(g.exe).parent_path().wstring();
  return SymInitializeW(h, dir.c_str(), FALSE) &&
         SymLoadModuleExW(h, nullptr, g.exe.c_str(), nullptr, g.base, g.size, nullptr, 0);
}

u64 sym(const char* name) {
  char buf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME]{};
  auto* si = (SYMBOL_INFO*)buf;
  si->SizeOfStruct = sizeof(SYMBOL_INFO);
  si->MaxNameLen = MAX_SYM_NAME;
  if (SymFromName(h, (std::string("factorio!") + name).c_str(), si)) return si->Address;
  missing.push_back(name);
  return 0;
}

DWORD info(DWORD ti, IMAGEHLP_SYMBOL_TYPE_INFO what) {
  DWORD v = 0;
  SymGetTypeInfo(h, g.base, ti, what, &v);
  return v;
}

std::wstring name(DWORD ti) {
  WCHAR* n = nullptr;
  std::wstring s;
  if (SymGetTypeInfo(h, g.base, ti, TI_GET_SYMNAME, &n) && n) { s = n; LocalFree(n); }
  return s;
}

std::optional<DWORD> type(const char* tname) {
  char buf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME]{};
  auto* si = (SYMBOL_INFO*)buf;
  si->SizeOfStruct = sizeof(SYMBOL_INFO);
  si->MaxNameLen = MAX_SYM_NAME;
  if (SymGetTypeFromName(h, g.base, tname, si)) return si->TypeIndex;
  return std::nullopt;
}

// Member (or base class) `m` of type `ti`, searching base classes too.
bool member(DWORD ti, const std::wstring& m, DWORD& off, DWORD& mti) {
  DWORD count = info(ti, TI_GET_CHILDRENCOUNT);
  std::vector<char> raw(sizeof(TI_FINDCHILDREN_PARAMS) + count * sizeof(ULONG));
  auto* p = (TI_FINDCHILDREN_PARAMS*)raw.data();
  p->Count = count;
  if (!SymGetTypeInfo(h, g.base, ti, TI_FINDCHILDREN, p)) return false;
  for (DWORD i = 0; i < count; ++i) {
    DWORD c = p->ChildId[i], tag = info(c, TI_GET_SYMTAG);
    if (tag == TAG_DATA && info(c, TI_GET_DATAKIND) != DATA_MEMBER) continue;
    if (tag != TAG_DATA && tag != TAG_BASECLASS) continue;
    DWORD t = info(c, TI_GET_TYPEID);
    while (info(t, TI_GET_SYMTAG) == TAG_TYPEDEF) t = info(t, TI_GET_TYPEID);
    if ((tag == TAG_DATA && name(c) == m) || (tag == TAG_BASECLASS && name(t) == m)) {
      off = info(c, TI_GET_OFFSET); mti = t; return true;
    }
    if (tag == TAG_BASECLASS) {
      DWORD o2, t2;
      if (member(t, m, o2, t2)) { off = info(c, TI_GET_OFFSET) + o2; mti = t2; return true; }
    }
  }
  return false;
}

// Offset of a dotted member path, e.g. off("FluidSegment", "buffer.fluid.amount").
u64 off(const char* tname, const char* path) {
  auto ti = type(tname);
  u64 total = 0;
  std::string err = std::string(tname) + "." + path;
  if (!ti) { missing.push_back(err); return 0; }
  DWORD cur = *ti;
  std::stringstream ss(path);
  for (std::string part; std::getline(ss, part, '.');) {
    DWORD o = 0, t = 0;
    if (!member(cur, std::wstring(part.begin(), part.end()), o, t)) { missing.push_back(err); return 0; }
    total += o; cur = t;
  }
  return total;
}

// Offset that may legitimately be absent (per-class lookups at runtime): nullopt, nothing recorded.
std::optional<u64> tryOff(const char* tname, const char* path) {
  size_t before = missing.size();
  u64 v = off(tname, path);
  if (missing.size() == before) return v;
  missing.resize(before);
  return std::nullopt;
}

// Undecorated symbol at an absolute address, e.g. "StorageTank::`vftable'".
std::string nameAt(u64 addr) {
  char buf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME]{};
  auto* si = (SYMBOL_INFO*)buf;
  si->SizeOfStruct = sizeof(SYMBOL_INFO);
  si->MaxNameLen = MAX_SYM_NAME;
  DWORD64 disp = 0;
  return SymFromAddr(h, addr, &disp, si) && disp == 0 ? si->Name : "";
}

u64 size(const char* tname) {
  auto ti = type(tname);
  ULONG64 len = 0;
  if (ti) SymGetTypeInfo(h, g.base, *ti, TI_GET_LENGTH, &len);
  if (!len) missing.push_back(std::string("sizeof ") + tname);
  return len;
}
}  // namespace pdb

// ---------------------------------------------------------------- layout
struct Layout {
  u64 sGlobal, sItems, sFluids, sInvHook, sFluHook, sOreHook, sSortHook, sAlarm;
  u64 gcGame, gameLocalPlayer, playerController, playerPhysical, playerStashed, playerBeforePause, vtCharCtrl, ctrlPlayer, ccCharacter, targeterTarget, charInventory;
  u64 stackSize, stackCount, stackId, stackItem, stackOwner;
  u64 invData, invSize, invFirstNonFull, invLastNonEmpty;
  u64 gcOtherSettings, scraperOn;
  u64 guiMap, guiMapSize, guiOff, guiSize, guiEntity, containerInv, invOwner;
  u64 sQualities, sShouldCreate, vtItem, vtInv, vtInvBar, vtInvFilters, vtInvFiltersBar;
  u64 stackQuality, stackCap, itemSpoil, invBar, filtersPlain, filtersBar, filterId, holderSize, holderBegin, filterSize;
  u64 protoName, itemFlags, itemStackSize;
  u64 fluId, fluTemp, fluAmt, fluVol, fluFilterId, fluFilterMin, fluFilterMax, fluFilterCount, fluBoxesBegin, fluBoxesEnd;
  u64 boxSegment, boxConnBegin, boxConnEnd, connTarget, connOpposite, connFlags, connType, connSize, fpDefaultTemp, fpMaxTemp;
  u64 entProto, entPost, entX, entY, oreAmt, resNormal, resInfinite;
  u64 entSurface, surfChunks, chunkPos, chunkAdvTiles, chunkAdvTilesSize, advTileSize;
  u64 protoTileSize, entConnector, connTile, connNextOnTile, connEntity, tileEntities, rsFront, rsBack, rsTrain, fwFluid, fwCapacity, trainManager, managerMap, mapTick, trainActivity, fluidId, fluidTemp, fluidAmt;
} L;

bool loadLayout() {
  using pdb::off; using pdb::sym;
  L.sGlobal = sym("global");
  L.sItems = sym("PrototypeList<ItemPrototype>::indexToPrototype");
  L.sFluids = sym("PrototypeList<FluidPrototype>::indexToPrototype");
  L.sInvHook = sym("CursorTransferLogic::cursorTransfer") + 0x66;  // instruction with RDI = clicked slot (CT, verified)
  L.sFluHook = sym("FluidSegment::addToDescription");             // RCX = segment
  L.sOreHook = sym("ResourceEntity::addToDescription");           // RCX = entity
  L.sSortHook = sym("Controller::checkSortInventoryAfterUpdate");  // end of tick, update thread
  L.sAlarm = sym("WakeUpList::alarm");  // wakes every inserter sleeping on one list

  L.gcGame = off("GlobalContext", "game");
  L.gameLocalPlayer = off("Game", "localPlayer");
  u64 cm = off("Player", "controllerManager");
  L.playerController = cm + off("ControllerManager", "controller");
  L.playerPhysical = cm + off("ControllerManager", "physicalController");
  L.playerStashed = cm + off("ControllerManager", "stashedController");
  L.playerBeforePause = cm + off("ControllerManager", "controllerBeforePause");  // paused: controller is a PauseController
  L.vtCharCtrl = sym("CharacterController::`vftable'");
  L.ctrlPlayer = off("Controller", "player");
  L.ccCharacter = off("CharacterController", "character");
  L.targeterTarget = off("TargeterBase", "target");
  L.charInventory = off("Character", "inventory");

  L.stackSize = pdb::size("ItemStack");
  L.stackCount = off("ItemStack", "count");
  L.stackId = off("ItemStack", "itemID");
  L.stackItem = off("ItemStack", "item");
  L.stackOwner = off("ItemStack", "owner");
  L.invData = off("Inventory", "data");
  L.invSize = off("Inventory", "dataSize");
  L.invFirstNonFull = off("Inventory", "firstNonFullIndex");
  L.invLastNonEmpty = off("Inventory", "lastNonEmptyIndex");
  L.gcOtherSettings = off("GlobalContext", "otherSettings");
  L.scraperOn = off("OtherSettings", "consistencyScraper.value");
  // Controller.guiTarget.data is a std::deque of open GUIs; the back is the one on screen.
  L.guiMap = off("Controller", "guiTarget.data._Mypair._Myval2._Map");
  L.guiMapSize = off("Controller", "guiTarget.data._Mypair._Myval2._Mapsize");
  L.guiOff = off("Controller", "guiTarget.data._Mypair._Myval2._Myoff");
  L.guiSize = off("Controller", "guiTarget.data._Mypair._Myval2._Mysize");
  L.guiEntity = off("GuiTargetData", "entity.target");
  L.containerInv = off("ContainerEntity", "inventory");
  L.invOwner = off("Inventory", "owner");
  // give: plain item prototypes into plain / bar / filtered inventories only
  L.sQualities = sym("PrototypeList<QualityPrototype>::indexToPrototype");
  L.vtItem = sym("ItemPrototype::`vftable'");
  L.sShouldCreate = sym("ItemPrototype::shouldCreateItem");
  L.vtInv = sym("Inventory::`vftable'");
  L.vtInvBar = sym("InventoryWithBar::`vftable'");
  L.vtInvFilters = sym("InventoryWithFilters::`vftable'");
  L.vtInvFiltersBar = sym("InventoryWithFiltersAndBar::`vftable'");
  L.stackQuality = off("ItemStack", "qualityID");
  L.stackCap = off("ItemStack", "stackSize");
  L.itemSpoil = off("ItemPrototype", "defaultSpoilTicks");
  L.invBar = off("Inventory", "bar");
  L.filtersPlain = off("InventoryWithFilters", "filters");
  L.filtersBar = off("InventoryWithFiltersAndBar", "filters");
  L.holderSize = off("HeapArray<IDWithQualityFilter<ID<ItemPrototype,unsigned short> > >::ValueHolder", "size");
  L.holderBegin = off("HeapArray<IDWithQualityFilter<ID<ItemPrototype,unsigned short> > >::ValueHolder", "begin");
  L.filterId = off("IDWithQualityFilter<ID<ItemPrototype,unsigned short> >", "baseID");
  L.filterSize = pdb::size("IDWithQualityFilter<ID<ItemPrototype,unsigned short> >");
  L.protoName = off("PrototypeBase", "name");
  L.itemFlags = off("ItemPrototype", "flags");
  L.itemStackSize = off("ItemPrototype", "stackSize");

  L.fluId = off("FluidSegment", "buffer.fluid.fluidID");
  L.fluTemp = off("FluidSegment", "buffer.fluid.temperature");
  L.fluAmt = off("FluidSegment", "buffer.fluid.amount");
  L.fluVol = off("FluidSegment", "buffer.volume");
  L.fluFilterId = off("FluidSegment", "buffer.filter.fluidID");
  L.fluFilterMin = off("FluidSegment", "buffer.filter.minTemperature");
  L.fluFilterMax = off("FluidSegment", "buffer.filter.maxTemperature");
  L.fluFilterCount = off("FluidSegment", "filteringFluidBoxes._Mypair._Myval2._Myval2._Mysize");
  L.fluBoxesBegin = off("FluidSegment", "fluidBoxes._Mypair._Myval2._Myfirst");
  L.fluBoxesEnd = off("FluidSegment", "fluidBoxes._Mypair._Myval2._Mylast");
  L.boxSegment = off("FluidBox", "fluidSegment");
  L.boxConnBegin = off("FluidBox", "connections.begin_");
  L.boxConnEnd = off("FluidBox", "connections.end_");
  L.connTarget = off("FluidBoxConnection", "target");
  L.connOpposite = off("FluidBoxConnection", "oppositeIndex");
  L.connFlags = off("FluidBoxConnection", "flags");
  L.connType = off("FluidBoxConnection", "type");
  L.connSize = pdb::size("FluidBoxConnection");
  L.fpDefaultTemp = off("FluidPrototype", "defaultTemperature");
  L.fpMaxTemp = off("FluidPrototype", "maxTemperature");

  L.entProto = off("Entity", "prototype");
  L.entPost = off("Entity", "PostTransferNotifiable");  // second vtable, structural check
  L.entX = off("Entity", "position.x");
  L.entY = off("Entity", "position.y");
  L.oreAmt = off("ResourceEntity", "resourceAmount");
  L.resNormal = off("ResourceEntityPrototype", "normalAmount");  // 100% yield of an infinite resource
  L.resInfinite = off("ResourceEntityPrototype", "infiniteType");  // oil-like; solid ores have a normalAmount too

  L.protoTileSize = off("EntityPrototype", "tileGridSizeStored");  // TilePosition {x, y}, tiles
  L.entConnector = off("Entity", "connector");  // link into the entity list of the tile it stands on
  L.connTile = off("EntityConnector", "advancedTile");
  L.connNextOnTile = off("EntityConnector", "nextOnTile");
  L.connEntity = off("EntityConnector", "entity");
  L.tileEntities = off("AdvancedTile", "entities");
  // ore / chest blocks: entities by position, from the per-tile entity lists of each chunk
  L.entSurface = off("Entity", "surface");
  L.surfChunks = off("Surface", "chunks");  // std::vector<Chunk*>
  L.chunkPos = off("Chunk", "position");    // ChunkPosition {x, y}, chunks
  L.chunkAdvTiles = off("Chunk", "advancedTiles");
  L.chunkAdvTilesSize = pdb::size("ChunkAdvancedTiles");
  L.advTileSize = pdb::size("AdvancedTile");

  // train fill: walk the rolling stock; fluid wagons hold one Fluid and a cached capacity
  L.rsFront = off("RollingStock", "connectedToFront") + L.targeterTarget;
  L.rsBack = off("RollingStock", "connectedToBack") + L.targeterTarget;
  L.rsTrain = off("RollingStock", "train");
  L.fwFluid = off("FluidWagon", "fluid");
  L.fwCapacity = off("FluidWagon", "cached.fluidCapacity");
  L.trainManager = off("Train", "manager");
  L.managerMap = off("TrainManager", "map");
  L.mapTick = off("Map", "entityTick");
  L.trainActivity = off("Train", "schedulable.tickOfLastActivity");
  L.fluidId = off("Fluid", "fluidID");
  L.fluidTemp = off("Fluid", "temperature");
  L.fluidAmt = off("Fluid", "amount");

  if (pdb::missing.empty()) return true;
  std::puts("These names are no longer in factorio.pdb (the game changed them):");
  for (auto& m : pdb::missing) std::printf("  %s\n", m.c_str());
  return false;
}

// ---------------------------------------------------------------- capture
enum class Reg { RDI, RCX };

void setDr(DWORD tid, u64 addr) {
  HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, tid);
  if (!th) return;
  SuspendThread(th);
  CONTEXT c{};
  c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
  if (GetThreadContext(th, &c)) {
    c.Dr0 = addr;
    c.Dr7 = (c.Dr7 & ~0xF0003ull) | (addr ? 1 : 0);  // L0, execute, length 1
    c.Dr6 = 0;
    SetThreadContext(th, &c);
  }
  ResumeThread(th);
  CloseHandle(th);
}

void setDrAll(u64 addr) {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  THREADENTRY32 te{sizeof te};
  for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
    if (te.th32OwnerProcessID == g.pid) setDr(te.th32ThreadID, addr);
  CloseHandle(snap);
}

// One-shot hardware execute breakpoint on every thread: the first thread that reaches
// `addr` is handed to onHit(CONTEXT&) (control + integer registers; return true to write
// them back), then the breakpoint is cleared and the debugger detaches. False on timeout.
template <class F> bool trap(u64 addr, const char* hint, DWORD timeoutMs, F onHit) {
  if (!DebugActiveProcess(g.pid)) {
    std::printf("Could not attach the debugger (%lu). Is Cheat Engine attached?\n", GetLastError());
    return false;
  }
  DebugSetProcessKillOnExit(FALSE);
  if (hint) std::printf("Armed - %s (%lus)\n", hint, timeoutMs / 1000);

  bool result = false, armed = false, cleanup = false;  // cleanup: never arm, never call onHit
  auto handle = [&](DEBUG_EVENT& e) -> DWORD {
    switch (e.dwDebugEventCode) {
      case CREATE_PROCESS_DEBUG_EVENT: if (e.u.CreateProcessInfo.hFile) CloseHandle(e.u.CreateProcessInfo.hFile); break;
      case LOAD_DLL_DEBUG_EVENT: if (e.u.LoadDll.hFile) CloseHandle(e.u.LoadDll.hFile); break;
      case CREATE_THREAD_DEBUG_EVENT: if (armed && !result && !cleanup) setDr(e.dwThreadId, addr); break;
      case EXCEPTION_DEBUG_EVENT: {
        auto& er = e.u.Exception.ExceptionRecord;
        if (er.ExceptionCode == EXCEPTION_BREAKPOINT && !armed) {
          if (!cleanup) setDrAll(addr);
          armed = true;
          return DBG_CONTINUE;
        }
        if (er.ExceptionCode == EXCEPTION_SINGLE_STEP && (u64)er.ExceptionAddress == addr) {
          if (!result && !cleanup) {
            HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, e.dwThreadId);
            CONTEXT c{};
            c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            if (th && GetThreadContext(th, &c)) {
              result = true;
              if (onHit(c)) SetThreadContext(th, &c);
            }
            if (th) CloseHandle(th);
            setDrAll(0);  // the process is frozen inside this event
          }
          return DBG_CONTINUE;
        }
        return DBG_EXCEPTION_NOT_HANDLED;
      }
    }
    return DBG_CONTINUE;
  };

  DEBUG_EVENT ev{};
  ULONGLONG deadline = GetTickCount64() + timeoutMs;
  while (!result && GetTickCount64() < deadline) {
    if (!WaitForDebugEvent(&ev, 100)) continue;
    DWORD st = handle(ev);
    ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, st);
    if (ev.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT) return false;
  }
  // Clear, drain what is already queued without arming a new thread (a timeout can meet a thread
  // being created), clear again, detach. A thread left armed after detach would crash the game.
  cleanup = true;
  setDrAll(0);
  for (int i = 0; i < 5 && WaitForDebugEvent(&ev, 50); ++i)
    ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, handle(ev));
  setDrAll(0);
  DebugActiveProcessStop(g.pid);
  if (!result && hint) std::puts("Timed out - nothing captured.");
  return result;
}

// Returns `reg` of the first thread that reaches `addr`.
std::optional<u64> capture(u64 addr, Reg reg, const char* hint, DWORD timeoutMs) {
  std::optional<u64> v;
  trap(addr, hint, timeoutMs, [&](CONTEXT& c) { v = reg == Reg::RDI ? c.Rdi : c.Rcx; return false; });
  return v;
}

struct GameCall { u64 fn, a1, a2; };

// Runs frozen() while the whole game is stopped at the entry of
// Controller::checkSortInventoryAfterUpdate (end of tick, update thread, where the game sorts
// inventories itself), so no robot, inserter, miner or checker can see a half-done edit. frozen()
// returns the calls the update thread must make next (at most maxCalls); the stub that makes them is
// written while the game is still stopped, so only what was really edited gets called. The thread is
// detoured through it: each call fn(a1, a2) in order, then a done flag, then a jump back. At a
// function entry only RCX (its one argument) is live; RDX is kept as well. False when no tick came
// (game paused) - then nothing ran.
//
// The stub is allocated before the game stops, sized for maxCalls, so a re-sync can never fail to fit
// after the edits are made. It is freed at the next edit, not when its flag is seen: the flag is
// set two instructions before the stub's last jump.
u64 stubToFree = 0;

template <class F> bool atEndOfTick(F frozen, size_t maxCalls = 0) {
  if (stubToFree) { VirtualFreeEx(g.proc, (LPVOID)stubToFree, 0, MEM_RELEASE); stubToFree = 0; }
  u64 size = 64 + 32 * (u64)maxCalls;  // 37 bytes of frame and exit, 32 per call; flag at the start
  u64 mem = maxCalls ? (u64)VirtualAllocEx(g.proc, nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE) : 0;
  if (maxCalls && !mem) { std::puts("Could not allocate the re-sync stub - nothing changed."); return false; }
  u64 flag = mem, code0 = mem + 16;
  bool detoured = false;
  bool hit = trap(L.sSortHook, nullptr, 3000, [&](CONTEXT& c) {
    cache.on = true;
    std::vector<GameCall> calls = frozen();
    cache = {};
    if (calls.empty()) return false;
    std::vector<uint8_t> code;
    auto b = [&](std::initializer_list<uint8_t> x) { code.insert(code.end(), x); };
    auto imm = [&](u64 v) { for (int i = 0; i < 8; ++i) code.push_back((uint8_t)(v >> (8 * i))); };
    b({0x51, 0x52, 0x48, 0x83, 0xEC, 0x28});  // push rcx; push rdx; sub rsp,28h (entry rsp = 8 mod 16 -> aligned)
    for (auto& k : calls) {
      b({0x48, 0xB9}); imm(k.a1);              // mov rcx, a1
      b({0x48, 0xBA}); imm(k.a2);              // mov rdx, a2
      b({0x48, 0xB8}); imm(k.fn);              // mov rax, fn
      b({0xFF, 0xD0});                         // call rax
    }
    b({0x48, 0x83, 0xC4, 0x28, 0x5A, 0x59});  // add rsp,28h; pop rdx; pop rcx
    b({0x48, 0xB8}); imm(flag);               // mov rax, flag
    b({0xC6, 0x00, 0x01});                    // mov byte [rax], 1
    b({0x48, 0xB8}); imm(L.sSortHook);        // mov rax, hook
    b({0xFF, 0xE0});                          // jmp rax
    SIZE_T n = 0;
    if (calls.size() > maxCalls || code0 + code.size() > mem + size ||
        !WriteProcessMemory(g.proc, (LPVOID)code0, code.data(), code.size(), &n) || n != code.size()) {
      std::puts("Warning: could not write the re-sync stub - network counts and chest icons may be stale.");
      return false;
    }
    FlushInstructionCache(g.proc, (LPCVOID)code0, code.size());
    c.Rip = code0;
    return detoured = true;
  });
  if (!detoured) {
    if (mem) VirtualFreeEx(g.proc, (LPVOID)mem, 0, MEM_RELEASE);  // never entered
    return hit;
  }
  for (int i = 0; i < 200; ++i, Sleep(10))
    if (rd<uint8_t>(flag).value_or(0)) {
      stubToFree = mem;
      return true;
    }
  std::puts("Warning: the game's re-sync call did not report back - network counts and chest icon may be stale.");
  return true;  // ponytail: stub left allocated; it may still be running
}

// ---------------------------------------------------------------- names
std::optional<std::string> readStdString(u64 base) {  // MSVC std::string
  auto len = rd<u64>(base + 0x10), cap = rd<u64>(base + 0x18);
  if (!len || !cap || *len < 1 || *len > 200 || *cap < *len) return std::nullopt;
  u64 src = *cap >= 16 ? ptr(base) : base;
  if (src < 0x10000) return std::nullopt;
  std::string s(*len, '\0');
  SIZE_T n = 0;
  if (!ReadProcessMemory(g.proc, (LPCVOID)src, s.data(), s.size(), &n) || n != s.size()) return std::nullopt;
  for (unsigned char ch : s) if (ch < 32 || ch > 126) return std::nullopt;
  return s;
}

struct ItemInfo { std::string name; bool unstackable = false; u32 stack = 0; };
std::unordered_map<u32, ItemInfo> items;
std::unordered_map<u32, std::string> fluids;

template <class F> int walkPrototypes(u64 list, F fn) {
  u64 b = ptr(list), e = ptr(list + 8);
  if (!b || e <= b) return 0;
  int c = 0;
  for (u64 i = 0; i < (e - b) / 8; ++i) {
    u64 p = ptr(b + i * 8);
    if (!p) continue;
    if (auto s = readStdString(p + L.protoName)) { fn((u32)i, p, *s); ++c; }
  }
  return c;
}

void reloadNames() {
  items.clear(); fluids.clear();
  int ni = walkPrototypes(L.sItems, [](u32 i, u64 p, const std::string& s) {
    items[i] = {s, (rd<u32>(p + L.itemFlags).value_or(0) & 0x80) != 0, rd<u32>(p + L.itemStackSize).value_or(0)};
  });
  int nf = walkPrototypes(L.sFluids, [](u32 i, u64, const std::string& s) { fluids[i] = s; });
  std::printf("Names loaded: %d items, %d fluids.\n", ni, nf);
}

// ---------------------------------------------------------------- inventory
u16 firstNonFull(u64 inv);
u16 lastNonEmpty(u64 inv);
std::vector<u64> sameKind(u64 anchor, int64_t reach);

bool looksLikeInventory(u64 a) {
  u64 arr = ptr(a + L.invData);
  u16 n = rd<u16>(a + L.invSize).value_or(0);
  return arr >= 0x10000 && n > 0 && n <= 20000 && rd<u32>(arr).has_value();
}

// The inventory picked by `inv`, `chest` or `inv click`, with what it looked like then. A reload or
// a mined chest frees it and the heap reuses the memory, so before every use it must still be the
// same object: same inventory vtable, same owner, same owner vtable.
struct Pick { u64 inv = 0, invVt = 0, owner = 0, ownerVt = 0; } pick;

Pick pickOf(u64 inv) {
  u64 owner = inv ? ptr(inv + L.invOwner) : 0;
  return inv ? Pick{inv, ptr(inv), owner, owner ? ptr(owner) : 0} : Pick{};
}

void setPick(u64 inv) { pick = pickOf(inv); }

// Still the same inventory of the same owner (a reload or a mined chest frees and reuses the memory).
bool stillThere(const Pick& p) {
  return p.inv && looksLikeInventory(p.inv) && ptr(p.inv) == p.invVt && ptr(p.inv + L.invOwner) == p.owner &&
         (!p.owner || ptr(p.owner) == p.ownerVt);
}

bool samePick(u64 inv) { return inv && inv == pick.inv && stillThere(pick); }

// The picked inventory, or 0 (with a message) when nothing is picked or it is gone.
u64 picked() {
  if (samePick(pick.inv)) return pick.inv;
  std::puts(pick.inv ? "The picked inventory is gone (reload, or the chest was removed) - pick it again (`inv` or `chest`)."
                     : "Pick an inventory first (`inv` or `chest`).");
  pick = {};
  return 0;
}

// The local player's character main inventory, or 0 (no character: map editor, dead, god mode).
// Only a CharacterController has a character; remote view stashes it, pausing parks it.
u64 myInventory() {
  u64 player = ptr(ptr(ptr(L.sGlobal) + L.gcGame) + L.gameLocalPlayer);
  if (!player) return 0;
  for (u64 slot : {L.playerController, L.playerPhysical, L.playerStashed, L.playerBeforePause}) {
    u64 ctrl = ptr(player + slot);
    if (!ctrl || ptr(ctrl) != L.vtCharCtrl || ptr(ctrl + L.ctrlPlayer) != player) continue;
    u64 character = ptr(ctrl + L.ccCharacter + L.targeterTarget);
    u64 inv = character ? character + L.charInventory : 0;
    if (inv && looksLikeInventory(inv)) return inv;
  }
  return 0;
}

// Entity whose GUI is open (top of Controller.guiTarget), or 0.
// MSVC deque of 0x70-byte elements: one element per block, block = (off + i) & (mapsize - 1).
u64 openEntity() {
  u64 player = ptr(ptr(ptr(L.sGlobal) + L.gcGame) + L.gameLocalPlayer);
  u64 ctrl = player ? ptr(player + L.playerController) : 0;
  if (!ctrl) return 0;
  u64 map = ptr(ctrl + L.guiMap), mapSize = ptr(ctrl + L.guiMapSize), first = ptr(ctrl + L.guiOff), n = ptr(ctrl + L.guiSize);
  if (!map || !n || !mapSize || (mapSize & (mapSize - 1))) return 0;
  u64 elem = ptr(map + ((first + n - 1) & (mapSize - 1)) * 8);
  // The target is a union keyed by openGuiType: for a trains/logistics/technology window it is not an
  // entity (0xBADC0DE seen). Accept only a pointer to an object with a vtable the PDB can name.
  u64 ent = elem ? ptr(elem + L.guiEntity) : 0;
  return ent && pdb::nameAt(ptr(ent)).ends_with("::`vftable'") ? ent : 0;
}

// Main inventory of the entity whose GUI is open, or 0: ContainerEntity.inventory (a pointer: chest,
// logistic chest, SE storage), else an embedded `inventory` / `trunkInventory` found by the entity's
// class (cargo wagon, car, spidertron). Accepted only if Inventory.owner points back at the entity.
std::string className(u64 obj) {
  std::string cls = pdb::nameAt(ptr(obj));
  return cls.substr(0, cls.find("::`vftable'"));
}

u64 mainInventory(u64 ent) {
  if (!ent) return 0;
  auto mine = [&](u64 inv) { return inv && looksLikeInventory(inv) && ptr(inv + L.invOwner) == ent + L.entPost; };
  if (u64 inv = ptr(ent + L.containerInv); mine(inv)) return inv;
  std::string cls = className(ent);
  for (const char* m : {"inventory", "trunkInventory"})
    if (auto o = pdb::tryOff(cls.c_str(), m); o && mine(ent + *o)) return ent + *o;
  return 0;
}

u64 openChest() { return mainInventory(openEntity()); }

void invList() {
  u64 inv = picked();
  if (!inv) return;
  u64 arr = ptr(inv + L.invData);
  u16 total = *rd<u16>(inv + L.invSize);
  bool synced = firstNonFull(inv) == rd<u16>(inv + L.invFirstNonFull).value_or(0xFFFF) &&
                lastNonEmpty(inv) == rd<u16>(inv + L.invLastNonEmpty).value_or(0xFFFF);
  std::string type = pdb::nameAt(ptr(inv));
  type = type.substr(0, type.find("::`vftable'"));
  u64 vt = ptr(inv);
  std::string bar = vt == L.vtInvBar || vt == L.vtInvFiltersBar ? ", bar at slot " + std::to_string(rd<u16>(inv + L.invBar).value_or(0)) : "";
  std::printf("Inventory %llX (%s), %u slots%s, indices %s\n", inv, type.c_str(), total, bar.c_str(),
              synced ? "in sync (editable)" : "NOT reproducible (edits refused)");
  for (u32 i = 0; i < total; ++i) {
    u64 a = arr + i * L.stackSize;
    u32 cnt = rd<u32>(a + L.stackCount).value_or(0);
    u16 id = rd<u16>(a + L.stackId).value_or(0);
    if (!id) continue;
    auto it = items.find(id);
    std::string name = it != items.end() ? it->second.name : "id " + std::to_string(id);
    std::string note;
    if (ptr(a + L.stackItem)) note = "[item data - not editable]";
    else if (it != items.end() && it->second.unstackable) note = "[unstackable]";
    else if (it != items.end()) note = "[max " + std::to_string(it->second.stack) + "]";
    std::printf("  %3u  %-40s #%-5u %8u  %s\n", i, name.c_str(), id, cnt, note.c_str());
  }
}

void invMine() {
  setPick(myInventory());
  if (!pick.inv) { std::puts("No character found (map editor, dead, or no save loaded)."); return; }
  invList();
}

void invChest() {
  setPick(openChest());
  if (!pick.inv) { std::puts("No chest, wagon or car window open - open one in game (or use `inv click`)."); return; }
  invList();
}

void invClick() {
  auto slot = capture(L.sInvHook, Reg::RDI, "click any item in an inventory (chest, car, your own)", 60000);
  if (!slot) return;
  u64 owner = ptr(*slot + L.stackOwner);
  if (!looksLikeInventory(owner)) { std::puts("That did not resolve to an inventory - try again."); return; }
  setPick(owner);
  invList();
}

// Returns a refusal reason, or nullptr when the slot was written.
const char* writeStack(u64 a, u32 count) {
  u16 id = rd<u16>(a + L.stackId).value_or(0);
  if (!id) return "empty slot - put one of the item there in game first";
  if (ptr(a + L.stackItem)) return "stack carries item data (armor, tools, spoilables)";
  if (count == 0) return "0 - an empty slot must be fully zeroed; drop the item in game instead";
  auto it = items.find(id);
  if (count > 1 && it != items.end() && it->second.unstackable) return "item is not stackable";
  return wr<u32>(a + L.stackCount, count) ? nullptr : "write failed";
}

// Inventory caches two indices that Inventory::checkConsistency recomputes and
// compares (a mismatch is a fatal error). Game::update runs ConsistencyScraper
// over every entity, so a stale index crashes the game minutes after an edit.
// Recompute them exactly like Inventory::computeFirstNonFullIndex and the
// lastNonEmpty loop in checkConsistency, using the prototype stack size as the
// slot limit. Before editing, the model must reproduce the cached values; an
// inventory with some other stack limit is refused instead of guessed.
u16 firstNonFull(u64 inv) {
  u64 arr = ptr(inv + L.invData);
  u16 total = rd<u16>(inv + L.invSize).value_or(0);
  for (u16 i = 0; i < total; ++i) {
    u64 a = arr + i * L.stackSize;
    u16 id = rd<u16>(a + L.stackId).value_or(0);
    auto it = items.find(id);
    if (!id || it == items.end() || rd<u32>(a + L.stackCount).value_or(0) < it->second.stack) return i;
  }
  return total;
}

u16 lastNonEmpty(u64 inv) {
  u64 arr = ptr(inv + L.invData);
  u16 n = rd<u16>(inv + L.invSize).value_or(0);
  while (n && !rd<u16>(arr + (n - 1) * L.stackSize + L.stackId).value_or(0)) --n;
  return n;
}

// Runs edit(i) for each inventory with the game stopped at the end of a tick (one stop for all),
// rewrites the cached indices, then re-syncs what the game derives from each inventory. The game
// updates those caches only by deltas from its own transfers: the container's top-4 items (chest
// picture, alt-mode icon), waiting inserters, circuit signals and the logistic network's per-item
// totals (LogisticPoint supplies). So the update thread makes the call Inventory::sort ends with,
// owner->inventorySorted(inventory), slot 0x18 of the owner's PostTransferNotifiable vtable.
// ContainerEntity/LogisticContainer::inventorySorted rebuild the frequency counter, wake inserters,
// re-evaluate signals and reInitSupply (recount). `extra` runs in the same stop (fluid wagons);
// `plan` gets the indices that passed the checks, before the first edit.
// Returns how many inventories were edited; -1 when nothing ran (paused).
template <class F, class X = void (*)(), class P = void (*)(const std::vector<size_t>&)>
int editInventories(const std::vector<u64>& invs, F edit, X extra = [] {}, const std::vector<std::string>& labels = {},
                    P plan = [](const std::vector<size_t>&) {}) {
  std::vector<u64> owners, fns;
  std::vector<std::vector<u64>> wakeLists(invs.size());
  for (size_t i = 0; i < invs.size(); ++i) {
    u64 owner = ptr(invs[i] + L.invOwner), fn = owner ? ptr(ptr(owner) + 0x18) : 0;
    if (fn && !pdb::nameAt(fn).ends_with("::inventorySorted")) {
      std::puts("Refused: the owner's vtable changed shape (inventorySorted not at slot 0x18) - re-check after a game update.");
      return 0;
    }
    owners.push_back(owner); fns.push_back(fn);
    // ContainerEntity::inventorySorted alarms only the input list (inserters waiting for room); the
    // ones waiting for items sleep on the output list until the game's own insert alarms it.
    u64 ent = owner ? owner - L.entPost : 0;
    std::string cls = ent && pdb::nameAt(ptr(ent)).ends_with("::`vftable'") ? className(ent) : "";
    if (!cls.empty())
      for (const char* m : {"inputWakeUpList", "outputWakeUpList", "wakeUpList"})
        if (auto o = pdb::tryOff(cls.c_str(), m)) wakeLists[i].push_back(owner - L.entPost + *o);
  }
  enum Why { OK, GONE, LIMITS };
  std::vector<Why> why(invs.size(), OK);
  int edited = 0;
  size_t maxCalls = 0;
  for (size_t i = 0; i < invs.size(); ++i) maxCalls += (fns[i] ? 1 : 0) + wakeLists[i].size();
  bool ran = atEndOfTick([&] {
    std::vector<GameCall> calls;
    std::vector<size_t> ok;
    for (size_t i = 0; i < invs.size(); ++i) {
      u64 inv = invs[i];
      if (!looksLikeInventory(inv) || ptr(inv + L.invOwner) != owners[i]) { why[i] = GONE; continue; }
      if (firstNonFull(inv) != rd<u16>(inv + L.invFirstNonFull).value_or(0xFFFF) ||
          lastNonEmpty(inv) != rd<u16>(inv + L.invLastNonEmpty).value_or(0xFFFF)) { why[i] = LIMITS; continue; }
      ok.push_back(i);
    }
    plan(ok);  // sees every inventory that will be edited before any is (balance)
    for (size_t i : ok) {
      u64 inv = invs[i];
      edit(i);
      wr<u16>(inv + L.invFirstNonFull, firstNonFull(inv));
      wr<u16>(inv + L.invLastNonEmpty, lastNonEmpty(inv));
      if (fns[i]) calls.push_back({fns[i], owners[i], inv});
      for (u64 list : wakeLists[i]) calls.push_back({L.sAlarm, list, 0});
      ++edited;
    }
    extra();
    return calls;
  }, maxCalls);
  if (!ran) { std::puts("Nothing changed: the game ran no tick (paused?) or the debugger could not attach."); return -1; }
  for (size_t i = 0; i < invs.size(); ++i) {
    std::string who = i < labels.size() ? labels[i] + ": " : "";
    if (why[i] == GONE) std::printf("%sNothing changed: the inventory is gone - pick it again.\n", who.c_str());
    if (why[i] == LIMITS) std::printf("%sRefused: this inventory's slot limits differ from item stack sizes (the trainer cannot keep its indices valid).\n", who.c_str());
  }
  return edited;
}

template <class F> bool editInventory(u64 inv, F edit) {
  return inv && editInventories({inv}, [&](size_t) { edit(); }) == 1;
}

void invSet(u32 slot, u32 count) {
  u64 inv = picked();
  if (!inv) return;
  u16 total = *rd<u16>(inv + L.invSize);
  if (slot >= total) { std::printf("Slot %u out of range (0..%u).\n", slot, total - 1u); return; }
  u64 a = ptr(inv + L.invData) + slot * L.stackSize;
  auto it = items.find(rd<u16>(a + L.stackId).value_or(0));
  // Inventory::canInsertCount adds stackSize - count as unsigned 32-bit: an overstacked slot wraps
  // it and the game promises room that is not there. Only your own inventory, where a ctrl+click
  // transfer splits the stack again.
  if (it != items.end() && count > it->second.stack && inv != myInventory()) {
    std::printf("Refused: more than a stack (%u) only in your own inventory (`inv`).\n", it->second.stack);
    return;
  }
  const char* why = nullptr;
  if (!editInventory(inv, [&] { why = writeStack(a, count); })) return;
  if (why) std::printf("Refused: %s.\n", why); else std::printf("Slot %u = %u\n", slot, count);
}

// Top up every simple stack in the inventory to its stack size; -1 when refused.
int invTopUp(u64 inv) {
  int n = 0;
  bool ok = editInventory(inv, [&] {
    u64 arr = ptr(inv + L.invData);
    u16 total = *rd<u16>(inv + L.invSize);
    for (u32 i = 0; i < total; ++i) {
      u64 a = arr + i * L.stackSize;
      auto it = items.find(rd<u16>(a + L.stackId).value_or(0));
      if (it == items.end() || it->second.unstackable || it->second.stack < 2) continue;
      if (rd<u32>(a + L.stackCount).value_or(0) < it->second.stack && !writeStack(a, it->second.stack)) ++n;
    }
  });
  return ok ? n : -1;
}

// The game's own test (ItemPrototype::shouldCreateItem): a stack carries an Item
// object when a virtual on the prototype returns true or the item can spoil.
// The vtable slot is read from shouldCreateItem's `call [rax+disp32]`; the
// virtual is a shared one-liner, `xor al,al; ret` meaning "no item data".
bool plainItem(u32 id) {
  u64 proto = ptr(ptr(L.sItems) + id * 8);
  if (!proto || rd<u64>(proto + L.itemSpoil).value_or(1) != 0) return false;
  uint8_t code[48]{};
  SIZE_T n = 0;
  ReadProcessMemory(g.proc, (LPCVOID)L.sShouldCreate, code, sizeof code, &n);
  for (size_t i = 0; i + 6 <= n; ++i)
    if (code[i] == 0xFF && code[i + 1] == 0x90) {  // call qword ptr [rax+disp32]
      int32_t slot;
      memcpy(&slot, code + i + 2, 4);
      u64 fn = ptr(ptr(proto) + slot);
      auto body = rd<u32>(fn);
      return body && (*body & 0xFFFFFF) == 0xC3C032;  // 32 C0 C3
    }
  return false;  // shouldCreateItem changed shape: refuse rather than guess
}

// A whole decimal number, or nothing.
std::optional<u32> number(const std::string& s) {
  u32 v = 0;
  auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  return ec == std::errc{} && end == s.data() + s.size() ? std::optional(v) : std::nullopt;
}

// An item id written #123 (as `find` and the listings show it), or nothing.
std::optional<u32> hashId(const std::string& s) {
  return s.size() > 1 && s[0] == '#' ? number(s.substr(1)) : std::nullopt;
}

// An item by internal name or by its id (#123).
u32 itemId(const std::string& name) {
  if (auto k = hashId(name); k && items.count(*k)) return *k;
  for (auto& [i, info] : items) if (info.name == name) return i;
  std::printf("No item '%s' (try `find`; item ids are written #123).\n", name.c_str());
  return 0;
}

// What `clear` and `chest clear` remove: item names, item ids (#123), slots (22) and slot ranges
// (22-90), separated by spaces or commas. Nothing selected means everything.
struct Selector {
  std::vector<u32> ids;
  std::vector<std::pair<u32, u32>> slots;
  bool empty() const { return ids.empty() && slots.empty(); }
  bool takes(u32 slot, u32 id) const {
    if (empty() || std::find(ids.begin(), ids.end(), id) != ids.end()) return true;
    for (auto [a, b] : slots) if (slot >= a && slot <= b) return true;
    return false;
  }
};

// Say which requested slots do not exist (slots run 0..size-1); once per inventory size.
void warnSlots(const std::vector<std::pair<u32, u32>>& slots, const std::vector<u64>& invs) {
  std::vector<u16> sizes;
  for (u64 inv : invs)
    if (u16 n = rd<u16>(inv + L.invSize).value_or(0); n && std::find(sizes.begin(), sizes.end(), n) == sizes.end()) sizes.push_back(n);
  for (u16 n : sizes)
    for (auto [a, b] : slots) {
      std::string r = a == b ? "Slot " + std::to_string(a) : "Slots " + std::to_string(a) + "-" + std::to_string(b);
      if (a >= n) std::printf("%s: past the last slot (%u) - ignored.\n", r.c_str(), n - 1u);
      else if (b >= n) std::printf("%s: only %u-%u exist.\n", r.c_str(), a, n - 1u);
    }
}

std::optional<Selector> parseSelector(const std::vector<std::string>& words) {
  Selector sel;
  for (auto& w : words) {
    std::istringstream parts(w);
    for (std::string t; std::getline(parts, t, ',');) {
      if (t.empty()) continue;
      size_t dash = t.find('-');
      auto lo = number(t.substr(0, dash)), hi = dash == std::string::npos ? lo : number(t.substr(dash + 1));
      if (lo && hi) {
        if (*lo > *hi) { std::printf("Slot range %s runs backwards.\n", t.c_str()); return std::nullopt; }
        sel.slots.push_back({*lo, *hi});
      } else if (u32 id = itemId(t)) sel.ids.push_back(id);
      else return std::nullopt;
    }
  }
  return sel;
}

// Where give may put an item: normal-quality stacks below the limit bar, in slots whose filter
// is empty or that item. Set up by canGive, which prints why when it refuses.
struct Placement {
  u16 limit = 0;
  u32 nFilters = 0;
  u64 fBegin = 0;
  u8 normal = 0;
  std::vector<std::pair<u32, u32>> slots;  // only these slot ranges (chest fill 10-15); empty: all
  bool inSlots(u32 i) const {
    if (slots.empty()) return true;
    for (auto [a, b] : slots) if (i >= a && i <= b) return true;
    return false;
  }
  bool accepts(u32 i, u32 id) const {
    if (i >= nFilters || !fBegin) return true;
    u16 f = rd<u16>(fBegin + i * L.filterSize + L.filterId).value_or(1);
    return f == 0 || f == id;
  }
};

bool canGive(u64 inv, u32 id, Placement& pl) {
  if (!plainItem(id) || items[id].unstackable || !items[id].stack) {
    std::puts("Refused: that item carries per-item data (ammo, armor, tools, spoilables, equipment).");
    return false;
  }
  walkPrototypes(L.sQualities, [&](u32 i, u64, const std::string& s) { if (s == "normal") pl.normal = (u8)i; });
  if (!pl.normal) { std::puts("Could not find the 'normal' quality."); return false; }
  if (L.stackId != L.stackCount + 4 || L.stackQuality != L.stackId + 2 || L.stackCap != L.stackQuality + 1) {
    std::puts("ItemStack layout changed - give is disabled until it is re-checked.");
    return false;
  }
  u64 vt = ptr(inv), filters = 0;
  if (vt == L.vtInvFilters) filters = ptr(inv + L.filtersPlain);
  else if (vt == L.vtInvFiltersBar) filters = ptr(inv + L.filtersBar);
  else if (vt != L.vtInv && vt != L.vtInvBar) { std::puts("Refused: this inventory type has its own stack or weight limits."); return false; }
  u16 total = *rd<u16>(inv + L.invSize);
  pl.limit = vt == L.vtInvBar || vt == L.vtInvFiltersBar ? std::min<u16>(total, rd<u16>(inv + L.invBar).value_or(0)) : total;
  pl.nFilters = filters ? rd<u32>(filters + L.holderSize).value_or(0) : 0;
  pl.fBegin = filters ? ptr(filters + L.holderBegin) : 0;
  return true;
}

// Put up to `want` of a plain item: top up same-item normal stacks, then fill empty slots
// (count, id, quality, cached stack size in one write). Never overstacks. Returns how many.
u64 placeItems(u64 inv, const Placement& pl, u32 id, u64 want) {
  u32 stack = items[id].stack;
  u64 arr = ptr(inv + L.invData), left = want;
  for (u32 i = 0; i < pl.limit && left; ++i) {
    u64 a = arr + i * L.stackSize;
    u32 cnt = rd<u32>(a + L.stackCount).value_or(0);
    if (!pl.inSlots(i) || rd<u16>(a + L.stackId).value_or(0) != id || ptr(a + L.stackItem) || rd<u8>(a + L.stackQuality).value_or(0) != pl.normal || cnt >= stack) continue;
    u32 add = (u32)std::min<u64>(stack - cnt, left);
    if (wr<u32>(a + L.stackCount, cnt + add)) left -= add;
  }
  for (u32 i = 0; i < pl.limit && left; ++i) {
    u64 a = arr + i * L.stackSize;
    if (!pl.inSlots(i) || rd<u64>(a).value_or(1) != 0 || ptr(a + L.stackItem) || !pl.accepts(i, id)) continue;
    u32 put = (u32)std::min<u64>(stack, left);
    u64 packed = put | (u64)id << 32 | (u64)pl.normal << 48 | (u64)std::min<u32>(stack, 255) << 56;
    if (wr<u64>(a, packed)) left -= put;
  }
  return want - left;
}

// How many of `id` placeItems could put in once every simple stack of `freed` is removed.
u64 room(u64 inv, const Placement& pl, u32 id, u32 freed) {
  u32 stack = items[id].stack;
  u64 arr = ptr(inv + L.invData), n = 0;
  for (u32 i = 0; i < pl.limit; ++i) {
    u64 a = arr + i * L.stackSize;
    u16 sid = rd<u16>(a + L.stackId).value_or(0);
    bool data = ptr(a + L.stackItem) != 0;
    u32 cnt = rd<u32>(a + L.stackCount).value_or(0);
    if (sid == id && !data && rd<u8>(a + L.stackQuality).value_or(0) == pl.normal) n += cnt < stack ? stack - cnt : 0;
    else if ((rd<u64>(a).value_or(1) == 0 || sid == freed) && !data && pl.accepts(i, id)) n += stack;
  }
  return n;
}

// Zero every simple stack of `id` (any quality): count, id, quality, cached stack size.
u64 removeItems(u64 inv, u32 id, int* kept = nullptr) {
  u64 arr = ptr(inv + L.invData), removed = 0;
  u16 total = *rd<u16>(inv + L.invSize);
  for (u32 i = 0; i < total; ++i) {
    u64 a = arr + i * L.stackSize;
    if (rd<u16>(a + L.stackId).value_or(0) != id) continue;
    if (ptr(a + L.stackItem)) { if (kept) ++*kept; continue; }
    u32 cnt = rd<u32>(a + L.stackCount).value_or(0);
    if (wr<u64>(a, 0)) removed += cnt;
  }
  return removed;
}

// Zero every simple stack the selector takes (everything when it is empty); per-name totals go to
// `removed`, stacks with item data are counted in `kept` and left in place.
void clearStacks(u64 inv, const Selector& sel, std::map<std::string, u64>& removed, int& kept) {
  u64 arr = ptr(inv + L.invData);
  u16 total = rd<u16>(inv + L.invSize).value_or(0);
  for (u32 j = 0; j < total; ++j) {
    u64 a = arr + j * L.stackSize;
    u16 sid = rd<u16>(a + L.stackId).value_or(0);
    if (!sid || !sel.takes(j, sid)) continue;
    if (ptr(a + L.stackItem)) { ++kept; continue; }
    u32 cnt = rd<u32>(a + L.stackCount).value_or(0);
    if (wr<u64>(a, 0)) removed[items.count(sid) ? items[sid].name : "id " + std::to_string(sid)] += cnt;
  }
}

void printRemoved(const std::map<std::string, u64>& removed, int kept) {
  for (auto& [name, n] : removed) std::printf("  -%llu %s\n", n, name.c_str());
  if (kept) std::printf("  %d stacks with item data left in place\n", kept);
}

// Remove what the selector names (items, #ids, slots) from the picked inventory; something must be
// named, so a typo never empties your own inventory.
void clearPicked(const std::vector<std::string>& words) {
  u64 inv = picked();
  auto sel = inv ? parseSelector(words) : std::nullopt;
  if (!sel) return;
  if (sel->empty()) { std::puts("Usage: clear <items, #ids, slots 22 or 22-90>"); return; }
  warnSlots(sel->slots, {inv});
  std::map<std::string, u64> removed;
  int kept = 0;
  if (!editInventory(inv, [&] { clearStacks(inv, *sel, removed, kept); })) return;
  printRemoved(removed, kept);
  if (removed.empty()) std::puts("  nothing to remove");
}

constexpr u64 FILL = ~0ull;  // give(): as many as fit

// Spawn `want` of a plain item into the picked inventory as normal-quality stacks.
void give(const std::string& name, u64 want, const std::vector<std::pair<u32, u32>>& slots = {}) {
  u64 inv = picked();
  u32 id = inv ? itemId(name) : 0;
  Placement pl;
  if (!id || !canGive(inv, id, pl)) return;
  pl.slots = slots;
  u64 put = 0;
  if (!editInventory(inv, [&] { put = placeItems(inv, pl, id, want); })) return;
  if (want == FILL) std::printf("Filled with %llu %s.\n", put, items[id].name.c_str());
  else std::printf("Gave %llu %s%s.\n", put, items[id].name.c_str(), put < want ? " (inventory full)" : "");
}

// Replace every simple stack of `from` with the same number of `to`, in one edit, and only when
// all of it fits under the same rules give uses (bar, filters, `to`'s stack size).
void swapItems(const std::string& from, const std::string& to) {
  u64 inv = picked();
  u32 fid = inv ? itemId(from) : 0, tid = fid ? itemId(to) : 0;
  Placement pl;
  if (!tid) return;
  if (fid == tid) { std::puts("Same item."); return; }
  if (!canGive(inv, tid, pl)) return;
  u64 have = 0, fits = 0, put = 0;
  bool done = false;
  bool ok = editInventory(inv, [&] {
    u64 arr = ptr(inv + L.invData);
    u16 total = *rd<u16>(inv + L.invSize);
    for (u32 i = 0; i < total; ++i) {
      u64 a = arr + i * L.stackSize;
      if (rd<u16>(a + L.stackId).value_or(0) == fid && !ptr(a + L.stackItem)) have += rd<u32>(a + L.stackCount).value_or(0);
    }
    fits = room(inv, pl, tid, fid);
    if (!have || have > fits) return;  // nothing written
    put = placeItems(inv, pl, tid, removeItems(inv, fid));
    done = true;
  });
  if (!ok) return;
  const char *f = items[fid].name.c_str(), *t = items[tid].name.c_str();  // names, also when ids were typed
  if (!have) std::printf("No %s here.\n", f);
  else if (!done) std::printf("Refused: %llu %s do not fit as %s here (room for %llu).\n", have, f, t, fits);
  else std::printf("Swapped %llu %s for %llu %s.\n", have, f, put, t);
}

u16 firstItem(u64 inv) {
  u64 arr = ptr(inv + L.invData);
  u16 total = rd<u16>(inv + L.invSize).value_or(0), id = 0;
  for (u32 i = 0; i < total && !id; ++i) id = rd<u16>(arr + i * L.stackSize + L.stackId).value_or(0);
  return items.count(id) ? id : 0;
}

// ---------------------------------------------------------------- chest
// Fill every free slot (up to the bar) of the open chest with the named item, or the item in its
// first occupied slot.
bool chestFill(const Selector& sel = {}) {
  setPick(openChest());
  if (!pick.inv) { std::puts("No chest, wagon or car window open."); return false; }
  u32 id = sel.ids.empty() ? firstItem(pick.inv) : sel.ids[0];
  if (!id) { if (sel.ids.empty()) std::puts("The chest is empty - name an item: chest fill <item>."); return false; }
  warnSlots(sel.slots, {pick.inv});
  give(items[id].name, FILL, sel.slots);
  return true;
}

// Every chest of the open chest's kind (same class and prototype) in its block: a flood fill from
// the open chest over chests whose centres are at most w + max(1, w/2) tiles apart (w = prototype
// size; 6x6 warehouses: 9), like `ore`. Each is accepted only if its main inventory's owner points
// back at it. Labels say where each one is.
struct Block { std::vector<u64> invs; std::vector<std::string> labels; };

// "<prototype name> at x,y" (tiles).
std::string entityLabel(u64 ent) {
  return readStdString(ptr(ent + L.entProto) + L.protoName).value_or("?") + " at " +
         std::to_string(rd<int32_t>(ent + L.entX).value_or(0) >> 8) + "," + std::to_string(rd<int32_t>(ent + L.entY).value_or(0) >> 8);
}

Block openBlock() {
  u64 ent = openEntity();
  if (!mainInventory(ent)) { std::puts("Open one chest of the block first."); return {}; }
  u64 proto = ptr(ent + L.entProto);
  int32_t w = std::max(rd<int32_t>(proto + L.protoTileSize).value_or(1), rd<int32_t>(proto + L.protoTileSize + 4).value_or(1));
  int64_t reach = (int64_t)(w + std::max(1, w / 2)) * 256;  // positions are 1/256 tile
  auto block = sameKind(ent, reach);
  if (block.empty()) { std::puts("The open chest is not linked into its tile - refused."); return {}; }
  std::string kind = readStdString(proto + L.protoName).value_or("?");
  Block b;
  for (u64 a : block)
    if (u64 inv = mainInventory(a)) { b.invs.push_back(inv); b.labels.push_back(entityLabel(a)); }
  std::printf("Block: %zu %s.\n", b.invs.size(), kind.c_str());
  return b;
}

// Fill every chest of the block in one game stop, each with its own first item or the named one.
void chestFillAll(const std::string& name, const std::vector<std::pair<u32, u32>>& slots = {}) {
  u32 item = 0;
  if (!name.empty() && !(item = itemId(name))) return;
  Block b = openBlock();
  warnSlots(slots, b.invs);
  std::vector<u64> invs;
  std::vector<u32> ids;
  std::vector<Placement> pls;
  std::vector<std::string> labels;
  int empty = 0;
  for (size_t k = 0; k < b.invs.size(); ++k) {
    u64 inv = b.invs[k];
    u32 id = item ? item : firstItem(inv);
    if (!id) { ++empty; continue; }
    Placement pl;
    if (!canGive(inv, id, pl)) continue;  // says why
    pl.slots = slots;
    invs.push_back(inv); ids.push_back(id); pls.push_back(pl); labels.push_back(b.labels[k]);
  }
  std::map<std::string, std::pair<u64, int>> put;  // item -> (count, chests)
  int full = 0;
  if (!invs.empty() && editInventories(invs, [&](size_t i) {
        u64 n = placeItems(invs[i], pls[i], ids[i], FILL);
        if (!n) { ++full; return; }
        auto& e = put[items[ids[i]].name];
        e.first += n; ++e.second;
      }, [] {}, labels) < 0) return;
  for (auto& [n, e] : put) std::printf("  +%llu %s (%d chests)\n", e.first, n.c_str(), e.second);
  if (full) std::printf("  %d already full\n", full);
  if (empty) std::printf("  %d empty skipped (name an item to fill those: chest fill all <item>)\n", empty);
}

// Empty the open chest (one) or every chest of its block (all) in one game stop: everything, or
// only what the selector names (items, #ids, slots; with `all` the same slots in every chest).
void chestClear(bool all, const std::vector<std::string>& words) {
  auto sel = parseSelector(words);
  if (!sel) return;
  Block b;
  if (all) b = openBlock();
  else if (setPick(openChest()), pick.inv) b = {{pick.inv}, {}};
  else std::puts("No chest, wagon or car window open.");
  if (b.invs.empty()) return;
  warnSlots(sel->slots, b.invs);
  std::map<std::string, u64> removed;
  int kept = 0;
  if (editInventories(b.invs, [&](size_t i) { clearStacks(b.invs[i], *sel, removed, kept); }, [] {}, b.labels) < 0) return;
  printRemoved(removed, kept);
  if (removed.empty()) std::puts("  nothing to remove");
}

// Chests picked one by one (`chest add`), wherever they are; re-checked before every use.
std::vector<Pick> chestList;
std::vector<std::string> chestLabels;

void chestAdd() {
  u64 ent = openEntity(), inv = mainInventory(ent);
  if (!inv) { std::puts("No chest, wagon or car window open."); return; }
  for (auto& p : chestList) if (p.inv == inv) { std::puts("Already in the list."); return; }
  chestList.push_back(pickOf(inv));
  chestLabels.push_back(entityLabel(ent));
  std::printf("Added %s (%zu in the list).\n", chestLabels.back().c_str(), chestList.size());
}

void chestShowList() {
  if (chestList.empty()) std::puts("The list is empty: open a chest, `chest add`; repeat.");
  for (size_t k = 0; k < chestList.size(); ++k)
    std::printf("  %zu  %s%s\n", k + 1, chestLabels[k].c_str(), stillThere(chestList[k]) ? "" : "  (gone)");
}

// Split `total` over chests with room `cap`: smallest room first, each takes what fits of an even
// share and the rest share the remainder. Empty when it does not all fit.
constexpr std::vector<u64> shares(const std::vector<u64>& cap, u64 total) {
  std::vector<size_t> order(cap.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::sort(order.begin(), order.end(), [&](size_t x, size_t y) { return cap[x] < cap[y]; });
  std::vector<u64> out(cap.size());
  for (size_t k = 0; k < order.size(); ++k) {
    size_t i = order[k];
    out[i] = std::min(cap[i], (total + order.size() - k - 1) / (order.size() - k));
    total -= out[i];
  }
  return total ? std::vector<u64>{} : out;
}
static_assert(shares({100, 100, 100}, 10) == std::vector<u64>{4, 3, 3});
static_assert(shares({2, 100, 100}, 10) == std::vector<u64>{2, 4, 4});
static_assert(shares({1, 1}, 3).empty());

// Even out one item over the listed chests (or the open chest's block) in one game stop: every
// simple normal-quality stack of it is taken out and the total put back in equal shares. A chest
// with less room (bar, filters, other items) gets what fits and the others share the rest, so
// nothing is created or lost. Other qualities and stacks with item data stay where they are.
void chestBalance(bool all, const std::string& name) {
  Block b;
  if (all) b = openBlock();
  else
    for (size_t k = 0; k < chestList.size(); ++k)
      if (stillThere(chestList[k])) { b.invs.push_back(chestList[k].inv); b.labels.push_back(chestLabels[k]); }
      else std::printf("%s: gone - skipped (`chest reset` and add it again).\n", chestLabels[k].c_str());
  if (b.invs.size() < 2) {
    std::puts(all ? "Nothing to balance: the block has one chest." : "Add at least two chests: open one, `chest add`; repeat.");
    return;
  }
  u32 id = name.empty() ? firstItem(b.invs[0]) : itemId(name);
  if (!id) { if (name.empty()) std::puts("The first chest is empty - name an item: chest balance <item>."); return; }
  size_t n = b.invs.size();
  std::vector<Placement> pls(n);
  for (size_t k = 0; k < n; ++k) if (!canGive(b.invs[k], id, pls[k])) return;  // says why
  u32 stack = items[id].stack;
  auto movable = [&](u64 a) {
    return rd<u16>(a + L.stackId).value_or(0) == id && !ptr(a + L.stackItem) && rd<u8>(a + L.stackQuality).value_or(0) == pls[0].normal;
  };
  std::vector<u64> had(n), target(n), placed(n);
  u64 total = 0;
  bool planned = false;
  auto plan = [&](const std::vector<size_t>& ok) {
    std::vector<u64> cap(n);
    for (size_t i : ok) {
      u64 arr = ptr(b.invs[i] + L.invData);
      u16 size = rd<u16>(b.invs[i] + L.invSize).value_or(0);
      for (u32 s = 0; s < size; ++s) {
        u64 a = arr + s * L.stackSize;
        bool mine = movable(a);
        if (mine) had[i] += rd<u32>(a + L.stackCount).value_or(0);
        if (s < pls[i].limit && (mine || (rd<u64>(a).value_or(1) == 0 && !ptr(a + L.stackItem))) && pls[i].accepts(s, id)) cap[i] += stack;
      }
      total += had[i];
    }
    std::vector<u64> okCap;
    for (size_t i : ok) okCap.push_back(cap[i]);
    auto split = shares(okCap, total);
    for (size_t k = 0; k < split.size(); ++k) target[ok[k]] = split[k];
    planned = ok.size() >= 2 && !split.empty();
  };
  int edited = editInventories(b.invs, [&](size_t i) {
    if (!planned) return;
    u64 arr = ptr(b.invs[i] + L.invData);
    u16 size = rd<u16>(b.invs[i] + L.invSize).value_or(0);
    for (u32 s = 0; s < size; ++s) if (movable(arr + s * L.stackSize)) wr<u64>(arr + s * L.stackSize, 0);
    placed[i] = placeItems(b.invs[i], pls[i], id, target[i]);
  }, [] {}, b.labels, plan);
  if (edited < 0) return;
  const char* item = items[id].name.c_str();
  if (!planned) { std::printf("Refused: %llu %s do not fit back in (items beyond a bar?) - nothing changed.\n", total, item); return; }
  std::printf("Balanced %llu %s over %d chests:\n", total, item, edited);
  u64 lost = 0;
  for (size_t i = 0; i < n; ++i) {
    if (!had[i] && !target[i]) continue;
    std::printf("  %-40s %8llu -> %llu\n", b.labels[i].c_str(), had[i], placed[i]);
    lost += target[i] - placed[i];
  }
  if (lost) std::printf("WARNING: %llu %s could not be placed back.\n", lost, item);
}

// ---------------------------------------------------------------- trains
// Rolling stock of the train whose wagon or locomotive window is open, from one end to the other.
// A wagon's front can face either way, so each step takes whichever neighbour link is not the
// stock we came from.
std::vector<u64> openTrain() {
  u64 ent = openEntity();
  if (!ent || !pdb::tryOff(className(ent).c_str(), "connectedToFront")) return {};
  auto next = [](u64 cur, u64 prev) {
    for (u64 o : {L.rsFront, L.rsBack})
      if (u64 n = ptr(cur + o); n && n != prev && !className(n).empty()) return n;
    return (u64)0;
  };
  u64 prev = 0, end = ent;
  for (int k = 0; k < 1000; ++k) {
    u64 n = next(end, prev);
    if (!n) break;
    prev = end; end = n;
  }
  std::vector<u64> all;
  prev = 0;
  for (u64 cur = end; cur && all.size() < 1000;) {
    all.push_back(cur);
    u64 n = next(cur, prev);
    prev = cur; cur = n;
  }
  return all;
}

// Names (items, #ids, fluids; a name can be both item and fluid) and cargo slots (22, 22-90),
// separated by spaces or commas; false, with a message, on an unknown name.
bool parseNames(const std::vector<std::string>& words, std::vector<u32>& itemIds, std::vector<u16>& fluidIds,
                std::vector<std::pair<u32, u32>>& slots) {
  std::vector<std::string> names;
  for (auto& w : words) {
    std::istringstream parts(w);
    for (std::string t; std::getline(parts, t, ',');) if (!t.empty()) names.push_back(t);
  }
  for (auto& n : names) {
    size_t dash = n.find('-');
    auto lo = number(n.substr(0, dash)), hi = dash == std::string::npos ? lo : number(n.substr(dash + 1));
    if (lo && hi) {
      if (*lo > *hi) { std::printf("Slot range %s runs backwards.\n", n.c_str()); return false; }
      slots.push_back({*lo, *hi});
      continue;
    }
    bool found = false;
    for (auto& [i, info] : items) if (info.name == n || hashId(n) == i) { itemIds.push_back(i); found = true; }
    for (auto& [i, f] : fluids) if (f == n) { fluidIds.push_back((u16)i); found = true; }
    if (!found) { std::printf("No item or fluid named '%s' (try `find`).\n", n.c_str()); return false; }
  }
  return true;
}

// The "inactivity" wait condition counts from Train.schedulable.tickOfLastActivity; the game
// stamps it with Map.entityTick when a wagon's contents change (FluidWagon::setFluid).
void stampActivity(u64 stock) {
  u64 train = ptr(stock + L.rsTrain);
  u64 map = train ? ptr(ptr(train + L.trainManager) + L.managerMap) : 0;
  if (auto tick = map ? rd<u64>(map + L.mapTick) : std::nullopt) wr<u64>(train + L.trainActivity, *tick);
}

struct Fl { u16 id; u16 pad; float temperature; int64_t amount; };  // Fluid
static_assert(sizeof(Fl) == 16);

// Fill every cargo wagon's free slots (bar and filters respected) and every fluid wagon to
// capacity, in one game stop. Without names each wagon uses what it already holds (first item /
// its fluid); a named item goes into every cargo wagon, a named fluid into every fluid wagon.
// A fluid wagon holding another fluid is left alone. Fluid wagons: FluidWagon::setFluid writes the
// Fluid and stamps the train's tickOfLastActivity (the "inactivity" wait condition); same here.
void trainFill(const std::vector<std::string>& names) {
  auto stock = openTrain();
  if (stock.empty()) { std::puts("Open a wagon or locomotive window of the train first."); return; }
  std::vector<u32> itemIds;
  std::vector<u16> fluidIds;
  std::vector<std::pair<u32, u32>> slots;  // cargo slots only, the same in every wagon
  if (!parseNames(names, itemIds, fluidIds, slots)) return;
  u32 item = itemIds.empty() ? 0 : itemIds.back();
  u16 fluid = fluidIds.empty() ? 0 : fluidIds.back();
  bool none = itemIds.empty() && fluidIds.empty();
  bool doCargo = none || item, doFluid = (none && slots.empty()) || fluid;
  if (L.fluidTemp != L.fluidId + 4 || L.fluidAmt != L.fluidId + 8) { std::puts("Fluid layout changed - refused."); return; }

  std::vector<u64> invs;
  std::vector<u32> ids;
  std::vector<Placement> pls;
  std::vector<std::string> labels;
  struct FluidJob { u64 ent; u16 id; float temp; int64_t cap; int64_t added = 0; };
  std::vector<FluidJob> jobs;
  int cargo = 0, fluidWagons = 0, emptyCargo = 0, emptyFluid = 0;
  for (size_t k = 0; k < stock.size(); ++k) {
    std::string cls = className(stock[k]), label = "Wagon " + std::to_string(k + 1);
    if (cls == "CargoWagon") {
      ++cargo;
      u64 inv = mainInventory(stock[k]);
      if (!doCargo) continue;
      if (!inv) { std::printf("%s: no inventory found - skipped.\n", label.c_str()); continue; }
      u32 id = item ? item : firstItem(inv);
      if (!id) { ++emptyCargo; continue; }
      Placement pl;
      if (!canGive(inv, id, pl)) continue;  // says why
      pl.slots = slots;
      invs.push_back(inv); ids.push_back(id); pls.push_back(pl); labels.push_back(label);
    } else if (cls == "FluidWagon") {
      ++fluidWagons;
      if (!doFluid) continue;
      Fl cur{};
      int64_t cap = rd<int64_t>(stock[k] + L.fwCapacity).value_or(0);
      if (!rd(stock[k] + L.fwFluid, cur) || cap <= 0) { std::printf("%s: fluid not readable - skipped.\n", label.c_str()); continue; }
      u16 id = fluid ? fluid : cur.id;
      if (!id) { ++emptyFluid; continue; }
      if (cur.id && cur.amount > 0 && cur.id != id) { std::printf("%s holds %s - left alone.\n", label.c_str(), fluids[cur.id].c_str()); continue; }
      u64 proto = ptr(ptr(L.sFluids) + id * 8);
      float temp = cur.id == id && cur.amount > 0 ? cur.temperature : rd<float>(proto + L.fpDefaultTemp).value_or(15);
      temp = std::min(temp, rd<float>(proto + L.fpMaxTemp).value_or(temp));
      jobs.push_back({stock[k], id, temp, cap});
    }
  }
  std::printf("Train: %zu vehicles, %d cargo wagons, %d fluid wagons.\n", stock.size(), cargo, fluidWagons);
  warnSlots(slots, invs);
  if (invs.empty() && jobs.empty()) {
    std::puts(names.empty() ? "Nothing to fill (empty wagons need a name: train fill <item or fluid>)." : "Nothing to fill.");
    return;
  }

  std::map<std::string, std::pair<u64, int>> put;  // item -> (count, wagons it went into)
  int full = 0;
  int edited = editInventories(invs, [&](size_t i) {
    u64 n = placeItems(invs[i], pls[i], ids[i], FILL);
    if (!n) { ++full; return; }
    auto& e = put[items[ids[i]].name];
    e.first += n; ++e.second;
  }, [&] {
    bool changed = !put.empty();
    for (auto& j : jobs) {
      Fl cur{};
      if (!rd(j.ent + L.fwFluid, cur) || (cur.id && cur.amount > 0 && cur.id != j.id) || cur.amount >= j.cap) continue;
      Fl f{j.id, cur.pad, j.temp, j.cap};
      if (wr(j.ent + L.fwFluid, f)) { j.added = j.cap - (cur.id == j.id ? cur.amount : 0); changed = true; }
    }
    if (changed) stampActivity(stock[0]);
  }, labels);
  if (edited < 0) return;
  for (auto& [name, e] : put) std::printf("  +%llu %s (%d wagons)\n", e.first, name.c_str(), e.second);
  std::map<std::string, std::pair<double, int>> added;
  int fullFluid = 0;
  for (auto& j : jobs) {
    if (!j.added) { ++fullFluid; continue; }
    auto& e = added[fluids[j.id]];
    e.first += j.added / FLUID_SCALE; ++e.second;
  }
  for (auto& [name, e] : added) std::printf("  +%.0f %s (%d wagons)\n", e.first, name.c_str(), e.second);
  if (full || fullFluid) std::printf("  %d already full\n", full + fullFluid);
  if (emptyCargo || emptyFluid) std::printf("  %d empty skipped (name an item or fluid to fill those)\n", emptyCargo + emptyFluid);
}

// Empty the open train in one game stop: without names every simple stack of every cargo wagon and
// every fluid wagon; with names only those items (cargo wagons) and fluids (fluid wagons). Stacks
// with item data stay. An empty fluid is all zero (fluid 0, temperature 0, amount 0), as setFluid
// leaves it.
void trainClear(const std::vector<std::string>& names) {
  auto stock = openTrain();
  if (stock.empty()) { std::puts("Open a wagon or locomotive window of the train first."); return; }
  std::vector<u32> itemIds;
  std::vector<u16> fluidIds;
  std::vector<std::pair<u32, u32>> slots;  // cargo slots, the same in every wagon
  if (!parseNames(names, itemIds, fluidIds, slots)) return;
  bool doCargo = names.empty() || !itemIds.empty() || !slots.empty(), doFluid = names.empty() || !fluidIds.empty();
  if (L.fluidTemp != L.fluidId + 4 || L.fluidAmt != L.fluidId + 8) { std::puts("Fluid layout changed - refused."); return; }

  std::vector<u64> invs, tanks;
  std::vector<std::string> labels;
  int cargo = 0, fluidWagons = 0;
  for (size_t k = 0; k < stock.size(); ++k) {
    std::string cls = className(stock[k]), label = "Wagon " + std::to_string(k + 1);
    if (cls == "CargoWagon") {
      ++cargo;
      if (!doCargo) continue;
      if (u64 inv = mainInventory(stock[k])) { invs.push_back(inv); labels.push_back(label); }
      else std::printf("%s: no inventory found - skipped.\n", label.c_str());
    } else if (cls == "FluidWagon") {
      ++fluidWagons;
      if (doFluid) tanks.push_back(stock[k]);
    }
  }
  std::printf("Train: %zu vehicles, %d cargo wagons, %d fluid wagons.\n", stock.size(), cargo, fluidWagons);
  warnSlots(slots, invs);
  if (invs.empty() && tanks.empty()) { std::puts("Nothing to clear."); return; }

  auto wanted = [](auto& list, auto id) { return list.empty() || std::find(list.begin(), list.end(), id) != list.end(); };
  std::map<std::string, u64> removed;
  std::map<std::string, double> drained;
  int kept = 0;
  Selector sel{itemIds, slots};
  int edited = editInventories(invs, [&](size_t i) { clearStacks(invs[i], sel, removed, kept); }, [&] {
    bool changed = !removed.empty();
    for (u64 t : tanks) {
      Fl cur{};
      if (!rd(t + L.fwFluid, cur) || !cur.id || !wanted(fluidIds, cur.id)) continue;
      Fl none{0, cur.pad, 0.0f, 0};
      if (wr(t + L.fwFluid, none)) { drained[fluids[cur.id]] += cur.amount / FLUID_SCALE; changed = true; }
    }
    if (changed) stampActivity(stock[0]);
  }, labels);
  if (edited < 0) return;
  printRemoved(removed, kept);
  for (auto& [name, n] : drained) std::printf("  -%.0f %s\n", n, name.c_str());
  if (removed.empty() && drained.empty()) std::puts("  nothing to remove");
}

// ---------------------------------------------------------------- fluids
u64 fluSeg = 0;

bool looksLikeSegment(u64 b) {
  auto id = rd<u16>(b + L.fluId); auto vol = rd<int64_t>(b + L.fluVol), amt = rd<int64_t>(b + L.fluAmt);
  return id && vol && amt && *id && *id <= 0x0FFF && *vol > 0 && *vol <= 1e15 && *amt >= 0 && *amt <= *vol * 4;
}

void fluShow() {
  if (!fluSeg || !looksLikeSegment(fluSeg)) { std::puts("No live segment - use `fluid` (stale after pipe changes)."); return; }
  u16 id = *rd<u16>(fluSeg + L.fluId);
  double amt = *rd<int64_t>(fluSeg + L.fluAmt) / FLUID_SCALE, vol = *rd<int64_t>(fluSeg + L.fluVol) / FLUID_SCALE;
  std::string name = fluids.count(id) ? fluids[id] : "id " + std::to_string(id);
  std::printf("Segment %llX  %s  %.0f / %.0f (%.0f%%)  %.1f C\n", fluSeg, name.c_str(), amt, vol, 100 * amt / vol,
              rd<float>(fluSeg + L.fluTemp).value_or(0));
}

// Ctrl+Shift+F1: segment under the cursor (FluidSegment::addToDescription fires on hover).
bool fluCapture() {
  auto b = capture(L.sFluHook, Reg::RCX, nullptr, 1500);
  if (!b || !looksLikeSegment(*b)) return false;
  fluSeg = *b;
  return true;
}

bool fluTopUp() { return wr<int64_t>(fluSeg + L.fluAmt, *rd<int64_t>(fluSeg + L.fluVol)); }

// Fluid segment of the tank/pipe whose window is open: any entity class with a `fluidBox` member.
u64 openFluidSegment(std::string* what = nullptr) {
  u64 ent = openEntity();
  if (!ent) return 0;
  std::string cls = pdb::nameAt(ptr(ent));
  auto cut = cls.find("::`vftable'");
  if (cut == std::string::npos) return 0;
  cls.resize(cut);
  if (what) *what = cls;
  auto boxOff = pdb::tryOff(cls.c_str(), "fluidBox");
  return boxOff ? ptr(ent + *boxOff + L.boxSegment) : 0;
}

// Put `name` into an empty fluid network (or top up one that already holds it), keeping
// every invariant FluidSegment/FluidBuffer/FluidBox::checkConsistency assert on.
void fillWith(const std::string& name) {
  std::string cls;
  u64 seg = openFluidSegment(&cls);
  if (!seg) { std::puts("Open the window of a storage tank or pipe first (no window, or that entity has no fluid box)."); return; }
  u16 fid = 0;
  for (auto& [i, n] : fluids) if (n == name) fid = (u16)i;
  if (!fid) { std::printf("No fluid named '%s' (try `find`).\n", name.c_str()); return; }
  u64 proto = ptr(ptr(L.sFluids) + fid * 8);

  u16 cur = rd<u16>(seg + L.fluId).value_or(0xFFFF);
  int64_t amt = rd<int64_t>(seg + L.fluAmt).value_or(-1), vol = rd<int64_t>(seg + L.fluVol).value_or(0);
  if (vol <= 0 || amt < 0 || amt > vol) { std::puts("That does not read like a fluid network - refused."); return; }
  if (cur && cur != fid && amt > 0) {
    std::printf("Refused: the network holds %s. `fluid` + `empty` it first, or pick %s.\n", fluids[cur].c_str(), fluids[cur].c_str());
    return;
  }
  // A building with a fluid filter (boiler input, machine recipe) pins the network's fluid and temperature range.
  bool filtered = rd<u64>(seg + L.fluFilterCount).value_or(1) != 0;
  u16 ffid = rd<u16>(seg + L.fluFilterId).value_or(0);
  float fmin = rd<float>(seg + L.fluFilterMin).value_or(0), fmax = rd<float>(seg + L.fluFilterMax).value_or(0);
  if (filtered && ffid && ffid != fid) { std::printf("Refused: a building on this network only accepts %s.\n", fluids[ffid].c_str()); return; }

  // Directly connected boxes that would not mix must share one segment. A neighbour segment
  // across a plain two-way connection means the game keeps them apart on purpose: refuse.
  for (u64 b = ptr(seg + L.fluBoxesBegin), e = ptr(seg + L.fluBoxesEnd); b && b < e; b += 8) {
    u64 box = ptr(b);
    for (u64 c = ptr(box + L.boxConnBegin), ce = ptr(box + L.boxConnEnd); c && c < ce; c += L.connSize) {
      u64 target = ptr(c + L.connTarget);
      if (!target || ptr(target + L.boxSegment) == seg) continue;
      u64 opp = ptr(target + L.boxConnBegin) + rd<u16>(c + L.connOpposite).value_or(0) * L.connSize;
      uint8_t flags = rd<uint8_t>(c + L.connFlags).value_or(0) | rd<uint8_t>(opp + L.connFlags).value_or(0);
      if (!(flags & 0x8) && !rd<uint8_t>(c + L.connType).value_or(1) && !rd<uint8_t>(opp + L.connType).value_or(1)) {
        std::puts("Refused: this network touches another network that the game keeps separate.");
        return;
      }
    }
  }

  float temp = cur == fid ? rd<float>(seg + L.fluTemp).value_or(0) : rd<float>(proto + L.fpDefaultTemp).value_or(15);
  if (filtered) temp = std::clamp(temp, fmin, fmax);
  temp = std::min(temp, rd<float>(proto + L.fpMaxTemp).value_or(temp));

  // Fluid {fluidID, temperature, amount} in one write so the checker never sees a half state.
  struct { u16 id; u16 pad; float temperature; int64_t amount; } fluid{};
  static_assert(sizeof fluid == 16);
  if (L.fluTemp != L.fluId + 4 || L.fluAmt != L.fluId + 8 || !rd(seg + L.fluId, fluid)) { std::puts("Fluid layout changed - refused."); return; }
  fluid.id = fid; fluid.temperature = temp; fluid.amount = vol;
  if (!wr(seg + L.fluId, fluid)) { std::puts("Write failed."); return; }
  fluSeg = seg;
  std::printf("%s: filled with %s at %.1f C.\n", cls.c_str(), name.c_str(), temp);
  fluShow();
}

// Flush the open tank/pipe network like the game's flush button: an empty buffer is fluid 0, temperature 0, amount 0.
void emptyOpen() {
  std::string cls;
  u64 seg = openFluidSegment(&cls);
  if (!seg) { std::puts("Open the window of a storage tank or pipe first."); return; }
  struct { u16 id; u16 pad; float temperature; int64_t amount; } fluid{};
  if (L.fluTemp != L.fluId + 4 || L.fluAmt != L.fluId + 8 || !rd(seg + L.fluId, fluid)) { std::puts("Fluid layout changed - refused."); return; }
  fluid.id = 0; fluid.temperature = 0; fluid.amount = 0;
  std::puts(wr(seg + L.fluId, fluid) ? "Emptied." : "Write failed.");
}

// ---------------------------------------------------------------- ore
struct Ore { u64 anchor = 0, vt1 = 0, vt2 = 0, proto = 0; int reach = 1; std::vector<u64> tiles; } ore;
u32 oreDefault = 500000;

// Entities of the anchor's kind (both vtables and prototype) chained to it at most `reach` apart
// (1/256 tile, per axis), anchor first; empty when the anchor is not on the map. Read from the
// per-tile entity lists of only the chunks the chain reaches (Surface.chunks -> Chunk.advancedTiles
// -> EntityConnector.nextOnTile), not by scanning the game's memory: a removed entity is unlinked
// from those lists even though its freed memory keeps the old bytes.
std::vector<u64> sameKind(u64 anchor, int64_t reach) {
  u64 vt1 = ptr(anchor), vt2 = ptr(anchor + L.entPost), proto = ptr(anchor + L.entProto), surface = ptr(anchor + L.entSurface);
  auto key = [](int64_t x, int64_t y) { return x << 32 ^ (u32)y; };
  std::unordered_map<int64_t, u64> chunks;  // chunk position -> Chunk
  u64 b = ptr(surface + L.surfChunks), e = ptr(surface + L.surfChunks + 8);
  std::vector<u64> list(e > b && e - b < (1ull << 28) ? (e - b) / 8 : 0);
  SIZE_T n = 0;
  if (!list.empty() && ReadProcessMemory(g.proc, (LPCVOID)b, list.data(), list.size() * 8, &n))
    for (u64 c : list)
      if (int32_t p[2]; rd(c + L.chunkPos, p)) chunks[key(p[0], p[1])] = c;

  struct Hit { u64 ent; int64_t x, y; };
  std::vector<Hit> found;  // every entity of the kind in the chunks read so far
  std::unordered_map<u64, bool> seen;
  std::unordered_map<int64_t, bool> loaded;
  std::vector<uint8_t> tiles(L.chunkAdvTilesSize);
  auto load = [&](int64_t cx, int64_t cy) {
    auto it = chunks.find(key(cx, cy));
    if (loaded[key(cx, cy)] || it == chunks.end()) return;
    loaded[key(cx, cy)] = true;
    if (!ReadProcessMemory(g.proc, (LPCVOID)(it->second + L.chunkAdvTiles), tiles.data(), tiles.size(), &n)) return;
    for (size_t t = 0; t + L.advTileSize <= tiles.size(); t += L.advTileSize) {
      u64 c = 0;
      std::memcpy(&c, tiles.data() + t + L.tileEntities, 8);
      for (int i = 0; c && i < 4096; ++i, c = ptr(c + L.connNextOnTile)) {
        u64 ent = ptr(c + L.connEntity);
        if (seen[ent] || ptr(ent) != vt1 || ptr(ent + L.entPost) != vt2 || ptr(ent + L.entProto) != proto) continue;
        seen[ent] = true;
        found.push_back({ent, rd<int32_t>(ent + L.entX).value_or(0), rd<int32_t>(ent + L.entY).value_or(0)});
      }
    }
  };
  constexpr int64_t CHUNK = 32 * 256;
  auto chunkOf = [](int64_t v) { return v >> 13; };  // arithmetic shift = floor(/CHUNK)
  static_assert(CHUNK == 1 << 13);
  int64_t ax = rd<int32_t>(anchor + L.entX).value_or(0), ay = rd<int32_t>(anchor + L.entY).value_or(0);
  load(chunkOf(ax), chunkOf(ay));
  if (!seen[anchor]) return {};

  std::vector<Hit> block{{anchor, ax, ay}};
  std::vector<bool> taken;
  for (size_t q = 0; q < block.size(); ++q) {
    auto [_, x, y] = block[q];
    for (int64_t cx = chunkOf(x - reach); cx <= chunkOf(x + reach); ++cx)
      for (int64_t cy = chunkOf(y - reach); cy <= chunkOf(y + reach); ++cy) load(cx, cy);
    taken.resize(found.size());
    for (size_t i = 0; i < found.size(); ++i)
      if (!taken[i] && found[i].ent != anchor && std::abs(found[i].x - x) <= reach && std::abs(found[i].y - y) <= reach) {
        taken[i] = true;
        block.push_back(found[i]);
      }
  }
  std::vector<u64> out;
  for (auto& h : block) out.push_back(h.ent);
  return out;
}

// Whether the entity is still linked into its tile's entity list (Entity.connector): walk that list
// from the tile's side and look for this entity's connector.
bool entityLive(u64 ent) {
  u64 conn = ent + L.entConnector, tile = ptr(conn + L.connTile);
  if (!tile || ptr(conn + L.connEntity) != ent) return false;
  u64 c = ptr(tile + L.tileEntities);
  for (int i = 0; c && i < 4096; ++i, c = ptr(c + L.connNextOnTile))
    if (c == conn) return true;
  return false;
}

// 100% yield of an infinite (oil-like) resource; 0 for solid ores.
u32 yieldBase(u64 proto) {
  return rd<u8>(proto + L.resInfinite).value_or(0) ? rd<u32>(proto + L.resNormal).value_or(0) : 0;
}

bool oreLive(u64 a) { return ptr(a) == ore.vt1 && ptr(a + L.entProto) == ore.proto && entityLive(a); }

// Every tile of the anchor's resource type within `ore.reach` tiles of the patch (flood fill by map position).
// Oil-like (infinite) resources have a 100% baseline and wells several tiles apart: those use the
// field reach.
bool orePopulate(bool quiet = false) {
  ore.vt1 = ptr(ore.anchor); ore.vt2 = ptr(ore.anchor + L.entPost); ore.proto = ptr(ore.anchor + L.entProto);
  u32 normal = yieldBase(ore.proto);
  ore.reach = normal ? OIL_REACH : 1;
  ore.tiles.clear();
  if (!ore.vt1 || !ore.vt2 || !ore.proto) { std::puts("Captured address is not an entity - capture again."); return false; }

  ore.tiles = sameKind(ore.anchor, ore.reach * 256);  // tile centres: 256 apart per tile
  u64 total = 0;
  for (u64 a : ore.tiles) total += rd<u32>(a + L.oreAmt).value_or(0);
  std::string name = readStdString(ore.proto + L.protoName).value_or("?");
  if (!quiet && normal)
    std::printf("%s: %zu wells in this field, the hovered one at %.0f%%. Check the count against the map, then `oil <target %%>`.\n",
                name.c_str(), ore.tiles.size(), rd<u32>(ore.anchor + L.oreAmt).value_or(0) * 100.0 / normal);
  else if (!quiet)
    std::printf("%s: %zu tiles in this patch, %llu total. Check the count against the map before writing.\n", name.c_str(), ore.tiles.size(), total);
  return !ore.tiles.empty();
}

bool oreCapture(bool quiet = false) {
  auto b = capture(L.sOreHook, Reg::RCX, quiet ? nullptr : "hover a tile of the patch (tooltip visible)", quiet ? 1500 : 60000);
  if (!b) return false;
  ore = {}; ore.anchor = *b;
  return orePopulate(quiet);
}

int oreWrite(u32 value) {
  int n = 0;
  for (u64 a : ore.tiles) if (oreLive(a) && wr<u32>(a + L.oreAmt, value)) ++n;
  return n;
}

void oreSet(u32 value) {
  if (ore.tiles.empty()) { std::puts("Capture first (`ore`)."); return; }
  if (value == 0) { std::puts("Refusing 0 - the game cannot clean up a zero tile written from outside. Use 1."); return; }
  int n = oreWrite(value);
  std::printf("Wrote %u into %d of %zu tiles. The map label updates after a re-chart or reload.\n", value, n, ore.tiles.size());
}

// Yield shown in game = amount / ResourceEntityPrototype.normalAmount; set every well of the field.
void oilSet(double tgt) {
  if (!ore.anchor || ptr(ore.anchor) != ore.vt1) { std::puts("Capture one well with `ore` first."); return; }
  if (tgt <= 0) { std::puts("Usage: oil <target %>"); return; }
  u32 normal = yieldBase(ore.proto);
  if (!normal) { std::puts("That resource has no yield baseline (not an oil-like resource)."); return; }
  if (!orePopulate(true)) return;
  oreSet((u32)std::max(1.0, std::floor(tgt / 100.0 * normal + 0.5)));
}

// ---------------------------------------------------------------- hotkeys
// Global hotkeys work while the game has focus. A beep reports the result:
// high = done, low = nothing under the cursor / refused.
std::mutex busy;

void beep(bool ok) { Beep(ok ? 1200 : 300, 120); }

void hotkeyThread() {
  struct Key { int id; UINT mod, vk; const char* what; } keys[] = {
      {1, MOD_CONTROL | MOD_SHIFT, VK_F1, "fill the hovered tank or pipe"},
      {2, MOD_CONTROL | MOD_SHIFT, VK_F2, "top up every stack in your main inventory"},
      {3, MOD_CONTROL | MOD_SHIFT, VK_F3, "set the hovered ore patch to the default amount (`ore default N`)"},
      {4, MOD_CONTROL | MOD_SHIFT, VK_F4, "fill the open chest with the item in its first slot"},
  };
  for (auto& k : keys)
    if (RegisterHotKey(nullptr, k.id, k.mod | MOD_NOREPEAT, k.vk)) std::printf("  Ctrl+Shift+F%d  %s\n", k.id, k.what);
    else std::printf("  Ctrl+Shift+F%d  unavailable (another program owns it)\n", k.id);
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (msg.message != WM_HOTKEY) continue;
    std::lock_guard lock(busy);
    switch (msg.wParam) {
      case 1: beep(fluCapture() && fluTopUp()); break;
      case 2: { u64 inv = myInventory(); int n = inv ? invTopUp(inv) : -1; beep(n >= 0); if (n > 0) std::printf("\n[hotkey] topped up %d stacks\n> ", n); break; }
      case 3: beep(oreCapture(true) && oreWrite(oreDefault) > 0); break;
      case 4: std::puts("\n[hotkey]"); beep(chestFill()); std::printf("> "); break;
    }
  }
}

// ---------------------------------------------------------------- window
// --window (the Start Menu shortcut): open at the position in trainer-window.txt next to the exe
// ("x y width height", pixels). Under Windows Terminal the console window is a hidden
// pseudo window owned by the terminal window, so the root owner is the window on screen.
std::filesystem::path windowFile() {
  wchar_t p[MAX_PATH]{};
  GetModuleFileNameW(nullptr, p, MAX_PATH);
  return std::filesystem::path(p).replace_filename(L"trainer-window.txt");
}

HWND trainerWindow() {
  HWND c = GetConsoleWindow();
  return c ? GetAncestor(c, GA_ROOTOWNER) : nullptr;
}

void restoreWindow() {
  int x, y, w, h;
  if (std::ifstream f(windowFile()); f >> x >> y >> w >> h && w > 100 && h > 100)
    SetWindowPos(trainerWindow(), nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

// ---------------------------------------------------------------- repl
const char* HELP = R"(Commands:
  inv                 your character's main inventory (no clicking)
  chest               the chest/wagon/car whose window is open in game (no clicking)
  chest fill [slots] [item]  fill its free slots (or only slots 22 / 22-90) with its first item or the named one
  chest clear [what]  empty it, or only what is named: items, #ids, slots 22 or 22-90 (spaces or commas)
  chest fill all [item] / chest clear all [items]  the same for every chest of that kind in its block
  chest add | list | reset   pick chests anywhere: open one, `chest add`; repeat
  chest balance [all] [item] even out an item over the picked chests (all: the open chest's block)
  train fill [names]  open wagon/locomotive: fill every wagon of its train (own item/fluid, or the named ones;
                      slots 22 / 22-90 limit every cargo wagon to those slots)
  train clear [names] empty every wagon of the train (everything, or only the named items/fluids/slots)
  inv click           capture: then click an item in any inventory (car, wagon, ...)
  set <slot> <count>  set a stack count (simple items only)
  topup               fill every simple stack of the listed inventory to its max
  give <item> <n>     add n of an item as normal stacks (e.g. give electronic-circuit 2000)
  clear <what>        remove from the listed inventory: items, #ids, slots 22 or 22-90
  swap <from> <to>    replace all of one item with the same number of another
  find <text>         item and fluid names containing text (e.g. find centrifuge); #id works as an item name
  tank                show the network of the tank/pipe whose window is open
  fill [fluid]        fill the tank/pipe whose window is open; name a fluid when it is empty
  empty               empty it (like the flush button)
  ore                 capture: then hover an ore tile (or one oil well)
  ore set <amount>    set every tile of the captured patch
  ore default <n>     amount used by Ctrl+Shift+F3 (now %u)
  oil <tgt%%>          oil field (captured with `ore`): set every well to that yield
  names | quit
)";

int main(int argc, char** argv) {
  if (argc == 3 && std::string(argv[1]) == "--layout") {  // offline check: resolve every name against an exe + pdb
    std::string e = argv[2];
    g.exe.assign(e.begin(), e.end()); g.base = 0x140000000;
    bool ok = pdb::open() && loadLayout();
    std::printf("%s  game %llX localPlayer %llX controller %llX character %llX inventory %llX stack %llX fluAmt %llX fluVol %llX oreAmt %llX entPost %llX entY %llX\n",
                ok ? "OK" : "FAILED", L.gcGame, L.gameLocalPlayer, L.playerController, L.ccCharacter + L.targeterTarget, L.charInventory,
                L.stackSize, L.fluAmt, L.fluVol, L.oreAmt, L.entPost, L.entY);
    return ok ? 0 : 1;
  }
  bool placed = argc == 2 && std::string(argv[1]) == "--window";
  if (placed) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);  // real pixels on scaled monitors
    restoreWindow();
  }
  std::puts("Factorio trainer - single player only. Back up your save.");
  if (!attach()) { std::cin.get(); return 1; }
  if (!pdb::open()) { std::printf("Could not load factorio.pdb (%lu).\n", GetLastError()); std::cin.get(); return 1; }
  if (!loadLayout()) { std::cin.get(); return 1; }
  reloadNames();
  bool scraper = rd<uint8_t>(ptr(ptr(L.sGlobal) + L.gcOtherSettings) + L.scraperOn).value_or(1);
  std::printf("Game consistency scraper: %s (inventory edits keep its indices valid)\n", scraper ? "ON" : "off");
  std::puts("Hotkeys (work while the game has focus):");
  std::thread(hotkeyThread).detach();
  Sleep(100);
  std::printf(HELP, oreDefault);

  std::string line;
  while (std::printf("> "), std::getline(std::cin, line)) {
    std::lock_guard lock(busy);
    std::istringstream rest(line);
    std::string cmd, arg;
    rest >> cmd >> arg;
    if (cmd == "quit" || cmd == "exit") break;
    else if (cmd == "inv") arg == "click" ? invClick() : invMine();
    else if (cmd == "chest" && (arg == "fill" || arg == "clear" || arg == "balance")) {
      std::vector<std::string> names;
      for (std::string n; rest >> n;) names.push_back(n);
      bool all = !names.empty() && names[0] == "all";
      if (all) names.erase(names.begin());
      if (arg == "clear") chestClear(all, names);
      else if (arg == "balance") names.size() > 1 ? (void)std::puts("Usage: chest balance [all] [item]") : chestBalance(all, names.empty() ? "" : names[0]);
      else if (auto sel = parseSelector(names); !sel) {}
      else if (sel->ids.size() > 1) std::puts("Usage: chest fill [all] [item] / chest fill <slots 22 or 22-90> [item] - one item.");
      else if (all) chestFillAll(sel->ids.empty() ? "" : items[sel->ids[0]].name, sel->slots);
      else chestFill(*sel);
    }
    else if (cmd == "train" && (arg == "fill" || arg == "clear")) {
      std::vector<std::string> names;
      for (std::string n; rest >> n;) names.push_back(n);
      arg == "fill" ? trainFill(names) : trainClear(names);
    }
    else if (cmd == "chest" && arg == "add") chestAdd();
    else if (cmd == "chest" && arg == "list") chestShowList();
    else if (cmd == "chest" && arg == "reset") { chestList.clear(); chestLabels.clear(); std::puts("List cleared."); }
    else if (cmd == "chest") invChest();
    else if (cmd == "set") { u32 s, c; std::istringstream a(line.substr(3)); if (a >> s >> c) invSet(s, c); else std::puts("Usage: set <slot> <count>"); }
    else if (cmd == "topup") { u64 inv = picked(); int n = inv ? invTopUp(inv) : -1; if (n >= 0) std::printf("Topped up %d stacks.\n", n); }
    else if (cmd == "fill" && !arg.empty()) fillWith(arg);
    else if (cmd == "fill") {
      u64 seg = openFluidSegment();
      u16 cur = seg ? rd<u16>(seg + L.fluId).value_or(0) : 0;
      if (!seg) std::puts("Open the window of a storage tank or pipe first.");
      else if (!cur) std::puts("It is empty - name a fluid: fill <fluid>.");
      else fillWith(fluids[cur]);
    }
    else if (cmd == "empty") emptyOpen();
    else if (cmd == "tank") {
      std::string cls;
      u64 seg = openFluidSegment(&cls);
      if (!seg) std::puts("No tank/pipe window open.");
      else {
        u16 id = rd<u16>(seg + L.fluId).value_or(0);
        std::printf("%s segment %llX: %s, %.0f / %.0f, filter %s (%llu filtering boxes), %llu boxes\n", cls.c_str(), seg,
                    id ? fluids[id].c_str() : "empty", rd<int64_t>(seg + L.fluAmt).value_or(0) / FLUID_SCALE,
                    rd<int64_t>(seg + L.fluVol).value_or(0) / FLUID_SCALE,
                    rd<u16>(seg + L.fluFilterId).value_or(0) ? fluids[*rd<u16>(seg + L.fluFilterId)].c_str() : "none",
                    rd<u64>(seg + L.fluFilterCount).value_or(0), (ptr(seg + L.fluBoxesEnd) - ptr(seg + L.fluBoxesBegin)) / 8);
      }
    }
    else if (cmd == "ore" && arg.empty()) oreCapture();
    else if (cmd == "ore" && (arg == "set" || arg == "default")) {
      u32 v;
      if (!(rest >> v) && arg == "default") std::printf("Ctrl+Shift+F3 sets %u (change: ore default <amount>).\n", oreDefault);
      else if (!rest) std::puts("Usage: ore set <amount> | ore default <amount>");
      else if (arg == "set") oreSet(v);
      else if (v) { oreDefault = v; std::printf("Ctrl+Shift+F3 now sets %u.\n", v); }
    }
    else if (cmd == "oil") { double t; std::istringstream a(line.substr(3)); if (a >> t) oilSet(t); else std::puts("Usage: oil <target %>"); }
    else if (cmd == "give") { u64 n; if (!arg.empty() && rest >> n && n) give(arg, n); else std::puts("Usage: give <item-name> <count>"); }
    else if (cmd == "clear") {
      std::vector<std::string> words{arg};
      for (std::string w; rest >> w;) words.push_back(w);
      clearPicked(words);
    }
    else if (cmd == "swap") { std::string to; if (arg.empty() || !(rest >> to)) std::puts("Usage: swap <from-item> <to-item>"); else swapItems(arg, to); }
    else if (cmd == "find" && !arg.empty()) {
      std::map<std::string, std::string> hits;  // sorted output
      for (auto& [i, info] : items)
        if (info.name.find(arg) != std::string::npos)
          hits[info.name] = "item  #" + std::to_string(i) + (info.unstackable ? "  unstackable" : "  stack " + std::to_string(info.stack));
      for (auto& [i, n] : fluids) if (n.find(arg) != std::string::npos) hits[n] = "fluid";
      for (auto& [n, what] : hits) std::printf("  %-45s %s\n", n.c_str(), what.c_str());
      if (hits.empty()) std::puts("  nothing matches");
    }
    else if (cmd == "names") reloadNames();
    else if (!cmd.empty()) std::printf(HELP, oreDefault);
  }
  return 0;
}
