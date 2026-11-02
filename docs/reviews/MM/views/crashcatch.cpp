// MM review (views, loose end a): a minimal debugger that runs a test executable and, on an access violation (or
// heap corruption / fast-fail), prints the faulting thread, address, module + offset, symbol and a stack walk, and
// writes a minidump. Usage: crashcatch <dumpdir> <exe> [args...]
// The child inherits the console, so doctest's own report (the running TEST CASE) is printed next to ours.
// Set _NO_DEBUG_HEAP=1 in the environment to keep the normal heap (a debugger otherwise enables the debug heap).
#include <windows.h>
#include <dbghelp.h>

#include <cstdio>
#include <cwchar>
#include <map>
#include <string>
#include <vector>

#pragma comment(lib, "dbghelp.lib")

namespace {

struct Module {
  DWORD64 base = 0;
  DWORD size = 0;
  std::wstring name;
};

std::map<DWORD, HANDLE> threads;
std::vector<Module> modules;
HANDLE process = nullptr;
DWORD processId = 0;

std::wstring nameOf(HANDLE file) {
  wchar_t buf[MAX_PATH * 2];
  if (file == nullptr || GetFinalPathNameByHandleW(file, buf, MAX_PATH * 2, FILE_NAME_NORMALIZED) == 0) return L"?";
  return std::wcsncmp(buf, L"\\\\?\\", 4) == 0 ? std::wstring(buf + 4) : std::wstring(buf);
}

void addModule(HANDLE file, LPVOID base) {
  Module m;
  m.base = reinterpret_cast<DWORD64>(base);
  m.name = nameOf(file);
  const DWORD64 r = SymLoadModuleExW(process, nullptr, m.name.c_str(), nullptr, m.base, 0, nullptr, 0);
  IMAGEHLP_MODULEW64 info{};
  info.SizeOfStruct = sizeof info;
  if (r != 0 && SymGetModuleInfoW64(process, m.base, &info)) m.size = info.ImageSize;
  modules.push_back(m);
}

std::wstring where(DWORD64 a) {
  wchar_t out[1024];
  const wchar_t* mod = L"?";
  DWORD64 off = a;
  for (const Module& m : modules)
    if (a >= m.base && (m.size == 0 || a < m.base + m.size)) {
      if (m.size == 0 && a > m.base + 0x10000000) continue;
      const size_t s = m.name.find_last_of(L"\\/");
      mod = m.name.c_str() + (s == std::wstring::npos ? 0 : s + 1);
      off = a - m.base;
    }
  alignas(SYMBOL_INFOW) char sb[sizeof(SYMBOL_INFOW) + 512 * sizeof(wchar_t)];
  auto* sym = reinterpret_cast<SYMBOL_INFOW*>(sb);
  sym->SizeOfStruct = sizeof(SYMBOL_INFOW);
  sym->MaxNameLen = 511;
  DWORD64 disp = 0;
  std::wstring s;
  if (SymFromAddrW(process, a, &disp, sym)) {
    IMAGEHLP_LINEW64 line{};
    line.SizeOfStruct = sizeof line;
    DWORD ld = 0;
    if (SymGetLineFromAddrW64(process, a, &ld, &line)) {
      const wchar_t* f = std::wcsrchr(line.FileName, L'\\');
      swprintf(out, 1024, L"%s+0x%llx  %s+0x%llx  (%s:%lu)", mod, off, sym->Name, disp, f ? f + 1 : line.FileName, line.LineNumber);
    } else {
      swprintf(out, 1024, L"%s+0x%llx  %s+0x%llx", mod, off, sym->Name, disp);
    }
  } else {
    swprintf(out, 1024, L"%s+0x%llx", mod, off);
  }
  return out;
}

void stack(HANDLE thread) {
  CONTEXT ctx{};
  ctx.ContextFlags = CONTEXT_FULL;
  if (!GetThreadContext(thread, &ctx)) return;
  STACKFRAME64 f{};
  f.AddrPC.Offset = ctx.Rip;
  f.AddrPC.Mode = AddrModeFlat;
  f.AddrFrame.Offset = ctx.Rbp;
  f.AddrFrame.Mode = AddrModeFlat;
  f.AddrStack.Offset = ctx.Rsp;
  f.AddrStack.Mode = AddrModeFlat;
  for (int i = 0; i < 40; ++i) {
    if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &f, &ctx, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) break;
    if (f.AddrPC.Offset == 0) break;
    std::fwprintf(stdout, L"[crashcatch]   #%02d %s\n", i, where(f.AddrPC.Offset).c_str());
  }
}

