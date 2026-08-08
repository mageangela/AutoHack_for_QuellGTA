#include "app_runtime.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>

namespace gta5::app::runtime {
	namespace {

		std::atomic<bool> g_running{ false };
		std::atomic<bool> g_stopRequested{ false };
		std::thread g_worker;

	}  // namespace

	bool Running() { return g_running.load(std::memory_order_relaxed); }
	void SetRunning(bool running) { g_running.store(running, std::memory_order_relaxed); }
	bool StopRequested() { return g_stopRequested.load(std::memory_order_relaxed); }
	void RequestStop() { g_stopRequested.store(true, std::memory_order_relaxed); }
	void ResetStopRequest() { g_stopRequested.store(false, std::memory_order_relaxed); }
	std::thread& WorkerThread() { return g_worker; }

	bool ConfigureLatencySensitiveProcess() {
		const bool prioritySet = SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS) != FALSE;

		PROCESS_POWER_THROTTLING_STATE state{};
		state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
		state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
		state.StateMask = 0;
		const bool throttlingDisabled = SetProcessInformation(
			GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state)) != FALSE;
		return prioritySet && throttlingDisabled;
	}

	bool ConfigureLatencySensitiveThread() {
		const bool prioritySet = SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL) != FALSE;

		THREAD_POWER_THROTTLING_STATE state{};
		state.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
		state.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
		state.StateMask = 0;
		const bool throttlingDisabled = SetThreadInformation(
			GetCurrentThread(), ThreadPowerThrottling, &state, sizeof(state)) != FALSE;
		return prioritySet && throttlingDisabled;
	}
}  // namespace gta5::app::runtime