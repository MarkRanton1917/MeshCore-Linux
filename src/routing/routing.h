// Copyright (c) 2026 Vladimir Egorov
// This project is licensed under the MIT License.
// See the LICENSE file in the root of the repository for the full license text.

#pragma once

#include "identity.h"
#include "packet.h"
#include "protocol.h"
#include "telemetry.h"

#include <cstdint>
#include <vector>

// Moving packets between the radio and the node above. This is the only module
// with a clock, and it never reads one: time arrives through tick(). The radio
// is an interface for the same reason — a five-node network has to run inside
// one process on a laptop.
//
// routing knows nothing about posts, clients or ACLs. It deals in packets and
// contacts, hands decrypted payloads up, and puts frames in the radio queue.
// Policy lives in Delegate, not here.
namespace routing {

using crypto::core::PublicKey;
using Millis = uint64_t;
using SendId = uint32_t;

struct RxMeta {
  Millis airtime = 0; // how long the frame held the air; sets the backoff scale
  int16_t snr = 0;
};

// Acks and returned paths go ahead of other people's flood: they are short,
// delay-sensitive, and decide whether the sender switches to direct.
enum class Priority {
  HIGH = 0,
  NORMAL = 1
};

// The radio owns the duty cycle budget and listen-before-talk. routing only
// hands it frames and never decides whether it may transmit now.
struct Radio {
  virtual ~Radio() = default;
  virtual void enqueue(ByteView frame, Priority priority) = 0;
};

struct Delegate {
  virtual ~Delegate() = default;

  virtual void onPayload(const identity::Contact& from, packet::PayloadType type, ByteView plain) = 0;

  // Separate from onPayload: an ANON_REQ arrives from somebody who is not a
  // contact yet, so there is no contact to hand over — only the raw key.
  virtual void onAnon(const PublicKey& from, ByteView plain) = 0;

  virtual void onAck(SendId id) = 0;
  virtual void onDeliveryFailed(SendId id) = 0;

  // Acknowledging is the default, but some payloads must not be acked at all:
  // a CLI command that gets one leaves the client confused. routing has no way
  // to tell those apart, the layer that decoded the payload does.
  virtual bool shouldAck(packet::PayloadType type, ByteView plain)
  {
    (void)type;
    (void)plain;
    return true;
  }

  // The one piece of policy left outside: transport code filtering, hop limits,
  // blocklists. Forwarding everything is the default.
  virtual bool shouldForward(const packet::Packet& p)
  {
    (void)p;
    return true;
  }
};

struct Config {
  // A flood spreads across the whole network; a known route should answer fast.
  Millis floodAckTimeout = 12000;
  Millis directAckBase = 4000;
  Millis directAckPerHop = 2000;
  uint8_t maxAttempts = 3;

  // Forwarding delay is airtime times this, plus a random spread. The spread is
  // not decoration: three repeaters that heard one flood and answer together
  // drown each other out, and the network does not recover on its own.
  uint32_t forwardAirtimeFactor = 2;
  Millis forwardJitter = 400;

  size_t maxPending = 32;
  size_t maxRoutes = 128;

  // Optional. routing publishes and never calls telemetry back, so leaving
  // this null switches the whole thing off.
  telemetry::Bus* bus = nullptr;
};

class Router {
public:
  Router(identity::Store& store, Radio& radio, Delegate& delegate, Config config = {});

  // The only entry point from the radio.
  void onFrame(ByteView frame, RxMeta meta);

  // Moves time: ack timeouts, deferred retransmissions, draining the queue.
  void tick(Millis now);

  SendId sendDirect(const PublicKey& to, packet::PayloadType type, ByteView payload, bool wantAck);
  void sendFlood(packet::PayloadType type, ByteView payload);

  bool hasRoute(const PublicKey& to) const;
  void forgetRoute(const PublicKey& to);
  size_t pendingCount() const
  {
    return pending_.size();
  }
  size_t queuedCount() const
  {
    return queue_.size();
  }

  // A path is recorded from the sender towards us; sending back needs it the
  // other way round. Getting the direction wrong survives the first flood
  // exchange and only breaks direct packets, so it is public and tested.
  static size_t reversePath(ByteView forward, ByteSpan out);

private:
  struct Delayed {
    Millis notBefore = 0;
    Priority priority = Priority::NORMAL;
    std::vector<uint8_t> frame;
  };

  struct Pending {
    SendId id = 0;
    uint32_t expectedAck = 0;
    Millis deadline = 0;
    uint8_t attempts = 0;
    bool viaFlood = true;
    PublicKey to;
    std::vector<uint8_t> frame;
  };

  struct Route {
    PublicKey to;
    std::vector<uint8_t> path;
  };

  struct Seen {
    uint32_t shortHash = 0;
    Millis at = 0;
  };

  bool alreadySeen(ByteView frame);
  void rememberSeen(ByteView frame);
  void queueFrame(std::vector<uint8_t> frame, Millis notBefore, Priority priority);
  void transmitNow(std::vector<uint8_t> frame, Priority priority);

  void deliverToSelf(const packet::Packet& p, ByteView payload);
  void deliverAnon(const packet::Packet& p, ByteView payload);
  void handleAck(ByteView payload);
  void learnRouteFrom(const packet::Packet& p, const PublicKey& sender);
  void sendPathReturn(const PublicKey& to, ByteView forward);
  void installRoute(const PublicKey& to, ByteView path);
  void considerForwarding(const packet::Packet& p, RxMeta meta);

  Route* findRoute(const PublicKey& to);
  const Route* findRoute(const PublicKey& to) const;

  std::vector<uint8_t> buildFrame(packet::PayloadType type, ByteView payload, bool direct, ByteView path) const;
  Millis jitter();

  identity::Store& store_;
  Radio& radio_;
  Delegate& delegate_;
  Config config_;

  Millis now_ = 0;
  SendId nextId_ = 1;

  std::vector<Seen> seen_;
  size_t seenNext_ = 0;

  std::vector<Delayed> queue_;
  std::vector<Pending> pending_;
  std::vector<Route> routes_;
};

} // namespace routing
