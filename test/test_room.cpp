// Copyright (c) 2026 Vladimir Egorov
// This project is licensed under the MIT License.
// See the LICENSE file in the root of the repository for the full license text.

// Tests for src/room.
//
// No network here at all: room depends only on abstractions, so a fake sender
// that collects outgoing calls in a vector plus a hand-cranked clock covers the
// whole module.

#include "check.h"
#include "platform.h"
#include "room.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <map>
#include <string>
#include <vector>

using check::section;
using check::that;

namespace core = crypto::core;

// ------------------------------------------------------------------ harness

struct Outgoing {
  core::PublicKey to;
  packet::PayloadType type = packet::PayloadType::REQ;
  std::vector<uint8_t> payload;
  bool wantAck = false;
  routing::SendId id = 0;
};

class FakeSender : public room::Sender {
public:
  routing::SendId sendDirect(const core::PublicKey& to,
    packet::PayloadType type,
    ByteView payload,
    bool wantAck) override
  {
    Outgoing item;
    item.to = to;
    item.type = type;
    item.payload.assign(payload.begin(), payload.end());
    item.wantAck = wantAck;
    item.id = nextId++;
    sent.push_back(std::move(item));
    return sent.back().id;
  }

  void sendFlood(packet::PayloadType type, ByteView payload) override
  {
    Outgoing item;
    item.type = type;
    item.payload.assign(payload.begin(), payload.end());
    flooded.push_back(std::move(item));
  }

  std::vector<Outgoing> sent;
  std::vector<Outgoing> flooded;
  routing::SendId nextId = 1;
};

// The host a command reaches for, reduced to counters. Every refusal the real
// one can produce — no permission for the clock, a read-only prefs file — is a
// flag here, because those are the paths that answer rather than act.
class FakeAdmin : public room::Admin {
public:
  void sendAdvert() override
  {
    adverts++;
  }

  bool setClock(uint32_t unixSeconds) override
  {
    if (!clockSettable) return false;
    clockSetTo = unixSeconds;
    return true;
  }

  std::string nodeName() const override
  {
    return name;
  }

  bool setNodeName(std::string_view value) override
  {
    if (!saveSetting("node.name", value)) return false;
    name.assign(value);
    return true;
  }

  bool saveSetting(std::string_view key, std::string_view value) override
  {
    if (!settingsWritable) return false;
    settings[std::string(key)] = std::string(value);
    return true;
  }

  void requestReboot() override
  {
    reboots++;
  }

  uint32_t uptime() const override
  {
    return uptimeSeconds;
  }

  int adverts = 0;
  int reboots = 0;
  uint32_t clockSetTo = 0;
  bool clockSettable = true;
  bool settingsWritable = true;
  std::string name = "room";
  uint32_t uptimeSeconds = 0;
  std::map<std::string, std::string> settings;
};

static std::string makeDir()
{
  char templ[] = "meshcore_room_XXXXXX";
  const char* dir = mkdtemp(templ);
  return dir ? std::string(dir) : std::string();
}

static void removeDir(const std::string& dir)
{
  for (const char* name : { "identity.key", "contacts.dat", "room.dat", "room.dat.tmp" }) {
    unlink((dir + "/" + name).c_str());
  }
  rmdir(dir.c_str());
}

static core::PublicKey keyOf(uint8_t tag)
{
  core::PublicKey pk;
  pk.data.fill(tag);
  return pk;
}

// Login plaintext: timestamp(4) syncSince(4) password(rest).
static std::vector<uint8_t> loginPayload(uint32_t timestamp, uint32_t syncSince, const std::string& password)
{
  std::vector<uint8_t> out;
  for (int i = 0; i < 4; i++)
    out.push_back((uint8_t)(timestamp >> (8 * i)));
  for (int i = 0; i < 4; i++)
    out.push_back((uint8_t)(syncSince >> (8 * i)));
  out.insert(out.end(), password.begin(), password.end());
  return out;
}

static std::vector<uint8_t> textPayload(uint32_t timestamp, room::TextType txtType, const std::string& body)
{
  packet::TextMsg message;
  message.timestamp = timestamp;
  message.txtType = (uint8_t)txtType;
  message.attempt = 0;
  message.message = ByteView { (const uint8_t*)body.data(), body.size() };

  std::vector<uint8_t> out(MAX_PACKET_PAYLOAD);
  auto size = packet::encodeText(message, ByteSpan { out.data(), out.size() });
  out.resize(size ? *size : 0);
  return out;
}

// REQ plaintext: timestamp(4) type(1) argument(rest).
static std::vector<uint8_t> requestPayload(uint32_t timestamp, room::RequestType type)
{
  std::vector<uint8_t> out;
  for (int i = 0; i < 4; i++)
    out.push_back((uint8_t)(timestamp >> (8 * i)));
  out.push_back((uint8_t)type);
  return out;
}

// A contact standing in for one that identity would have produced.
static identity::Contact contactOf(const core::PublicKey& pk)
{
  identity::Contact contact;
  contact.pk = pk;
  return contact;
}

struct Fixture {
  std::string dir = makeDir();
  identity::Store store;
  FakeSender sender;
  room::Room roomInstance;

  explicit Fixture(room::Config config)
    : store(),
      sender(),
      roomInstance(store, sender, config)
  {
    store.loadOrCreate(dir);
    roomInstance.load(dir);
    roomInstance.setServerTime(10000);
  }
  ~Fixture()
  {
    removeDir(dir);
  }
};

static room::Config defaultConfig()
{
  room::Config config;
  config.adminPassword = "admin-secret";
  config.guestPassword = "guest";
  return config;
}

// A command reply is a text message like any other, minus the author prefix a
// pushed post carries.
static std::string replyText(const Outgoing& out)
{
  auto message = packet::decodeText(ByteView { out.payload.data(), out.payload.size() });
  if (!message.has_value() || (room::TextType)message->txtType != room::TextType::CLI) return {};
  return std::string((const char*)message->message.data(), message->message.size());
}

static void runCommand(room::Room& room, const core::PublicKey& from, uint32_t timestamp, const std::string& line)
{
  const identity::Contact contact = contactOf(from);
  auto payload = textPayload(timestamp, room::TextType::CLI, line);
  room.onPayload(contact, packet::PayloadType::TXT_MSG, ByteView { payload.data(), payload.size() });
}

static bool contains(const std::string& haystack, const std::string& needle)
{
  return haystack.find(needle) != std::string::npos;
}

// Everything a client receives for a post, "name: " and all.
static std::string pushedBody(const Outgoing& out)
{
  auto message = packet::decodeText(ByteView { out.payload.data(), out.payload.size() });
  if (!message.has_value()) return {};
  return std::string((const char*)message->message.data(), message->message.size());
}

// And the message on its own, which is what most of these tests care about.
static std::string pushedText(const Outgoing& out)
{
  const std::string body = pushedBody(out);
  const size_t colon = body.find(": ");
  return colon == std::string::npos ? body : body.substr(colon + 2);
}

// ------------------------------------------------------------------ tests

