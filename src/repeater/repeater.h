// Copyright (c) 2026 Vladimir Egorov
// This project is licensed under the MIT License.
// See the LICENSE file in the root of the repository for the full license text.

#pragma once

#include "defines.h"
#include "packet.h"

#include <array>
#include <cstdint>
#include <vector>

// Transit policy: the one place that decides whether somebody else's packet is
// carried on. It holds no radio, no clock and no key — the packet, the time and
// how much of the airtime budget is gone are all handed in, and what comes back
// is yes or no. That is why the whole module tests in a loop with no network.
//
// Forwarding itself belongs to routing; this only says whether it may happen.
// A policy exists only on the node types that repeat, and a node with none
// attached carries nothing at all — which is what a plain room server is.
namespace repeater {

using Millis = uint64_t;

struct Config {
  bool enabled = true;

  // Below MAX_HOP_COUNT on purpose: sixty-three is what the format allows, not
  // what a sane network needs. A packet already through a dozen nodes is lost
  // rather than late.
  uint8_t maxHops = 12;

  // Floods per previous hop, per minute. One neighbour with a wedged
  // transmitter must not spend the whole node's airtime.
  uint32_t floodPerMinute = 30;

  // Transit stops once this much of the sliding window has gone, leaving the
  // rest for our own traffic: a room that cannot answer its clients because it
  // is busy repeating strangers is worse than one that repeats less. Zero
  // switches the check off.
  uint32_t dutyCeilingPermille = 80;

  // Node hashes we refuse to carry for. One byte and ambiguous like every other
  // hash here, so this blocks a hash and not a key — and blocks every node that
  // happens to share it.
  std::vector<uint8_t> blocked;
};

// Every refusal is counted apart from the others. "The repeater dropped four
// thousand packets" is not an answer; which counter moved is the diagnosis.
struct Stats {
  uint32_t forwarded = 0;
  uint32_t hopLimit = 0;
  uint32_t blocked = 0;
  uint32_t rateLimited = 0;
  uint32_t budget = 0;
};

class Policy {
public:
  explicit Policy(Config config = {});

  // Time and load arrive as arguments for the reason routing takes its clock
  // through tick(): a policy that reads a clock of its own cannot be tested at
  // speed, and this one is asked about every packet that lands.
  bool shouldForward(const packet::Packet& p, Millis now, uint32_t usedPermille);

  void setEnabled(bool on)
  {
    config_.enabled = on;
  }
  bool enabled() const
  {
    return config_.enabled;
  }

  void setMaxHops(uint8_t hops);
  uint8_t maxHops() const
  {
    return config_.maxHops;
  }

  const Stats& stats() const
  {
    return stats_;
  }
  void clearStats()
  {
    stats_ = Stats {};
  }

private:
  // Which neighbour handed us this packet: the last hop in the path. One that
  // arrived with no path came straight from whoever wrote it, and that hash is
  // nowhere in the frame, so all of those share the last bucket.
  static size_t bucketOf(const packet::Packet& p);
  bool isBlocked(const packet::Packet& p) const;
  bool withinRate(size_t bucket, Millis now);

  struct Bucket {
    Millis windowStart = 0;
    uint32_t count = 0;
  };

  Config config_;
  Stats stats_;

  // One slot per possible previous hop, so the table never grows and never
  // needs evicting: 256 hashes plus the bucket for packets heard first-hand.
  std::array<Bucket, REPEATER_SOURCE_BUCKETS> buckets_ {};
};

} // namespace repeater
