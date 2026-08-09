// Copyright (c) 2026 Vladimir Egorov
// This project is licensed under the MIT License.
// See the LICENSE file in the root of the repository for the full license text.

#pragma once

#include "core.h"
#include "packet.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

// Who we are and who we know. Unlike packet and crypto, this module holds
// state, split by how long each part lives: the identity and the contacts
// survive a reboot, the derived secrets do not.
namespace identity {

using crypto::core::PrivateKey;
using crypto::core::PublicKey;
using crypto::core::SharedSecret;

// Low four bits of the advert flags byte.
enum class NodeType : uint8_t {
  UNKNOWN = 0,
  CHAT = 1,
  REPEATER = 2,
  ROOM_SERVER = 3,
  SENSOR = 4
};

// What remember() did. ADDED is worth logging, STALE is worth counting: it
// means somebody is replaying old adverts, or a peer's clock went backwards.
enum class Update {
  ADDED,
  UPDATED,
  STALE,
  REJECTED // malformed advert, should not reach us at all
};

// A peer learned from an advert.
struct Contact {
  PublicKey pk;
  uint32_t timestamp = 0; // advert time, and the replay guard
  NodeType type = NodeType::UNKNOWN;
  bool hasLocation = false;
  int32_t latitude = 0; // degrees x 1e6
  int32_t longitude = 0;
  std::string name;

  // What the radio heard, as opposed to what the advert claimed: the link, not
  // the peer. Filled in from adverts, because an advert is the one packet that
  // names its sender before anything is decrypted — everything else arrives
  // behind a one-byte hash and is only attributed after the MAC checks out.
  uint32_t lastHeard = 0; // wall clock, seconds
  int16_t snr = 0; // decibels, of the last frame from this contact
  uint8_t hops = 0; // how many nodes it came through; 0 is a neighbour

  uint8_t hash() const
  {
    return pk.data[0];
  }

  // A contact we hear directly. What "neighbour" means here and nowhere else,
  // so a second opinion cannot creep in somewhere later.
  bool isNeighbour() const
  {
    return lastHeard != 0 && hops == 0;
  }
};

class Store {
public:
  Store();

  // Reads the identity and contacts from dir, creating the identity on first
  // run. False only on a real failure: no permission, full disk, corrupt file.
  // A damaged key is never replaced silently — a node that quietly changes
  // identity is seen by the whole network as a stranger, and nobody notices.
  bool loadOrCreate(const std::string& dir);

  const PublicKey& selfPk() const
  {
    return self_.pk;
  }
  const PrivateKey& selfSk() const
  {
    return self_.sk;
  }
  // Precomputed because every received packet needs it: stripSelf compares it
  // against the first hop, deduplication against our own returning packets.
  uint8_t selfHash() const
  {
    return selfHash_;
  }

  const Contact* find(const PublicKey& pk) const;

  // One byte of hash is ambiguous — collisions are near certain past a couple
  // of hundred nodes — so this returns every candidate. The caller tells them
  // apart by whose MAC checks out. Returning a single contact here would drop
  // other people's messages silently.
  std::span<const Contact* const> findByHash(uint8_t hash) const;

  // Takes an advert whose signature the caller has already verified. Rejects
  // one whose timestamp is not newer than the stored one, otherwise anyone
  // could roll our records back by replaying a genuine old advert.
  Update remember(const packet::Advert& advert);

  // What the radio heard, kept apart from what the advert said: remember() has
  // only the packet's claims, and this has the link. Silently does nothing for
  // a key we do not know — an unknown contact is not worth a slot until it has
  // introduced itself.
  void noteHeard(const PublicKey& pk, uint32_t at, int16_t snr, uint8_t hops);

  // Neighbours first, most recently heard first within that. Returned as
  // pointers into the store, so they live exactly as long as the store does.
  std::vector<const Contact*> neighbours(size_t limit) const;

  // Cached ECDH. Not const: it fills the cache, and hiding that behind mutable
  // would be worse. The pointer stays valid until a later call evicts the slot.
  const SharedSecret* secretFor(const PublicKey& pk);

  // Writes the contacts out through a temporary file and a rename. Called on a
  // timer and at shutdown, never per advert: adverts arrive constantly, and a
  // synchronous write for each would stall the node and wear out an SD card.
  bool flush();

  size_t contactCount() const
  {
    return contacts_.size();
  }

private:
  struct CachedSecret {
    PublicKey pk;
    SharedSecret secret;
    uint64_t usedAt = 0;
  };

  Contact* findMutable(const PublicKey& pk);
  void indexContact(const Contact& contact);
  void reindex();
  Contact& allocateSlot();
  bool loadContacts(const std::string& path);
  bool writeContacts(const std::string& path) const;

  crypto::core::KeyPair self_ {};
  uint8_t selfHash_ = 0;
  std::string dir_;

  std::vector<Contact> contacts_;
  std::array<std::vector<const Contact*>, 256> buckets_;

  std::vector<CachedSecret> secrets_;
  uint64_t clock_ = 0;
};

} // namespace identity