static void testLogin()
{
  section("room: login");

  Fixture fixture(defaultConfig());
  auto& room = fixture.roomInstance;
  const core::PublicKey guest = keyOf(0x11);

  auto payload = loginPayload(1000, 0, "guest");
  room.onAnon(guest, ByteView { payload.data(), payload.size() });

  that("a response went out", fixture.sender.sent.size() == 1);
  that("it is a RESPONSE",
    fixture.sender.sent.size() == 1 && fixture.sender.sent[0].type == packet::PayloadType::RESPONSE);
  that("client remembered", room.clientCount() == 1);
  that("guest is not admin", room.findClient(guest) && room.findClient(guest)->access == room::Access::GUEST);

  // Replaying the same packet must change nothing.
  room.onAnon(guest, ByteView { payload.data(), payload.size() });
  that("a replayed login is ignored", fixture.sender.sent.size() == 1);

  auto older = loginPayload(999, 0, "guest");
  room.onAnon(guest, ByteView { older.data(), older.size() });
  that("an older login is ignored", fixture.sender.sent.size() == 1);

  auto newer = loginPayload(1001, 0, "guest");
  room.onAnon(guest, ByteView { newer.data(), newer.size() });
  that("a newer login is accepted", fixture.sender.sent.size() == 2);

  // Two responses with the same content must still differ on the wire.
  that("responses differ despite identical content", fixture.sender.sent[0].payload != fixture.sender.sent[1].payload);

  const core::PublicKey admin = keyOf(0x22);
  auto adminLogin = loginPayload(2000, 0, "admin-secret");
  room.onAnon(admin, ByteView { adminLogin.data(), adminLogin.size() });
  that("admin password grants admin", room.findClient(admin) && room.findClient(admin)->access == room::Access::ADMIN);

  const core::PublicKey stranger = keyOf(0x33);
  auto wrong = loginPayload(3000, 0, "nope");
  const size_t before = fixture.sender.sent.size();
  room.onAnon(stranger, ByteView { wrong.data(), wrong.size() });
  that("a wrong password is refused", fixture.sender.sent.size() == before && room.findClient(stranger) == nullptr);

  auto tiny = loginPayload(4000, 0, "");
  tiny.resize(3);
  room.onAnon(stranger, ByteView { tiny.data(), tiny.size() });
  that("a truncated login is ignored", room.findClient(stranger) == nullptr);
}

static void testAnonymousRead()
{
  section("room: anonymous read");

  room::Config config = defaultConfig();
  config.allowAnonymousRead = true;
  Fixture fixture(config);
  auto& room = fixture.roomInstance;

  const core::PublicKey visitor = keyOf(0x44);
  auto payload = loginPayload(1000, 0, "not-the-password");
  room.onAnon(visitor, ByteView { payload.data(), payload.size() });

  const room::Client* client = room.findClient(visitor);
  that("let in with cut-down rights", client && client->access == room::Access::READ_ONLY);
  that("may read", client && room::Room::can(*client, room::Action::READ));
  that("may not post", client && !room::Room::can(*client, room::Action::POST));
  that("may not run commands", client && !room::Room::can(*client, room::Action::COMMAND));
}

static void testPosting()
{
  section("room: posting");

  Fixture fixture(defaultConfig());
  auto& room = fixture.roomInstance;

  const core::PublicKey guest = keyOf(0x11);
  auto login = loginPayload(1000, 0, "guest");
  room.onAnon(guest, ByteView { login.data(), login.size() });

  auto post = textPayload(10000, room::TextType::PLAIN, "hello room");
  room.onPayload(contactOf(guest), packet::PayloadType::TXT_MSG, ByteView { post.data(), post.size() });
  that("post accepted", room.postCount() == 1);
  that("text stored", room.postCount() == 1 && room.posts()[0].text == "hello room");
  that("author prefix stored",
    room.postCount() == 1 && std::memcmp(room.posts()[0].author.data(), guest.data.data(), POST_AUTHOR_PREFIX) == 0);

  // A read-only client must not be able to write.
  room::Config readerConfig = defaultConfig();
  readerConfig.allowAnonymousRead = true;
  Fixture readerFixture(readerConfig);
  const core::PublicKey visitor = keyOf(0x55);
  auto visitorLogin = loginPayload(1000, 0, "wrong");
  readerFixture.roomInstance.onAnon(visitor, ByteView { visitorLogin.data(), visitorLogin.size() });
  readerFixture.roomInstance.onPayload(
    contactOf(visitor), packet::PayloadType::TXT_MSG, ByteView { post.data(), post.size() });
  that("a read-only client cannot post", readerFixture.roomInstance.postCount() == 0);

  // Over-long text is truncated, not refused: the client never learns why.
  // The margin is narrow — anything past ~179 characters does not fit a frame
  // at all and never reaches us.
  const std::string tooLong(MAX_POST_TEXT + 20, 'x');
  auto longPost = textPayload(10001, room::TextType::PLAIN, tooLong);
  room.onPayload(contactOf(guest), packet::PayloadType::TXT_MSG, ByteView { longPost.data(), longPost.size() });
  that("long text truncated, not rejected", room.postCount() == 2 && room.posts()[1].text.size() == MAX_POST_TEXT);

  // A clock from the future must not poison the ordering.
  auto future = textPayload(99999999, room::TextType::PLAIN, "from the future");
  room.onPayload(contactOf(guest), packet::PayloadType::TXT_MSG, ByteView { future.data(), future.size() });
  that("a wild timestamp is replaced with the server's", room.postCount() == 3 && room.posts()[2].timestamp == 10000);

  auto sane = textPayload(10050, room::TextType::PLAIN, "close enough");
  room.onPayload(contactOf(guest), packet::PayloadType::TXT_MSG, ByteView { sane.data(), sane.size() });
  that("a plausible timestamp is kept", room.postCount() == 4 && room.posts()[3].timestamp == 10050);

  // An unknown sender has no client row.
  const core::PublicKey nobody = keyOf(0x66);
  room.onPayload(contactOf(nobody), packet::PayloadType::TXT_MSG, ByteView { post.data(), post.size() });
  that("a sender who never logged in is ignored", room.postCount() == 4);
}

static void testAckPolicy()
{
  section("room: acknowledgement policy");

  Fixture fixture(defaultConfig());
  auto& room = fixture.roomInstance;

  auto post = textPayload(10000, room::TextType::PLAIN, "post");
  that("a post is acknowledged", room.shouldAck(packet::PayloadType::TXT_MSG, ByteView { post.data(), post.size() }));

  auto command = textPayload(10000, room::TextType::CLI, "reboot");
  that("a CLI command is not acknowledged",
    !room.shouldAck(packet::PayloadType::TXT_MSG, ByteView { command.data(), command.size() }));

  // Even a refused post gets one, or the client retries to exhaustion.
  room::Config readerConfig = defaultConfig();
  readerConfig.allowAnonymousRead = true;
  Fixture readerFixture(readerConfig);
  const core::PublicKey visitor = keyOf(0x55);
  auto login = loginPayload(1000, 0, "wrong");
  readerFixture.roomInstance.onAnon(visitor, ByteView { login.data(), login.size() });
  readerFixture.roomInstance.onPayload(
    contactOf(visitor), packet::PayloadType::TXT_MSG, ByteView { post.data(), post.size() });
  that("a post refused on rights is still acknowledged",
    readerFixture.roomInstance.shouldAck(packet::PayloadType::TXT_MSG, ByteView { post.data(), post.size() }));

  that("the room stays a repeater", room.shouldForward(packet::Packet {}));
}

