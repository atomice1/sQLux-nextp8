// C glue functions for profiler - provides extern "C" interface to C++ profiler

#include "profiler_thread.h"
#include "profiler_client.h"

extern "C" {

void Profiler_Initialize(void) {
    Profiler::InitializeProfiler();
    Profiler::InitializeClient();
}

void Profiler_Shutdown(void) {
    Profiler::CleanupClient();
    Profiler::ShutdownProfiler();
}

} // extern "C"
