// Copyright (c) 2026 Vladimir Egorov
// This project is licensed under the MIT License.
// See the LICENSE file in the root of the repository for the full license text.

// Tests for src/routing.
//
// The radio interface is what makes this possible: several nodes run in one
// process over a shared in-memory queue plus a "who hears whom" matrix, with a
// clock the test moves by hand. The random source is scripted too, so the
// backoff spread is reproducible.

#include "check.h"
#include "routing.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

using check::section;
using check::that;

namespace core = crypto::core;

// ------------------------------------------------------------------ scripted RNG

// A counter, not a constant: two repeaters must draw different values or the
// spread they are meant to provide would not exist.
static uint32_t rngCounter = 0;
static void scriptedRandom(ByteSpan out)
{
  for (size_t i = 0; i < out.size(); i++) {
    rngCounter = rngCounter * 1103515245u + 12345u;
    out[i] = (uint8_t)(rngCounter >> 16);
  }
}

// ------------------------------------------------------------------ harness

struct Sent {
  routing::Millis at = 0;
  routing::Priority priority = routing::Priority::NORMAL;
  std::vector<uint8_t> frame;
};

class Network;

class TestRadio : public routing::Radio {
public:
  TestRadio(Network& network, size_t index)
    : network_(network),
      index_(index)
  {
  }
  void enqueue(ByteView frame, routing::Priority priority) override;

  std::vector<Sent> outbox;

private:
  Network& network_;
  size_t index_;
};

struct Received {
  packet::PayloadType type = packet::PayloadType::REQ;
  std::vector<uint8_t> plain;
  core::PublicKey from;
};

class TestDelegate : public routing::Delegate {
public:
  void onPayload(const identity::Contact& from, packet::PayloadType type, ByteView plain) override
  {
    Received item;
    item.type = type;
    item.from = from.pk;
    item.plain.assign(plain.begin(), plain.end());
    payloads.push_back(std::move(item));
  }
  void onAnon(const core::PublicKey& from, ByteView plain) override
  {
    anonFrom = from;
    anonPlain.assign(plain.begin(), plain.end());
    anonCount++;
  }
  void onAck(routing::SendId id) override
  {
    acked.push_back(id);
  }
  void onDeliveryFailed(routing::SendId id) override
  {
    failed.push_back(id);
  }
  bool shouldForward(const packet::Packet& p) override
  {
    (void)p;
    return forwarding;
  }

  std::vector<Received> payloads;
  std::vector<routing::SendId> acked;
  std::vector<routing::SendId> failed;
  core::PublicKey anonFrom;
  std::string anonPlain;
  int anonCount = 0;
  bool forwarding = true;
};

struct Node {
  std::string dir;
  identity::Store store;
  TestDelegate delegate;
  std::unique_ptr<TestRadio> radio;
  std::unique_ptr<routing::Router> router;
  bool alive = true;
};

class Network {
public:
  ~Network()
  {
    for (auto& node : nodes_) {
      for (const char* name : { "identity.key", "contacts.dat", "contacts.dat.tmp" }) {
        unlink((node->dir + "/" + name).c_str());
      }
      rmdir(node->dir.c_str());
    }
  }

  size_t add(routing::Config config = {})
  {
    char templ[] = "meshcore_routing_XXXXXX";
    const char* dir = mkdtemp(templ);

    auto node = std::make_unique<Node>();
    node->dir = dir ? dir : "";
    node->store.loadOrCreate(node->dir);
    node->radio = std::make_unique<TestRadio>(*this, nodes_.size());
    node->router = std::make_unique<routing::Router>(node->store, *node->radio, node->delegate, config);
    nodes_.push_back(std::move(node));

    visible_.resize(nodes_.size());
    for (auto& row : visible_)
      row.resize(nodes_.size(), false);
    return nodes_.size() - 1;
  }

  void link(size_t a, size_t b)
  {
    visible_[a][b] = true;
    visible_[b][a] = true;
  }

  Node& node(size_t i)
  {
    return *nodes_[i];
  }
  routing::Millis now() const
  {
    return now_;
  }

  // Everyone learns everyone else's key, the way a round of adverts would.
  void introduceAll()
  {
    for (size_t i = 0; i < nodes_.size(); i++) {
      for (size_t j = 0; j < nodes_.size(); j++) {
        if (i == j) continue;
        packet::Advert advert;
        advert.publicKey = nodes_[j]->store.selfPk().view();
        advert.timestamp = 100;
        advert.appdata = ByteView {};
        nodes_[i]->store.remember(advert);
      }
    }
  }

