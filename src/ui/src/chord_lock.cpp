// T7.16b: see pq/ui/chord_lock.h.

#include "pq/ui/chord_lock.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace pq::ui {

bool restoreCapsLock() noexcept {
#ifdef _WIN32
  INPUT in[2] = {};
  in[0].type = INPUT_KEYBOARD;
  in[0].ki.wVk = VK_CAPITAL;
  in[0].ki.wScan = static_cast<WORD>(MapVirtualKeyW(VK_CAPITAL, MAPVK_VK_TO_VSC));
  in[1] = in[0];
  in[1].ki.dwFlags = KEYEVENTF_KEYUP;
  return SendInput(2, in, sizeof(INPUT)) == 2;
#else
  return false;
#endif
}

}  // namespace pq::ui
