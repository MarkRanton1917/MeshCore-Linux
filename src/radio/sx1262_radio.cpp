// Copyright (c) 2026 Vladimir Egorov
// This project is licensed under the MIT License.
// See the LICENSE file in the root of the repository for the full license text.

#include "radio.h"

#ifdef SX1262_RADIO

#include <RadioLib.h>
#include <hal/RPi/PiHal.h>

#include <cmath>

using namespace radio;

namespace {

// RadioLib spells "no pin" as an all-ones sentinel; the options spell it -1,
// because a config file that has to say 4294967295 is a config file nobody
// gets right.
uint32_t pinOrNone(int32_t pin)
{
  return pin < 0 ? RADIOLIB_NC : (uint32_t)pin;
}

// Added to the frame's own airtime to decide a transmission has gone missing.
// Long enough that no honest frame trips it, short enough that a chip which has
// stopped answering does not take the node off the air until somebody notices.
constexpr Millis kTransmitGraceMs = 500;

} // namespace

// The three RadioLib objects, declared in the order that destroys them safely:
// the radio points at the module, the module at the HAL, so they go the other
// way round. Kept out of the header because everything above this module
// includes radio.h, and behind RadioLib come a HAL and lgpio.
struct Sx1262Radio::Device {
  PiHal hal;
  Module module;
  SX1262 radio;

  explicit Device(const Sx1262Options& options)
    : hal(options.spiChipSelect, options.spiSpeedHz, options.spiBus, options.gpioChip),
      module(&hal,
        pinOrNone(options.pinNss),
        pinOrNone(options.pinDio1),
        pinOrNone(options.pinReset),
        pinOrNone(options.pinBusy)),
      radio(&module)
  {
  }
};

Sx1262Radio::Sx1262Radio(Sx1262Options options, Params params)
  : options_(options),
    params_(params),
    duty_(params.dutyCyclePercent, params.dutyWindowMs),
    queue_(params.queueDepth)
{
}

Sx1262Radio::~Sx1262Radio()
{
  if (device_ == nullptr) return;

  // A keyed PA is the one failure that would outlive the process.
  device_->radio.standby();
  device_->radio.sleep();
  device_->module.term();
}

bool Sx1262Radio::open()
{
  device_ = std::make_unique<Device>(options_);

  // RadioLib takes MHz and kHz as floats. What float32 rounds away here is far
  // below the chip's own 0.95 Hz frequency step, and further still below what
  // two crystals drift apart between themselves.
  const float frequencyMhz = (float)((double)params_.frequencyHz / 1e6);
  const float bandwidthKhz = (float)((double)params_.bandwidthHz / 1e3);

  int16_t state = device_->radio.begin(frequencyMhz, bandwidthKhz, params_.spreadingFactor, params_.codingRate,
    params_.syncWord, options_.txPowerDbm, params_.preambleSymbols, options_.tcxoVoltage, options_.useRegulatorLdo);
  if (state != RADIOLIB_ERR_NONE) {
    // The two halves are worth telling apart, because they send you to opposite
    // ends of the board. RADIOLIB_ERR_CHIP_NOT_FOUND means the version register
    // read back as nothing at all: the chip is not on the bus, which is wiring
    // and never the oscillator -- begin() looks for the chip before it sets the
    // TCXO, and finds its way to the crystal by itself when the voltage is
    // wrong. Anything else came back from a chip that is answering, and there
    // the TCXO voltage is the first suspect.
    if (state == RADIOLIB_ERR_CHIP_NOT_FOUND) {
      platform::Log::write(platform::LogLevel::ERROR,
        "sx1262: the chip does not answer (%d) -- check NSS (%d), BUSY (%d), RESET (%d), gpiochip%u and "
        "/dev/spidev%u.%u. NSS of -1 is right only where the board wires it to a CE line",
        state, (int)options_.pinNss, (int)options_.pinBusy, (int)options_.pinReset, (unsigned)options_.gpioChip,
        (unsigned)options_.spiBus, (unsigned)options_.spiChipSelect);
    }
    else {
      platform::Log::write(platform::LogLevel::ERROR,
        "sx1262: begin failed (%d) -- the chip answers, so this is a setting: the TCXO voltage above all", state);
    }
    device_.reset();
    return false;
  }

  // begin() turns DIO2 into the switch control on its own. Saying it again is
  // how a board that has no such switch says otherwise.
  state = device_->radio.setDio2AsRfSwitch(options_.dio2AsRfSwitch);
  if (state != RADIOLIB_ERR_NONE) {
    platform::Log::write(platform::LogLevel::ERROR, "sx1262: cannot configure DIO2 (%d)", state);
    device_.reset();
    return false;
  }

  // RadioLib's table drives the receive pin high in receive and the transmit
  // pin high in transmit; which of ours goes where is the board's business, and
  // Sx1262Options is where that argument is had.
  if (options_.pinRxEnable >= 0 || options_.pinTxEnable >= 0) {
    device_->module.setRfSwitchPins(pinOrNone(options_.pinRxEnable), pinOrNone(options_.pinTxEnable));
  }

  // Left alone this is 60 mA, which the PA passes well before 22 dBm, and the
  // chip's answer is to protect itself in the middle of a frame.
  state = device_->radio.setCurrentLimit(options_.currentLimitMa);
  if (state != RADIOLIB_ERR_NONE) {
    platform::Log::write(platform::LogLevel::ERROR, "sx1262: cannot set the current limit (%d)", state);
    device_.reset();
    return false;
  }

  if (!startListening()) {
    device_.reset();
    return false;
  }

  platform::Log::write(platform::LogLevel::INFO, "sx1262: listening on %.3f MHz, SF%u, %.1f kHz, %d dBm",
    (double)frequencyMhz, (unsigned)params_.spreadingFactor, (double)bandwidthKhz, (int)options_.txPowerDbm);
  return true;
}