  // One step: move the clock, let every router act, then deliver whatever the
  // radios accepted to the nodes that can hear them.
  void step(routing::Millis to)
  {
    now_ = to;
    for (auto& node : nodes_) {
      if (node->alive) node->router->tick(now_);
    }

    std::vector<std::pair<size_t, Sent>> inFlight;
    for (size_t i = 0; i < nodes_.size(); i++) {
      for (Sent& sent : nodes_[i]->radio->outbox)
        inFlight.push_back({ i, sent });
      nodes_[i]->radio->outbox.clear();
    }

    for (auto& [source, sent] : inFlight) {
      transmitted.push_back({ source, sent });
      for (size_t target = 0; target < nodes_.size(); target++) {
        if (target == source || !visible_[source][target] || !nodes_[target]->alive) continue;
        routing::RxMeta meta;
        meta.airtime = 50;
        nodes_[target]->router->onFrame(ByteView { sent.frame.data(), sent.frame.size() }, meta);
      }
    }
  }

  void run(routing::Millis until, routing::Millis stepSize = 100)
  {
    for (routing::Millis t = now_ + stepSize; t <= until; t += stepSize)
      step(t);
  }

  std::vector<std::pair<size_t, Sent>> transmitted;

private:
  std::vector<std::unique_ptr<Node>> nodes_;
  std::vector<std::vector<bool>> visible_;
  routing::Millis now_ = 0;
};

void TestRadio::enqueue(ByteView frame, routing::Priority priority)
{
  Sent sent;
  sent.at = network_.now();
  sent.priority = priority;
  sent.frame.assign(frame.begin(), frame.end());
  outbox.push_back(std::move(sent));
}

static std::vector<uint8_t> textPayload(const char* body)
{
  std::vector<uint8_t> out(TEXT_MSG_PREFIX_SIZE);
  out[4] = 0;
  out.insert(out.end(), body, body + std::strlen(body));
  return out;
}

// ------------------------------------------------------------------ tests

static void testReversePath()
{
  section("routing: path reversal");

  // The trap: a path is recorded from the sender towards us, and sending back
  // needs it the other way round. Getting this wrong survives the first flood
  // and only breaks direct packets afterwards.
  std::vector<uint8_t> forward { 0x11, 0x22, 0x33 };
  std::vector<uint8_t> out(MAX_PATH_SIZE);
  const size_t length =
    routing::Router::reversePath(ByteView { forward.data(), forward.size() }, ByteSpan { out.data(), out.size() });

  that("length preserved", length == 3);
  that("order flipped", out[0] == 0x33 && out[1] == 0x22 && out[2] == 0x11);

  std::vector<uint8_t> single { 0x42 };
  that("single hop is its own reverse",
    routing::Router::reversePath(ByteView { single.data(), 1 }, ByteSpan { out.data(), out.size() }) == 1
      && out[0] == 0x42);

  that("empty path stays empty", routing::Router::reversePath(ByteView {}, ByteSpan { out.data(), out.size() }) == 0);

  std::vector<uint8_t> tooSmall(2);
  that("refuses a short buffer",
    routing::Router::reversePath(
      ByteView { forward.data(), forward.size() }, ByteSpan { tooSmall.data(), tooSmall.size() })
      == 0);
}

static void testFloodReachesFarNode()
{
  section("routing: flood across two repeaters");

  Network net;
  const size_t a = net.add(), r1 = net.add(), r2 = net.add(), c = net.add();
  net.link(a, r1);
  net.link(r1, r2);
  net.link(r2, c);
  net.introduceAll();

  const auto payload = textPayload("hello");
  net.node(a).router->sendDirect(
    net.node(c).store.selfPk(), packet::PayloadType::TXT_MSG, ByteView { payload.data(), payload.size() }, true);
  net.run(3000);

  that("far node got the message", net.node(c).delegate.payloads.size() == 1);
  that("payload intact",
    net.node(c).delegate.payloads.size() == 1
      && std::memcmp(net.node(c).delegate.payloads[0].plain.data() + TEXT_MSG_PREFIX_SIZE, "hello", 5) == 0);
  that("nobody between delivered it upward",
    net.node(r1).delegate.payloads.empty() && net.node(r2).delegate.payloads.empty());
  that("sender saw the ack", net.node(a).delegate.acked.size() == 1);
  that("no delivery failure", net.node(a).delegate.failed.empty());
}

static void testDuplicateDropped()
{
  section("routing: duplicate suppression");

  Network net;
  const size_t a = net.add(), b = net.add();
  net.link(a, b);
  net.introduceAll();

  const auto payload = textPayload("once");
  net.node(a).router->sendDirect(
    net.node(b).store.selfPk(), packet::PayloadType::TXT_MSG, ByteView { payload.data(), payload.size() }, false);
  net.step(100);

  // Replay the very same frame at b.
  const auto& first = net.transmitted.front().second.frame;
  routing::RxMeta meta;
  meta.airtime = 50;
  const size_t before = net.node(b).delegate.payloads.size();
  net.node(b).router->onFrame(ByteView { first.data(), first.size() }, meta);

  that("delivered exactly once", before == 1 && net.node(b).delegate.payloads.size() == 1);
}

