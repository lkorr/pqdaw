#pragma once

// Lets doctest print std::string_view operands of CHECK(a == b) (it has no built-in conversion).

#include <doctest/doctest.h>

#include <string_view>

namespace doctest {
template <>
struct StringMaker<std::string_view> {
  static String convert(std::string_view v) { return String(v.data(), static_cast<String::size_type>(v.size())); }
};
}  // namespace doctest
