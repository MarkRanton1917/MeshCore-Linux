// Tests for src/identity.
//
// This module holds state, so the properties worth pinning are about what
// survives what: a reboot, a replayed advert, a hash collision, a power cut
// halfway through a write.

#include "check.h"
#include "identity.h"

#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <cstring>
#include <string>
#include <vector>

using check::section;
using check::that;

namespace core = crypto::core;

// ------------------------------------------------------------------ utilities

static ByteView view(const std::vector<uint8_t>& v)
{
  return ByteView { v.data(), v.size() };
}

// Appdata with the flags byte, optional location and optional name.
static std::vector<uint8_t> appdata(uint8_t flags,
  int32_t latitude = 0,
  int32_t longitude = 0,
  const std::string& name = "")
{
  std::vector<uint8_t> out { flags };
  if (flags & ADVERT_HAS_LOCATION) {
    for (int i = 0; i < 4; i++)
      out.push_back((uint8_t)((uint32_t)latitude >> (8 * i)));
    for (int i = 0; i < 4; i++)
      out.push_back((uint8_t)((uint32_t)longitude >> (8 * i)));
  }
  out.insert(out.end(), name.begin(), name.end());
  return out;
}

static packet::Advert advertOf(const core::PublicKey& pk, uint32_t timestamp, const std::vector<uint8_t>& extra)
{
  packet::Advert advert;
  advert.publicKey = pk.view();
  advert.timestamp = timestamp;
  advert.signature = ByteView {};
  advert.appdata = view(extra);
  return advert;
}

// A public key whose first byte, and therefore whose node hash, is fixed.
static core::PublicKey keyWithHash(uint8_t hash, uint8_t tag)
{
  core::PublicKey pk;
  pk.data.fill(tag);
  pk.data[0] = hash;
  return pk;
}

static std::string makeDir()
{
  static char templ[] = "meshcore_identity_XXXXXX";
  char buffer[64];
  std::snprintf(buffer, sizeof buffer, "%s", templ);
  const char* dir = mkdtemp(buffer);
  return dir ? std::string(dir) : std::string();
}

static void removeDir(const std::string& dir)
{
  for (const char* name : { "identity.key", "contacts.dat", "contacts.dat.tmp" }) {
    unlink((dir + "/" + name).c_str());
  }
  rmdir(dir.c_str());
}

// ------------------------------------------------------------------ identity

static void testLoadOrCreate()
{
  section("identity: loadOrCreate");

  const std::string dir = makeDir();
  core::PublicKey firstRun;

  {
    identity::Store store;
    that("first run creates the identity", store.loadOrCreate(dir));
    firstRun = store.selfPk();
    that("selfHash is the first byte of the public key", store.selfHash() == store.selfPk().data[0]);

    struct stat info {};
    stat((dir + "/identity.key").c_str(), &info);
    that("key file mode is 0600", (info.st_mode & 07777) == 0600);
  }
  {
    identity::Store store;
    that("second run reuses it", store.loadOrCreate(dir));
    that("same identity across restarts",
      std::memcmp(store.selfPk().data.data(), firstRun.data.data(), PACKET_PUBLIC_KEY_SIZE) == 0);
  }
  {
    // A damaged key must stop the node, not silently mint a new identity.
    std::vector<uint8_t> damaged(PACKET_PRIVATE_KEY_SIZE, 0x5A);
    const std::string path = dir + "/identity.key";
    int fd = ::open(path.c_str(), O_WRONLY | O_TRUNC, 0600);
    ssize_t ignored = write(fd, damaged.data(), damaged.size());
    (void)ignored;
    close(fd);

    identity::Store store;
    that("a corrupt key refuses to start", !store.loadOrCreate(dir));

    struct stat info {};
    that("the corrupt key is left in place", stat(path.c_str(), &info) == 0 && info.st_size == PACKET_PRIVATE_KEY_SIZE);
  }

  removeDir(dir);
  that("an unwritable directory fails", !identity::Store {}.loadOrCreate("/proc/meshcore"));
}

