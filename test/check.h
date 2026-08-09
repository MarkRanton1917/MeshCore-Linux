#pragma once

#include "defines.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// Minimal harness: no framework, no dependencies.
namespace check {

inline int checks = 0;
inline int failures = 0;

inline void section(const char* name)
{
  std::printf("\n%s\n", name);
}

inline void that(const char* name, bool passed)
{
  checks++;
  if (!passed) failures++;
  std::printf("  %s  %s\n", passed ? "ok  " : "FAIL", name);
}

inline int report()
{
  std::printf("\n%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}

inline std::vector<std::uint8_t> fromHex(const char* hex)
{
  std::vector<std::uint8_t> out;
  for (const char* p = hex; p[0] && p[1]; p += 2) {
    out.push_back((std::uint8_t)std::stoul(std::string(p, 2), nullptr, 16));
  }
  return out;
}

inline bool equalsHex(ByteView actual, const char* hex)
{
  const std::vector<std::uint8_t> expected = fromHex(hex);
  return actual.size() == expected.size() && std::memcmp(actual.data(), expected.data(), expected.size()) == 0;
}

} // namespace check
