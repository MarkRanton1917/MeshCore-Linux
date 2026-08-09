// Copyright (c) 2026 Vladimir Egorov
// This project is licensed under the MIT License.
// See the LICENSE file in the root of the repository for the full license text.

// Tests for src/radio.
//
// The hardware driver cannot run here, but everything above the SPI line can:
// airtime, the duty cycle budget, the priority queue, and the two fake radios
// that make mesh scenarios possible on a laptop.

#include "check.h"
#include "radio.h"

#include <cstring>
#include <string>
#include <vector>

using check::section;
using check::that;

static ByteView view(const std::vector<uint8_t>& v)
{
  return ByteView { v.data(), v.size() };
}

class Collecting : public radio::RxSink {
public:
  void onFrame(ByteView frame, const radio::RxMeta& meta) override
  {
    frames.push_back(std::vector<uint8_t>(frame.begin(), frame.end()));
    metas.push_back(meta);
  }
  std::vector<std::vector<uint8_t>> frames;
  std::vector<radio::RxMeta> metas;
};

static void testAirtime()
{
  section("radio: airtime");

  radio::Params params; // SF11, 62.5 kHz, CR 4/5

  const uint32_t small = radio::airtimeUsFor(params, 16);
  const uint32_t large = radio::airtimeUsFor(params, 160);
  that("longer frames take longer", large > small);
  that("a short frame is hundreds of milliseconds at SF11/62.5k", small > 200000 && small < 2000000);

  radio::Params faster = params;
  faster.spreadingFactor = 7;
  that("a lower spreading factor is quicker", radio::airtimeUsFor(faster, 160) < radio::airtimeUsFor(params, 160));

  radio::Params wider = params;
  wider.bandwidthHz = 250000;
  that("a wider bandwidth is quicker", radio::airtimeUsFor(wider, 160) < radio::airtimeUsFor(params, 160));

  radio::Params coded = params;
  coded.codingRate = 8; // 4/8
  that("more coding costs time", radio::airtimeUsFor(coded, 160) > large);
}

static void testDutyCycle()
{
  section("radio: duty cycle budget");

  // 10% of a one-second window is 100 ms of airtime.
  radio::DutyCycle duty(10, 1000);

  that("an empty budget allows", duty.allows(0, 50000));
  duty.record(0, 50000);
  that("half spent still allows a small one", duty.allows(0, 40000));
  that("but not one that overruns", !duty.allows(0, 60000));

  duty.record(0, 50000);
  that("full budget refuses", !duty.allows(0, 1000));
  that("usage reported in permille", duty.usedPermille(0) == 100);

  // The window slides: once the old records age out the budget returns.
  that("still blocked just before the window ends", !duty.allows(999, 1000));
  that("free again once the window passes", duty.allows(1000, 50000));
  that("usage falls back to zero", duty.usedPermille(1000) == 0);

  radio::DutyCycle unlimited(100, 1000);
  that("100 percent never blocks", unlimited.allows(0, 999999999));
}

static void testTxQueue()
{
  section("radio: priority queue");

  radio::TxQueue queue(3);
  std::vector<uint8_t> a { 1 }, b { 2 }, c { 3 }, d { 4 };

  that("accepts up to its depth",
    queue.push(view(a), radio::Priority::NORMAL) && queue.push(view(b), radio::Priority::NORMAL)
      && queue.push(view(c), radio::Priority::HIGH));
  that("overflow is refused", !queue.push(view(d), radio::Priority::NORMAL));

  std::vector<uint8_t> out;
  that("high priority jumps the queue", queue.pop(out) && out.size() == 1 && out[0] == 3);
  that("then oldest first", queue.pop(out) && out[0] == 1);
  that("then the next", queue.pop(out) && out[0] == 2);
  that("empty queue reports empty", !queue.pop(out));

  // Transit sits below our own traffic, and the priority has to survive the
  // trip out of the queue: the duty cycle puts frames back, and one that came
  // back promoted would overtake the answers it is supposed to yield to.
  radio::TxQueue levels(3);
  levels.push(view(a), radio::Priority::LOW);
  levels.push(view(b), radio::Priority::NORMAL);
  levels.push(view(c), radio::Priority::HIGH);

  radio::Priority priority = radio::Priority::NORMAL;
  that("high first", levels.pop(out, &priority) && out[0] == 3 && priority == radio::Priority::HIGH);
  that("then ours", levels.pop(out, &priority) && out[0] == 2 && priority == radio::Priority::NORMAL);
  that("transit last", levels.pop(out, &priority) && out[0] == 1 && priority == radio::Priority::LOW);
}

