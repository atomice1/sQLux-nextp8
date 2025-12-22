// Callgrind file format serializer

#ifndef PROFILER_CALLGRIND_H
#define PROFILER_CALLGRIND_H

#include <string>
#include <fstream>

namespace Profiler {

class GroupedProfilerData;

// Serialize profiler data to callgrind format
class CallgrindSerializer {
public:
    CallgrindSerializer();
    ~CallgrindSerializer();

    // Write profiler data to a file in callgrind format
    // Returns true on success
    bool WriteToFile(const std::string& filename, const GroupedProfilerData& data);

private:
    void WriteHeader(std::ofstream& out, const GroupedProfilerData& data);
    void WriteBody(std::ofstream& out, const GroupedProfilerData& data);
};

} // namespace Profiler

#endif // PROFILER_CALLGRIND_H