static void testSyncFlow()
{
  section("room: handing posts over");

  Fixture fixture(defaultConfig());
  auto& room = fixture.roomInstance;

  const core::PublicKey author = keyOf(0x11);
  const core::PublicKey reader = keyOf(0x22);

  auto authorLogin = loginPayload(1000, 0, "guest");
  room.onAnon(author, ByteView { authorLogin.data(), authorLogin.size() });
  auto readerLogin = loginPayload(1000, 0, "guest");
  room.onAnon(reader, ByteView { readerLogin.data(), readerLogin.size() });
  fixture.sender.sent.clear();

  for (int i = 0; i < 3; i++) {
    auto post = textPayload((uint32_t)(10000 + i), room::TextType::PLAIN, "post " + std::to_string(i));
    room.onPayload(contactOf(author), packet::PayloadType::TXT_MSG, ByteView { post.data(), post.size() });
  }
  that("three posts stored", room.postCount() == 3);

  // One post per tick, and only after the ack does the bookmark move.
  room.tick(1000);
  that("one post pushed", fixture.sender.sent.size() == 1);
  that("pushed to the reader, not the author",
    std::memcmp(fixture.sender.sent[0].to.data.data(), reader.data.data(), 32) == 0);
  that("acknowledgement requested", fixture.sender.sent[0].wantAck);
  that("it is the oldest post", pushedText(fixture.sender.sent[0]) == "post 0");

  room.tick(1100);
  that("nothing else while one is in flight", fixture.sender.sent.size() == 1);

  room.onAck(fixture.sender.sent[0].id);
  that("bookmark moved on ack", room.findClient(reader) && room.findClient(reader)->syncSeq == 1);

  room.tick(1200);
  that("next post follows", fixture.sender.sent.size() == 2 && pushedText(fixture.sender.sent[1]) == "post 1");

  // A failed delivery must not move the bookmark.
  room.onDeliveryFailed(fixture.sender.sent[1].id);
  that("bookmark unchanged after a failure", room.findClient(reader) && room.findClient(reader)->syncSeq == 1);

  room.tick(1300);
  that("held back during the retry pause", fixture.sender.sent.size() == 2);

  room.tick(1300 + 6000);
  that("same post retried after the pause",
    fixture.sender.sent.size() == 3 && pushedText(fixture.sender.sent[2]) == "post 1");

  // The author never gets its own posts back.
  bool authorGotAnything = false;
  for (const Outgoing& out : fixture.sender.sent) {
    if (std::memcmp(out.to.data.data(), author.data.data(), 32) == 0) authorGotAnything = true;
  }
  that("the author is not sent its own posts", !authorGotAnything);
}

static void testRoundRobin()
{
  section("room: round robin between clients");

  Fixture fixture(defaultConfig());
  auto& room = fixture.roomInstance;

  const core::PublicKey author = keyOf(0x11);
  const core::PublicKey slow = keyOf(0x22);
  const core::PublicKey quick = keyOf(0x33);

  for (const core::PublicKey& pk : { author, slow, quick }) {
    auto login = loginPayload(1000, 0, "guest");
    room.onAnon(pk, ByteView { login.data(), login.size() });
  }
  fixture.sender.sent.clear();

  for (int i = 0; i < 2; i++) {
    auto post = textPayload((uint32_t)(10000 + i), room::TextType::PLAIN, "p" + std::to_string(i));
    room.onPayload(contactOf(author), packet::PayloadType::TXT_MSG, ByteView { post.data(), post.size() });
  }

  room.tick(1000);
  room.tick(1100);
  that("both readers were served", fixture.sender.sent.size() == 2);

  bool servedSlow = false, servedQuick = false;
  for (const Outgoing& out : fixture.sender.sent) {
    if (std::memcmp(out.to.data.data(), slow.data.data(), 32) == 0) servedSlow = true;
    if (std::memcmp(out.to.data.data(), quick.data.data(), 32) == 0) servedQuick = true;
  }
  that("neither starved the other", servedSlow && servedQuick);

  // The first client never acknowledges; the second must keep being served.
  room.onAck(fixture.sender.sent[1].id);
  room.tick(1200);
  that("the silent client does not block the other", fixture.sender.sent.size() == 3);
}

static void testRingBuffer()
{
  section("room: the ring buffer drops the oldest");

  Fixture fixture(defaultConfig());
  auto& room = fixture.roomInstance;

  const core::PublicKey author = keyOf(0x11);
  auto login = loginPayload(1000, 0, "guest");
  room.onAnon(author, ByteView { login.data(), login.size() });

  for (int i = 0; i < MAX_POSTS; i++) {
    auto post = textPayload((uint32_t)(10000 + i), room::TextType::PLAIN, "p" + std::to_string(i));
    room.onPayload(contactOf(author), packet::PayloadType::TXT_MSG, ByteView { post.data(), post.size() });
  }
  that("buffer full", room.postCount() == MAX_POSTS);
  that("oldest is the first written", room.posts()[0].text == "p0");

  auto extra = textPayload(10000 + MAX_POSTS, room::TextType::PLAIN, "p32");
  room.onPayload(contactOf(author), packet::PayloadType::TXT_MSG, ByteView { extra.data(), extra.size() });

  that("still capped at 32", room.postCount() == MAX_POSTS);
  that("post 33 evicted post 1", room.posts()[0].text == "p1");
  that("newest kept", room.posts()[MAX_POSTS - 1].text == "p32");

  // A client that fell 33 behind loses exactly the evicted one.
  const core::PublicKey latecomer = keyOf(0x22);
  auto lateLogin = loginPayload(1000, 0, "guest");
  room.onAnon(latecomer, ByteView { lateLogin.data(), lateLogin.size() });
  room.tick(1000);
  that("the latecomer starts from what is left",
    fixture.sender.sent.size() >= 1 && pushedText(fixture.sender.sent.back()) == "p1");
}

static void testPersistence()
{
  section("room: state survives a restart");

  const std::string dir = makeDir();
  const core::PublicKey author = keyOf(0x11);

  {
    identity::Store store;
    FakeSender sender;
    room::Room instance(store, sender, defaultConfig());
    store.loadOrCreate(dir);
    instance.load(dir);
    instance.setServerTime(10000);

    auto login = loginPayload(1000, 5, "guest");
    instance.onAnon(author, ByteView { login.data(), login.size() });
    auto post = textPayload(10000, room::TextType::PLAIN, "remembered");
    instance.onPayload(contactOf(author), packet::PayloadType::TXT_MSG, ByteView { post.data(), post.size() });

    that("flush succeeds", instance.flush());
  }
  {
    identity::Store store;
    FakeSender sender;
    room::Room instance(store, sender, defaultConfig());
    store.loadOrCreate(dir);
    that("reload succeeds", instance.load(dir));
    that("posts restored", instance.postCount() == 1 && instance.posts()[0].text == "remembered");
    that("clients restored", instance.clientCount() == 1);

    const room::Client* client = instance.findClient(author);
    that("rights restored", client && client->access == room::Access::GUEST);
    that("bookmark restored", client && client->syncSeq == 0);
    that("replay guard restored", client && client->lastLogin == 1000);
    that("the post kept its seq", instance.posts()[0].seq == 1);

    // The counter has to survive too. Handing the next post seq 1 again would
    // make it look already delivered to anyone bookmarked past it.
    instance.addPost(keyOf(0x77), 10001, "after the restart");
    that("the seq counter carried over", instance.posts()[1].seq == 2);

    // And it still refuses a replayed login after the restart.
    auto replay = loginPayload(1000, 0, "guest");
    instance.onAnon(author, ByteView { replay.data(), replay.size() });
    that("replay still refused after restart", sender.sent.empty());
  }

  removeDir(dir);
}

