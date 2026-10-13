// pdbq: query factorio.pdb offline. FACTORIO_EXE = full path of factorio.exe.
//   pdbq type <TypeName>     members of a class: offset, size, type, name (base classes too)
//   pdbq sym <mask>          symbols matching a DbgHelp mask, e.g. *Inventory* or *::instance
//   pdbq data <mask>         same, data symbols (globals/statics) only
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>

#include <cstdio>
#include <string>

constexpr DWORD64 BASE = 0x140000000;
// cvconst.h values (DIA SDK header, not shipped with dbghelp.h)
enum : DWORD { T_UDT = 11, T_PTR = 14, T_ARRAY = 15, T_BASE = 16, T_TYPEDEF = 17, T_DATA = 7, T_BASECLASS = 18, DATA_MEMBER = 7 };
enum : DWORD { B_CHAR = 2, B_WCHAR = 3, B_INT = 6, B_UINT = 7, B_FLOAT = 8, B_BOOL = 10, B_LONG = 13, B_ULONG = 14 };
HANDLE h = (HANDLE)(uintptr_t)0xF00D;

std::wstring symName(DWORD ti) {
  WCHAR* n = nullptr;
  std::wstring s;
  if (SymGetTypeInfo(h, BASE, ti, TI_GET_SYMNAME, &n) && n) { s = n; LocalFree(n); }
  return s;
}

std::wstring typeName(DWORD ti, int depth = 0) {
  DWORD tag = 0;
  SymGetTypeInfo(h, BASE, ti, TI_GET_SYMTAG, &tag);
  DWORD sub = 0;
  ULONG64 len = 0;
  switch (tag) {
    case T_PTR:
      SymGetTypeInfo(h, BASE, ti, TI_GET_TYPEID, &sub);
      return (depth > 4 ? L"?" : typeName(sub, depth + 1)) + L"*";
    case T_ARRAY: {
      DWORD count = 0;
      SymGetTypeInfo(h, BASE, ti, TI_GET_TYPEID, &sub);
      SymGetTypeInfo(h, BASE, ti, TI_GET_COUNT, &count);
      return typeName(sub, depth + 1) + L"[" + std::to_wstring(count) + L"]";
    }
    case T_BASE: {
      DWORD bt = 0;
      SymGetTypeInfo(h, BASE, ti, TI_GET_BASETYPE, &bt);
      SymGetTypeInfo(h, BASE, ti, TI_GET_LENGTH, &len);
      const wchar_t* k = bt == B_FLOAT ? L"float" : bt == B_BOOL ? L"bool" : bt == B_CHAR || bt == B_WCHAR ? L"char"
                       : bt == B_UINT || bt == B_ULONG ? L"u" : bt == B_INT || bt == B_LONG ? L"i" : L"base";
      return std::wstring(k) + std::to_wstring(len * 8);
    }
    case T_TYPEDEF:
      SymGetTypeInfo(h, BASE, ti, TI_GET_TYPEID, &sub);
      return typeName(sub, depth + 1);
    default: {
      auto n = symName(ti);
      return n.empty() ? L"<tag " + std::to_wstring(tag) + L">" : n;
    }
  }
}