static void testSwitchesToDirect()
{
  section("routing: flood then direct");

  Network net;
  const size_t a = net.add(), r = net.add(), c = net.add();
  net.link(a, r);
  net.link(r, c);
  net.introduceAll();

  const auto payload = textPayload("first");
  net.node(a).router->sendDirect(
    net.node(c).store.selfPk(), packet::PayloadType::TXT_MSG, ByteView { payload.data(), payload.size() }, true);
  net.run(3000);

  that("first exchange arrived", net.node(c).delegate.payloads.size() == 1);
  that("receiver learned the way back", net.node(c).router->hasRoute(net.node(a).store.selfPk()));
  that("sender learned a route from the returned path", net.node(a).router->hasRoute(net.node(c).store.selfPk()));

  net.transmitted.clear();
  const auto second = textPayload("second");
  net.node(a).router->sendDirect(
    net.node(c).store.selfPk(), packet::PayloadType::TXT_MSG, ByteView { second.data(), second.size() }, true);
  net.run(6000);

  bool sentDirect = false;
  for (const auto& [source, sent] : net.transmitted) {
    if (source != a) continue;
    auto parsed = packet::parse(ByteView { sent.frame.data(), sent.frame.size() });
    if (parsed && parsed->routeType() == packet::RouteType::DIRECT) sentDirect = true;
  }
  that("second message went direct", sentDirect);
  that("second message arrived", net.node(c).delegate.payloads.size() == 2);
}

static void testRepeaterLossResetsRoute()
{
  section("routing: a dead repeater resets the route");

  Network net;
  const size_t a = net.add(), r = net.add(), c = net.add();
  net.link(a, r);
  net.link(r, c);
  net.introduceAll();

  const auto payload = textPayload("first");
  net.node(a).router->sendDirect(
    net.node(c).store.selfPk(), packet::PayloadType::TXT_MSG, ByteView { payload.data(), payload.size() }, true);
  net.run(3000);
  that("route established", net.node(a).router->hasRoute(net.node(c).store.selfPk()));

  // The repeater in the middle is switched off.
  net.node(r).alive = false;

  const auto second = textPayload("second");
  net.node(a).router->sendDirect(
    net.node(c).store.selfPk(), packet::PayloadType::TXT_MSG, ByteView { second.data(), second.size() }, true);
  net.run(60000, 500);

  that("delivery reported as failed", net.node(a).delegate.failed.size() == 1);
  that("route forgotten", !net.node(a).router->hasRoute(net.node(c).store.selfPk()));
  that("nothing new reached the far node", net.node(c).delegate.payloads.size() == 1);
}

static void testRepeatersDoNotCollide()
{
  section("routing: two repeaters spread their retransmissions");

  Network net;
  const size_t a = net.add(), r1 = net.add(), r2 = net.add(), c = net.add();
  net.link(a, r1);
  net.link(a, r2);
  net.link(r1, c);
  net.link(r2, c);
  net.introduceAll();

  const auto payload = textPayload("spread");
  net.node(a).router->sendDirect(
    net.node(c).store.selfPk(), packet::PayloadType::TXT_MSG, ByteView { payload.data(), payload.size() }, false);
  net.run(3000, 10);

  routing::Millis firstAt = 0, secondAt = 0;
  bool sawFirst = false, sawSecond = false;
  for (const auto& [source, sent] : net.transmitted) {
    if (source == r1 && !sawFirst) {
      firstAt = sent.at;
      sawFirst = true;
    }
    if (source == r2 && !sawSecond) {
      secondAt = sent.at;
      sawSecond = true;
    }
  }

  that("both repeaters forwarded", sawFirst && sawSecond);
  that("they did not transmit in the same slot", sawFirst && sawSecond && firstAt != secondAt);
  that("the message still arrived", net.node(c).delegate.payloads.size() == 1);
}

