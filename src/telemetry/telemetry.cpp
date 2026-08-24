// Copyright (c) 2026 Vladimir Egorov
// This project is licensed under the MIT License.
// See the LICENSE file in the root of the repository for the full license text.

#include "telemetry.h"

#include <cstdio>

using namespace telemetry;

static const char* kNames[] = {
  "frames_received",
  "frames_transmitted",
  "frames_duplicate",
  "decrypt_failed",
  "frames_forwarded",
  "frames_refused",
  "ack_timeouts",
  "contacts_added",
  "client_logins",
  "posts_added",
  "routes_reset",
  "adverts_echoed",
};

static_assert(sizeof(kNames) / sizeof(kNames[0]) == (size_t)EventType::Count, "every event type needs a metric name");

Bus::Bus(size_t capacity)
{
  ring_.resize(capacity == 0 ? 1 : capacity);
}

void Bus::publish(EventType type, uint32_t at, uint32_t detail)
{
  if (count_ == ring_.size()) {
    dropped_++; // losing events beats stalling the packet path
    return;
  }

  const size_t tail = (head_ + count_) % ring_.size();
  ring_[tail] = Event { type, at, detail };
  count_++;
}

bool Bus::poll(Event& out)
{
  if (count_ == 0) return false;

  out = ring_[head_];
  head_ = (head_ + 1) % ring_.size();
  count_--;
  return true;
}

void Collector::drain(Bus& bus)
{
  Event event;
  while (bus.poll(event)) {
    if ((size_t)event.type < (size_t)EventType::Count) counters_.byType[(size_t)event.type]++;
  }
  counters_.dropped = bus.dropped();
}

void Collector::observe(uint32_t txQueueDepth, uint32_t dutyCyclePermille)
{
  counters_.txQueueDepth = txQueueDepth;
  counters_.dutyCyclePermille = dutyCyclePermille;
}

std::string telemetry::prometheusText(const Counters& counters)
{
  std::string out;
  char line[160];

  for (size_t i = 0; i < (size_t)EventType::Count; i++) {
    std::snprintf(line, sizeof line, "# TYPE meshcore_%s counter\nmeshcore_%s %llu\n", kNames[i], kNames[i],
      (unsigned long long)counters.byType[i]);
    out += line;
  }

  std::snprintf(
    line, sizeof line, "# TYPE meshcore_tx_queue_depth gauge\nmeshcore_tx_queue_depth %u\n", counters.txQueueDepth);
  out += line;

  std::snprintf(line, sizeof line, "# TYPE meshcore_duty_cycle_permille gauge\nmeshcore_duty_cycle_permille %u\n",
    counters.dutyCyclePermille);
  out += line;

  std::snprintf(line, sizeof line, "# TYPE meshcore_events_dropped counter\nmeshcore_events_dropped %llu\n",
    (unsigned long long)counters.dropped);
  out += line;

  return out;
}

void telemetry::logCounters(const Counters& counters)
{
  platform::Log::write(platform::LogLevel::INFO,
    "rx=%llu tx=%llu dup=%llu undecryptable=%llu forwarded=%llu refused=%llu acktimeout=%llu echo=%llu "
    "queue=%u duty=%u.%u%% dropped=%llu",
    (unsigned long long)counters.byType[(size_t)EventType::FrameRx],
    (unsigned long long)counters.byType[(size_t)EventType::FrameTx],
    (unsigned long long)counters.byType[(size_t)EventType::FrameDuplicate],
    (unsigned long long)counters.byType[(size_t)EventType::DecryptFailed],
    (unsigned long long)counters.byType[(size_t)EventType::Forwarded],
    (unsigned long long)counters.byType[(size_t)EventType::ForwardRefused],
    (unsigned long long)counters.byType[(size_t)EventType::AckTimeout],
    (unsigned long long)counters.byType[(size_t)EventType::AdvertEchoed], counters.txQueueDepth,
    counters.dutyCyclePermille / 10, counters.dutyCyclePermille % 10, (unsigned long long)counters.dropped);
}
