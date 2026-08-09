// Copyright (c) 2026 Vladimir Egorov
// This project is licensed under the MIT License.
// See the LICENSE file in the root of the repository for the full license text.

#pragma once

#include "identity.h"
#include "routing.h"
#include "telemetry.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The noticeboard. By this point the lower layers have absorbed the hard parts:
// room never sees a byte off the air, knows nothing about the radio, and counts
// no airtime. It works on decoded objects and one state row per client, which
// is why the whole module tests without a network.
namespace room {

using crypto::core::PublicKey;
using routing::Millis;
using routing::SendId;

// What room needs from routing. An interface so a test can collect the outgoing
// calls in a vector instead of transmitting them.
struct Sender {
  virtual ~Sender() = default;
  virtual SendId sendDirect(const PublicKey& to, packet::PayloadType type, ByteView payload, bool wantAck) = 0;
};

// What a command needs and the room does not own: the radio, the system clock
// and the process itself all sit above it. Optional like the bus — with no host
// attached those commands answer "unsupported" rather than pretending.
struct Admin {
  virtual ~Admin() = default;

  // Floods a fresh advert. Wanted on demand and after the name changes.
  virtual void sendAdvert() = 0;

  // Steps the wall clock. False when the host refuses — usually not root.
  virtual bool setClock(uint32_t unixSeconds) = 0;

  virtual std::string nodeName() const = 0;
  virtual bool setNodeName(std::string_view name) = 0;

  // Persists a setting under the same key the config file uses, so what an
  // admin changed over the air is still in force after a restart. False means
  // it could not be stored — and then the room refuses the change outright
  // rather than applying one that quietly expires at the next reboot.
  virtual bool saveSetting(std::string_view key, std::string_view value) = 0;

  // Requested, not performed: the loop finishes the tick and saves state first.
  virtual void requestReboot() = 0;

  // Seconds since the process started. The room has no clock of its own.
  virtual uint32_t uptime() const = 0;
};

// txt_type, the upper six bits of the text flags byte. packet carries it as a
// plain number and never looks at it: what the values mean is the room's
// business, and nothing below this layer has any use for the distinction.
enum class TextType : uint8_t {
  PLAIN = 0, // somebody posting to the board
  CLI = 1, // an admin command, answered rather than acked
  SIGNED = 2 // what the room pushes back out, author prefix and all
};

// The leading byte of a REQ plaintext. Like the response bodies, these are ours
// to define: the protocol calls a REQ payload application data and says nothing
// about what goes in it.
enum class RequestType : uint8_t {
  STATUS = 0x01, // how many posts, how many clients, how many unread
  KEEP_ALIVE = 0x02 // the same, and a nudge to resume pushing straight away
};

enum class Access : uint8_t {
  NONE = 0,
  READ_ONLY = 1, // only when anonymous reading is allowed
  GUEST = 2,
  ADMIN = 3
};

enum class Action {
  READ,
  POST,
  COMMAND
};

struct Post {
  // Ours, monotonic, and the only thing synchronisation is allowed to count on.
  // Timestamps come from client clocks: two posts can share one to the second,
  // and a bookmark that is a timestamp then cannot name the boundary between
  // them — whichever arrives second is skipped for good.
  uint32_t seq = 0;

  uint32_t timestamp = 0;
  std::array<uint8_t, POST_AUTHOR_PREFIX> author {};
  std::string text;
};

struct Client {
  PublicKey pk;
  Access access = Access::NONE;
  uint32_t syncSeq = 0; // everything up to this seq has been handed over
  uint32_t lastLogin = 0; // replay guard for the login packet
  uint32_t lastCommand = 0; // and for commands, which are worth replaying
  uint32_t lastRequest = 0; // and for REQs, kept apart so one cannot block the other
  bool pending = false;
  SendId pendingId = 0;
  uint32_t pendingSeq = 0; // seq of the post in flight
  Millis retryAfter = 0;
};

struct Config {
  std::string adminPassword;
  std::string guestPassword;
  bool allowAnonymousRead = false;

  // A client clock can be anything. Outside this window its timestamp is
  // replaced with ours, or one post from the future breaks sorting and
  // everybody else's syncSince along with it.
  uint32_t clockWindow = 3600;

