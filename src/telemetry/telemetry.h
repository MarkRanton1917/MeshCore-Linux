// Copyright (c) 2026 Vladimir Egorov
// This project is licensed under the MIT License.
// See the LICENSE file in the root of the repository for the full license text.

#pragma once

#include "platform.h"

#include <cstdint>
#include <string>
#include <vector>

// The module you can switch off with a flag while everything else keeps
// working. If that is not true, something is wired the wrong way round.
//
// routing and room never call telemetry. They publish events to a bus and
// telemetry subscribes, or in six months the routing logic contains an MQTT
// client and can be neither disabled nor tested without a broker.
namespace telemetry {

using platform::Millis;

enum class EventType : uint8_t {
  FrameRx = 0,
  FrameTx,
  FrameDuplicate,
  DecryptFailed,
  Forwarded,
  AckTimeout,
  ContactAdded,
  ClientLogin,
  PostAdded,
  RouteReset,
  Count
};

struct Event {
  EventType type = EventType::FrameRx;
  uint32_t at = 0;
  uint32_t detail = 0; // length, id, whatever the type implies
};

// Publishing is a non-blocking write into a ring. When the consumer falls
// behind, events are lost on purpose: telemetry may not stall packet handling
// or blow an ack deadline.
class Bus {
public:
  explicit Bus(size_t capacity = 256);

  void publish(EventType type, uint32_t at, uint32_t detail = 0);
  bool poll(Event& out);

  uint64_t dropped() const
  {
    return dropped_;
  }
  size_t pending() const
  {
    return count_;
  }

private:
  std::vector<Event> ring_;
  size_t head_ = 0;
  size_t count_ = 0;
  uint64_t dropped_ = 0;
};

struct Counters {
  uint64_t byType[(size_t)EventType::Count] = {};

  // The two that matter most in operation: a growing queue and a spent budget
  // mean the node is overloaded, and you want to see that before people
  // complain.
  uint32_t txQueueDepth = 0;
  uint32_t dutyCyclePermille = 0;
  uint64_t dropped = 0;
};

// Drains the bus into counters. Called from the main loop, never from the
// packet path.
class Collector {
public:
  void drain(Bus& bus);
  void observe(uint32_t txQueueDepth, uint32_t dutyCyclePermille);

  const Counters& counters() const
  {
    return counters_;
  }

private:
  Counters counters_;
};

// Text for a /metrics endpoint. Rendering is separate from serving on purpose:
// the HTTP listener belongs to the host, not here.
std::string prometheusText(const Counters& counters);

// Always available, costs nothing when the level is above debug.
void logCounters(const Counters& counters);

} // namespace telemetry