bool Sx1262Radio::send(ByteView frame, Priority priority)
{
  return queue_.push(frame, priority);
}

uint32_t Sx1262Radio::airtimeUs(size_t length) const
{
  return airtimeUsFor(params_, length);
}

bool Sx1262Radio::canTransmitNow() const
{
  // Half duplex is the second term: mid-transmission there is no air to offer,
  // however much budget is left.
  return device_ != nullptr && !transmitting_ && duty_.allows(now_, airtimeUs(MAX_PACKET_FRAME));
}

bool Sx1262Radio::startListening()
{
  const int16_t state = device_->radio.startReceive();
  if (state != RADIOLIB_ERR_NONE) {
    platform::Log::write(platform::LogLevel::ERROR, "sx1262: cannot enter receive (%d)", state);
    return false;
  }
  return true;
}

void Sx1262Radio::collect()
{
  if (device_->radio.checkIrq(RADIOLIB_IRQ_RX_DONE) != 1) return;

  uint8_t frame[MAX_PACKET_FRAME];
  size_t length = device_->radio.getPacketLength();
  if (length > sizeof frame) length = sizeof frame; // a LoRa frame cannot be longer, but the register can lie

  const int16_t state = device_->radio.readData(frame, length);

  // readData leaves the chip receiving, but only on the path where it
  // succeeded. Re-arming regardless costs two register writes and removes the
  // question of which path we took.
  startListening();

  if (state == RADIOLIB_ERR_CRC_MISMATCH) return; // the air is noisy; this is ordinary
  if (state != RADIOLIB_ERR_NONE) {
    platform::Log::write(platform::LogLevel::WARN, "sx1262: read failed (%d)", state);
    return;
  }
  if (length == 0 || sink_ == nullptr) return;

  RxMeta meta;
  meta.at = now_;
  meta.airtimeUs = airtimeUs(length);
  meta.rssi = (int16_t)std::lround(device_->radio.getRSSI());
  meta.snr = (int16_t)std::lround(device_->radio.getSNR());
  sink_->onFrame(ByteView { frame, length }, meta);
}

void Sx1262Radio::startTransmit(Millis now)
{
  std::vector<uint8_t> frame;
  Priority priority = Priority::NORMAL;
  if (!queue_.pop(frame, &priority)) return;
  if (frame.empty() || frame.size() > MAX_PACKET_FRAME) return; // cannot be a real frame

  const uint32_t airtime = airtimeUs(frame.size());
  if (!duty_.allows(now, airtime)) {
    // Budget spent: hold it rather than break the rules, and at the priority it
    // arrived with -- see the same spot in VirtualRadio.
    queue_.push(ByteView { frame.data(), frame.size() }, priority);
    return;
  }

  const int16_t state = device_->radio.startTransmit(frame.data(), frame.size());
  if (state != RADIOLIB_ERR_NONE) {
    platform::Log::write(platform::LogLevel::ERROR, "sx1262: transmit refused (%d)", state);
    startListening();
    return;
  }

  // Charged when it starts rather than when it ends: a frame the chip never
  // reports finishing still held the air for its whole length.
  duty_.record(now, airtime);
  transmitting_ = true;
  transmitDeadline_ = now + airtime / 1000 + kTransmitGraceMs;
}

void Sx1262Radio::finishTransmit()
{
  device_->radio.finishTransmit();
  transmitting_ = false;
  startListening();
}

void Sx1262Radio::tick(Millis now)
{
  now_ = now;
  if (device_ == nullptr) return;

  if (transmitting_) {
    if (device_->radio.checkIrq(RADIOLIB_IRQ_TX_DONE) == 1) {
      finishTransmit();
    }
    else if (now >= transmitDeadline_) {
      // The chip has stopped answering. Back to receive and carry on: a node
      // waiting forever for a TX_DONE that is not coming has left the network
      // without telling anybody.
      platform::Log::write(platform::LogLevel::WARN, "sx1262: transmission never completed, returning to receive");
      finishTransmit();
    }
    return; // deaf until it lands, so there is nothing else worth doing
  }

  // Receive first: a frame sitting in the buffer would otherwise be lost to the
  // transmission we are about to start.
  collect();
  startTransmit(now);
}

#endif // SX1262_RADIO
