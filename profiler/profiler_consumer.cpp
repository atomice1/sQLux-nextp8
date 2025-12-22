// Profiler buffer consumer implementation

#include "profiler_consumer.h"

namespace Profiler {

BufferConsumer::BufferConsumer(ProfilerData& data)
    : data_(data) {
}

BufferConsumer::~BufferConsumer() {
}

void BufferConsumer::ProcessBuffer(EventBuffer* buffer) {
    if (!buffer) {
        return;
    }

    // Process all events in the buffer
    for (size_t i = 0; i < buffer->count; ++i) {
        data_.ProcessEvent(buffer->events[i]);
    }

    // Clear the buffer after processing
    buffer->Clear();
}

} // namespace Profiler
