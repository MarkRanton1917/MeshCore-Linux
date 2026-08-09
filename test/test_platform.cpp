// Copyright (c) 2026 Vladimir Egorov
// This project is licensed under the MIT License.
// See the LICENSE file in the root of the repository for the full license text.

// Tests for src/platform.
//
// The point of this module is that nothing else makes a system call, so what
// matters here is the two clocks staying separate and the store surviving a
// power cut halfway through a write.

#include "check.h"
#include "platform.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <string>
#include <vector>

using check::section;
using check::that;

static ByteView view(const std::vector<uint8_t>& v)
{
  return ByteView { v.data(), v.size() };
}

static void testFakeClock()
{
  section("platform: fake clock");

  platform::FakeClock clock;
  clock.setMono(1000);
  clock.setWall(1700000000);

  that("starts where it was put", clock.mono() == 1000 && clock.wall() == 1700000000);

  clock.advance(2500);
  that("monotonic advances in milliseconds", clock.mono() == 3500);
  that("wall advances in seconds", clock.wall() == 1700000002);

  // The two must be independent: an NTP jump moves wall time only.
  clock.setWall(1800000000);
  that("wall can jump without touching monotonic", clock.mono() == 3500 && clock.wall() == 1800000000);
}

static void testSystemClock()
{
  section("platform: system clock");

  platform::SystemClock clock;
  const platform::Millis first = clock.mono();
  const platform::Millis second = clock.mono();
  that("monotonic never goes backwards", second >= first);
  that("wall time looks like a unix timestamp", clock.wall() > 1577836800u);

  // A node advertising itself dated 1970 corrupts its neighbours' records.
  that("1970 is rejected as unsynced", !platform::SystemClock::wallLooksSynced(0));
  that("a current timestamp passes", platform::SystemClock::wallLooksSynced(clock.wall()));
}

static void testMemoryStore()
{
  section("platform: memory store");

  platform::MemoryStore store;
  that("missing key reads as nothing", !store.read("identity").has_value());

  std::vector<uint8_t> data { 1, 2, 3, 4 };
  that("write succeeds", store.write("identity", view(data)));

  auto back = store.read("identity");
  that("reads back what was written", back && *back == data);

  std::vector<uint8_t> replacement { 9 };
  store.write("identity", view(replacement));
  that("overwrites in place", store.read("identity")->size() == 1);

  that("keys are independent", !store.read("contacts").has_value());
}

static void testFileStore()
{
  section("platform: file store");

  char templ[] = "meshcore_platform_XXXXXX";
  const std::string dir = mkdtemp(templ);
  {
    platform::FileStore store(dir);
    that("missing key reads as nothing", !store.read("contacts").has_value());

    std::vector<uint8_t> data(300);
    for (size_t i = 0; i < data.size(); i++)
      data[i] = (uint8_t)i;
    that("write succeeds", store.write("contacts", view(data)));

    auto back = store.read("contacts");
    that("round-trip", back && *back == data);

    struct stat info {};
    that("file mode is 0600", stat((dir + "/contacts.dat").c_str(), &info) == 0 && (info.st_mode & 07777) == 0600);
    that("no temporary left behind", stat((dir + "/contacts.dat.tmp").c_str(), &info) != 0);

    std::vector<uint8_t> shorter { 7, 7 };
    store.write("contacts", view(shorter));
    that("rewrite truncates", store.read("contacts")->size() == 2);

    std::vector<uint8_t> empty;
    that("empty value is legal",
      store.write("empty", view(empty)) && store.read("empty").has_value() && store.read("empty")->empty());
  }
  {
    // A fresh store over the same directory sees the same data: this is what a
    // restart looks like.
    platform::FileStore store(dir);
    that("data survives a restart", store.read("contacts").has_value());
  }

  for (const char* name : { "contacts.dat", "empty.dat" })
    unlink((dir + "/" + name).c_str());
  rmdir(dir.c_str());
}

static void testConfig()
{
  section("platform: config");

  platform::Config config;
  const char* text = "{\n"
                     "  \"name\": \"node-one\",\n"
                     "  \"radio\": {\n"
                     "    \"frequency\": 869525000,\n"
                     "    \"spreading\": 11,\n"
                     "    \"peers\": [\"127.0.0.1:1\", \"127.0.0.1:2\"],\n"
                     "    \"group\": null\n"
                     "  },\n"
                     "  \"room\": { \"anonymous\": true, \"quiet\": false }\n"
                     "}\n";
  that("parses", config.loadFromString(text));

  that("top-level key", config.get("name") == "node-one");
  that("nesting prefixes the key", config.getInt("radio.frequency") == 869525000);
  that("second key in an object", config.getInt("radio.spreading") == 11);
  that("booleans", config.getBool("room.anonymous") && !config.getBool("room.quiet"));

  const std::vector<std::string> peers = config.getList("radio.peers");
  that("arrays become lists", peers.size() == 2 && peers[1] == "127.0.0.1:2");
  that("an array also reads as text", config.get("radio.peers") == "127.0.0.1:1,127.0.0.1:2");
  that("a scalar has no list", config.getList("name").empty());

  that("null is empty", config.has("radio.group") && config.get("radio.group", "x").empty());

  that("missing key falls back", config.get("nothing", "default") == "default");
  that("missing int falls back", config.getInt("radio.nothing", 42) == 42);
  that("non-numeric int falls back", config.getInt("name", 7) == 7);
  that("has() reports presence", config.has("name") && !config.has("absent"));

  platform::Config broken;
  that("malformed json is an error", !broken.loadFromString("{ \"a\": }"));
  that("a bare value is not a config", !broken.loadFromString("42"));
  that("an array of objects is refused", !broken.loadFromString("{ \"a\": [ {} ] }"));
  that("a comment is not json", !broken.loadFromString("// hello\n{}"));

  that("a rejected file changes nothing",
    !config.loadFromString("{ \"name\": \"other\", ") && config.get("name") == "node-one");
}

static void testLogLevels()
{
  section("platform: log levels");

  platform::Log::setLevel(platform::LogLevel::WARN);
  that("level is remembered", platform::Log::level() == platform::LogLevel::WARN);

  // The failed decrypt on every 256th foreign packet has to sit below the
  // default, or the log is unusable on a busy network.
  that("debug is quieter than the default", (int)platform::LogLevel::DEBUG > (int)platform::LogLevel::INFO);

  platform::Log::setLevel(platform::LogLevel::INFO);
  that("restored", platform::Log::level() == platform::LogLevel::INFO);
}

int main()
{
  testFakeClock();
  testSystemClock();
  testMemoryStore();
  testFileStore();
  testConfig();
  testLogLevels();
  return check::report();
}
