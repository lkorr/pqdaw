#include "pq/tuning/js_number.h"

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace pq::tuning::js {

std::string toFixed(double x, int digits) {
  if (std::isnan(x)) return "NaN";
  if (std::isinf(x)) return x > 0 ? "Infinity" : "-Infinity";
  const bool negative = x < 0.0;
  const double a = std::fabs(x);
  if (a >= 1e21) return toString(x);
  // The UCRT prints the exact decimal expansion of a double; 1100 fractional digits cover every double (the longest
  // expansion has 1074). Truncate after digit `digits`, then round half up on the exact next digit.
  static constexpr int kExact = 1100;
  char buf[1500];
  const int len = std::snprintf(buf, sizeof buf, "%.*f", kExact, a);
  std::string s(buf, static_cast<size_t>(len));
  const size_t point = s.find('.');
  const char next = s[point + 1 + static_cast<size_t>(digits)];
  s.resize(digits > 0 ? point + 1 + static_cast<size_t>(digits) : point);
  if (next >= '5') {
    // Increment the decimal string, carrying over the point.
    size_t i = s.size();
    for (;;) {
      if (i == 0) {
        s.insert(s.begin(), '1');
        break;
      }
      --i;
      if (s[i] == '.') continue;
      if (s[i] == '9') {
        s[i] = '0';
        continue;
      }
      ++s[i];
      break;
    }
  }
  return negative ? "-" + s : s;
}

std::string toString(double x) {
  if (std::isnan(x)) return "NaN";
  if (std::isinf(x)) return x > 0 ? "Infinity" : "-Infinity";
  if (x == 0.0) return "0";  // also -0
  // Shortest round-trip digits in scientific form: d[.ddd]e±XX.
  char buf[64];
  const auto r = std::to_chars(buf, buf + sizeof buf, std::fabs(x), std::chars_format::scientific);
  const std::string_view sci(buf, static_cast<size_t>(r.ptr - buf));
  const size_t e = sci.find('e');
  std::string digits;
  for (char c : sci.substr(0, e))
    if (c != '.') digits.push_back(c);
  const int exp10 = std::atoi(std::string(sci.substr(e + 1)).c_str());
  const int k = static_cast<int>(digits.size());
  const int n = exp10 + 1;  // ECMAScript: value = 0.digits × 10^n
  std::string out = x < 0 ? "-" : "";
  if (k <= n && n <= 21) {
    out += digits;
    out.append(static_cast<size_t>(n - k), '0');
  } else if (0 < n && n <= 21) {
    out += digits.substr(0, static_cast<size_t>(n));
    out += '.';
    out += digits.substr(static_cast<size_t>(n));
  } else if (-6 < n && n <= 0) {
    out += "0.";
    out.append(static_cast<size_t>(-n), '0');
    out += digits;
  } else {
    out += digits.substr(0, 1);
    if (k > 1) {
      out += '.';
      out += digits.substr(1);
    }
    out += 'e';
    out += (n - 1 >= 0) ? '+' : '-';
    out += std::to_string(std::abs(n - 1));
  }
  return out;
}

}  // namespace pq::tuning::js
