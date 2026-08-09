// Copyright (c) 2026 Vladimir Egorov
// This project is licensed under the MIT License.
// See the LICENSE file in the root of the repository for the full license text.

#include "identity.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace identity;

namespace core = crypto::core;

// The on-disk contact format is ours alone and has nothing to do with the air
// protocol, so it can change freely — which is exactly why the first byte is a
// version number.
//
//   byte 0            format version
//   then per contact: pk(32) timestamp(4) type(1) flags(1) lat(4) lon(4)
//                     nameLength(1) name(nameLength)
//                     [v2] lastHeard(4) snr(2) hops(1)
//
// The v2 tail sits after the name rather than before it, so a v1 record is a v2
// record that stops early and one reader handles both.

static constexpr size_t kContactFixedSize = PACKET_PUBLIC_KEY_SIZE + 4 + 1 + 1 + 4 + 4 + 1;
static constexpr size_t kContactHeardSize = 4 + 2 + 1;

static uint32_t readUint32(ByteView view, size_t offset)
{
  return (uint32_t)view[offset] | (uint32_t)view[offset + 1] << 8 | (uint32_t)view[offset + 2] << 16
    | (uint32_t)view[offset + 3] << 24;
}

static void appendUint32(std::vector<uint8_t>& out, uint32_t value)
{
  out.push_back((uint8_t)(value));
  out.push_back((uint8_t)(value >> 8));
  out.push_back((uint8_t)(value >> 16));
  out.push_back((uint8_t)(value >> 24));
}

static bool samePublicKey(const PublicKey& a, const PublicKey& b)
{
  return std::memcmp(a.data.data(), b.data.data(), PACKET_PUBLIC_KEY_SIZE) == 0;
}

// Fills the descriptive fields from the advert appdata. Anything truncated
// stops the walk: a short appdata is not a reason to reject the advert, whose
// signature already covered it.
static void applyAppdata(Contact& contact, ByteView appdata)
{
  contact.type = NodeType::UNKNOWN;
  contact.hasLocation = false;
  contact.latitude = 0;
  contact.longitude = 0;
  contact.name.clear();

  if (appdata.empty()) return;

  const uint8_t flags = appdata[0];
  contact.type = (NodeType)(flags & ADVERT_TYPE_MASK);

  size_t offset = 1;
  if (flags & ADVERT_HAS_LOCATION) {
    if (appdata.size() < offset + ADVERT_LOCATION_SIZE) return;
    contact.latitude = (int32_t)readUint32(appdata, offset);
    contact.longitude = (int32_t)readUint32(appdata, offset + 4);
    contact.hasLocation = true;
    offset += ADVERT_LOCATION_SIZE;
  }
  if (flags & ADVERT_HAS_FEATURE1) {
    if (appdata.size() < offset + ADVERT_FEATURE_SIZE) return;
    offset += ADVERT_FEATURE_SIZE;
  }
  if (flags & ADVERT_HAS_FEATURE2) {
    if (appdata.size() < offset + ADVERT_FEATURE_SIZE) return;
    offset += ADVERT_FEATURE_SIZE;
  }
  if (flags & ADVERT_HAS_NAME) {
    size_t length = appdata.size() - offset;
    if (length > MAX_CONTACT_NAME) length = MAX_CONTACT_NAME;
    contact.name.assign((const char*)appdata.data() + offset, length);
  }
}

Store::Store()
{
  // Reserved once and never grown: the hash index holds pointers into this
  // vector, and a reallocation would leave every one of them dangling.
  contacts_.reserve(MAX_CONTACTS);
  secrets_.reserve(MAX_SECRET_CACHE);
}

bool Store::loadOrCreate(const std::string& dir)
{
  if (mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) return false;
  dir_ = dir;

  const std::string keyPath = dir + "/identity.key";
  if (access(keyPath.c_str(), F_OK) == 0) {
    // Present but unreadable means damaged, wrong permissions, or truncated.
    // Refuse to start rather than come up as somebody else.
    if (!core::loadKeypair(self_, keyPath)) return false;
  }
  else {
    self_ = core::generateKeypair();
    if (!core::saveKeypair(self_, keyPath)) return false;
  }
  selfHash_ = self_.pk.data[0];

  const std::string contactsPath = dir + "/contacts.dat";
  if (access(contactsPath.c_str(), F_OK) == 0 && !loadContacts(contactsPath)) return false;

  return true;
}