static void testVirtualRadio()
{
  section("radio: virtual radio");

  radio::Medium medium;
  radio::Params params;
  params.dutyCyclePercent = 100; // budget tested separately

  radio::VirtualRadio a(medium, params);
  radio::VirtualRadio b(medium, params);
  radio::VirtualRadio far(medium, params);

  Collecting sinkB, sinkFar;
  b.setSink(&sinkB);
  far.setSink(&sinkFar);
  medium.link(0, 1); // a hears b, b hears a; far hears nobody

  std::vector<uint8_t> frame { 0x11, 0x00, 1, 2, 3 };
  that("send only queues", a.send(view(frame)) && sinkB.frames.empty());

  a.tick(100);
  that("tick puts it on the air", sinkB.frames.size() == 1);
  that("frame arrives intact", sinkB.frames.size() == 1 && sinkB.frames[0] == frame);
  that("metadata carries airtime", sinkB.metas.size() == 1 && sinkB.metas[0].airtimeUs == a.airtimeUs(frame.size()));
  that("a node out of range hears nothing", sinkFar.frames.empty());

  // A repeater somebody switched off.
  b.setPowered(false);
  a.send(view(frame));
  a.tick(200);
  that("an unpowered node receives nothing", sinkB.frames.size() == 1);

  b.setPowered(true);
  a.send(view(frame));
  a.tick(300);
  that("and hears again once powered", sinkB.frames.size() == 2);

  // The budget stops the queue rather than breaking the rules.
  radio::Params tight = params;
  tight.dutyCyclePercent = 10;
  tight.dutyWindowMs = 2000; // 200 ms of airtime, under one frame at SF11
  radio::VirtualRadio limited(medium, tight);
  Collecting sinkLimited;
  medium.link(3, 1);
  limited.send(view(frame));
  limited.tick(0);
  const size_t afterFirst = sinkB.frames.size();
  for (int i = 0; i < 5; i++) {
    limited.send(view(frame));
    limited.tick((radio::Millis)(i + 1));
  }
  that("a spent budget holds frames back", sinkB.frames.size() < afterFirst + 5);
  that("and canTransmitNow says so", !limited.canTransmitNow());
}

static void testReplayRadio()
{
  section("radio: replay radio");

  std::vector<radio::ReplayRadio::Capture> captures;
  for (int i = 0; i < 3; i++) {
    radio::ReplayRadio::Capture capture;
    capture.at = (radio::Millis)(100 * (i + 1));
    capture.frame = { (uint8_t)(0x10 + i), 0x00, 0x42 };
    capture.meta.rssi = (int16_t)(-40 - i);
    captures.push_back(capture);
  }

  radio::ReplayRadio replay(captures);
  Collecting sink;
  replay.setSink(&sink);

  replay.tick(50);
  that("nothing before its time", sink.frames.empty());

  replay.tick(100);
  that("the first frame replays on schedule", sink.frames.size() == 1);
  that("captured metadata comes with it", sink.metas.size() == 1 && sink.metas[0].rssi == -40);

  replay.tick(250);
  that("catching up delivers everything due", sink.frames.size() == 2);
  that("not yet finished", !replay.finished());

  replay.tick(1000);
  that("the whole dump plays out", sink.frames.size() == 3 && replay.finished());

  std::vector<uint8_t> outgoing { 9, 9 };
  that("what the stack sends is captured, not aired",
    replay.send(view(outgoing)) && replay.transmitted().size() == 1 && replay.transmitted()[0] == outgoing);
}

static void testUdpRadio()
{
  section("radio: udp transport");

  // Two nodes on the loopback, each listing the other as a peer. The peer
  // lists are the topology.
  radio::Params params;
  params.dutyCyclePercent = 100;

  radio::UdpOptions left;
  left.listenPort = 45501;
  radio::UdpOptions right;
  right.listenPort = 45502;
  left.peers = { "127.0.0.1:45502" };
  right.peers = { "127.0.0.1:45501" };

  radio::UdpRadio a(left, params);
  radio::UdpRadio b(right, params);
  that("both sockets open", a.open() && b.open());
  that("bound where asked", a.boundPort() == 45501 && b.boundPort() == 45502);

  Collecting sinkA, sinkB;
  a.setSink(&sinkA);
  b.setSink(&sinkB);

  std::vector<uint8_t> frame { 0x11, 0x00, 1, 2, 3 };
  that("send only queues", a.send(view(frame)) && sinkB.frames.empty());

  a.tick(100);
  b.tick(110);
  that("the peer received it", sinkB.frames.size() == 1);
  that("frame arrives intact", sinkB.frames.size() == 1 && sinkB.frames[0] == frame);
  that("airtime is reported", sinkB.metas.size() == 1 && sinkB.metas[0].airtimeUs == b.airtimeUs(frame.size()));

  // The sender id exists exactly to stop this.
  a.tick(120);
  that("the sender does not hear itself", sinkA.frames.empty());

  std::vector<uint8_t> reply { 0x15, 0x00, 9 };
  b.send(view(reply));
  b.tick(200);
  a.tick(210);
  that("the reverse direction works", sinkA.frames.size() == 1 && sinkA.frames[0] == reply);

  // A node nobody lists cannot be heard by anyone.
  radio::UdpOptions lonely;
  lonely.listenPort = 45503;
  radio::UdpRadio outsider(lonely, params);
  Collecting sinkOutsider;
  outsider.setSink(&sinkOutsider);
  that("an unlisted node opens", outsider.open());

  a.send(view(frame));
  a.tick(300);
  outsider.tick(310);
  that("and hears nothing", sinkOutsider.frames.empty());

  radio::UdpOptions broken;
  broken.listenPort = 45504;
  broken.peers = { "not-an-address" };
  radio::UdpRadio invalid(broken, params);
  that("a malformed peer is refused", !invalid.open());
}

int main()
{
  testAirtime();
  testDutyCycle();
  testTxQueue();
  testVirtualRadio();
  testReplayRadio();
  testUdpRadio();
  return check::report();
}
