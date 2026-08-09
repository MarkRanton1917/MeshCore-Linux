#include "platform.h"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace platform;

using Json = nlohmann::json;

LogLevel Log::level_ = LogLevel::INFO;

static std::string scalarToText(const Json& node);
static bool flatten(const Json& node,
  const std::string& prefix,
  std::map<std::string, std::string>& values,
  std::map<std::string, std::vector<std::string>>& lists);

// clock

Millis SystemClock::mono() const
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (Millis)ts.tv_sec * 1000 + (Millis)(ts.tv_nsec / 1000000);
}

uint32_t SystemClock::wall() const
{
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (uint32_t)ts.tv_sec;
}

// 2020-01-01. Anything earlier means the clock has not been set yet.
bool SystemClock::wallLooksSynced(uint32_t seconds)
{
  return seconds > 1577836800u;
}

// store

FileStore::FileStore(std::string directory)
  : directory_(std::move(directory))
{
  mkdir(directory_.c_str(), 0700);
}

std::string FileStore::pathFor(std::string_view key) const
{
  return directory_ + "/" + std::string(key) + ".dat";
}

std::optional<std::vector<uint8_t>> FileStore::read(std::string_view key)
{
  const std::string path = pathFor(key);
  int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return std::nullopt;

  struct stat info;
  if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
    close(fd);
    return std::nullopt;
  }

  std::vector<uint8_t> data((size_t)info.st_size);
  size_t filled = 0;
  while (filled < data.size()) {
    const ssize_t got = ::read(fd, data.data() + filled, data.size() - filled);
    if (got < 0 && errno == EINTR) continue;
    if (got <= 0) {
      close(fd);
      return std::nullopt;
    }
    filled += (size_t)got;
  }
  close(fd);
  return data;
}

bool FileStore::write(std::string_view key, ByteView data)
{
  const std::string target = pathFor(key);
  const std::string temporary = target + ".tmp";

  int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) return false;

  size_t written = 0;
  while (written < data.size()) {
    const ssize_t chunk = ::write(fd, data.data() + written, data.size() - written);
    if (chunk < 0 && errno == EINTR) continue;
    if (chunk < 0) {
      close(fd);
      unlink(temporary.c_str());
      return false;
    }
    written += (size_t)chunk;
  }

  if (fsync(fd) != 0 || close(fd) != 0) {
    unlink(temporary.c_str());
    return false;
  }
  if (rename(temporary.c_str(), target.c_str()) != 0) {
    unlink(temporary.c_str());
    return false;
  }

  // The rename itself has to reach the disk, not only the contents.
  int dirFd = ::open(directory_.c_str(), O_RDONLY | O_DIRECTORY);
  if (dirFd >= 0) {
    fsync(dirFd);
    close(dirFd);
  }
  return true;
}

std::optional<std::vector<uint8_t>> MemoryStore::read(std::string_view key)
{
  auto found = entries_.find(std::string(key));
  if (found == entries_.end()) return std::nullopt;
  return found->second;
}

bool MemoryStore::write(std::string_view key, ByteView data)
{
  entries_[std::string(key)].assign(data.begin(), data.end());
  return true;
}

// log

void Log::setLevel(LogLevel level)
{
  level_ = level;
}

LogLevel Log::level()
{
  return level_;
}

void Log::write(LogLevel level, const char* format, ...)
{
  if ((int)level > (int)level_) return;

  static const char* names[] = { "ERROR", "WARN", "INFO", "DEBUG" };
  std::fprintf(stderr, "[%s] ", names[(int)level]);

  va_list args;
  va_start(args, format);
  std::vfprintf(stderr, format, args);
  va_end(args);

  std::fputc('\n', stderr);
}


// config

static std::string scalarToText(const Json& node)
{
  if (node.is_string()) return node.get<std::string>();
  if (node.is_boolean()) return node.get<bool>() ? "true" : "false";
  if (node.is_null()) return std::string();
  return node.dump();
}

static bool flatten(const Json& node,
  const std::string& prefix,
  std::map<std::string, std::string>& values,
  std::map<std::string, std::vector<std::string>>& lists)
{
  if (node.is_object()) {
    for (const auto& [key, child] : node.items()) {
      if (key.empty()) return false;
      const std::string full = prefix.empty() ? key : prefix + "." + key;
      if (!flatten(child, full, values, lists)) return false;
    }
    return true;
  }

  if (node.is_array()) {
    std::vector<std::string> items;
    std::string joined;
    for (const auto& element : node) {
      if (element.is_object() || element.is_array()) return false;
      if (!items.empty()) joined += ",";
      items.push_back(scalarToText(element));
      joined += items.back();
    }
    values[prefix] = joined;
    lists[prefix] = std::move(items);
    return true;
  }

  values[prefix] = scalarToText(node);
  return true;
}

bool Config::loadFromString(std::string_view text)
{
  const Json document = Json::parse(text, nullptr, false, false);
  if (document.is_discarded()) return false;
  if (!document.is_object()) return false;

  std::map<std::string, std::string> values;
  std::map<std::string, std::vector<std::string>> lists;
  if (!flatten(document, std::string(), values, lists)) return false;

  values_ = std::move(values);
  lists_ = std::move(lists);
  return true;
}

bool Config::loadFile(const std::string& path)
{
  int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return false;

  std::string text;
  char buffer[512];
  ssize_t got = 0;
  while ((got = ::read(fd, buffer, sizeof buffer)) > 0)
    text.append(buffer, (size_t)got);
  close(fd);

  return got == 0 && loadFromString(text);
}

bool Config::has(std::string_view key) const
{
  return values_.find(std::string(key)) != values_.end();
}

std::string Config::get(std::string_view key, std::string_view fallback) const
{
  auto found = values_.find(std::string(key));
  return found == values_.end() ? std::string(fallback) : found->second;
}

std::vector<std::string> Config::getList(std::string_view key) const
{
  auto found = lists_.find(std::string(key));
  return found == lists_.end() ? std::vector<std::string> {} : found->second;
}

long Config::getInt(std::string_view key, long fallback) const
{
  auto found = values_.find(std::string(key));
  if (found == values_.end() || found->second.empty()) return fallback;

  char* end = nullptr;
  const long value = std::strtol(found->second.c_str(), &end, 10);
  return (end != nullptr && *end == '\0') ? value : fallback;
}

bool Config::getBool(std::string_view key, bool fallback) const
{
  auto found = values_.find(std::string(key));
  if (found == values_.end()) return fallback;

  const std::string& value = found->second;
  if (value == "true" || value == "yes" || value == "1") return true;
  if (value == "false" || value == "no" || value == "0") return false;
  return fallback;
}
