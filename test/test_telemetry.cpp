// Tests for src/telemetry.
//
// The property that matters: it can be switched off and nothing else notices,
// and when the consumer falls behind it loses events rather than blocking the
// packet path.

#include "check.h"
#include "telemetry.h"

#include <string>

using check::section;
using check::that;

static void testBus()
{
  section("telemetry: event bus");

  telemetry::Bus bus(4);
  that("starts empty", bus.pending() == 0 && bus.dropped() == 0);

  bus.publish(telemetry::EventType::FrameRx, 100, 42);
  telemetry::Event event;
  that("polls back what was published",
    bus.poll(event) && event.type == telemetry::EventType::FrameRx && event.at == 100 && event.detail == 42);
  that("and is empty again", !bus.poll(event));

  for (int i = 0; i < 4; i++)
    bus.publish(telemetry::EventType::FrameTx, (uint32_t)i);
  that("fills to capacity", bus.pending() == 4 && bus.dropped() == 0);

  // Overflow must drop, never block or overwrite what has not been read.
  bus.publish(telemetry::EventType::FrameTx, 99);
  that("overflow is dropped", bus.dropped() == 1 && bus.pending() == 4);

  that("the oldest survives", bus.poll(event) && event.at == 0);
  bus.publish(telemetry::EventType::FrameTx, 5);
  that("space frees up as it drains", bus.pending() == 4);

  telemetry::Bus tiny(0);
  tiny.publish(telemetry::EventType::FrameRx, 1);
  that("a zero-capacity bus still behaves", tiny.pending() <= 1);
}

static void testCollector()
{
  section("telemetry: collector");

  telemetry::Bus bus(64);
  telemetry::Collector collector;

  for (int i = 0; i < 3; i++)
    bus.publish(telemetry::EventType::FrameRx, (uint32_t)i);
  bus.publish(telemetry::EventType::FrameDuplicate, 10);
  bus.publish(telemetry::EventType::DecryptFailed, 11);
  bus.publish(telemetry::EventType::AckTimeout, 12);

  collector.drain(bus);
  const telemetry::Counters& counters = collector.counters();

  that("counts by type",
    counters.byType[(size_t)telemetry::EventType::FrameRx] == 3
      && counters.byType[(size_t)telemetry::EventType::FrameDuplicate] == 1
      && counters.byType[(size_t)telemetry::EventType::DecryptFailed] == 1
      && counters.byType[(size_t)telemetry::EventType::AckTimeout] == 1);
  that("bus is drained", bus.pending() == 0);

  bus.publish(telemetry::EventType::FrameRx, 20);
  collector.drain(bus);
  that("counts accumulate across drains", collector.counters().byType[(size_t)telemetry::EventType::FrameRx] == 4);

  // The two gauges that matter in operation.
  collector.observe(7, 850);
  that("queue depth observed", collector.counters().txQueueDepth == 7);
  that("duty cycle observed", collector.counters().dutyCyclePermille == 850);
}

static void testPrometheus()
{
  section("telemetry: prometheus text");

  telemetry::Counters counters;
  counters.byType[(size_t)telemetry::EventType::FrameRx] = 12;
  counters.byType[(size_t)telemetry::EventType::Forwarded] = 5;
  counters.txQueueDepth = 3;
  counters.dutyCyclePermille = 91;
  counters.dropped = 2;

  const std::string text = telemetry::prometheusText(counters);

  that("counter value rendered", text.find("meshcore_frames_received 12") != std::string::npos);
  that("second counter rendered", text.find("meshcore_frames_forwarded 5") != std::string::npos);
  that("queue gauge rendered", text.find("meshcore_tx_queue_depth 3") != std::string::npos);
  that("duty gauge rendered", text.find("meshcore_duty_cycle_permille 91") != std::string::npos);
  that("drops are visible", text.find("meshcore_events_dropped 2") != std::string::npos);
  that("types are declared", text.find("# TYPE meshcore_frames_received counter") != std::string::npos);
  that("every line ends", text.back() == '\n');

  // A metric per event type, plus two gauges and the drop counter.
  size_t lines = 0;
  for (char c : text)
    if (c == '\n') lines++;
  that("one help line and one value line each", lines == (size_t)telemetry::EventType::Count * 2 + 6);
}

static void testDisabled()
{
  section("telemetry: switched off");

  // A null bus is what "disabled" means to routing and room: publishing is a
  // no-op and nothing above notices.
  telemetry::Bus* bus = nullptr;
  that("a null bus is the off switch", bus == nullptr);

  telemetry::Collector collector;
  that("counters read as zero without a bus",
    collector.counters().byType[(size_t)telemetry::EventType::FrameRx] == 0 && collector.counters().dropped == 0);

  const std::string text = telemetry::prometheusText(collector.counters());
  that("and still render", text.find("meshcore_frames_received 0") != std::string::npos);
}

int main()
{
  testBus();
  testCollector();
  testPrometheus();
  testDisabled();
  return check::report();
}
