// MM review (views): resolves module offsets to symbol + line offline. Usage: symres <exe or dll> <hex offset>...
#include <windows.h>
#include <dbghelp.h>

#include <cstdio>
#include <cwchar>

#pragma comment(lib, "dbghelp.lib")

int wmain(int argc, wchar_t** argv) {
  if (argc < 3) return 2;
  HANDLE self = GetCurrentProcess();
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS);
  if (!SymInitializeW(self, nullptr, FALSE)) return 3;
  const DWORD64 base = 0x10000000ull;
  if (SymLoadModuleExW(self, nullptr, argv[1], nullptr, base, 0, nullptr, 0) == 0) {
    std::fwprintf(stderr, L"load failed %lu\n", GetLastError());
    return 4;
  }
  for (int i = 2; i < argc; ++i) {
    const DWORD64 a = base + std::wcstoull(argv[i], nullptr, 16);
    alignas(SYMBOL_INFOW) char sb[sizeof(SYMBOL_INFOW) + 512 * sizeof(wchar_t)];
    auto* sym = reinterpret_cast<SYMBOL_INFOW*>(sb);
    sym->SizeOfStruct = sizeof(SYMBOL_INFOW);
    sym->MaxNameLen = 511;
    DWORD64 disp = 0;
    IMAGEHLP_LINEW64 line{};
    line.SizeOfStruct = sizeof line;
    DWORD ld = 0;
    const bool s = SymFromAddrW(self, a, &disp, sym) != 0;
    const bool l = SymGetLineFromAddrW64(self, a, &ld, &line) != 0;
    std::fwprintf(stdout, L"%s: %s+0x%llx  %s:%lu\n", argv[i], s ? sym->Name : L"?", disp, l ? line.FileName : L"?", l ? line.LineNumber : 0);
  }
  return 0;
}
