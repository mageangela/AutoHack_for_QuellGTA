#pragma once

#include <thread>

namespace gta5::app::runtime {

bool Running();
void SetRunning(bool running);
bool StopRequested();
void RequestStop();
void ResetStopRequest();
std::thread& WorkerThread();

}  // namespace gta5::app::runtime
