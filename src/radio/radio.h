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

// The chip itself, over SPI. Compiled only where lgpio is — a Raspberry Pi and
// its kin — so the declaration sits behind the macro the build sets: a node
// built on a laptop has the other three drivers and not this one.
#ifdef SX1262_RADIO

// Pin numbers are BCM, the numbering lgpio and every HAT datasheet use. The
// defaults are the Waveshare SX1262 XXXM LoRaWAN/GNSS HAT, taken from its own
// driver rather than guessed. Nothing here can be probed, so a different board
// means reading its schematic and setting every one of them.
struct Sx1262Options {
  uint8_t spiBus = 0; // SPI0 is /dev/spidev0.*
  uint8_t spiChipSelect = 0; // the CE line within it: /dev/spidev0.0
  uint32_t spiSpeedHz = 2000000; // the chip takes 18 MHz; the ribbon is the limit
  uint8_t gpioChip = 0; // gpiochip0 through the Pi 4; the Pi 5 moved it to 4

  // NSS is a plain GPIO on this HAT rather than one of the SPI controller's CE
  // lines, so the driver drives it itself. -1 remains legal and still means
  // "the SPI controller owns chip select", for a board that does wire NSS to
  // CE0 or CE1; on this one it leaves the chip permanently unselected, every
  // register reads back as nothing, and begin() reports error -2.
  int32_t pinNss = 21;
  int32_t pinBusy = 20;
  int32_t pinReset = 18;
  int32_t pinDio1 = 16;

  // The RF switch, and the one place where the silkscreen lies. On this HAT the
  // pin labelled TXEN is high while *receiving*: its own driver pulls BCM 6 low
  // to transmit and high to listen. So it belongs in the receive slot, and the
  // transmit side is DIO2's job below. Backwards, the node hears nothing and
  // the PA talks into a switch pointed the wrong way.
  int32_t pinRxEnable = 6;
  int32_t pinTxEnable = -1;

  // The other half of that switch: RXEN is soldered straight to DIO2, which the
  // chip raises for exactly the length of a transmission.
  bool dio2AsRfSwitch = true;

  // The setting that fails silently and completely. A board with a TCXO needs
  // its voltage here; this one has a plain 32 MHz crystal and needs 0, and the
  // wrong answer means the chip never leaves reset.
  float tcxoVoltage = 0.0f;
  bool useRegulatorLdo = false;

  int8_t txPowerDbm = 22; // the SX1262's ceiling, and what the HAT is rated for
  float currentLimitMa = 140.0f; // what 22 dBm draws; RadioLib would leave it at 60
};

// Half duplex for real, not as an approximation: while a frame is going out the
// receiver is deaf, so the queue in front of it is the only thing keeping the
// node's own replies ahead of other people's packets.
//
// Driven entirely from tick(): the IRQ line is read over SPI rather than
// through a GPIO interrupt, because the node's loop already comes round every
// few milliseconds and a callback would arrive on lgpio's thread, inside a
// driver that owns no lock.
class Sx1262Radio : public IRadio {
public:
  Sx1262Radio(Sx1262Options options, Params params = {});
  ~Sx1262Radio() override;

  // Resets the chip, configures it, and leaves it listening. False leaves the
  // reason in the log; a radio that will not start has no useful degraded mode.
  bool open();

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
  bool startListening();
  void collect();
  void startTransmit(Millis now);
  void finishTransmit();

  Sx1262Options options_;
  Params params_;
  DutyCycle duty_;
  TxQueue queue_;
  RxSink* sink_ = nullptr;
  Millis now_ = 0;

  // Opaque so that no RadioLib header reaches this one. Everything above the
  // module includes radio.h, and behind RadioLib come a HAL and lgpio.
  struct Device;
  std::unique_ptr<Device> device_;

  // A transmission is in flight from the moment the frame is handed to the
  // chip until TX_DONE comes back. The deadline is the escape hatch: a chip
  // that stops answering must not take the node off the air for good.
  bool transmitting_ = false;
  Millis transmitDeadline_ = 0;
};

#endif // SX1262_RADIO

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
