// Copyright (c) 2026 Vladimir Egorov
// This project is licensed under the MIT License.
// See the LICENSE file in the root of the repository for the full license text.

#pragma once

#include "core.h"
#include "platform.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

// The only module that knows about SPI, GPIO and physics. Everything above it
// sees five methods, and only two of them get used often.
namespace radio {

using platform::Millis;

struct RxMeta {
  int16_t rssi = 0;
  int16_t snr = 0;
  Millis at = 0;
  uint32_t airtimeUs = 0;
};

struct RxSink {
  virtual ~RxSink() = default;
  virtual void onFrame(ByteView frame, const RxMeta& meta) = 0;
};

// Three levels, and the third is the point: acks and returned paths ahead of
// everything, our own traffic next, and other people's packets last. A node
// that repeats at the same priority as it answers spends its air on strangers
// while its own clients wait.
enum class Priority {
  HIGH = 0,
  NORMAL = 1,
  LOW = 2
};

struct IRadio {
  virtual ~IRadio() = default;

  // Into the queue, not into the air: the real transmission is asynchronous.
  // False means the queue overflowed, never "it did not go out".
  virtual bool send(ByteView frame, Priority priority = Priority::NORMAL) = 0;

  virtual void tick(Millis now) = 0;
  virtual void setSink(RxSink* sink) = 0;

  // The one physical quantity that leaks upward: routing sizes its forwarding
  // delay from it.
  virtual uint32_t airtimeUs(size_t length) const = 0;

  virtual bool canTransmitNow() const = 0;
};

// These must match the network bit for bit. One wrong field and the node hears
// nobody, with no error anywhere.
struct Params {
  uint32_t frequencyHz = 869525000;
  uint8_t spreadingFactor = 11;
  uint32_t bandwidthHz = 62500; // MeshCore runs narrow
  uint8_t codingRate = 5; // 4/5
  uint8_t preambleSymbols = 16;
  uint8_t syncWord = 0x12;

  uint8_t dutyCyclePercent = 10; // 10% per sliding hour on 868 MHz in Europe
  Millis dutyWindowMs = 3600000; // the sliding hour; other regions differ
  size_t queueDepth = 16;
};

// LoRa airtime per the SX1276/SX1262 datasheet formula.
uint32_t airtimeUsFor(const Params& params, size_t payloadLength);

// A sliding hour of "when and for how long", so the budget is spent on measured
// airtime rather than on a guess. Exceeding it is a regulatory breach, not a
// little extra traffic.
class DutyCycle {
public:
  DutyCycle(uint8_t percent, Millis window = 3600000);

  void record(Millis at, uint32_t airtimeUs);
  bool allows(Millis now, uint32_t airtimeUs) const;
  uint32_t usedPermille(Millis now) const;

private:
  void expire(Millis now) const;

  struct Slot {
    Millis at = 0;
    uint32_t airtimeUs = 0;
  };

  uint8_t percent_;
  Millis window_;
  mutable std::deque<Slot> slots_;
  mutable uint64_t usedUs_ = 0;
};

// A priority queue in front of a half-duplex radio: while transmitting it hears
// nothing, so the queue is needed even when traffic looks light.
class TxQueue {
public:
  explicit TxQueue(size_t depth);

  bool push(ByteView frame, Priority priority);

  // The priority comes back with the frame, for the one caller that has to put
  // it back: a transmission the duty cycle refused belongs where it was, not at
  // the head of the queue.
  bool pop(std::vector<uint8_t>& out, Priority* priority = nullptr);
  size_t size() const
  {
    return entries_.size();
  }

private:
  struct Entry {
    Priority priority = Priority::NORMAL;
    uint64_t sequence = 0;
    std::vector<uint8_t> frame;
  };

  size_t depth_;
  uint64_t sequence_ = 0;
  std::vector<Entry> entries_;
};

// Shared air for tests: one process, several nodes, a matrix of who hears whom.
// Airtime is computed with the same formula but never actually spent.
class Medium {
public:
  size_t attach(class VirtualRadio* node);
  void link(size_t a, size_t b);
  void unlink(size_t a, size_t b);
  bool hears(size_t listener, size_t speaker) const;
  void broadcast(size_t from, ByteView frame, const RxMeta& meta);

private:
  std::vector<VirtualRadio*> nodes_;
  std::vector<std::vector<bool>> visible_;
};

class VirtualRadio : public IRadio {
public:
  VirtualRadio(Medium& medium, Params params = {});

