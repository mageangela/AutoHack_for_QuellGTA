#pragma once

#include <windows.h>

#include <chrono>
#include <memory>
#include <utility>
#include <vector>

namespace gta5::input {

struct Key {
  WORD scanCode = 0;
  bool extended = false;

  static Key FromVirtualKey(WORD virtualKey);
};

enum class Action {
  Tap,
  LongPress,
};

struct Command {
  Key key;
  Action action = Action::Tap;
};

enum class JobStatus {
  Invalid,
  Pending,
  Succeeded,
  Failed,
  Canceled,
};

class Job {
 public:
  struct State;

  Job() = default;
  explicit Job(std::shared_ptr<State> state) : state_(std::move(state)) {}
  JobStatus Status() const;
  std::chrono::steady_clock::time_point StartedAt() const;
  bool Pending() const { return Status() == JobStatus::Pending; }
  bool Succeeded() const { return Status() == JobStatus::Succeeded; }
  explicit operator bool() const { return state_ != nullptr; }

 private:
  std::shared_ptr<State> state_;
  friend Job QueueSequence(const std::vector<Command>&, HWND);
  friend Job QueueImmediate(Key, std::chrono::steady_clock::time_point, HWND);
};

// Called by the application boundary. Game modules never read UI timing directly.
void ConfigureSequenceTiming(std::chrono::milliseconds hold,
                             std::chrono::milliseconds gap);

Job QueueSequence(const std::vector<Command>& commands,
                  HWND expectedForeground = nullptr);

inline Job QueueSequence(const std::vector<Key>& keys,
                         HWND expectedForeground = nullptr) {
  std::vector<Command> commands;
  commands.reserve(keys.size());
  for (const Key& key : keys) commands.push_back({key, Action::Tap});
  return QueueSequence(commands, expectedForeground);
}

// Standalone timing path for slider; sequence timing configuration is ignored.
Job QueueImmediate(
    Key key,
    std::chrono::steady_clock::time_point executeAt = std::chrono::steady_clock::now(),
    HWND expectedForeground = nullptr);

// Drops queued work and wakes the dispatcher so a currently held key is released.
void CancelAll();

}  // namespace gta5::input