// Two posts inside one second are ordinary — a client sends both halves of a
// thought, or two people answer at once. A bookmark that is a timestamp cannot
// name the boundary between them, so the second was skipped for good.
static void testSameSecondPosts()
{
  section("room: posts sharing a second");

  Fixture fixture(defaultConfig());
  auto& room = fixture.roomInstance;
  auto& sent = fixture.sender.sent;

  const core::PublicKey author = keyOf(0x11);
  const core::PublicKey reader = keyOf(0x22);

  auto authorLogin = loginPayload(1000, 0, "guest");
  room.onAnon(author, ByteView { authorLogin.data(), authorLogin.size() });
  auto readerLogin = loginPayload(1000, 0, "guest");
  room.onAnon(reader, ByteView { readerLogin.data(), readerLogin.size() });
  sent.clear();

  for (const char* text : { "first", "second", "third" }) {
    auto post = textPayload(10000, room::TextType::PLAIN, text);
    room.onPayload(contactOf(author), packet::PayloadType::TXT_MSG, ByteView { post.data(), post.size() });
  }
  that("all three stored", room.postCount() == 3);
  that("each got its own seq", room.posts()[0].seq == 1 && room.posts()[1].seq == 2 && room.posts()[2].seq == 3);
  that("and they really do share a second", room.posts()[0].timestamp == room.posts()[2].timestamp);

  // One per tick, each acknowledged, and none of them lost on the way.
  std::vector<std::string> delivered;
  for (int round = 0; round < 3; round++) {
    room.tick((platform::Millis)(1000 + round * 100));
    if (sent.empty()) break;

    delivered.push_back(pushedText(sent.back()));
    room.onAck(sent.back().id);
  }

  that("every post reached the reader", delivered.size() == 3);
  that("in order, none skipped",
    delivered.size() == 3 && delivered[0] == "first" && delivered[1] == "second" && delivered[2] == "third");

  room.tick(2000);
  that("and nothing is sent twice", sent.size() == 3);
}

// A REQ is how a client asks without posting anything: how many unread posts
// are waiting, and — after a sleep — a nudge to start sending them again.
static void testRequests()
{
  section("room: status requests");

  Fixture fixture(defaultConfig());
  auto& room = fixture.roomInstance;
  auto& sent = fixture.sender.sent;

  const core::PublicKey author = keyOf(0x11);
  const core::PublicKey reader = keyOf(0x22);

  // A stranger gets nothing at all, not even an error.
  auto early = requestPayload(2000, room::RequestType::STATUS);
  room.onPayload(contactOf(reader), packet::PayloadType::REQ, ByteView { early.data(), early.size() });
  that("an unknown client is ignored", sent.empty());

  auto authorLogin = loginPayload(1000, 0, "guest");
  room.onAnon(author, ByteView { authorLogin.data(), authorLogin.size() });
  auto readerLogin = loginPayload(1000, 0, "guest");
  room.onAnon(reader, ByteView { readerLogin.data(), readerLogin.size() });
  sent.clear();

  room.addPost(author, 10000, "one");
  room.addPost(author, 10001, "two");

  auto status = requestPayload(2000, room::RequestType::STATUS);
  room.onPayload(contactOf(reader), packet::PayloadType::REQ, ByteView { status.data(), status.size() });

  that("a status reply went out", sent.size() == 1);
  that("as a RESPONSE", sent.size() == 1 && sent[0].type == packet::PayloadType::RESPONSE);
  that("asking for no ack", sent.size() == 1 && !sent[0].wantAck);

  // serverTime(4) code(1) reqType(1) access(1) posts(1) clients(1) unread(1)...
  const std::vector<uint8_t>& body = sent[0].payload;
  that("the body is the documented length", body.size() == 16);
  that("it names the request it answers", body.size() > 5 && body[5] == (uint8_t)room::RequestType::STATUS);
  that("and carries the rights", body.size() > 6 && body[6] == (uint8_t)room::Access::GUEST);
  that("post count", body.size() > 7 && body[7] == 2);
  that("client count", body.size() > 8 && body[8] == 2);
  that("both posts are unread for the reader", body.size() > 9 && body[9] == 2);

  // The author wrote them, so nothing is waiting for it.
  sent.clear();
  room.onPayload(contactOf(author), packet::PayloadType::REQ, ByteView { status.data(), status.size() });
  that("the author has nothing unread", sent.size() == 1 && sent[0].payload[9] == 0);
  sent.clear();

  // A REQ must never be acked: the response is the receipt.
  that("no ack for a REQ", !room.shouldAck(packet::PayloadType::REQ, ByteView { status.data(), status.size() }));

  // Replay: backwards is refused, the same timestamp is answered again.
  auto older = requestPayload(1999, room::RequestType::STATUS);
  room.onPayload(contactOf(reader), packet::PayloadType::REQ, ByteView { older.data(), older.size() });
  that("an older request is dropped", sent.empty());

  room.onPayload(contactOf(reader), packet::PayloadType::REQ, ByteView { status.data(), status.size() });
  that("the same timestamp is answered again", sent.size() == 1);
  sent.clear();

  // A keep-alive lifts the pause a failed delivery imposed, so the client that
  // has just woken up does not wait out a timer set for a node that was away.
  room.tick(1000);
  that("a push went out", sent.size() == 1);
  room.onDeliveryFailed(sent.back().id);
  sent.clear();

  room.tick(1100);
  that("held back during the retry pause", sent.empty());

  auto alive = requestPayload(2001, room::RequestType::KEEP_ALIVE);
  room.onPayload(contactOf(reader), packet::PayloadType::REQ, ByteView { alive.data(), alive.size() });
  that("the keep-alive is answered", sent.size() == 1);

  room.tick(1200);
  that("and pushing resumes at once", sent.size() == 2);

  auto unknown = requestPayload(2002, (room::RequestType)0x7F);
  sent.clear();
  room.onPayload(contactOf(reader), packet::PayloadType::REQ, ByteView { unknown.data(), unknown.size() });
  that("an unknown request type is ignored", sent.empty());

  std::vector<uint8_t> truncated = requestPayload(2003, room::RequestType::STATUS);
  truncated.resize(3);
  room.onPayload(contactOf(reader), packet::PayloadType::REQ, ByteView { truncated.data(), truncated.size() });
  that("a truncated request is ignored", sent.empty());
}

