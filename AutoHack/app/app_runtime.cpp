#include "app_runtime.h"

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

}  // namespace gta5::app::runtime