// Client-side buffer management implementation

#include "profiler_client.h"
#include "profiler_data.h"

// Forward declarations of global buffer pointers (defined at end of file)
extern "C" {
    extern uint32_t* profiler_current_buffer_ptr;
    extern uint32_t* profiler_buffer_end_ptr;
}

namespace Profiler {

ClientBufferManager::ClientBufferManager()
    : current_buffer_(nullptr) {
    // Get the first empty buffer from the profiler
    ProfilerThread* profiler = GetProfiler();
    if (profiler) {
        current_buffer_ = profiler->GetEmptyBuffer();
        UpdateGlobalPointers();
    }
}

ClientBufferManager::~ClientBufferManager() {
    // Update count based on how much of buffer was used
    if (current_buffer_) {
        current_buffer_->count = ::profiler_current_buffer_ptr - current_buffer_->events;

        // Push any remaining data
        if (current_buffer_->count > 0) {
            ProfilerThread* profiler = GetProfiler();
            if (profiler) {
                profiler->PushFilledBuffer(current_buffer_);
            }
        }
    }
}

EventBuffer* ClientBufferManager::GetCurrentBuffer() {
    return current_buffer_;
}

void ClientBufferManager::SwitchBuffer() {
    ProfilerThread* profiler = GetProfiler();
    if (!profiler) {
        return;
    }

    // Update count based on how much of buffer was used
    if (current_buffer_) {
        current_buffer_->count = ::profiler_current_buffer_ptr - current_buffer_->events;
        profiler->PushFilledBuffer(current_buffer_);
    }

    // Get a new empty buffer
    current_buffer_ = profiler->GetEmptyBuffer();
    UpdateGlobalPointers();
}

void ClientBufferManager::UpdateGlobalPointers() {
    if (current_buffer_) {
        ::profiler_current_buffer_ptr = current_buffer_->events;
        ::profiler_buffer_end_ptr = current_buffer_->events + BUFFER_SIZE;
    } else {
        ::profiler_current_buffer_ptr = nullptr;
        ::profiler_buffer_end_ptr = nullptr;
    }
}

// Global instance
static ClientBufferManager* g_client_buffer_manager = nullptr;

ClientBufferManager* GetClientBufferManager() {
    if (!g_client_buffer_manager) {
        g_client_buffer_manager = new ClientBufferManager();
    }
    return g_client_buffer_manager;
}

void InitializeClient() {
    // Force creation of client buffer manager to initialize global pointers
    GetClientBufferManager();
}

void CleanupClient() {
    if (g_client_buffer_manager) {
        delete g_client_buffer_manager;
        g_client_buffer_manager = nullptr;
    }
}

} // namespace Profiler

// Global buffer pointers for fast access from inline functions
uint32_t* profiler_current_buffer_ptr = nullptr;
uint32_t* profiler_buffer_end_ptr = nullptr;

// C-linkage function for switching buffers
extern "C" void Profiler_SwitchBuffer(void) {
    Profiler::ClientBufferManager* mgr = Profiler::GetClientBufferManager();
    if (mgr) {
        mgr->SwitchBuffer();
    }
}
