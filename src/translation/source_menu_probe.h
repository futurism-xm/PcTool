#pragma once
#include <windows.h>
#include <future>

namespace translation {
struct SourceMenuState { bool available{},menu{}; };
// Runs provider calls away from the window thread. No text is read.
std::future<SourceMenuState> ProbeSourceMenu(DWORD process);
}