static void testEndpointStillForwards()
{
  section("routing: an endpoint is also a repeater");

  // The classic mistake is writing steps 4 and 5 as if/else. A room server that
  // is the addressee must still pass the flood on, or the nodes behind it lose
  // messages now and then.
  Network net;
  const size_t a = net.add(), room = net.add(), behind = net.add();
  net.link(a, room);
  net.link(room, behind);
  net.introduceAll();

  const auto payload = textPayload("post");
  net.node(a).router->sendDirect(
    net.node(room).store.selfPk(), packet::PayloadType::TXT_MSG, ByteView { payload.data(), payload.size() }, false);
  net.run(3000);

  that("the addressee delivered it upward", net.node(room).delegate.payloads.size() == 1);

  bool forwarded = false;
  for (const auto& [source, sent] : net.transmitted) {
    if (source != room) continue;
    auto parsed = packet::parse(ByteView { sent.frame.data(), sent.frame.size() });
    if (parsed && parsed->payloadType() == packet::PayloadType::TXT_MSG) forwarded = true;
  }
  that("and forwarded it anyway", forwarded);
}

// Builds an ANON_REQ payload: the sender is not a contact, so its key rides
// inside the packet.
static std::vector<uint8_t> anonPayload(const core::PublicKey& from,
  const core::PublicKey& to,
  const core::PrivateKey& fromSk,
  const char* body)
{
  auto secret = core::deriveShared(fromSk, to);
  std::vector<uint8_t> plain(body, body + std::strlen(body));
  std::vector<uint8_t> cipher(MAX_PACKET_PAYLOAD);
  auto sealed =
    crypto::protocol::seal(*secret, ByteView { plain.data(), plain.size() }, ByteSpan { cipher.data(), cipher.size() });

  std::vector<uint8_t> out;
  out.reserve(ANON_REQ_PREFIX_SIZE + sealed->ciphertextLength);
  out.push_back(to.data[0]);
  out.insert(out.end(), from.data.begin(), from.data.end());
  out.insert(out.end(), sealed->mac.data.begin(), sealed->mac.data.end());
  out.insert(out.end(), cipher.begin(), cipher.begin() + (long)sealed->ciphertextLength);
  return out;
}

static void testAnonRequest()
{
  section("routing: anonymous request");

  Network net;
  const size_t stranger = net.add(), r = net.add(), room = net.add();
  net.link(stranger, r);
  net.link(r, room);
  // Deliberately no introduceAll: a login arrives from somebody unknown.

  const auto payload = anonPayload(
    net.node(stranger).store.selfPk(), net.node(room).store.selfPk(), net.node(stranger).store.selfSk(), "login");
  net.node(stranger).router->sendFlood(packet::PayloadType::ANON_REQ, ByteView { payload.data(), payload.size() });
  net.run(3000);

  that("the room saw the login", net.node(room).delegate.anonCount == 1);
  that("it did not arrive as a normal payload", net.node(room).delegate.payloads.empty());
  that("the plaintext was decrypted with the key from the packet", net.node(room).delegate.anonPlain == "login");
  that("the sender is reported by raw key",
    std::memcmp(net.node(room).delegate.anonFrom.data.data(), net.node(stranger).store.selfPk().data.data(),
      PACKET_PUBLIC_KEY_SIZE)
      == 0);
  that("a route back was kept for the response", net.node(room).router->hasRoute(net.node(stranger).store.selfPk()));
  that("the repeater passed it on but read nothing",
    net.node(r).delegate.anonCount == 0 && net.node(r).delegate.payloads.empty());

  // Addressed to somebody else: the room must not even try.
  Network other;
  const size_t sender2 = other.add(), room2 = other.add(), bystander = other.add();
  other.link(sender2, room2);
  other.link(sender2, bystander);
  const auto elsewhere = anonPayload(other.node(sender2).store.selfPk(), other.node(room2).store.selfPk(),
    other.node(sender2).store.selfSk(), "not yours");
  other.node(sender2).router->sendFlood(packet::PayloadType::ANON_REQ, ByteView { elsewhere.data(), elsewhere.size() });
  other.run(2000);
  that("a bystander ignores it", other.node(bystander).delegate.anonCount == 0);
}

static void testForwardingPolicy()
{
  section("routing: shouldForward is the only policy hook");

  Network net;
  const size_t a = net.add(), r = net.add(), c = net.add();
  net.link(a, r);
  net.link(r, c);
  net.introduceAll();
  net.node(r).delegate.forwarding = false;

  const auto payload = textPayload("blocked");
  net.node(a).router->sendDirect(
    net.node(c).store.selfPk(), packet::PayloadType::TXT_MSG, ByteView { payload.data(), payload.size() }, false);
  net.run(3000);

  that("a refusing repeater stops the packet", net.node(c).delegate.payloads.empty());
}

int main()
{
  if (!core::init(scriptedRandom)) {
    std::printf("backend init failed\n");
    return 1;
  }

  testReversePath();
  testFloodReachesFarNode();
  testDuplicateDropped();
  testSwitchesToDirect();
  testRepeaterLossResetsRoute();
  testRepeatersDoNotCollide();
  testEndpointStillForwards();
  testAnonRequest();
  testForwardingPolicy();

  return check::report();
}
