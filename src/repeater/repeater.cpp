// Copyright (c) 2026 Vladimir Egorov
// This project is licensed under the MIT License.
// See the LICENSE file in the root of the repository for the full license text.

#include "repeater.h"

#include <algorithm>

using namespace repeater;

Policy::Policy(Config config)
  : config_(std::move(config))
{
  setMaxHops(config_.maxHops);
}

void Policy::setMaxHops(uint8_t hops)
{
  // Zero would stop everything at the first node, and anything past the format
  // limit is the format limit. Clamped rather than refused: this is reached
  // from a config file and from an admin typing over the air, and neither is
  // worth failing a start or dropping a command over.
  config_.maxHops = std::clamp<uint8_t>(hops, 1, MAX_HOP_COUNT);
}

size_t Policy::bucketOf(const packet::Packet& p)
{
  const size_t hops = p.pathSize();
  if (hops == 0) return REPEATER_DIRECT_BUCKET;

  // The last hash written is the last node that touched it, which is the one
  // whose transmitter we are actually hearing.
  return p.path[hops - 1];
}

bool Policy::isBlocked(const packet::Packet& p) const
{
  if (config_.blocked.empty()) return false;

  for (size_t i = 0; i < p.pathSize(); i++) {
    if (std::find(config_.blocked.begin(), config_.blocked.end(), p.path[i]) != config_.blocked.end()) {
      return true;
    }
  }
  return false;
}

bool Policy::withinRate(size_t bucket, Millis now)
{
  if (config_.floodPerMinute == 0) return true;

  Bucket& slot = buckets_[bucket];

  // A fresh window rather than a sliding one: cruder, and it costs one counter
  // per neighbour instead of a list of timestamps per neighbour. What this has
  // to stop is a node transmitting without pause, and a coarse window stops
  // that just as well.
  if (now < slot.windowStart || now - slot.windowStart >= REPEATER_RATE_WINDOW_MS) {
    slot.windowStart = now;
    slot.count = 0;
  }

  if (slot.count >= config_.floodPerMinute) return false;

  slot.count++;
  return true;
}

bool Policy::shouldForward(const packet::Packet& p, Millis now, uint32_t usedPermille)
{
  // Switched off is not the same as dropping: nothing is counted, because there
  // is no transit here to have refused.
  if (!config_.enabled) return false;

  // Flood counts the hops behind it, direct counts the ones still ahead. Either
  // way the number is how far this packet travels, and past the limit it is a
  // packet going in circles.
  if (p.hopCount >= config_.maxHops) {
    stats_.hopLimit++;
    return false;
  }

  if (isBlocked(p)) {
    stats_.blocked++;
    return false;
  }

  if (config_.dutyCeilingPermille != 0 && usedPermille >= config_.dutyCeilingPermille) {
    stats_.budget++;
    return false;
  }

  // Only flood is rate limited. A direct packet follows a route somebody
  // already learned, arrives one at a time, and is the traffic this node exists
  // to carry; a flood is what storms.
  const bool flood = p.routeType() == packet::RouteType::FLOOD || p.routeType() == packet::RouteType::TRANSPORT_FLOOD;
  if (flood && !withinRate(bucketOf(p), now)) {
    stats_.rateLimited++;
    return false;
  }

  stats_.forwarded++;
  return true;
}