// A channel is a key, not a roster: anyone holding it can post and nobody
// signs anything. So what arrives may become a post and nothing else, and what
// the board receives directly goes back out so the two do not drift apart.
// Guessing a password over the air is slow but free, and nobody is watching.
// A post is pushed as "name: text". The four raw key bytes this used to send
// were a field only the room understood, and every client rendered them as four
// bytes of rubbish in front of the message.
static void testAuthorPrefix()
{
  section("room: the author a client sees");

  Fixture fixture(defaultConfig());
  auto& room = fixture.roomInstance;
  auto& sent = fixture.sender.sent;

  // An advert is how a name is learned, so the store has to hear one first.
  const core::PublicKey author = keyOf(0x11);
  packet::Advert advert;
  advert.publicKey = author.view();
  advert.timestamp = 100;
  const std::vector<uint8_t> appdata = { ADVERT_HAS_NAME | 1, 'A', 'l', 'i', 'c', 'e' };
  advert.appdata = ByteView { appdata.data(), appdata.size() };
  fixture.store.remember(advert);

  const core::PublicKey reader = keyOf(0x22);
  auto login = loginPayload(1000, 0, "guest");
  room.onAnon(reader, ByteView { login.data(), login.size() });
  sent.clear();

  room.addPost(author, 10000, "hello");
  room.tick(1000);
  that("the name leads the message", sent.size() == 1 && pushedBody(sent[0]) == "Alice: hello");
  room.onAck(sent.back().id);
  sent.clear();

  // Somebody we have never heard an advert from: stable beats blank.
  const core::PublicKey unknown = keyOf(0xAB);
  room.addPost(unknown, 10001, "who am i");
  room.tick(1100);
  that("an unknown author falls back to the stored prefix",
    sent.size() == 1 && pushedBody(sent[0]) == "?abababab: who am i");
  room.onAck(sent.back().id);
  sent.clear();

  // A channel post has no author key at all, so there is nothing to put there.
  const core::PublicKey nobody {};
  room.addPost(nobody, 10002, "over the channel");
  room.tick(1200);
  that("a channel post gets no prefix", sent.size() == 1 && pushedBody(sent[0]) == "over the channel");
  room.onAck(sent.back().id);
  sent.clear();

  // The longest name and the longest text still have to fit one frame, and it
  // is the name that gives way, not what somebody wrote.
  const core::PublicKey verbose = keyOf(0x33);
  packet::Advert loud;
  loud.publicKey = verbose.view();
  loud.timestamp = 100;
  std::vector<uint8_t> longName = { ADVERT_HAS_NAME | 1 };
  for (int i = 0; i < 40; i++)
    longName.push_back('x');
  loud.appdata = ByteView { longName.data(), longName.size() };
  fixture.store.remember(loud);

  const std::string longest(MAX_POST_TEXT, 'y');
  room.addPost(verbose, 10003, longest);
  room.tick(1300);
  that("the name is capped", sent.size() == 1 && pushedBody(sent[0]).find(':') == MAX_POST_AUTHOR_NAME);
  that("and the text survives whole", sent.size() == 1 && pushedText(sent[0]) == longest);
  that("the whole body still fits a frame", sent.size() == 1 && pushedBody(sent[0]).size() <= MAX_PUSH_BODY);
}

static void testLoginLockout()
{
  section("room: wrong passwords");

  room::Config config = defaultConfig();
  config.maxLoginAttempts = 3;
  config.loginLockout = 60000;

  Fixture fixture(config);
  auto& room = fixture.roomInstance;
  const core::PublicKey guesser = keyOf(0x11);

  room.tick(1000);
  for (uint32_t attempt = 0; attempt < 3; attempt++) {
    auto wrong = loginPayload(1000 + attempt, 0, "nope");
    room.onAnon(guesser, ByteView { wrong.data(), wrong.size() });
  }
  that("no guess got in", room.clientCount() == 0 && fixture.sender.sent.empty());

  // The right password now, and it must still be refused.
  auto right = loginPayload(2000, 0, "guest");
  room.onAnon(guesser, ByteView { right.data(), right.size() });
  that("the key is locked out even with the right password", room.clientCount() == 0);

  // Somebody else is unaffected: one shared counter would let anyone close the
  // room by guessing badly on purpose.
  const core::PublicKey innocent = keyOf(0x22);
  auto theirs = loginPayload(2000, 0, "guest");
  room.onAnon(innocent, ByteView { theirs.data(), theirs.size() });
  that("another key still gets in", room.findClient(innocent) != nullptr);

  room.tick(1000 + 60000);
  auto afterWait = loginPayload(2001, 0, "guest");
  room.onAnon(guesser, ByteView { afterWait.data(), afterWait.size() });
  that("the lockout expires", room.findClient(guesser) != nullptr);

  // And the count went with it: three more wrong ones are needed, not one.
  auto wrongAgain = loginPayload(2002, 0, "nope");
  room.onAnon(guesser, ByteView { wrongAgain.data(), wrongAgain.size() });
  auto stillFine = loginPayload(2003, 0, "guest");
  room.onAnon(guesser, ByteView { stillFine.data(), stillFine.size() });
  that("one wrong guess after the wait is not enough to lock again",
    room.findClient(guesser) && room.findClient(guesser)->lastLogin == 2003);

  // A replayed login is not a wrong password, or replaying somebody's own
  // login back at them would be enough to lock them out.
  Fixture replayed(config);
  auto& room2 = replayed.roomInstance;
  const core::PublicKey victim = keyOf(0x33);
  auto login = loginPayload(1000, 0, "guest");
  room2.onAnon(victim, ByteView { login.data(), login.size() });
  for (int i = 0; i < 5; i++)
    room2.onAnon(victim, ByteView { login.data(), login.size() });

  auto fresh = loginPayload(1001, 0, "guest");
  room2.onAnon(victim, ByteView { fresh.data(), fresh.size() });
  that("a replay storm does not lock the victim out",
    room2.findClient(victim) && room2.findClient(victim)->lastLogin == 1001);

  // Anonymous reading must not turn the admin password into a free target.
  room::Config open = defaultConfig();
  open.maxLoginAttempts = 3;
  open.allowAnonymousRead = true;
  Fixture guessing(open);
  auto& room3 = guessing.roomInstance;
  const core::PublicKey prober = keyOf(0x44);

  for (uint32_t attempt = 0; attempt < 3; attempt++) {
    auto probe = loginPayload(1000 + attempt, 0, "admin-guess");
    room3.onAnon(prober, ByteView { probe.data(), probe.size() });
  }
  auto admin = loginPayload(2000, 0, "admin-secret");
  room3.onAnon(prober, ByteView { admin.data(), admin.size() });
  that("guessing counts even when anonymous reading lets you in",
    room3.findClient(prober) == nullptr || room3.findClient(prober)->access != room::Access::ADMIN);

  // An empty password is not a guess: it is how an anonymous reader arrives.
  const core::PublicKey reader = keyOf(0x55);
  for (uint32_t attempt = 0; attempt < 5; attempt++) {
    auto anonymous = loginPayload(1000 + attempt, 0, "");
    room3.onAnon(reader, ByteView { anonymous.data(), anonymous.size() });
  }
  that("an anonymous reader is never locked out",
    room3.findClient(reader) && room3.findClient(reader)->access == room::Access::READ_ONLY);
}

// The table is full and somebody new logs in. Dropping the head of the list is
// how the one admin who logs in rarely gets pushed out by a room of guests.
static void testEviction()
{
  section("room: who gets evicted");

  Fixture fixture(defaultConfig());
  auto& room = fixture.roomInstance;

  // An admin arrives first, so it is the head of the list and the old victim.
  const core::PublicKey admin = keyOf(0x01);
  auto adminLogin = loginPayload(1000, 0, "admin-secret");
  room.onAnon(admin, ByteView { adminLogin.data(), adminLogin.size() });

  // Fill the rest with guests, the second one deliberately the least recent.
  for (size_t i = 1; i < MAX_ROOM_CLIENTS; i++) {
    core::PublicKey guest;
    guest.data.fill((uint8_t)i);
    guest.data[1] = (uint8_t)(i >> 8);
    auto login = loginPayload(i == 1 ? 500 : (uint32_t)(2000 + i), 0, "guest");
    room.onAnon(guest, ByteView { login.data(), login.size() });
  }
  that("the table is full", room.clientCount() == MAX_ROOM_CLIENTS);

  core::PublicKey stale;
  stale.data.fill(1);
  stale.data[1] = 0;
  that("the least recent guest is there", room.findClient(stale) != nullptr);

  const core::PublicKey newcomer = keyOf(0xFE);
  auto newLogin = loginPayload(9000, 0, "guest");
  room.onAnon(newcomer, ByteView { newLogin.data(), newLogin.size() });

  that("the newcomer got in", room.findClient(newcomer) != nullptr);
  that("the admin survived", room.findClient(admin) && room.findClient(admin)->access == room::Access::ADMIN);
  that("the guest away longest went instead", room.findClient(stale) == nullptr);
  that("and the table did not grow", room.clientCount() == MAX_ROOM_CLIENTS);
}