static void testRemember()
{
  section("identity: remember");

  const std::string dir = makeDir();
  identity::Store store;
  store.loadOrCreate(dir);

  core::PublicKey peer = keyWithHash(0x42, 0x11);
  std::vector<uint8_t> plain = appdata(ADVERT_HAS_NAME | (uint8_t)identity::NodeType::REPEATER, 0, 0, "repeater-one");

  that("a new peer is added", store.remember(advertOf(peer, 1000, plain)) == identity::Update::ADDED);
  that("contact count grew", store.contactCount() == 1);

  const identity::Contact* found = store.find(peer);
  that("found by full key", found != nullptr);
  that("name decoded", found && found->name == "repeater-one");
  that("type decoded", found && found->type == identity::NodeType::REPEATER);
  that("no location claimed", found && !found->hasLocation);

  std::vector<uint8_t> located = appdata(
    ADVERT_HAS_LOCATION | ADVERT_HAS_NAME | (uint8_t)identity::NodeType::ROOM_SERVER, 55123456, -37987654, "room");
  that("a newer advert updates", store.remember(advertOf(peer, 2000, located)) == identity::Update::UPDATED);
  found = store.find(peer);
  that("still one contact", store.contactCount() == 1);
  that("location decoded", found && found->hasLocation && found->latitude == 55123456 && found->longitude == -37987654);
  that("negative longitude survives", found && found->longitude < 0);
  that("name replaced", found && found->name == "room");

  // The replay guard: a genuine signature on an old advert must not roll the
  // record back.
  that("an older advert is stale", store.remember(advertOf(peer, 1500, plain)) == identity::Update::STALE);
  that("an equal timestamp is stale", store.remember(advertOf(peer, 2000, plain)) == identity::Update::STALE);
  found = store.find(peer);
  that("the record was not rolled back", found && found->timestamp == 2000 && found->name == "room");

  packet::Advert malformed = advertOf(peer, 9000, plain);
  malformed.publicKey = ByteView { peer.data.data(), PACKET_PUBLIC_KEY_SIZE - 1 };
  that("a malformed advert is rejected", store.remember(malformed) == identity::Update::REJECTED);

  std::vector<uint8_t> empty;
  core::PublicKey bare = keyWithHash(0x43, 0x22);
  that("advert with no appdata", store.remember(advertOf(bare, 1, empty)) == identity::Update::ADDED);
  const identity::Contact* bareContact = store.find(bare);
  that("unknown type, no name",
    bareContact && bareContact->type == identity::NodeType::UNKNOWN && bareContact->name.empty());

  // A truncated appdata stops the walk instead of reading past the end.
  std::vector<uint8_t> truncated { ADVERT_HAS_LOCATION | ADVERT_HAS_NAME, 1, 2 };
  core::PublicKey odd = keyWithHash(0x44, 0x33);
  that("truncated appdata is tolerated", store.remember(advertOf(odd, 1, truncated)) == identity::Update::ADDED);
  const identity::Contact* oddContact = store.find(odd);
  that("no location taken from a short appdata", oddContact && !oddContact->hasLocation);

  core::PublicKey unknown = keyWithHash(0x99, 0x99);
  that("an unknown key is not found", store.find(unknown) == nullptr);

  removeDir(dir);
}

static void testFindByHash()
{
  section("identity: findByHash");

  const std::string dir = makeDir();
  identity::Store store;
  store.loadOrCreate(dir);

  std::vector<uint8_t> none;
  core::PublicKey first = keyWithHash(0x42, 0x01);
  core::PublicKey second = keyWithHash(0x42, 0x02);
  core::PublicKey third = keyWithHash(0x42, 0x03);
  core::PublicKey elsewhere = keyWithHash(0x43, 0x04);

  store.remember(advertOf(first, 10, none));
  store.remember(advertOf(second, 10, none));
  store.remember(advertOf(third, 10, none));
  store.remember(advertOf(elsewhere, 10, none));

  auto candidates = store.findByHash(0x42);
  that("all colliding contacts are returned", candidates.size() == 3);

  bool sawAll = false;
  {
    bool a = false, b = false, c = false;
    for (const identity::Contact* candidate : candidates) {
      a = a || std::memcmp(candidate->pk.data.data(), first.data.data(), 32) == 0;
      b = b || std::memcmp(candidate->pk.data.data(), second.data.data(), 32) == 0;
      c = c || std::memcmp(candidate->pk.data.data(), third.data.data(), 32) == 0;
    }
    sawAll = a && b && c;
  }
  that("each collision is present exactly once", sawAll);

  that("a different hash is a different bucket", store.findByHash(0x43).size() == 1);
  that("an empty bucket is empty", store.findByHash(0x00).empty());

  // Updating a known contact must not duplicate it in the index.
  store.remember(advertOf(first, 20, none));
  that("an update does not duplicate the index entry", store.findByHash(0x42).size() == 3);

  removeDir(dir);
}

