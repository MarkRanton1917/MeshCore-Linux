// Copyright (c) 2026 Vladimir Egorov
// This project is licensed under the MIT License.
// See the LICENSE file in the root of the repository for the full license text.

#pragma once

#include "defines.h"

#include <cstdarg>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The dull module whose only job is that no other file makes a system call.
namespace platform {

using Millis = uint64_t;

// Two clocks, and mixing them is the bug you find once a week and cannot
// explain: monotonic never jumps and measures timeouts, wall time can move by a
// minute after an NTP sync and fills protocol timestamps.
struct IClock {
  virtual ~IClock() = default;
  virtual Millis mono() const = 0;
  virtual uint32_t wall() const = 0;
};

class SystemClock : public IClock {
public:
  Millis mono() const override;
  uint32_t wall() const override;

  // A node that advertises itself dated 1970 corrupts its neighbours' records,
  // so the host waits for the first sync before starting.
  static bool wallLooksSynced(uint32_t seconds);
};

// Hands the test moves by itself. Without it a timeout test either cannot be
// written or takes twelve seconds.
class FakeClock : public IClock {
public:
  Millis mono() const override
  {
    return mono_;
  }
  uint32_t wall() const override
  {
    return wall_;
  }

  void advance(Millis by)
  {
    mono_ += by;
    wall_ += (uint32_t)(by / 1000);
  }
  void setMono(Millis value)
  {
    mono_ = value;
  }
  void setWall(uint32_t value)
  {
    wall_ = value;
  }

private:
  Millis mono_ = 0;
  uint32_t wall_ = 0;
};

// Key-value, not file paths. Three keys exist: our identity, the contacts, the
// room clients.
struct IStore {
  virtual ~IStore() = default;
  virtual std::optional<std::vector<uint8_t>> read(std::string_view key) = 0;
  virtual bool write(std::string_view key, ByteView data) = 0;
};

class FileStore : public IStore {
public:
  explicit FileStore(std::string directory);

  std::optional<std::vector<uint8_t>> read(std::string_view key) override;

  // Temporary file then rename: losing power mid-write must not cost the
  // contact list.
  bool write(std::string_view key, ByteView data) override;

private:
  std::string pathFor(std::string_view key) const;
  std::string directory_;
};

class MemoryStore : public IStore {
public:
  std::optional<std::vector<uint8_t>> read(std::string_view key) override;
  bool write(std::string_view key, ByteView data) override;

private:
  std::map<std::string, std::vector<uint8_t>> entries_;
};

// Levels exist because of the failed decrypt on roughly every 256th foreign
// packet: at warn the log drowns, at debug it is a counter you can look at.
enum class LogLevel {
  ERROR = 0,
  WARN = 1,
  INFO = 2,
  DEBUG = 3
};

class Log {
public:
  static void setLevel(LogLevel level);
  static LogLevel level();
  static void write(LogLevel level, const char* format, ...);

private:
  static LogLevel level_;
};

// Parsed once at startup into a struct. Nothing reads it on a hot path.
class Config {
public:
  bool loadFromString(std::string_view text);
  bool loadFile(const std::string& path);

  std::string get(std::string_view key, std::string_view fallback = "") const;
  long getInt(std::string_view key, long fallback = 0) const;
  bool getBool(std::string_view key, bool fallback = false) const;
  std::vector<std::string> getList(std::string_view key) const;
  bool has(std::string_view key) const;

private:
  std::map<std::string, std::string> values_;
  std::map<std::string, std::vector<std::string>> lists_;
};

} // namespace platform