  Millis retryDelay = 5000;
  uint16_t firmwareVersion = 1;

  // Optional, same rule as routing: room publishes, never calls telemetry.
  telemetry::Bus* bus = nullptr;

  // Optional too. Absent, the commands that reach outside the room refuse.
  Admin* admin = nullptr;
};

class Room : public routing::Delegate {
public:
  Room(identity::Store& store, Sender& sender, Config config = {});

  // Server time is the truth: a Pi has NTP. Injected, never read here.
  void setServerTime(uint32_t unixSeconds);

  bool load(const std::string& dir);
  bool flush();

  // ---- routing::Delegate
  void onAnon(const PublicKey& from, ByteView plain) override;
  void onPayload(const identity::Contact& from, packet::PayloadType type, ByteView plain) override;
  void onAck(SendId id) override;
  void onDeliveryFailed(SendId id) override;
  bool shouldAck(packet::PayloadType type, ByteView plain) override;

  // The room stays a repeater. A packet addressed to it still travels on.
  bool shouldForward(const packet::Packet& p) override;

  void tick(Millis now);

  bool addPost(const PublicKey& author, uint32_t timestamp, std::string_view text);

  // Three outcomes: admin, guest, neither — and if anonymous reading is on,
  // neither still gets in with cut-down rights.
  Client* authenticate(const PublicKey& pk, std::string_view password, uint32_t timestamp);

  // Admin only, and the reply is the whole acknowledgement: a CLI command is
  // never acked, so one that answers nothing is indistinguishable from a node
  // that has stopped listening. Every path through here replies.
  bool handleCommand(Client& client, uint32_t timestamp, std::string_view line);

  // Round robin from where we stopped, not from the head of the list: otherwise
  // the first client with a bad antenna starves everybody behind it.
  Client* nextClientToPush();
  void pushNextPost(Client& client);

  // One place, called from three. Scattering the checks is how a new branch
  // ends up without one.
  static bool can(const Client& client, Action action);

  Client* findClient(const PublicKey& pk);
  size_t clientCount() const
  {
    return clients_.size();
  }
  size_t postCount() const
  {
    return posts_.size();
  }
  const std::vector<Post>& posts() const
  {
    return posts_;
  }

  // How much this client has still to receive. Reported in a status reply so a
  // client can tell "nothing new" from "the room never heard me".
  size_t unreadFor(const Client& client) const;

private:
  bool handleText(const identity::Contact& from, ByteView plain);
  bool handleRequest(Client& client, ByteView plain);
  void sendStatusResponse(const Client& client, RequestType type);

  const Post* nextPostFor(const Client& client) const;

  // A client bookmarks by time — it has never heard of our sequence numbers —
  // so a login has to be translated. Deliberately cautious: it stops at the
  // first post the client has not seen, even if later ones are older still.
  // Handing over a post twice is a duplicate; skipping one loses it.
  uint32_t seqFromTimestamp(uint32_t bookmark) const;

  void sendLoginResponse(const PublicKey& to, const Client& client);

  // Commands split off one per verb; the dispatcher stays readable and each one
  // owns its own reply.
  void commandClock(const PublicKey& to, std::string_view rest);
  void commandSet(const PublicKey& to, std::string_view rest);
  void commandGet(const PublicKey& to, std::string_view rest);
  void commandClear(const PublicKey& to, std::string_view rest);
  void sendCliReply(const PublicKey& to, std::string_view text);
  void replyf(const PublicKey& to, const char* format, ...);

  uint32_t sanitiseTimestamp(uint32_t claimed) const;
  bool writeState(const std::string& path) const;
  bool readState(const std::string& path);

  identity::Store& store_;
  Sender& sender_;
  Config config_;

  Millis now_ = 0;
  uint32_t serverTime_ = 0;
  std::string dir_;

  std::vector<Post> posts_; // ring buffer, oldest first
  std::vector<Client> clients_;
  size_t nextClientIdx_ = 0;

  // Saved with the state. Restarting at 1 while a client still holds a bookmark
  // of 40 would leave every new post looking already delivered.
  uint32_t nextSeq_ = 1;
};

} // namespace room
