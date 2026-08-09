// Copyright (c) 2026 Vladimir Egorov
// This project is licensed under the MIT License.
// See the LICENSE file in the root of the repository for the full license text.

#include "radio.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace radio;

// ------------------------------------------------------------------ airtime

uint32_t radio::airtimeUsFor(const Params& params, size_t payloadLength)
{
  const double sf = params.spreadingFactor;
  const double symbolUs = (double)(1u << params.spreadingFactor) * 1e6 / params.bandwidthHz;

  // Low data rate optimisation kicks in when a symbol lasts over 16 ms, which
  // at MeshCore's 62.5 kHz happens well before SF12.
  const double lowRateOptimise = symbolUs > 16000.0 ? 1.0 : 0.0;

  const double numerator = 8.0 * (double)payloadLength - 4.0 * sf + 28.0 + 16.0; // CRC on, explicit header
  const double denominator = 4.0 * (sf - 2.0 * lowRateOptimise);
  double payloadSymbols = std::ceil(numerator / denominator) * (double)params.codingRate;
  if (payloadSymbols < 0) payloadSymbols = 0;
  payloadSymbols += 8.0;

  const double preambleUs = ((double)params.preambleSymbols + 4.25) * symbolUs;
  return (uint32_t)(preambleUs + payloadSymbols * symbolUs);
}

// ------------------------------------------------------------------ duty cycle

DutyCycle::DutyCycle(uint8_t percent, Millis window)
  : percent_(percent),
    window_(window)
{
}

void DutyCycle::expire(Millis now) const
{
  while (!slots_.empty() && slots_.front().at + window_ <= now) {
    usedUs_ -= slots_.front().airtimeUs;
    slots_.pop_front();
  }
}

void DutyCycle::record(Millis at, uint32_t airtimeUs)
{
  expire(at);
  slots_.push_back(Slot { at, airtimeUs });
  usedUs_ += airtimeUs;
}

bool DutyCycle::allows(Millis now, uint32_t airtimeUs) const
{
  if (percent_ >= 100) return true;

  expire(now);
  const uint64_t budgetUs = (uint64_t)window_ * 1000 * percent_ / 100;
  return usedUs_ + airtimeUs <= budgetUs;
}

uint32_t DutyCycle::usedPermille(Millis now) const
{
  expire(now);
  const uint64_t windowUs = (uint64_t)window_ * 1000;
  return windowUs == 0 ? 0 : (uint32_t)(usedUs_ * 1000 / windowUs);
}

// ------------------------------------------------------------------ tx queue

TxQueue::TxQueue(size_t depth)
  : depth_(depth)
{
}

bool TxQueue::push(ByteView frame, Priority priority)
{
  if (entries_.size() >= depth_) return false; // overflow, not a send failure

  Entry entry;
  entry.priority = priority;
  entry.sequence = sequence_++;
  entry.frame.assign(frame.begin(), frame.end());
  entries_.push_back(std::move(entry));
  return true;
}

bool TxQueue::pop(std::vector<uint8_t>& out)
{
  if (entries_.empty()) return false;

  // Highest priority first, oldest within a priority.
  size_t best = 0;
  for (size_t i = 1; i < entries_.size(); i++) {
    const Entry& candidate = entries_[i];
    const Entry& current = entries_[best];
    if (candidate.priority < current.priority
      || (candidate.priority == current.priority && candidate.sequence < current.sequence)) {
      best = i;
    }
  }

  out = std::move(entries_[best].frame);
  entries_.erase(entries_.begin() + (long)best);
  return true;
}

// ------------------------------------------------------------------ medium

size_t Medium::attach(VirtualRadio* node)
{
  nodes_.push_back(node);
  visible_.resize(nodes_.size());
  for (auto& row : visible_)
    row.resize(nodes_.size(), false);
  return nodes_.size() - 1;
}

void Medium::link(size_t a, size_t b)
{
  visible_[a][b] = true;
  visible_[b][a] = true;
}

void Medium::unlink(size_t a, size_t b)
{
  visible_[a][b] = false;
  visible_[b][a] = false;
}

bool Medium::hears(size_t listener, size_t speaker) const
{
  return visible_[speaker][listener];
}

void Medium::broadcast(size_t from, ByteView frame, const RxMeta& meta)
{
  for (size_t target = 0; target < nodes_.size(); target++) {
    if (target == from || !visible_[from][target]) continue;
    nodes_[target]->deliver(frame, meta);
  }
}

// ------------------------------------------------------------------ virtual

VirtualRadio::VirtualRadio(Medium& medium, Params params)
  : medium_(medium),
    params_(params),
    duty_(params.dutyCyclePercent, params.dutyWindowMs),
    queue_(params.queueDepth)
{
  index_ = medium_.attach(this);
}

bool VirtualRadio::send(ByteView frame, Priority priority)
{
  return queue_.push(frame, priority);
}

uint32_t VirtualRadio::airtimeUs(size_t length) const
{
  return airtimeUsFor(params_, length);
}

bool VirtualRadio::canTransmitNow() const
{
  return powered_ && duty_.allows(now_, airtimeUs(MAX_PACKET_FRAME));
}

void VirtualRadio::tick(Millis now)
{
  now_ = now;
  if (!powered_) return;

  std::vector<uint8_t> frame;
  while (queue_.pop(frame)) {
    const uint32_t airtime = airtimeUs(frame.size());
    if (!duty_.allows(now_, airtime)) {
      // Budget spent: put it back and wait rather than break the rules.
      queue_.push(ByteView { frame.data(), frame.size() }, Priority::HIGH);
      return;
    }
    duty_.record(now_, airtime);

    RxMeta meta;
    meta.at = now_;
    meta.airtimeUs = airtime;
    meta.rssi = -60;
    meta.snr = 8;
    medium_.broadcast(index_, ByteView { frame.data(), frame.size() }, meta);
  }
}

void VirtualRadio::deliver(ByteView frame, const RxMeta& meta)
{
  if (!powered_ || sink_ == nullptr) return;
  sink_->onFrame(frame, meta);
}

// ------------------------------------------------------------------ replay

ReplayRadio::ReplayRadio(std::vector<Capture> captures, Params params)
  : captures_(std::move(captures)),
    params_(params)
{
  std::stable_sort(captures_.begin(), captures_.end(), [](const Capture& a, const Capture& b) { return a.at < b.at; });
}

bool ReplayRadio::send(ByteView frame, Priority priority)
{
  (void)priority;
  transmitted_.push_back(std::vector<uint8_t>(frame.begin(), frame.end()));
  return true;
}

uint32_t ReplayRadio::airtimeUs(size_t length) const
{
  return airtimeUsFor(params_, length);
}

void ReplayRadio::tick(Millis now)
{
  while (next_ < captures_.size() && captures_[next_].at <= now) {
    if (sink_ != nullptr) {
      const Capture& capture = captures_[next_];
      sink_->onFrame(ByteView { capture.frame.data(), capture.frame.size() }, capture.meta);
    }
    next_++;
  }
}