const Contact* Store::find(const PublicKey& pk) const
{
  for (const Contact* candidate : buckets_[pk.data[0]]) {
    if (samePublicKey(candidate->pk, pk)) return candidate;
  }
  return nullptr;
}

Contact* Store::findMutable(const PublicKey& pk)
{
  return const_cast<Contact*>(find(pk));
}

std::span<const Contact* const> Store::findByHash(uint8_t hash) const
{
  return { buckets_[hash].data(), buckets_[hash].size() };
}

void Store::indexContact(const Contact& contact)
{
  buckets_[contact.hash()].push_back(&contact);
}

void Store::reindex()
{
  for (auto& bucket : buckets_)
    bucket.clear();
  for (const Contact& contact : contacts_)
    indexContact(contact);
}

// Returns a slot for a new contact, evicting the one that last advertised
// longest ago — staleness, not insertion order, is what makes a contact useless.
Contact& Store::allocateSlot()
{
  if (contacts_.size() < MAX_CONTACTS) {
    contacts_.emplace_back();
    return contacts_.back();
  }

  size_t oldest = 0;
  for (size_t i = 1; i < contacts_.size(); i++) {
    if (contacts_[i].timestamp < contacts_[oldest].timestamp) oldest = i;
  }
  contacts_[oldest] = Contact {};
  return contacts_[oldest];
}

Update Store::remember(const packet::Advert& advert)
{
  if (advert.publicKey.size() != PACKET_PUBLIC_KEY_SIZE) return Update::REJECTED;

  PublicKey pk;
  std::memcpy(pk.data.data(), advert.publicKey.data(), PACKET_PUBLIC_KEY_SIZE);

  if (Contact* known = findMutable(pk)) {
    // Equal timestamps count as stale too: a replayed advert carries a genuine
    // signature and would otherwise be indistinguishable from a fresh one.
    if (advert.timestamp <= known->timestamp) return Update::STALE;

    known->timestamp = advert.timestamp;
    applyAppdata(*known, advert.appdata);
    return Update::UPDATED;
  }

  const bool evicting = contacts_.size() >= MAX_CONTACTS;

  Contact& fresh = allocateSlot();
  fresh.pk = pk;
  fresh.timestamp = advert.timestamp;
  applyAppdata(fresh, advert.appdata);

  // Eviction reused a slot that another bucket still points at.
  if (evicting) {
    reindex();
  }
  else {
    indexContact(fresh);
  }
  return Update::ADDED;
}

void Store::noteHeard(const PublicKey& pk, uint32_t at, int16_t snr, uint8_t hops)
{
  Contact* contact = findMutable(pk);
  if (contact == nullptr) return;

  contact->lastHeard = at;
  contact->snr = snr;
  contact->hops = hops;
}

std::vector<const Contact*> Store::neighbours(size_t limit) const
{
  std::vector<const Contact*> found;
  if (limit == 0) return found;

  for (const Contact& contact : contacts_) {
    if (contact.isNeighbour()) found.push_back(&contact);
  }

  // Most recently heard first: a list of neighbours is read to find out who is
  // there now, and the ones that answered a minute ago are the answer.
  std::sort(found.begin(), found.end(), [](const Contact* a, const Contact* b) { return a->lastHeard > b->lastHeard; });

  if (found.size() > limit) found.resize(limit);
  return found;
}

const SharedSecret* Store::secretFor(const PublicKey& pk)
{
  clock_++;

  for (CachedSecret& entry : secrets_) {
    if (samePublicKey(entry.pk, pk)) {
      entry.usedAt = clock_;
      return &entry.secret;
    }
  }

  auto derived = core::deriveShared(self_.sk, pk);
  if (!derived) return nullptr; // degenerate peer key

  CachedSecret* slot = nullptr;
  if (secrets_.size() < MAX_SECRET_CACHE) {
    secrets_.emplace_back();
    slot = &secrets_.back();
  }
  else {
    slot = &secrets_[0];
    for (CachedSecret& entry : secrets_) {
      if (entry.usedAt < slot->usedAt) slot = &entry;
    }
  }

  slot->pk = pk;
  slot->secret = *derived;
  slot->usedAt = clock_;
  return &slot->secret;
}

