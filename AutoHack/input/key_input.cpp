#include "key_input.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

namespace gta5::input {

struct Job::State {
  std::atomic<JobStatus> status{JobStatus::Pending};
  std::atomic<long long> startedAtNs{0};
};

namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kLongPressHold = std::chrono::milliseconds(2000);
constexpr auto kImmediateHold = std::chrono::milliseconds(42);

struct Stroke {
  Key key;
  std::chrono::milliseconds hold{0};
  std::chrono::milliseconds gap{0};
};

struct Request {
  std::vector<Stroke> strokes;
  Clock::time_point startAt{};
  HWND expectedForeground = nullptr;
  std::uint64_t generation = 0;
  std::shared_ptr<Job::State> state;
};

std::atomic<long long> g_sequenceHoldMs{20};
std::atomic<long long> g_sequenceGapMs{20};

bool SendKey(const Key& key, bool up) {
  INPUT input{};
  input.type = INPUT_KEYBOARD;
  input.ki.wScan = key.scanCode;
  input.ki.dwFlags = KEYEVENTF_SCANCODE |
      (key.extended ? KEYEVENTF_EXTENDEDKEY : 0) |
      (up ? KEYEVENTF_KEYUP : 0);
  return SendInput(1, &input, sizeof(input)) == 1;
}

class Dispatcher {
 public:
  Dispatcher() : worker_([this] { Run(); }) {}

  ~Dispatcher() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
      CancelPendingLocked();
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
  }

  void Enqueue(Request request) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      request.generation = generation_;
      queue_.push_back(std::move(request));
    }
    cv_.notify_one();
  }

  void CancelAll() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++generation_;
      CancelPendingLocked();
    }
    cv_.notify_all();
  }

 private:
  bool WaitUntil(Clock::time_point deadline, std::uint64_t generation) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_until(lock, deadline, [&] {
      return stopping_ || generation != generation_;
    });
    return !stopping_ && generation == generation_;
  }

  bool Active(std::uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    return !stopping_ && generation == generation_;
  }

  void CancelPendingLocked() {
    for (auto& request : queue_) {
      request.state->status.store(JobStatus::Canceled, std::memory_order_release);
    }
    queue_.clear();
  }

  void Run() {
    for (;;) {
      Request request;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
        if (stopping_) return;
        request = std::move(queue_.front());
        queue_.pop_front();
      }

      if (!WaitUntil(request.startAt, request.generation)) {
        request.state->status.store(JobStatus::Canceled, std::memory_order_release);
        continue;
      }

      JobStatus result = JobStatus::Succeeded;
      for (const Stroke& stroke : request.strokes) {
        if (!Active(request.generation)) {
          result = JobStatus::Canceled;
          break;
        }
        if (request.expectedForeground && GetForegroundWindow() != request.expectedForeground) {
          result = JobStatus::Failed;
          break;
        }
        if (!SendKey(stroke.key, false)) {
          result = JobStatus::Failed;
          break;
        }
        const auto startedAtNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     Clock::now().time_since_epoch())
                                     .count();
        long long expectedStart = 0;
        request.state->startedAtNs.compare_exchange_strong(
            expectedStart, startedAtNs,
            std::memory_order_release, std::memory_order_relaxed);

        const bool stillActive = WaitUntil(Clock::now() + stroke.hold, request.generation);
        const bool released = SendKey(stroke.key, true);
        if (!stillActive) {
          result = JobStatus::Canceled;
          break;
        }
        if (!released) {
          result = JobStatus::Failed;
          break;
        }
        if (!WaitUntil(Clock::now() + stroke.gap, request.generation)) {
          result = JobStatus::Canceled;
          break;
        }
      }
      request.state->status.store(result, std::memory_order_release);
    }
  }

  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<Request> queue_;
  std::thread worker_;
  std::uint64_t generation_ = 1;
  bool stopping_ = false;
};

Dispatcher& GetDispatcher() {
  static Dispatcher dispatcher;
  return dispatcher;
}

Job Enqueue(std::vector<Stroke> strokes, Clock::time_point startAt,
            HWND expectedForeground) {
  auto state = std::make_shared<Job::State>();
  if (strokes.empty()) {
    state->status.store(JobStatus::Succeeded, std::memory_order_release);
    return Job(std::move(state));
  }
  for (const Stroke& stroke : strokes) {
    if (!stroke.key.scanCode) {
      state->status.store(JobStatus::Failed, std::memory_order_release);
      return Job(std::move(state));
    }
  }
  Request request;
  request.strokes = std::move(strokes);
  request.startAt = startAt;
  request.expectedForeground = expectedForeground;
  request.state = state;
  GetDispatcher().Enqueue(std::move(request));
  return Job(std::move(state));
}

}  // namespace

Key Key::FromVirtualKey(WORD virtualKey) {
  switch (virtualKey) {
    case VK_UP: return {0x48, true};
    case VK_DOWN: return {0x50, true};
    case VK_LEFT: return {0x4B, true};
    case VK_RIGHT: return {0x4D, true};
    case VK_RETURN: return {0x1C, false};
    default:
      return {static_cast<WORD>(MapVirtualKeyW(virtualKey, MAPVK_VK_TO_VSC)), false};
  }
}

JobStatus Job::Status() const {
  return state_ ? state_->status.load(std::memory_order_acquire) : JobStatus::Invalid;
}

std::chrono::steady_clock::time_point Job::StartedAt() const {
  if (!state_) return {};
  const long long ns = state_->startedAtNs.load(std::memory_order_acquire);
  if (ns <= 0) return {};
  return std::chrono::steady_clock::time_point(std::chrono::nanoseconds(ns));
}

void ConfigureSequenceTiming(std::chrono::milliseconds hold,
                             std::chrono::milliseconds gap) {
  g_sequenceHoldMs.store(std::max<long long>(0, hold.count()), std::memory_order_relaxed);
  g_sequenceGapMs.store(std::max<long long>(0, gap.count()), std::memory_order_relaxed);
}

Job QueueSequence(const std::vector<Command>& commands, HWND expectedForeground) {
  const auto hold = std::chrono::milliseconds(
      g_sequenceHoldMs.load(std::memory_order_relaxed));
  const auto gap = std::chrono::milliseconds(
      g_sequenceGapMs.load(std::memory_order_relaxed));
  std::vector<Stroke> strokes;
  strokes.reserve(commands.size());
  for (const Command& command : commands) {
    const auto commandHold = command.action == Action::LongPress ? kLongPressHold : hold;
    const auto commandGap = command.action == Action::LongPress ? std::chrono::milliseconds{0} : gap;
    strokes.push_back({command.key, commandHold, commandGap});
  }
  return Enqueue(std::move(strokes), Clock::now(), expectedForeground);
}

Job QueueImmediate(Key key, Clock::time_point executeAt, HWND expectedForeground) {
  return Enqueue({{key, kImmediateHold, std::chrono::milliseconds{0}}},
                 executeAt, expectedForeground);
}

void CancelAll() { GetDispatcher().CancelAll(); }

}  // namespace gta5::input
