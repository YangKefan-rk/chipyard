#include "native_probe.h"
#include <svdpi.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace difftest_s::native {
namespace {
constexpr size_t max_pending_frames = 64;
constexpr uint32_t max_harts = 256;
constexpr uint32_t max_payload_bits = 1 << 20;

struct TerminalDrainControl {
  std::atomic<uint32_t> harts{0};
  std::atomic<uint64_t> requested{0};
  std::atomic<bool> complete{false};
};
TerminalDrainControl &terminal_control() {
  static TerminalDrainControl control;
  return control;
}

std::string json_string(const std::string &value) {
  constexpr char hex[] = "0123456789abcdef";
  std::string result = "\"";
  for (unsigned char character : value) {
    if (character == '"' || character == '\\') {
      result += '\\';
      result += static_cast<char>(character);
    } else if (character < 0x20) {
      result += "\\u00";
      result += hex[character >> 4];
      result += hex[character & 15];
    } else {
      result += static_cast<char>(character);
    }
  }
  return result + '"';
}

class NativeIngress {
public:
  bool install(NativeFrameConsumer consume, NativeFinalizer finish, NativeFailureObserver failure) {
    std::lock_guard<std::mutex> guard(lock_);
    if (started_ || consume_ || !consume || !finish)
      return false;
    consume_ = std::move(consume);
    finish_ = std::move(finish);
    failure_observer_ = std::move(failure);
    return true;
  }

  void sample(uint32_t abi, uint32_t kind, uint32_t hart, uint32_t harts, uint64_t tick,
              bool reset, uint64_t epoch, uint64_t cycle, uint32_t bits, const uint32_t *words) {
    std::lock_guard<std::mutex> guard(lock_);
    started_ = true;
    if (sample_count_ == UINT64_MAX) {
      fail(rvwmo::CheckStatus::Invalid, "native sample counter exhausted");
      return;
    }
    ++sample_count_;
    if (finished_) {
      fail(rvwmo::CheckStatus::Invalid, "native sample arrived after finalization");
      return;
    }
    const auto *layout = find_native_layout(kind, hart);
    if (abi != 1 || !layout || layout->abi_version != abi || !words || !harts || harts > max_harts ||
        hart >= harts || layout->harts != harts || layout->payload_bits != bits || !bits || bits > max_payload_bits) {
      fail(rvwmo::CheckStatus::Invalid, "native snapshot layout, roster or ABI mismatch");
      return;
    }
    if (roster_size_ && (roster_size_ != harts || kind_ != kind)) {
      fail(rvwmo::CheckStatus::Unsupported, "native source changed DUT kind or hart roster");
      return;
    }
    roster_size_ = harts;
    kind_ = kind;
    auto previous = last_tick_.find(hart);
    if ((previous == last_tick_.end() && tick != 0) ||
        (previous != last_tick_.end() && (previous->second == UINT64_MAX || tick != previous->second + 1))) {
      fail(rvwmo::CheckStatus::Invalid, "native source tick skipped, repeated or wrapped");
      return;
    }
    if (pending_.size() >= max_pending_frames && !pending_.count(tick)) {
      fail(rvwmo::CheckStatus::Incomplete, "native lossless framing buffer exhausted");
      return;
    }
    last_tick_[hart] = tick;
    auto inserted = pending_.try_emplace(tick);
    auto &frame = inserted.first->second;
    if (inserted.second) {
      frame = {tick, epoch, cycle, reset, std::vector<NativeHartSample>(harts)};
    } else if (frame.epoch != epoch || frame.source_cycle != cycle || frame.reset != reset) {
      fail(rvwmo::CheckStatus::Unsupported, "native hart clocks, reset or source epochs do not share one domain");
      return;
    }
    auto &slot = frame.samples[hart];
    if (slot.layout) {
      fail(rvwmo::CheckStatus::Invalid, "native source delivered the same hart twice in a frame");
      return;
    }
    slot.layout = layout;
    slot.words.assign(words, words + (bits + 31) / 32);
    if (bits % 32 && (slot.words.back() >> (bits % 32))) {
      fail(rvwmo::CheckStatus::Invalid, "native snapshot has nonzero bits beyond declared layout");
      return;
    }
    drain();
  }