bool Store::loadContacts(const std::string& path)
{
  int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return false;

  struct stat info;
  if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
    close(fd);
    return false;
  }

  std::vector<uint8_t> blob((size_t)info.st_size);
  size_t filled = 0;
  while (filled < blob.size()) {
    const ssize_t got = read(fd, blob.data() + filled, blob.size() - filled);
    if (got < 0 && errno == EINTR) continue;
    if (got <= 0) {
      close(fd);
      return false;
    }
    filled += (size_t)got;
  }
  close(fd);

  if (blob.empty() || blob[0] > CONTACTS_FORMAT_VERSION || blob[0] < CONTACTS_FORMAT_MIN_VERSION) return false;
  const uint8_t version = blob[0];

  std::vector<Contact> parsed;
  parsed.reserve(MAX_CONTACTS);

  const ByteView view { blob.data(), blob.size() };
  size_t offset = 1;
  while (offset < view.size()) {
    if (view.size() - offset < kContactFixedSize) return false;
    if (parsed.size() >= MAX_CONTACTS) return false;

    Contact contact;
    std::memcpy(contact.pk.data.data(), view.data() + offset, PACKET_PUBLIC_KEY_SIZE);
    offset += PACKET_PUBLIC_KEY_SIZE;

    contact.timestamp = readUint32(view, offset);
    offset += 4;
    contact.type = (NodeType)view[offset++];
    contact.hasLocation = view[offset++] != 0;
    contact.latitude = (int32_t)readUint32(view, offset);
    offset += 4;
    contact.longitude = (int32_t)readUint32(view, offset);
    offset += 4;

    const uint8_t nameLength = view[offset++];
    if (nameLength > MAX_CONTACT_NAME || view.size() - offset < nameLength) return false;
    contact.name.assign((const char*)view.data() + offset, nameLength);
    offset += nameLength;

    // A file written before the node kept link quality has none to give. The
    // fields stay empty and fill in with the next packet from that contact,
    // which is the honest answer to "when did we last hear it".
    if (version >= 2) {
      if (view.size() - offset < kContactHeardSize) return false;
      contact.lastHeard = readUint32(view, offset);
      offset += 4;
      contact.snr = (int16_t)(uint16_t)((uint16_t)view[offset] | (uint16_t)view[offset + 1] << 8);
      offset += 2;
      contact.hops = view[offset++];
    }

    parsed.push_back(std::move(contact));
  }

  contacts_ = std::move(parsed);
  contacts_.reserve(MAX_CONTACTS);
  reindex();
  return true;
}

bool Store::writeContacts(const std::string& path) const
{
  std::vector<uint8_t> blob;
  blob.push_back(CONTACTS_FORMAT_VERSION);

  for (const Contact& contact : contacts_) {
    blob.insert(blob.end(), contact.pk.data.begin(), contact.pk.data.end());
    appendUint32(blob, contact.timestamp);
    blob.push_back((uint8_t)contact.type);
    blob.push_back(contact.hasLocation ? 1 : 0);
    appendUint32(blob, (uint32_t)contact.latitude);
    appendUint32(blob, (uint32_t)contact.longitude);

    const uint8_t nameLength = (uint8_t)std::min<size_t>(contact.name.size(), MAX_CONTACT_NAME);
    blob.push_back(nameLength);
    blob.insert(blob.end(), contact.name.begin(), contact.name.begin() + nameLength);

    appendUint32(blob, contact.lastHeard);
    blob.push_back((uint8_t)((uint16_t)contact.snr));
    blob.push_back((uint8_t)((uint16_t)contact.snr >> 8));
    blob.push_back(contact.hops);
  }

  int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) return false;

  size_t written = 0;
  while (written < blob.size()) {
    const ssize_t chunk = write(fd, blob.data() + written, blob.size() - written);
    if (chunk < 0 && errno == EINTR) continue;
    if (chunk < 0) {
      close(fd);
      return false;
    }
    written += (size_t)chunk;
  }

  if (fsync(fd) != 0) {
    close(fd);
    return false;
  }
  return close(fd) == 0;
}

bool Store::flush()
{
  if (dir_.empty()) return false;

  // Temporary file then rename: losing power mid-write must not leave a
  // half-written contact list where the old one used to be.
  const std::string target = dir_ + "/contacts.dat";
  const std::string temporary = target + ".tmp";

  if (!writeContacts(temporary)) {
    unlink(temporary.c_str());
    return false;
  }
  if (rename(temporary.c_str(), target.c_str()) != 0) {
    unlink(temporary.c_str());
    return false;
  }

  // The rename itself needs to reach the disk, not just the file contents.
  int dirFd = ::open(dir_.c_str(), O_RDONLY | O_DIRECTORY);
  if (dirFd >= 0) {
    fsync(dirFd);
    close(dirFd);
  }
  return true;
}