static void testChannels()
{
  section("room: channels");

  crypto::core::SharedSecret secret;
  for (size_t i = 0; i < PACKET_SHARED_SECRET_SIZE; i++)
    secret.span()[i] = (uint8_t)(i + 1);

  room::Channel channel;
  channel.name = "public";
  channel.secret = secret;
  channel.hash = room::channelHashOf(secret);

  room::Config config = defaultConfig();
  config.channels.push_back(channel);

  Fixture fixture(config);
  auto& room = fixture.roomInstance;
  auto& flooded = fixture.sender.flooded;

  that("the hash comes from the key alone", room::channelHashOf(secret) == channel.hash);

  // Wrap a text message the way a member would.
  auto seal = [&](const crypto::core::SharedSecret& key, uint8_t hash, const std::string& text) {
    auto plain = textPayload(10000, room::TextType::PLAIN, text);
    std::vector<uint8_t> cipher(MAX_PACKET_PAYLOAD);
    auto sealed =
      crypto::protocol::seal(key, ByteView { plain.data(), plain.size() }, ByteSpan { cipher.data(), cipher.size() });

    packet::GroupMsg group;
    group.channelHash = hash;
    group.cipherMac = sealed->mac.view();
    group.ciphertext = ByteView { cipher.data(), sealed->ciphertextLength };

    std::vector<uint8_t> payload(MAX_PACKET_PAYLOAD);
    auto size = packet::encodeGroup(group, ByteSpan { payload.data(), payload.size() });
    payload.resize(size ? *size : 0);
    return payload;
  };

  auto message = seal(secret, channel.hash, "from the channel");
  room.onGroup(packet::PayloadType::GRP_TXT, ByteView { message.data(), message.size() });
  that("a channel message becomes a post", room.postCount() == 1);
  that("with its text", room.postCount() == 1 && room.posts()[0].text == "from the channel");
  that("and no author, so nobody is excluded from it",
    room.postCount() == 1 && room.posts()[0].author == std::array<uint8_t, POST_AUTHOR_PREFIX> {});
  that("it is not echoed back to the channel it came from", flooded.empty());

  // Right hash, wrong key: the MAC is what actually decides.
  crypto::core::SharedSecret impostor;
  for (size_t i = 0; i < PACKET_SHARED_SECRET_SIZE; i++)
    impostor.span()[i] = (uint8_t)(0xF0 + i);
  auto forged = seal(impostor, channel.hash, "not a member");
  room.onGroup(packet::PayloadType::GRP_TXT, ByteView { forged.data(), forged.size() });
  that("a wrong key is refused even on the right hash", room.postCount() == 1);

  auto elsewhere = seal(secret, (uint8_t)(channel.hash + 1), "another channel");
  room.onGroup(packet::PayloadType::GRP_TXT, ByteView { elsewhere.data(), elsewhere.size() });
  that("another channel's hash is ignored", room.postCount() == 1);

  room.onGroup(packet::PayloadType::GRP_DATA, ByteView { message.data(), message.size() });
  that("GRP_DATA is not text and is left alone", room.postCount() == 1);

  std::vector<uint8_t> rubbish { 0x01, 0x02 };
  room.onGroup(packet::PayloadType::GRP_TXT, ByteView { rubbish.data(), rubbish.size() });
  that("a truncated group message is ignored", room.postCount() == 1);

  // A post that came in over a login goes out to the channel.
  const core::PublicKey author = keyOf(0x11);
  auto login = loginPayload(1000, 0, "guest");
  room.onAnon(author, ByteView { login.data(), login.size() });
  auto post = textPayload(10001, room::TextType::PLAIN, "from a client");
  room.onPayload(contactOf(author), packet::PayloadType::TXT_MSG, ByteView { post.data(), post.size() });

  that("a direct post is published to the channel", flooded.size() == 1);
  that("as GRP_TXT", flooded.size() == 1 && flooded[0].type == packet::PayloadType::GRP_TXT);

  // And a member can read it back with the channel key.
  auto sentOut = packet::decodeGroup(ByteView { flooded[0].payload.data(), flooded[0].payload.size() });
  that("it carries our channel hash", sentOut && sentOut->channelHash == channel.hash);

  crypto::protocol::Mac mac;
  if (sentOut) std::memcpy(mac.data.data(), sentOut->cipherMac.data(), PACKET_MAC_SIZE);
  std::vector<uint8_t> plain(MAX_PACKET_PAYLOAD);
  auto length = sentOut ?
    crypto::protocol::open(secret, mac, sentOut->ciphertext, ByteSpan { plain.data(), plain.size() }) :
    std::nullopt;
  auto readBack = length ? packet::decodeText(ByteView { plain.data(), *length }) : std::nullopt;
  that("a member decrypts it",
    readBack && std::string((const char*)readBack->message.data(), readBack->message.size()) == "from a client");

  // A room with no channels floods nothing at all.
  Fixture bare(defaultConfig());
  bare.roomInstance.onAnon(author, ByteView { login.data(), login.size() });
  bare.roomInstance.onPayload(contactOf(author), packet::PayloadType::TXT_MSG, ByteView { post.data(), post.size() });
  that("no channels means no flood", bare.sender.flooded.empty());
}