  int exit_status() {
    std::lock_guard<std::mutex> guard(lock_);
    return finished_ && failure_ ? EXIT_SUCCESS : EXIT_FAILURE;
  }

  void finalize() {
    std::lock_guard<std::mutex> guard(lock_);
    if (finished_)
      return;
    finished_ = true;
    if (!started_ || !pending_.empty())
      fail(rvwmo::CheckStatus::Incomplete, "native source ended with missing hart snapshots");
    if (!consume_ || !finish_) {
      fail(rvwmo::CheckStatus::Incomplete, "native transport has no registered Runtime and REF sink");
    } else {
      try {
        auto result = finish_();
        if (!result)
          fail(result.status == rvwmo::CheckStatus::Pending ? rvwmo::CheckStatus::Incomplete : result.status,
               result.message);
      } catch (const std::exception &error) {
        fail(rvwmo::CheckStatus::Incomplete, std::string("native Runtime finalizer threw: ") + error.what());
      } catch (...) {
        fail(rvwmo::CheckStatus::Incomplete, "native Runtime finalizer threw an unknown exception");
      }
    }
    if (trace_.is_open()) {
      trace_.flush();
      if (!trace_)
        fail(rvwmo::CheckStatus::Incomplete, "native trace output failed");
    }
    std::fprintf(stderr,
      "DIFFTEST-S Native {\"frames\":%llu,\"samples\":%llu,\"harts\":%u,\"status\":%u,\"reason\":%s}\n",
      static_cast<unsigned long long>(frame_count_), static_cast<unsigned long long>(sample_count_),
      roster_size_, static_cast<unsigned>(failure_.status), json_string(failure_.message).c_str());
  }

private:
  std::mutex lock_;
  NativeFrameConsumer consume_;
  NativeFinalizer finish_;
  NativeFailureObserver failure_observer_;
  std::map<uint64_t, NativeFrame> pending_;
  std::map<uint32_t, uint64_t> last_tick_;
  std::ofstream trace_;
  rvwmo::CheckResult failure_;
  uint32_t roster_size_ = 0;
  uint32_t kind_ = 0;
  uint64_t frame_count_ = 0;
  uint64_t sample_count_ = 0;
  uint64_t last_epoch_ = 0;
  uint64_t last_source_cycle_ = 0;
  bool started_ = false;
  bool finished_ = false;
  bool left_reset_ = false;

  void fail(rvwmo::CheckStatus status, const std::string &message) {
    if (!failure_)
      return;
    failure_ = {status, 0, message};
    if (failure_observer_) {
      try {
        failure_observer_(failure_);
      } catch (const std::exception &error) {
        std::fprintf(stderr, "DIFFTEST-S native failure observer threw: %s\n", error.what());
      } catch (...) {
        std::fprintf(stderr, "DIFFTEST-S native failure observer threw an unknown exception\n");
      }
    }
  }

  void trace(const NativeFrame &frame) {
    if (frame_count_ == 0) {
      const char *path = std::getenv("DIFFTEST_S_NATIVE_TRACE");
      if (path && *path) {
        trace_.open(path, std::ios::binary);
        if (!trace_)
          fail(rvwmo::CheckStatus::Incomplete, "cannot open native trace output");
      }
    }
    if (!trace_.is_open())
      return;
    const uint64_t header[] = {UINT64_C(0x4454534e41544956), 1, frame.transport_tick, frame.epoch,
                               frame.source_cycle, frame.reset, frame.samples.size()};
    trace_.write(reinterpret_cast<const char *>(header), sizeof(header));
    for (const auto &sample : frame.samples) {
      const uint32_t fields[] = {sample.layout->kind, sample.layout->hart, sample.layout->payload_bits,
                                 static_cast<uint32_t>(sample.words.size())};
      trace_.write(reinterpret_cast<const char *>(fields), sizeof(fields));
      trace_.write(reinterpret_cast<const char *>(sample.words.data()), sample.words.size() * sizeof(uint32_t));
    }
    if (!trace_)
      fail(rvwmo::CheckStatus::Incomplete, "native trace output failed");
  }