void dump(const wchar_t* dir, DWORD tid, EXCEPTION_RECORD& rec, HANDLE thread) {
  wchar_t path[MAX_PATH];
  swprintf(path, MAX_PATH, L"%s\\crash_%lu_%lu.dmp", dir, processId, GetTickCount());
  HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return;
  CONTEXT ctx{};
  ctx.ContextFlags = CONTEXT_FULL;
  GetThreadContext(thread, &ctx);
  EXCEPTION_POINTERS ep{&rec, &ctx};
  MINIDUMP_EXCEPTION_INFORMATION mei{tid, &ep, FALSE};
  // ClientPointers FALSE: the pointers are ours (copies)
  const BOOL ok = MiniDumpWriteDump(process, processId, f, static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory),
                                    &mei, nullptr, nullptr);
  CloseHandle(f);
  std::fwprintf(stdout, L"[crashcatch] minidump %s: %s\n", ok ? L"written" : L"FAILED", path);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc < 3) {
    std::fwprintf(stderr, L"usage: crashcatch <dumpdir> <exe> [args...]\n");
    return 2;
  }
  std::wstring cmd = L"\"" + std::wstring(argv[2]) + L"\"";
  for (int i = 3; i < argc; ++i) cmd += L" \"" + std::wstring(argv[i]) + L"\"";
  STARTUPINFOW si{};
  si.cb = sizeof si;
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE, DEBUG_ONLY_THIS_PROCESS, nullptr, nullptr, &si, &pi)) {
    std::fwprintf(stderr, L"CreateProcess failed %lu\n", GetLastError());
    return 2;
  }
  process = pi.hProcess;
  processId = pi.dwProcessId;
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS);
  SymInitializeW(process, nullptr, FALSE);
  bool initialBp = true;
  int reported = 0;
  DWORD exitCode = 0;
  for (;;) {
    DEBUG_EVENT ev{};
    if (!WaitForDebugEvent(&ev, INFINITE)) break;
    DWORD cont = DBG_CONTINUE;
    switch (ev.dwDebugEventCode) {
      case CREATE_PROCESS_DEBUG_EVENT:
        threads[ev.dwThreadId] = ev.u.CreateProcessInfo.hThread;
        addModule(ev.u.CreateProcessInfo.hFile, ev.u.CreateProcessInfo.lpBaseOfImage);
        if (ev.u.CreateProcessInfo.hFile) CloseHandle(ev.u.CreateProcessInfo.hFile);
        break;
      case CREATE_THREAD_DEBUG_EVENT: threads[ev.dwThreadId] = ev.u.CreateThread.hThread; break;
      case EXIT_THREAD_DEBUG_EVENT: threads.erase(ev.dwThreadId); break;
      case LOAD_DLL_DEBUG_EVENT:
        addModule(ev.u.LoadDll.hFile, ev.u.LoadDll.lpBaseOfDll);
        if (ev.u.LoadDll.hFile) CloseHandle(ev.u.LoadDll.hFile);
        break;
      case EXCEPTION_DEBUG_EVENT: {
        EXCEPTION_RECORD& r = ev.u.Exception.ExceptionRecord;
        const DWORD code = r.ExceptionCode;
        if (code == EXCEPTION_BREAKPOINT && initialBp) {
          initialBp = false;
          break;
        }
        cont = DBG_EXCEPTION_NOT_HANDLED;
        const bool interesting = code == EXCEPTION_ACCESS_VIOLATION || code == 0xC0000374 || code == 0xC0000409 ||
                                 code == EXCEPTION_BREAKPOINT || code == EXCEPTION_ILLEGAL_INSTRUCTION || code == EXCEPTION_STACK_OVERFLOW ||
                                 code == 0xC0000008 /* invalid handle */;
        if (interesting && reported < 6) {
          ++reported;
          const auto addr = reinterpret_cast<DWORD64>(r.ExceptionAddress);
          std::fwprintf(stdout, L"[crashcatch] %s-chance exception 0x%08lX in thread %lu at %s\n",
                        ev.u.Exception.dwFirstChance ? L"first" : L"SECOND", code, ev.dwThreadId, where(addr).c_str());
          if (code == EXCEPTION_ACCESS_VIOLATION && r.NumberParameters >= 2)
            std::fwprintf(stdout, L"[crashcatch]   %s address 0x%llx\n",
                          r.ExceptionInformation[0] == 0 ? L"read" : r.ExceptionInformation[0] == 1 ? L"write" : L"execute",
                          static_cast<unsigned long long>(r.ExceptionInformation[1]));
          auto it = threads.find(ev.dwThreadId);
          if (it != threads.end()) {
            stack(it->second);
            if (code != EXCEPTION_BREAKPOINT && reported <= 2) dump(argv[1], ev.dwThreadId, r, it->second);
          }
          std::fflush(stdout);
        }
        break;
      }
      case EXIT_PROCESS_DEBUG_EVENT: exitCode = ev.u.ExitProcess.dwExitCode; break;
      default: break;
    }
    ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, cont);
    if (ev.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT) break;
  }
  std::fwprintf(stdout, L"[crashcatch] exit code 0x%08lX\n", exitCode);
  return static_cast<int>(exitCode);
}