static void testCommands()
{
  section("room: admin commands");

  FakeAdmin admin;
  room::Config config = defaultConfig();
  config.admin = &admin;

  Fixture fixture(config);
  auto& room = fixture.roomInstance;
  auto& sent = fixture.sender.sent;

  const core::PublicKey adminKey = keyOf(0x22);
  const core::PublicKey guestKey = keyOf(0x11);

  auto adminLogin = loginPayload(1000, 0, "admin-secret");
  room.onAnon(adminKey, ByteView { adminLogin.data(), adminLogin.size() });
  auto guestLogin = loginPayload(1000, 0, "guest");
  room.onAnon(guestKey, ByteView { guestLogin.data(), guestLogin.size() });
  sent.clear();

  // A guest gets an answer, not silence: nothing back is how a dead node looks.
  runCommand(room, guestKey, 2000, "ver");
  that("a guest is refused out loud", sent.size() == 1 && contains(replyText(sent[0]), "admin rights"));
  sent.clear();

  runCommand(room, adminKey, 2000, "ver");
  that("ver answers", sent.size() == 1 && contains(replyText(sent[0]), "MeshCore room v1"));
  that("a reply asks for no ack", sent.size() == 1 && !sent[0].wantAck);
  that("a reply is a text message", sent.size() == 1 && sent[0].type == packet::PayloadType::TXT_MSG);
  sent.clear();

  // 2026-01-01 00:00:00 UTC, and the formatting is ours rather than the C
  // library's, so it is worth pinning exactly.
  room.setServerTime(1767225600u);
  runCommand(room, adminKey, 2001, "clock");
  that("clock reports UTC", sent.size() == 1 && contains(replyText(sent[0]), "00:00:00 1/1/2026 UTC"));
  sent.clear();

  runCommand(room, adminKey, 2002, "time 1767312000");
  that("the clock is set", admin.clockSetTo == 1767312000u);
  that("and confirmed", sent.size() == 1 && contains(replyText(sent[0]), "OK"));
  sent.clear();

  runCommand(room, adminKey, 2003, "time 100");
  that("a timestamp before 2020 is refused", admin.clockSetTo == 1767312000u);
  that("with a reason", sent.size() == 1 && contains(replyText(sent[0]), "before 2020"));
  sent.clear();

  runCommand(room, adminKey, 2004, "time yesterday");
  that("a non-numeric timestamp is refused", sent.size() == 1 && contains(replyText(sent[0]), "ERR"));
  sent.clear();

  admin.clockSettable = false;
  runCommand(room, adminKey, 2005, "time 1767398400");
  that("a host that cannot set the clock says so", sent.size() == 1 && contains(replyText(sent[0]), "ERR"));
  sent.clear();

  runCommand(room, adminKey, 2006, "advert");
  that("advert reaches the host", admin.adverts == 1 && sent.size() == 1);
  sent.clear();

  // The rest of the line, spaces and all.
  runCommand(room, adminKey, 2007, "set name Radio Hut");
  that("the name is set", admin.name == "Radio Hut");
  that("and announced at once", admin.adverts == 2);
  sent.clear();

  runCommand(room, adminKey, 2008, "get name");
  that("get name reads it back", sent.size() == 1 && contains(replyText(sent[0]), "Radio Hut"));
  sent.clear();

  admin.uptimeSeconds = 3720;
  room.addPost(adminKey, 1767225600u, "hello");
  runCommand(room, adminKey, 2009, "get stats");
  that("stats count the posts", sent.size() == 1 && contains(replyText(sent[0]), "posts 1/32"));
  that("and the uptime", sent.size() == 1 && contains(replyText(sent[0]), "up 1h02m"));
  sent.clear();

  runCommand(room, adminKey, 2010, "clear posts");
  that("posts are gone", room.postCount() == 0);
  that("and counted in the reply", sent.size() == 1 && contains(replyText(sent[0]), "1 posts cleared"));
  sent.clear();

  runCommand(room, adminKey, 2011, "set password after-air");
  that("the new password is never echoed", sent.size() == 1 && !contains(replyText(sent[0]), "after-air"));
  that("it is stored under the config's own key", admin.settings["room.admin_password"] == "after-air");
  sent.clear();

  const core::PublicKey newcomer = keyOf(0x44);
  auto withNew = loginPayload(4000, 0, "after-air");
  room.onAnon(newcomer, ByteView { withNew.data(), withNew.size() });
  that("the new password grants admin",
    room.findClient(newcomer) && room.findClient(newcomer)->access == room::Access::ADMIN);

  const core::PublicKey stale = keyOf(0x55);
  auto withOld = loginPayload(4000, 0, "admin-secret");
  room.onAnon(stale, ByteView { withOld.data(), withOld.size() });
  that("the old one stops working", room.findClient(stale) == nullptr);
  sent.clear();

  // Clearing the guest password closes that role off — and the empty value has
  // to be stored, or the config would put the old one back at the next start.
  runCommand(room, adminKey, 2012, "set guest.password");
  const core::PublicKey wouldBeGuest = keyOf(0x66);
  auto guestAttempt = loginPayload(4000, 0, "guest");
  room.onAnon(wouldBeGuest, ByteView { guestAttempt.data(), guestAttempt.size() });
  that("a cleared password refuses everyone", room.findClient(wouldBeGuest) == nullptr);
  that("and the empty value is stored, not skipped",
    admin.settings.count("room.guest_password") == 1 && admin.settings["room.guest_password"].empty());
  sent.clear();

  runCommand(room, adminKey, 2012, "set anonymous.read on");
  that("a flag is stored in the config's spelling", admin.settings["room.anonymous_read"] == "true");
  sent.clear();

  // Off again, or the checks below cannot tell a refused password from one that
  // was accepted and then downgraded to anonymous reading.
  runCommand(room, adminKey, 2012, "set anonymous.read off");
  sent.clear();

  // A setting that cannot be stored must not take effect either: the admin
  // would be told it had, and the next restart would disagree.
  admin.settingsWritable = false;
  runCommand(room, adminKey, 2012, "set password never-stored");
  that("an unstorable change is refused", sent.size() == 1 && contains(replyText(sent[0]), "ERR"));
  sent.clear();

  const core::PublicKey afterFailure = keyOf(0x88);
  auto withRejected = loginPayload(5000, 0, "never-stored");
  room.onAnon(afterFailure, ByteView { withRejected.data(), withRejected.size() });
  that("and did not take effect in memory", room.findClient(afterFailure) == nullptr);

  const core::PublicKey stillWorks = keyOf(0x99);
  auto withPrevious = loginPayload(5000, 0, "after-air");
  room.onAnon(stillWorks, ByteView { withPrevious.data(), withPrevious.size() });
  that("the previous password still does",
    room.findClient(stillWorks) && room.findClient(stillWorks)->access == room::Access::ADMIN);
  admin.settingsWritable = true;
  sent.clear();

  runCommand(room, adminKey, 2013, "wibble");
  that(
    "an unknown command is named back", sent.size() == 1 && contains(replyText(sent[0]), "unknown command 'wibble'"));
  sent.clear();

  // A captured command replayed later must not run a second time; the same one
  // resent by a client that saw no reply must be answered again.
  runCommand(room, adminKey, 1, "advert");
  that("an older command is dropped", sent.empty() && admin.adverts == 2);

  runCommand(room, adminKey, 2013, "advert");
  that("the same timestamp is answered again", sent.size() == 1 && admin.adverts == 3);
  sent.clear();

  runCommand(room, adminKey, 2014, "reboot");
  that("the reply goes out before the reboot", sent.size() == 1 && contains(replyText(sent[0]), "rebooting"));
  that("and the host is asked", admin.reboots == 1);
}

// The room persists no settings of its own: it hands them to the host under the
// config's own key names, and the host lays them over the config at the next
// start. What has to hold here is that nothing settable is left out of that
// handover, or it would silently expire at the first restart.
static void testSettingsReachTheHost()
{
  section("room: every settable change reaches the host");

  FakeAdmin admin;
  room::Config config = defaultConfig();
  config.admin = &admin;

  Fixture fixture(config);
  auto& room = fixture.roomInstance;

  const core::PublicKey adminKey = keyOf(0x22);
  auto login = loginPayload(1000, 0, "admin-secret");
  room.onAnon(adminKey, ByteView { login.data(), login.size() });

  runCommand(room, adminKey, 2000, "set name Radio Hut");
  runCommand(room, adminKey, 2001, "set password one");
  runCommand(room, adminKey, 2002, "set guest.password two");
  runCommand(room, adminKey, 2003, "set anonymous.read off");

  that("node.name stored", admin.settings["node.name"] == "Radio Hut");
  that("room.admin_password stored", admin.settings["room.admin_password"] == "one");
  that("room.guest_password stored", admin.settings["room.guest_password"] == "two");
  that("room.anonymous_read stored", admin.settings["room.anonymous_read"] == "false");
  that("and nothing else was written", admin.settings.size() == 4);

  // Every key handed over has to be one the config actually reads back, or the
  // overlay would shadow nothing at all.
  platform::Config effective;
  that("config parses", effective.loadFromString(R"({"node":{"name":"seed"},
    "room":{"admin_password":"seed","guest_password":"seed","anonymous_read":true}})"));

  platform::Overlay overlay(fixture.dir + "/overrides.json");
  for (const auto& [key, value] : admin.settings) {
    that("the host can store it", overlay.set(key, value));
  }
  effective.applyOverlay(overlay);

  that("node.name shadows the config", effective.get("node.name") == "Radio Hut");
  that("room.admin_password shadows the config", effective.get("room.admin_password") == "one");
  that("room.guest_password shadows the config", effective.get("room.guest_password") == "two");
  that("room.anonymous_read shadows the config", !effective.getBool("room.anonymous_read", true));

  unlink((fixture.dir + "/overrides.json").c_str());
}