  void drain() {
    while (!pending_.empty()) {
      auto first = pending_.begin();
      bool complete = true;
      for (const auto &sample : first->second.samples)
        complete = complete && sample.layout;
      if (!complete)
        return;
      const auto &frame = first->second;
      if (left_reset_) {
        if (frame.reset || frame.epoch != last_epoch_)
          fail(rvwmo::CheckStatus::Unsupported, "native reset or epoch change requires a new observation session");
        else if (last_source_cycle_ == UINT64_MAX || frame.source_cycle != last_source_cycle_ + 1)
          fail(rvwmo::CheckStatus::Invalid, "native source cycle skipped, repeated or wrapped");
      }
      if (!frame.reset) {
        left_reset_ = true;
        last_epoch_ = frame.epoch;
        last_source_cycle_ = frame.source_cycle;
      }
      trace(frame);
      if (consume_) {
        auto result = consume_(frame);
        if (!result && result.status != rvwmo::CheckStatus::Pending)
          fail(result.status, result.message);
      }
      if (frame_count_ == UINT64_MAX) {
        fail(rvwmo::CheckStatus::Invalid, "native frame counter exhausted");
        return;
      }
      ++frame_count_;
      pending_.erase(first);
    }
  }
};

NativeIngress &ingress() {
  static NativeIngress instance;
  return instance;
}
} // namespace

bool register_native_consumer(NativeFrameConsumer consume, NativeFinalizer finish, NativeFailureObserver failure) {
  return ingress().install(std::move(consume), std::move(finish), std::move(failure));
}

int native_exit_status() {
  return ingress().exit_status();
}

bool enable_native_terminal_drain(uint32_t harts) {
  if (!harts || harts > 64)
    return false;
  uint32_t expected = 0;
  return terminal_control().harts.compare_exchange_strong(expected, harts);
}

void request_native_fetch_stop(uint32_t hart) {
  auto &control = terminal_control();
  if (hart >= control.harts.load())
    throw std::invalid_argument("terminal fetch stop names an unregistered hart");
  control.requested.fetch_or(uint64_t{1} << hart);
}

void complete_native_terminal_drain() {
  auto &control = terminal_control();
  const auto harts = control.harts.load();
  const auto expected = harts == 64 ? UINT64_MAX : (uint64_t{1} << harts) - 1;
  if (!harts || control.requested.load() != expected)
    throw std::logic_error("terminal drain cannot complete before every hart has a stop request");
  control.complete.store(true);
}

bool native_terminal_drain_complete() {
  return terminal_control().harts.load() && terminal_control().complete.load();
}
} // namespace difftest_s::native

extern "C" void difftest_s_native_sample(int abi, int kind, int hart, int harts, uint64_t tick,
                                         svBit reset, uint64_t epoch, uint64_t cycle, int bits,
                                         const svBitVecVal *words) {
  try {
    difftest_s::native::ingress().sample(abi, kind, hart, harts, tick, reset, epoch, cycle, bits, words);
  } catch (const std::exception &error) {
    std::fprintf(stderr, "DIFFTEST-S native transport exception: %s\n", error.what());
    std::abort();
  }
}

extern "C" void difftest_s_native_finish() {
  try {
    difftest_s::native::ingress().finalize();
  } catch (const std::exception &error) {
    std::fprintf(stderr, "DIFFTEST-S native finalization exception: %s\n", error.what());
    std::abort();
  } catch (...) {
    std::fprintf(stderr, "DIFFTEST-S native finalization exception: unknown exception\n");
    std::abort();
  }
}

extern "C" svBit difftest_s_native_stop_requested(int kind, int hart) {
  const auto &control = difftest_s::native::terminal_control();
  return kind == 1 && hart >= 0 && static_cast<uint32_t>(hart) < control.harts.load() &&
      ((control.requested.load() >> hart) & 1);
}

extern "C" svBit difftest_s_native_defer_finish() {
  // HB success is released only after a complete eval/frame in the C++ main.
  // Calling $finish from one posedge block could strand the other hart samples.
  return difftest_s::native::terminal_control().harts.load() != 0;
}