  bool send(ByteView frame, Priority priority = Priority::NORMAL) override;
  void tick(Millis now) override;
  void setSink(RxSink* sink) override
  {
    sink_ = sink;
  }
  uint32_t airtimeUs(size_t length) const override;
  bool canTransmitNow() const override;

  void deliver(ByteView frame, const RxMeta& meta);

  // Off the air entirely: a repeater somebody switched off.
  void setPowered(bool powered)
  {
    powered_ = powered;
  }
  size_t queueDepth() const
  {
    return queue_.size();
  }
  uint32_t usedPermille() const
  {
    return duty_.usedPermille(now_);
  }

private:
  Medium& medium_;
  Params params_;
  DutyCycle duty_;
  TxQueue queue_;
  RxSink* sink_ = nullptr;
  size_t index_ = 0;
  Millis now_ = 0;
  bool powered_ = true;
};

// Several processes on one machine, or several machines on a LAN. Each node
// lists the peers it can hear, so the peer lists are the visibility matrix:
// a chain A-R-C is three configs, not a special mode.
struct UdpOptions {
  std::string bindAddress = "127.0.0.1";
  uint16_t listenPort = 0; // 0 lets the kernel choose

  // "host:port" each. Where this node's transmissions go, and therefore who
  // hears it.
  std::vector<std::string> peers;

  // Optional. Joined for receiving and added to the peers for sending, so a
  // LAN needs no peer list at all — where multicast is available.
  std::string multicastGroup;
  uint16_t multicastPort = 4242;
};

// UDP transport for the test bench. Datagrams carry an 8-byte sender id ahead
// of the frame: with multicast loopback, and with a peer list that includes
// ourselves, we would otherwise receive our own transmissions. That header
// belongs to this transport and never reaches the protocol.
class UdpRadio : public IRadio {
public:
  UdpRadio(UdpOptions options, Params params = {});
  ~UdpRadio() override;

  // Opens the socket. False leaves the reason in the log.
  bool open();
  uint16_t boundPort() const
  {
    return boundPort_;
  }

  bool send(ByteView frame, Priority priority = Priority::NORMAL) override;
  void tick(Millis now) override;
  void setSink(RxSink* sink) override
  {
    sink_ = sink;
  }
  uint32_t airtimeUs(size_t length) const override;
  bool canTransmitNow() const override;

  size_t queueDepth() const
  {
    return queue_.size();
  }
  uint32_t usedPermille() const
  {
    return duty_.usedPermille(now_);
  }

private:
  void drainSocket();
  void transmit(ByteView frame);

  UdpOptions options_;
  Params params_;
  DutyCycle duty_;
  TxQueue queue_;
  RxSink* sink_ = nullptr;

  // Kept as plain numbers so no socket header leaks into this interface.
  struct Endpoint {
    uint32_t address = 0; // network byte order
    uint16_t port = 0;
  };

  int socket_ = -1;
  uint16_t boundPort_ = 0;
  uint64_t selfId_ = 0;
  Millis now_ = 0;
  std::vector<Endpoint> destinations_;
};

// Replays a captured dump with its original timing. What you reach for when a
// bug happened on the real network and has to happen again.
class ReplayRadio : public IRadio {
public:
  struct Capture {
    Millis at = 0;
    std::vector<uint8_t> frame;
    RxMeta meta;
  };

  explicit ReplayRadio(std::vector<Capture> captures, Params params = {});

  bool send(ByteView frame, Priority priority = Priority::NORMAL) override;
  void tick(Millis now) override;
  void setSink(RxSink* sink) override
  {
    sink_ = sink;
  }
  uint32_t airtimeUs(size_t length) const override;
  bool canTransmitNow() const override
  {
    return true;
  }

  const std::vector<std::vector<uint8_t>>& transmitted() const
  {
    return transmitted_;
  }
  bool finished() const
  {
    return next_ >= captures_.size();
  }

private:
  std::vector<Capture> captures_;
  Params params_;
  RxSink* sink_ = nullptr;
  size_t next_ = 0;
  std::vector<std::vector<uint8_t>> transmitted_;
};

} // namespace radio