// An advert as identity would have taken it, so the store has a contact with a
// name to show.
static std::vector<uint8_t> namedAppdata(const std::string& name)
{
  std::vector<uint8_t> out { (uint8_t)(ADVERT_HAS_NAME | (uint8_t)identity::NodeType::CHAT) };
  out.insert(out.end(), name.begin(), name.end());
  return out;
}

static void testTransitCommands()
{
  section("room: driving the repeater over the air");

  FakeAdmin admin;
  repeater::Policy policy;

  room::Config config = defaultConfig();
  config.admin = &admin;
  config.forwarder = &policy;

  Fixture fixture(config);
  auto& room = fixture.roomInstance;
  room.setRadioLoad(43); // 4.3% of the sliding hour spent, well under the ceiling

  const core::PublicKey adminKey = keyOf(0x22);
  auto login = loginPayload(1000, 0, "admin-secret");
  room.onAnon(adminKey, ByteView { login.data(), login.size() });
  fixture.sender.sent.clear();

  // Something to count, and something refused, so both numbers are real.
  packet::Packet carried;
  carried.header = (uint8_t)(((uint8_t)packet::PayloadType::TXT_MSG << 2) | (uint8_t)packet::RouteType::FLOOD);
  carried.hopCount = 1;
  that("the room carries what the policy allows", room.shouldForward(carried));

  packet::Packet tooFar = carried;
  tooFar.hopCount = MAX_HOP_COUNT;
  that("and refuses what it will not", !room.shouldForward(tooFar));

  runCommand(room, adminKey, 2000, "get stats");
  const std::string stats = replyText(fixture.sender.sent.back());
  that("stats count the board", contains(stats, "posts 0/32"));
  that("and the transit", contains(stats, "fwd 1") && contains(stats, "refused 1"));
  that("and how much air is left", contains(stats, "duty 4.3%"));

  // Air spent is what the ceiling is for: past it the node keeps answering its
  // own clients and stops carrying strangers.
  room.setRadioLoad(500);
  that("a spent budget stops transit", !room.shouldForward(carried));
  room.setRadioLoad(43);
  that("and it resumes when there is air again", room.shouldForward(carried));

  runCommand(room, adminKey, 2001, "get transit");
  const std::string transit = replyText(fixture.sender.sent.back());
  that("the breakdown says which counter moved", contains(transit, "hops 1") && contains(transit, "blocked 0"));
  that("and whether it is repeating at all", contains(transit, "repeat on"));

  // Switching transit off has to outlive a restart, or the config file brings
  // it back at the next start and the node quietly repeats again.
  runCommand(room, adminKey, 2002, "set repeat off");
  that("the reply says so", contains(replyText(fixture.sender.sent.back()), "OK - repeating off"));
  that("the policy stopped", !policy.enabled() && !room.shouldForward(carried));
  that("and the host stored it", admin.settings["repeater.enabled"] == "false");

  runCommand(room, adminKey, 2003, "set repeat on");
  that("and back on again", policy.enabled() && admin.settings["repeater.enabled"] == "true");

  runCommand(room, adminKey, 2004, "set hops.max 5");
  that("the limit moved", policy.maxHops() == 5 && admin.settings["repeater.max_hops"] == "5");

  runCommand(room, adminKey, 2005, "set hops.max 0");
  that("nonsense is refused", contains(replyText(fixture.sender.sent.back()), "ERR"));
  that("and the limit is left alone", policy.maxHops() == 5);

  runCommand(room, adminKey, 2006, "clear stats");
  that("counters can be zeroed", policy.stats().forwarded == 0 && policy.stats().hopLimit == 0);

  // Every key the commands hand over has to be one the config reads back, or
  // the overlay shadows nothing.
  platform::Config effective;
  that("config parses", effective.loadFromString(R"({"repeater":{"enabled":true,"max_hops":12}})"));
  platform::Overlay overlay(fixture.dir + "/overrides.json");
  that("the host can store the switch", overlay.set("repeater.enabled", "false"));
  that("and the limit", overlay.set("repeater.max_hops", "5"));
  effective.applyOverlay(overlay);
  that("both shadow the config",
    !effective.getBool("repeater.enabled", true) && effective.getInt("repeater.max_hops") == 5);
  unlink((fixture.dir + "/overrides.json").c_str());
}

static void testNeighbourCommand()
{
  section("room: who the node can hear");

  FakeAdmin admin;
  room::Config config = defaultConfig();
  config.admin = &admin;

  Fixture fixture(config);
  auto& room = fixture.roomInstance;

  const core::PublicKey adminKey = keyOf(0x22);
  auto login = loginPayload(1000, 0, "admin-secret");
  room.onAnon(adminKey, ByteView { login.data(), login.size() });

  runCommand(room, adminKey, 2000, "get neighbors");
  that("an empty neighbourhood says so", contains(replyText(fixture.sender.sent.back()), "no neighbours"));

  core::PublicKey peer = keyOf(0x77);
  const std::vector<uint8_t> extra = namedAppdata("hilltop");
  packet::Advert advert;
  advert.publicKey = peer.view();
  advert.timestamp = 100;
  advert.appdata = ByteView { extra.data(), extra.size() };
  fixture.store.remember(advert);

  // Heard 16 minutes before the server time the fixture set.
  fixture.store.noteHeard(peer, 9040, -3, 0);

  runCommand(room, adminKey, 2001, "get neighbors");
  const std::string list = replyText(fixture.sender.sent.back());
  that("the name is there", contains(list, "hilltop"));
  that("with the hash", contains(list, "77"));
  that("how well we hear it", contains(list, "-3dB"));
  that("and how long ago", contains(list, "16m"));

  // A contact several hops away is not somebody we can hear.
  core::PublicKey distant = keyOf(0x88);
  const std::vector<uint8_t> other = namedAppdata("faraway");
  advert.publicKey = distant.view();
  advert.appdata = ByteView { other.data(), other.size() };
  fixture.store.remember(advert);
  fixture.store.noteHeard(distant, 9500, 9, 3);

  runCommand(room, adminKey, 2002, "get neighbours");
  that("the far one is left out", !contains(replyText(fixture.sender.sent.back()), "faraway"));

  // The British spelling answers too: half the clients will type it.
  that("both spellings work", contains(replyText(fixture.sender.sent.back()), "hilltop"));
}

int main()
{
  if (!core::init(nullptr)) {
    std::printf("backend init failed\n");
    return 1;
  }

  testLogin();
  testAnonymousRead();
  testPosting();
  testAckPolicy();
  testSyncFlow();
  testRoundRobin();
  testRingBuffer();
  testPersistence();
  testSameSecondPosts();
  testRequests();
  testChannels();
  testAuthorPrefix();
  testLoginLockout();
  testEviction();
  testCommands();
  testSettingsReachTheHost();
  testTransitCommands();
  testNeighbourCommand();

  return check::report();
}
