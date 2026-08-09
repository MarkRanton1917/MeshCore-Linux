// Tests for src/room.
//
// No network here at all: room depends only on abstractions, so a fake sender
// that collects outgoing calls in a vector plus a hand-cranked clock covers the
// whole module.

#include "check.h"
#include "room.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
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

  std::vector<Outgoing> sent;
  routing::SendId nextId = 1;
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

static std::vector<uint8_t> textPayload(uint32_t timestamp, uint8_t txtType, const std::string& body)
{
  packet::TextMsg message;
  message.timestamp = timestamp;
  message.txtType = txtType;
  message.attempt = 0;
  message.message = ByteView { (const uint8_t*)body.data(), body.size() };

  std::vector<uint8_t> out(MAX_PACKET_PAYLOAD);
  auto size = packet::encodeText(message, ByteSpan { out.data(), out.size() });
  out.resize(size ? *size : 0);
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

// The post text a client actually receives, minus the author prefix.
static std::string pushedText(const Outgoing& out)
{
  auto message = packet::decodeText(ByteView { out.payload.data(), out.payload.size() });
  if (!message.has_value() || message->message.size() < POST_AUTHOR_PREFIX) return {};
  return std::string(
    (const char*)message->message.data() + POST_AUTHOR_PREFIX, message->message.size() - POST_AUTHOR_PREFIX);
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

  auto post = textPayload(10000, TEXT_TYPE_PLAIN, "hello room");
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
  auto longPost = textPayload(10001, TEXT_TYPE_PLAIN, tooLong);
  room.onPayload(contactOf(guest), packet::PayloadType::TXT_MSG, ByteView { longPost.data(), longPost.size() });
  that("long text truncated, not rejected", room.postCount() == 2 && room.posts()[1].text.size() == MAX_POST_TEXT);

  // A clock from the future must not poison the ordering.
  auto future = textPayload(99999999, TEXT_TYPE_PLAIN, "from the future");
  room.onPayload(contactOf(guest), packet::PayloadType::TXT_MSG, ByteView { future.data(), future.size() });
  that("a wild timestamp is replaced with the server's", room.postCount() == 3 && room.posts()[2].timestamp == 10000);

  auto sane = textPayload(10050, TEXT_TYPE_PLAIN, "close enough");
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

  auto post = textPayload(10000, TEXT_TYPE_PLAIN, "post");
  that("a post is acknowledged", room.shouldAck(packet::PayloadType::TXT_MSG, ByteView { post.data(), post.size() }));

  auto command = textPayload(10000, TEXT_TYPE_CLI, "reboot");
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
    auto post = textPayload((uint32_t)(10000 + i), TEXT_TYPE_PLAIN, "post " + std::to_string(i));
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
  that("bookmark moved on ack", room.findClient(reader) && room.findClient(reader)->syncSince == 10000);

  room.tick(1200);
  that("next post follows", fixture.sender.sent.size() == 2 && pushedText(fixture.sender.sent[1]) == "post 1");

  // A failed delivery must not move the bookmark.
  room.onDeliveryFailed(fixture.sender.sent[1].id);
  that("bookmark unchanged after a failure", room.findClient(reader) && room.findClient(reader)->syncSince == 10000);

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
    auto post = textPayload((uint32_t)(10000 + i), TEXT_TYPE_PLAIN, "p" + std::to_string(i));
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
    auto post = textPayload((uint32_t)(10000 + i), TEXT_TYPE_PLAIN, "p" + std::to_string(i));
    room.onPayload(contactOf(author), packet::PayloadType::TXT_MSG, ByteView { post.data(), post.size() });
  }
  that("buffer full", room.postCount() == MAX_POSTS);
  that("oldest is the first written", room.posts()[0].text == "p0");

  auto extra = textPayload(10000 + MAX_POSTS, TEXT_TYPE_PLAIN, "p32");
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
    auto post = textPayload(10000, TEXT_TYPE_PLAIN, "remembered");
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
    that("bookmark restored", client && client->syncSince == 5);
    that("replay guard restored", client && client->lastLogin == 1000);

    // And it still refuses a replayed login after the restart.
    auto replay = loginPayload(1000, 0, "guest");
    instance.onAnon(author, ByteView { replay.data(), replay.size() });
    that("replay still refused after restart", sender.sent.empty());
  }

  removeDir(dir);
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

  return check::report();
}