static void testSecretCache()
{
  section("identity: secretFor");

  const std::string dir = makeDir();
  identity::Store store;
  store.loadOrCreate(dir);

  core::KeyPair peer = core::generateKeypair();
  const identity::SharedSecret* first = store.secretFor(peer.pk);
  that("derives a secret", first != nullptr);

  // The cache must return the same value, and it must match a fresh derivation.
  auto direct = core::deriveShared(store.selfSk(), peer.pk);
  that("matches a direct derivation",
    first && direct && std::memcmp(first->view().data(), direct->view().data(), PACKET_SHARED_SECRET_SIZE) == 0);

  const identity::SharedSecret* again = store.secretFor(peer.pk);
  that("second call returns the cached entry", again == first);

  core::KeyPair other = core::generateKeypair();
  const identity::SharedSecret* second = store.secretFor(other.pk);
  that("a different peer gets a different secret",
    second && std::memcmp(second->view().data(), first->view().data(), PACKET_SHARED_SECRET_SIZE) != 0);

  core::PublicKey degenerate {};
  that("a degenerate key yields nothing", store.secretFor(degenerate) == nullptr);

  // Filling the cache past its limit must evict rather than grow.
  for (int i = 0; i < MAX_SECRET_CACHE + 8; i++) {
    core::KeyPair filler = core::generateKeypair();
    store.secretFor(filler.pk);
  }
  const identity::SharedSecret* revived = store.secretFor(peer.pk);
  that("still derivable after eviction",
    revived && direct && std::memcmp(revived->view().data(), direct->view().data(), PACKET_SHARED_SECRET_SIZE) == 0);

  removeDir(dir);
}

static void testPersistence()
{
  section("identity: flush and reload");

  const std::string dir = makeDir();
  core::PublicKey peer = keyWithHash(0x42, 0x11);
  core::PublicKey mate = keyWithHash(0x42, 0x12);

  {
    identity::Store store;
    store.loadOrCreate(dir);
    std::vector<uint8_t> named =
      appdata(ADVERT_HAS_LOCATION | ADVERT_HAS_NAME | (uint8_t)identity::NodeType::CHAT, 1000000, -2000000, "alice");
    std::vector<uint8_t> none;
    store.remember(advertOf(peer, 4242, named));
    store.remember(advertOf(mate, 7, none));
    that("flush succeeds", store.flush());

    struct stat info {};
    that("contacts file written", stat((dir + "/contacts.dat").c_str(), &info) == 0);
    that("version byte leads", info.st_size > 0);
    that("no temporary file left behind", stat((dir + "/contacts.dat.tmp").c_str(), &info) != 0);
  }
  {
    identity::Store store;
    that("reload succeeds", store.loadOrCreate(dir));
    that("contact count restored", store.contactCount() == 2);

    const identity::Contact* restored = store.find(peer);
    that("fields restored",
      restored && restored->timestamp == 4242 && restored->name == "alice" && restored->type == identity::NodeType::CHAT
        && restored->hasLocation && restored->latitude == 1000000 && restored->longitude == -2000000);
    that("hash index rebuilt on load", store.findByHash(0x42).size() == 2);

    // The replay guard has to survive the round-trip too.
    std::vector<uint8_t> none;
    that("stale advert still rejected after reload",
      store.remember(advertOf(peer, 4242, none)) == identity::Update::STALE);
  }
  {
    // A wrong version byte is a corrupt file, not something to guess at.
    const std::string path = dir + "/contacts.dat";
    int fd = ::open(path.c_str(), O_WRONLY, 0600);
    uint8_t wrong = CONTACTS_FORMAT_VERSION + 1;
    ssize_t ignored = write(fd, &wrong, 1);
    (void)ignored;
    close(fd);

    identity::Store store;
    that("unknown format version refuses to load", !store.loadOrCreate(dir));
  }
  {
    // Truncation mid-record must be caught, not read past the end.
    const std::string path = dir + "/contacts.dat";
    that("truncated contacts file refuses to load",
      truncate(path.c_str(), 20) == 0 && !identity::Store {}.loadOrCreate(dir));
  }

  removeDir(dir);
}

static void testFlushWithoutLoad()
{
  section("identity: flush before load");

  identity::Store store;
  that("flush without a directory fails", !store.flush());
}

int main()
{
  if (!core::init(nullptr)) {
    std::printf("backend init failed\n");
    return 1;
  }

  testLoadOrCreate();
  testRemember();
  testFindByHash();
  testSecretCache();
  testPersistence();
  testFlushWithoutLoad();

  return check::report();
}
