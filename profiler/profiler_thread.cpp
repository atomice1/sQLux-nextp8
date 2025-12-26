// Profiler thread implementation

#include "profiler_thread.h"
#include "profiler_invariants.h"
#include "profiler_grouped.h"
#include <iostream>
#include <csignal>
#include <chrono>

namespace Profiler {

// Static instance for signal handling
ProfilerThread* ProfilerThread::instance_ = nullptr;

ProfilerThread::ProfilerThread()
    : running_(false),
      should_flush_(false),
      consumer_(data_),
      output_filename_("callgrind.out") {
    instance_ = this;
}

ProfilerThread::~ProfilerThread() {
    Stop();
    instance_ = nullptr;
}

void ProfilerThread::Start() {
    if (running_.load()) {
        return;
    }

    running_.store(true);
    should_flush_.store(false);

    // Initialize empty buffers
    InitializeEmptyBuffers();

    // Start the profiler thread
    thread_ = std::thread(&ProfilerThread::ThreadFunc, this);

    // Install signal handlers
    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);
}

void ProfilerThread::Stop() {
    if (!running_.load()) {
        return;
    }

    running_.store(false);

    // Wake up the thread if it's waiting
    filled_cv_.notify_all();
    empty_cv_.notify_all();

    // Wait for thread to finish
    if (thread_.joinable()) {
        thread_.join();
    }

    // Final flush
    FlushToFile();
}

void ProfilerThread::PushFilledBuffer(EventBuffer* buffer) {
    std::lock_guard<std::mutex> lock(filled_mutex_);
    filled_buffers_.push_back(buffer);
    filled_cv_.notify_one();
}

EventBuffer* ProfilerThread::GetEmptyBuffer() {
    std::unique_lock<std::mutex> lock(empty_mutex_);

    // Wait for an empty buffer to become available
    while (empty_buffers_.empty() && running_.load()) {
        // Print warning if we're blocking
        static bool warned = false;
        if (!warned) {
            std::cerr << "WARNING: Profiler main thread blocked waiting for empty buffer. "
                      << "Profiler thread may be falling behind.\n";
            warned = true;
        }
        empty_cv_.wait(lock);
    }

    if (empty_buffers_.empty()) {
        return nullptr;
    }

    EventBuffer* buffer = empty_buffers_.front();
    empty_buffers_.pop_front();
    return buffer;
}

void ProfilerThread::SetOutputFilename(const std::string& filename) {
    std::lock_guard<std::mutex> lock(output_mutex_);
    output_filename_ = filename;
}

void ProfilerThread::Flush() {
    should_flush_.store(true);
    filled_cv_.notify_one();
}

void ProfilerThread::ThreadFunc() {
    auto last_flush_time = std::chrono::steady_clock::now();
    const auto flush_interval = std::chrono::seconds(30);  // Flush every 30 seconds

    while (running_.load()) {
        EventBuffer* buffer = nullptr;

        {
            std::unique_lock<std::mutex> lock(filled_mutex_);

            // Wait for a filled buffer or timeout
            filled_cv_.wait_for(lock, std::chrono::seconds(1), [this] {
                return !filled_buffers_.empty() || !running_.load() || should_flush_.load();
            });

            if (!filled_buffers_.empty()) {
                buffer = filled_buffers_.front();
                filled_buffers_.pop_front();
            }
        }

        // Process the buffer if we got one
        if (buffer) {
            consumer_.ProcessBuffer(buffer);

            // Return buffer to empty queue
            {
                std::lock_guard<std::mutex> lock(empty_mutex_);
                empty_buffers_.push_back(buffer);
                empty_cv_.notify_one();
            }
        }

        // Check if it's time to flush
        auto now = std::chrono::steady_clock::now();
        if (should_flush_.load() || (now - last_flush_time) >= flush_interval) {
            FlushToFile();
            last_flush_time = now;
            should_flush_.store(false);
        }
    }
}

void ProfilerThread::InitializeEmptyBuffers() {
    std::lock_guard<std::mutex> lock(empty_mutex_);
    empty_buffers_.clear();

    // Add all buffers from the pool to the empty queue
    for (size_t i = 0; i < BUFFER_POOL_SIZE; ++i) {
        buffer_pool_[i].Clear();
        empty_buffers_.push_back(&buffer_pool_[i]);
    }
}

void ProfilerThread::FlushToFile() {
    std::lock_guard<std::mutex> lock(output_mutex_);

    // Finalize the data before writing (creates synthetic root)
    data_.Finalize();

    // Convert to grouped format
    GroupedProfilerData grouped = ConvertToGroupedData(data_);

    // Check invariants on both raw and grouped data
    //CheckProfilerInvariants(data_);
    //CheckGroupedInvariants(grouped);

    if (!serializer_.WriteToFile(output_filename_, grouped)) {
        std::cerr << "Failed to write profiler data to " << output_filename_ << std::endl;
    } else {
        //std::cout << "Profiler data flushed to " << output_filename_ << std::endl;
    }
}

void ProfilerThread::HandleSignal(int signal) {
    std::cout << "\nReceived signal " << signal << ", flushing profiler data...\n";
    Flush();

    // Restore default signal handler and re-raise signal
    std::signal(signal, SIG_DFL);
    std::raise(signal);
}

void ProfilerThread::SignalHandler(int signal) {
    if (instance_) {
        instance_->HandleSignal(signal);
    }
}

// Global instance management
static ProfilerThread* g_profiler = nullptr;

void InitializeProfiler() {
    if (!g_profiler) {
        g_profiler = new ProfilerThread();
        g_profiler->Start();
    }
}

void ShutdownProfiler() {
    if (g_profiler) {
        g_profiler->Stop();
        delete g_profiler;
        g_profiler = nullptr;
    }
}

ProfilerThread* GetProfiler() {
    return g_profiler;
}

} // namespace Profiler