bool dumpType(const char* name) {
  char buf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME]{};
  auto* si = (SYMBOL_INFO*)buf;
  si->SizeOfStruct = sizeof(SYMBOL_INFO);
  si->MaxNameLen = MAX_SYM_NAME;
  if (!SymGetTypeFromName(h, BASE, name, si)) { std::printf("type %s not found (%lu)\n", name, GetLastError()); return false; }
  ULONG64 size = 0;
  SymGetTypeInfo(h, BASE, si->TypeIndex, TI_GET_LENGTH, &size);
  std::printf("%s  size 0x%llX\n", name, size);
  DWORD count = 0;
  SymGetTypeInfo(h, BASE, si->TypeIndex, TI_GET_CHILDRENCOUNT, &count);
  auto* p = (TI_FINDCHILDREN_PARAMS*)calloc(1, sizeof(TI_FINDCHILDREN_PARAMS) + count * sizeof(ULONG));
  p->Count = count;
  SymGetTypeInfo(h, BASE, si->TypeIndex, TI_FINDCHILDREN, p);
  for (DWORD i = 0; i < count; ++i) {
    DWORD c = p->ChildId[i], tag = 0, off = 0, type = 0;
    SymGetTypeInfo(h, BASE, c, TI_GET_SYMTAG, &tag);
    if (tag != T_DATA && tag != T_BASECLASS) continue;
    DWORD kind = 0;
    if (tag == T_DATA && SymGetTypeInfo(h, BASE, c, TI_GET_DATAKIND, &kind) && kind != DATA_MEMBER) continue;
    SymGetTypeInfo(h, BASE, c, TI_GET_OFFSET, &off);
    SymGetTypeInfo(h, BASE, c, TI_GET_TYPEID, &type);
    ULONG64 len = 0;
    SymGetTypeInfo(h, BASE, type, TI_GET_LENGTH, &len);
    if (tag == T_BASECLASS)
      std::printf("  +0x%04lX  %5llX  base %ls\n", off, len, typeName(type).c_str());
    else
      std::printf("  +0x%04lX  %5llX  %-60ls %ls\n", off, len, typeName(type).c_str(), symName(c).c_str());
  }
  free(p);
  return true;
}

BOOL CALLBACK onSym(PSYMBOL_INFO si, ULONG, PVOID dataOnly) {
  bool isData = si->Tag == T_DATA;
  if (dataOnly && !isData) return TRUE;
  std::printf("%-5s rva %08llX  size %5lX  %s\n", isData ? "data" : "func", si->Address - BASE, si->Size, si->Name);
  return TRUE;
}

int main(int argc, char** argv) {
  if (argc < 3) { std::puts("usage: pdbq type <Name> | sym <mask> | data <mask> | addr <rva>..."); return 2; }
  std::string cmd = argv[1];
  if (cmd != "type" && cmd != "sym" && cmd != "data" && cmd != "addr") { std::printf("unknown command %s\n", cmd.c_str()); return 2; }
  WCHAR env[MAX_PATH];
  DWORD len = GetEnvironmentVariableW(L"FACTORIO_EXE", env, MAX_PATH);  // >= MAX_PATH: too long, env not filled
  if (!len || len >= MAX_PATH) {
    std::puts("set FACTORIO_EXE to the full path of factorio.exe (factorio.pdb next to it)");
    return 2;
  }
  std::wstring exe = env, dir = exe.substr(0, exe.find_last_of(L"\\/"));
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_FAIL_CRITICAL_ERRORS);
  if (!SymInitializeW(h, dir.c_str(), FALSE) ||
      !SymLoadModuleExW(h, nullptr, exe.c_str(), nullptr, BASE, 0, nullptr, 0)) {
    std::printf("load failed %lu\n", GetLastError());
    return 1;
  }
  bool ok = true;  // exit status 1 when a query found nothing
  if (cmd == "type") ok = dumpType(argv[2]);
  else if (cmd == "addr") {  // function containing an RVA
    char buf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME]{};
    auto* si = (SYMBOL_INFO*)buf;
    si->SizeOfStruct = sizeof(SYMBOL_INFO);
    si->MaxNameLen = MAX_SYM_NAME;
    DWORD64 disp = 0;
    for (int i = 2; i < argc; ++i)
      if (SymFromAddr(h, BASE + strtoull(argv[i], nullptr, 16), &disp, si)) std::printf("%s = %s+0x%llX\n", argv[i], si->Name, disp);
      else { std::printf("%s = ?\n", argv[i]); ok = false; }
  }
  else ok = SymEnumSymbols(h, BASE, argv[2], onSym, cmd == "data" ? (PVOID)1 : nullptr);
  SymCleanup(h);
  return ok ? 0 : 1;
}
