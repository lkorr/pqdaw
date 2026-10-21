// pqc.exe: arguments arrive as UTF-16 (wmain) and are passed on as UTF-8.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <iostream>
#include <string>
#include <vector>

#include "pq/pqc/pqc.h"

namespace {
std::string toUtf8(const wchar_t* w) {
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) return {};
  std::string s(static_cast<size_t>(n - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) args.push_back(toUtf8(argv[i]));
  SetConsoleOutputCP(CP_UTF8);  // diagnostics quote UTF-8 source lines
  const int code = pq::pqc::run(args, std::cout, std::cerr);
  std::cout.flush();
  return code;
}
