// Client-side buffer management

#ifndef PROFILER_CLIENT_H
#define PROFILER_CLIENT_H

#include <stdint.h>
#include "profiler_consumer.h"
#include "profiler_thread.h"

#ifdef __cplusplus
extern "C" {
#endif

// C-linkage function for switching buffers
void Profiler_SwitchBuffer(void);

#ifdef __cplusplus
}

// C++ API for internal use
namespace Profiler {

// Client-side buffer manager for the main thread
class ClientBufferManager {
public:
    ClientBufferManager();
    ~ClientBufferManager();

    // Get the current buffer for adding events
    EventBuffer* GetCurrentBuffer();

    // Switch to a new buffer (called when current buffer is full)
    void SwitchBuffer();

    // Update the global buffer pointers after switching buffers
    void UpdateGlobalPointers();

private:
    EventBuffer* current_buffer_;
};

// Global client buffer manager
ClientBufferManager* GetClientBufferManager();

// Initialize the client buffer manager (must be called before emulation starts)
void InitializeClient();

// Cleanup the client buffer manager
void CleanupClient();

} // namespace Profiler

#endif // __cplusplus

#endif // PROFILER_CLIENT_H
