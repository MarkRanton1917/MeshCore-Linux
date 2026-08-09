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
  uint32_t timestamp = 0;
  std::array<uint8_t, POST_AUTHOR_PREFIX> author {};
  std::string text;
};

struct Client {
  PublicKey pk;
  Access access = Access::NONE;
  uint32_t syncSince = 0; // everything up to here has been handed over
  uint32_t lastLogin = 0; // replay guard for the login packet
  bool pending = false;
  SendId pendingId = 0;
  uint32_t pendingPost = 0; // timestamp of the post in flight
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

private:
  bool handleText(const identity::Contact& from, ByteView plain);
  const Post* nextPostFor(const Client& client) const;
  void sendLoginResponse(const PublicKey& to, const Client& client);
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
};

} // namespace room
