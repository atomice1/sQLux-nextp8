// Profiler thread implementation

#ifndef PROFILER_THREAD_H
#define PROFILER_THREAD_H

#include "profiler_data.h"
#include "profiler_consumer.h"
#include "profiler_callgrind.h"
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <atomic>
#include <string>

namespace Profiler {

// Main profiler thread manager
class ProfilerThread {
public:
    ProfilerThread();
    ~ProfilerThread();

    // Start the profiler thread
    void Start();

    // Stop the profiler thread
    void Stop();

    // Push a filled buffer to the processing queue
    void PushFilledBuffer(EventBuffer* buffer);

    // Get an empty buffer (blocks if none available)
    EventBuffer* GetEmptyBuffer();

    // Set output filename
    void SetOutputFilename(const std::string& filename);

    // Flush current data to file
    void Flush();

private:
    void ThreadFunc();
    void InitializeEmptyBuffers();
    void FlushToFile();
    void HandleSignal(int signal);

    static void SignalHandler(int signal);
    static ProfilerThread* instance_;

    std::thread thread_;
    std::atomic<bool> running_;
    std::atomic<bool> should_flush_;

    // Filled buffers queue (producer: main thread, consumer: profiler thread)
    std::deque<EventBuffer*> filled_buffers_;
    std::mutex filled_mutex_;
    std::condition_variable filled_cv_;

    // Empty buffers queue (producer: profiler thread, consumer: main thread)
    std::deque<EventBuffer*> empty_buffers_;
    std::mutex empty_mutex_;
    std::condition_variable empty_cv_;

    ProfilerData data_;
    BufferConsumer consumer_;
    CallgrindSerializer serializer_;

    std::string output_filename_;
    std::mutex output_mutex_;

    // Pool of buffer objects
    static constexpr size_t BUFFER_POOL_SIZE = 16;
    EventBuffer buffer_pool_[BUFFER_POOL_SIZE];
};

// Global profiler instance management
void InitializeProfiler();
void ShutdownProfiler();
ProfilerThread* GetProfiler();

} // namespace Profiler

#endif // PROFILER_THREAD_H
