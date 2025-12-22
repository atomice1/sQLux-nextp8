// Grouped profiler data - instructions organized by function

#ifndef PROFILER_GROUPED_H
#define PROFILER_GROUPED_H

#include "profiler_data.h"
#include <cstdint>
#include <map>
#include <vector>
#include <set>

namespace Profiler {

// A call from one function to another
struct FunctionCall {
    uint32_t caller_address;      // Address of instruction making the call
    uint32_t target_function;     // Entry point of called function
    uint64_t call_count;
    uint64_t inclusive_instructions;
    uint64_t inclusive_instr_fetches;
    uint64_t inclusive_data_reads;
    uint64_t inclusive_data_writes;
};

// A single instruction within a function
struct GroupedInstruction {
    uint32_t address;
    const InstructionCost cost;  // Pointer to original cost data
};

// A function (group of instructions with same entry point)
struct GroupedFunction {
    uint32_t entry_address;                      // Function entry point
    std::vector<GroupedInstruction> instructions; // All instructions in this function
    std::vector<FunctionCall> calls;             // Calls to other functions

    // Computed totals
    uint64_t total_self_instructions;
    uint64_t total_self_instr_fetches;
    uint64_t total_self_data_reads;
    uint64_t total_self_data_writes;
};

// Container for all grouped functions
class GroupedProfilerData {
public:
    GroupedProfilerData();
    ~GroupedProfilerData();

    const std::map<uint32_t, GroupedFunction>& GetFunctions() const {
        return functions_;
    }

    const std::set<uint32_t>& GetEntryPoints() const {
        return entry_points_;
    }

    uint64_t GetTotalInstructions() const {
        return total_instructions_;
    }

    void AddFunction(uint32_t entry, GroupedFunction&& func);
    void SetTotalInstructions(uint64_t total);

private:
    friend GroupedProfilerData ConvertToGroupedData(const ProfilerData& data);

    std::map<uint32_t, GroupedFunction> functions_;
    std::set<uint32_t> entry_points_;  // Top-level entry points (executed with empty stack)
    uint64_t total_instructions_;
};

// Convert raw ProfilerData to grouped format
GroupedProfilerData ConvertToGroupedData(const ProfilerData& data);

} // namespace Profiler

#endif // PROFILER_GROUPED_H
