// Profiler buffer consumer

#ifndef PROFILER_CONSUMER_H
#define PROFILER_CONSUMER_H

#include "profiler_data.h"
#include <cstdint>
#include <cstddef>

namespace Profiler {

// Buffer of profile events
constexpr size_t BUFFER_SIZE = 8192;  // 8K events per buffer

struct EventBuffer {
    uint32_t events[BUFFER_SIZE];
    size_t count;  // Number of valid events in buffer

    EventBuffer() : count(0) {}

    void Clear() {
        count = 0;
    }

    bool IsFull() const {
        return count >= BUFFER_SIZE;
    }

    void AddEvent(uint32_t event) {
        if (count < BUFFER_SIZE) {
            events[count++] = event;
        }
    }
};

// Consumer processes buffers and adds events to ProfilerData
class BufferConsumer {
public:
    BufferConsumer(ProfilerData& data);
    ~BufferConsumer();

    // Process all events in a buffer
    void ProcessBuffer(EventBuffer* buffer);

private:
    ProfilerData& data_;
};

} // namespace Profiler

#endif // PROFILER_CONSUMER_H
