// Copyright (c) 2026 Vladimir Egorov
// This project is licensed under the MIT License.
// See the LICENSE file in the root of the repository for the full license text.

// Tests for src/repeater.
//
// The property that matters: a decision about a packet, taken from the packet
// alone. No radio, no clock, no keys — time and load are arguments, so a minute
// of rate limiting passes in one line and an exhausted duty cycle needs no
// duty cycle.

#include "check.h"
#include "repeater.h"

using check::section;
using check::that;

// A packet as it arrives, with a path already on it: hops is how many nodes
// have written their hash so far, and the last one is who we heard it from.
static packet::Packet flood(uint8_t hops, uint8_t lastHop = 0x11)
{
  packet::Packet p;
  p.header = (uint8_t)(((uint8_t)packet::PayloadType::TXT_MSG << 2) | (uint8_t)packet::RouteType::FLOOD);
  p.hashSize = NODE_HASH_SIZE;
  p.hopCount = hops;
  for (uint8_t i = 0; i < hops; i++)
    p.path[i] = (uint8_t)(0x80 + i);
  if (hops > 0) p.path[hops - 1] = lastHop;
  p.payloadSize = 8;
  return p;
}

static packet::Packet direct(uint8_t hops)
{
  packet::Packet p = flood(hops);
  p.header = (uint8_t)(((uint8_t)packet::PayloadType::TXT_MSG << 2) | (uint8_t)packet::RouteType::DIRECT);
  return p;
}

static void testForwardsByDefault()
{
  section("repeater: an empty policy carries everything");

  repeater::Policy policy;
  that("a fresh flood travels on", policy.shouldForward(flood(1), 0, 0));
  that("so does a direct packet", policy.shouldForward(direct(3), 0, 0));
  that("and both are counted", policy.stats().forwarded == 2);
}

static void testSwitchedOff()
{
  section("repeater: switched off");

  repeater::Config config;
  config.enabled = false;
  repeater::Policy policy(config);

  that("nothing travels on", !policy.shouldForward(flood(1), 0, 0));

  // Refusing everything is not the same as dropping: there is no transit here
  // to have refused, and a counter climbing would read as a fault.
  const repeater::Stats& stats = policy.stats();
  that("and nothing is counted as a drop",
    stats.forwarded == 0 && stats.hopLimit == 0 && stats.blocked == 0 && stats.rateLimited == 0 && stats.budget == 0);

  policy.setEnabled(true);
  that("switching it back on carries again", policy.shouldForward(flood(1), 0, 0));
}

static void testHopLimit()
{
  section("repeater: hop limit");

  repeater::Config config;
  config.maxHops = 4;
  repeater::Policy policy(config);

  that("under the limit travels on", policy.shouldForward(flood(3), 0, 0));
  that("at the limit stops", !policy.shouldForward(flood(4), 0, 0));
  that("past it stops too", !policy.shouldForward(flood(9), 0, 0));
  that("counted as a hop limit", policy.stats().hopLimit == 2);

  // A route longer than the network's limit should not exist, and carrying one
  // is how a loop stays alive.
  that("direct is held to the same limit", !policy.shouldForward(direct(6), 0, 0));

  policy.setMaxHops(0);
  that("zero hops is clamped to something usable", policy.maxHops() == 1);
  policy.setMaxHops(200);
  that("and so is more than the format allows", policy.maxHops() == MAX_HOP_COUNT);
}

static void testBlocklist()
{
  section("repeater: blocked hashes");

  repeater::Config config;
  config.blocked = { 0x42 };
  repeater::Policy policy(config);

  that("a stranger travels on", policy.shouldForward(flood(2, 0x41), 0, 0));
  that("the blocked neighbour does not", !policy.shouldForward(flood(2, 0x42), 0, 0));

  // Anywhere in the path, not only the hop we heard: a packet that went through
  // a blocked node is a packet we refused to carry for it.
  packet::Packet earlier = flood(3, 0x41);
  earlier.path[0] = 0x42;
  that("blocked earlier in the path counts", !policy.shouldForward(earlier, 0, 0));
  that("counted as blocked", policy.stats().blocked == 2);
}

static void testRateLimit()
{
  section("repeater: one neighbour cannot spend the whole budget");

  repeater::Config config;
  config.perSourcePerMinute = 3;
  repeater::Policy policy(config);

  for (int i = 0; i < 3; i++)
    that("inside the allowance", policy.shouldForward(flood(1, 0x11), 1000, 0));
  that("and then it waits", !policy.shouldForward(flood(1, 0x11), 1000, 0));

  // Per neighbour, not per node: one loud transmitter must not silence the
  // quiet ones behind it.
  that("a different neighbour is unaffected", policy.shouldForward(flood(1, 0x22), 1000, 0));

  // Direct traffic follows a route somebody already learned and arrives one at
  // a time. It is what this node exists to carry.
  for (int i = 0; i < 10; i++)
    policy.shouldForward(direct(2), 1000, 0);
  that("direct packets are not rate limited", policy.stats().rateLimited == 1);

  that("the window reopens", policy.shouldForward(flood(1, 0x11), 1000 + REPEATER_RATE_WINDOW_MS, 0));
}

static void testDutyCeiling()
{
  section("repeater: transit gives way to our own traffic");

  repeater::Config config;
  config.dutyCeilingPermille = 80;
  repeater::Policy policy(config);

  that("air to spare", policy.shouldForward(flood(1), 0, 79));
  that("at the ceiling transit stops", !policy.shouldForward(flood(1), 0, 80));
  that("counted against the budget", policy.stats().budget == 1);

  config.dutyCeilingPermille = 0;
  repeater::Policy unlimited(config);
  that("zero switches the check off", unlimited.shouldForward(flood(1), 0, 999));
}

static void testStatsCleared()
{
  section("repeater: statistics reset");

  repeater::Policy policy;
  policy.shouldForward(flood(1), 0, 0);
  that("something was counted", policy.stats().forwarded == 1);

  policy.clearStats();
  that("and can be zeroed", policy.stats().forwarded == 0);
}

int main()
{
  testForwardsByDefault();
  testSwitchedOff();
  testHopLimit();
  testBlocklist();
  testRateLimit();
  testDutyCeiling();
  testStatsCleared();
  return check::report();
}
