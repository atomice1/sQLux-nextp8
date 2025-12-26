// Grouped profiler data implementation

#include "profiler_grouped.h"
#include <set>

namespace Profiler {

GroupedProfilerData::GroupedProfilerData()
    : total_instructions_(0) {
}

GroupedProfilerData::~GroupedProfilerData() {
}

void GroupedProfilerData::AddFunction(uint32_t entry, GroupedFunction&& func) {
    functions_[entry] = std::move(func);
}

void GroupedProfilerData::SetTotalInstructions(uint64_t total) {
    total_instructions_ = total;
}

GroupedProfilerData ConvertToGroupedData(const ProfilerData& data) {
    GroupedProfilerData grouped;
    grouped.SetTotalInstructions(data.GetTotalInstructions());

    const auto& costs = data.GetInstructionCosts();

    if (costs.empty()) {
        return grouped;
    }

    // Collect all function entry points:
    // 1. Call targets (functions called by other code)
    // 2. Jump targets that cross existing function call targets
    std::set<uint32_t> function_entries;

    // Add call targets
    for (const auto& entry : costs) {
        const InstructionCost& cost = entry.second;
        for (const auto& call_entry : cost.calls) {
            function_entries.insert(call_entry.first);
        }
    }

    // Add jump targets that cross function call targets
    // If a jump from A to C crosses a function entry B (where B is between A and C),
    // then C should also be a function entry point
    // Track which jumps crossed function boundaries for later synthesis
    std::set<std::pair<uint32_t, uint32_t>> cross_boundary_jumps;  // (source, target) pairs

    for (const auto& entry : costs) {
        uint32_t source_addr = entry.first;
        const InstructionCost& cost = entry.second;

        for (const auto& jump_entry : cost.jumps) {
            uint32_t target_addr = jump_entry.first;
            // Check if this jump crosses any function entries
            uint32_t min_addr = std::min(source_addr, target_addr);
            uint32_t max_addr = std::max(source_addr, target_addr);

            // Find if there's any function entry between source and target (exclusive)
            auto lower = function_entries.upper_bound(min_addr);
            auto upper = function_entries.lower_bound(max_addr);

            if (lower != upper) {
                // There's at least one function entry between source and target
                // Mark the jump target as a function entry point
                function_entries.insert(target_addr);
                // Track this jump for call synthesis
                cross_boundary_jumps.insert(std::make_pair(source_addr, target_addr));
            }
        }
    }

    // Build a map from each instruction to its function (nearest entry point <= address)
    std::map<uint32_t, uint32_t> instruction_to_function;
    for (const auto& entry : costs) {
        uint32_t address = entry.first;

        // Find the largest function entry <= this address
        auto it = function_entries.upper_bound(address);
        if (it != function_entries.begin()) {
            --it;
            instruction_to_function[address] = *it;
        } else {
            // No function entry before this address - treat as its own function
            instruction_to_function[address] = address;
        }
    }

    // Group instructions by function
    std::map<uint32_t, std::vector<std::pair<uint32_t, InstructionCost>>> temp_functions;
    for (const auto& entry : costs) {
        uint32_t address = entry.first;
        uint32_t func_addr = instruction_to_function[address];
        temp_functions[func_addr].push_back({address, entry.second});
    }

    // Build GroupedFunction objects
    for (const auto& func_entry : temp_functions) {
        uint32_t func_addr = func_entry.first;
        const auto& instructions = func_entry.second;

        GroupedFunction func;
        func.entry_address = func_addr;
        func.total_self_instructions = 0;
        func.total_self_instr_fetches = 0;
        func.total_self_data_reads = 0;
        func.total_self_data_writes = 0;

        // Add all instructions and accumulate costs
        for (const auto& instr : instructions) {
            uint32_t addr = instr.first;
            const InstructionCost& cost = instr.second;

            func.instructions.push_back({addr, cost});
            func.total_self_instructions += cost.self_cost;
            func.total_self_instr_fetches += cost.instr_fetches;
            func.total_self_data_reads += cost.data_reads;
            func.total_self_data_writes += cost.data_writes;

            // Process calls from this instruction
            for (const auto& call_entry : cost.calls) {
                uint32_t target_addr = call_entry.first;
                const auto& call_info = call_entry.second;

                // Find which function the target belongs to
                auto it = instruction_to_function.find(target_addr);
                if (it != instruction_to_function.end()) {
                    uint32_t target_func = it->second;

                    FunctionCall fc;
                    fc.caller_address = addr;
                    fc.target_function = target_func;
                    fc.call_count = call_info.call_count;
                    fc.inclusive_instructions = call_info.inclusive_instructions;
                    fc.inclusive_instr_fetches = call_info.inclusive_instr_fetches;
                    fc.inclusive_data_reads = call_info.inclusive_data_reads;
                    fc.inclusive_data_writes = call_info.inclusive_data_writes;
                    func.calls.push_back(std::move(fc));
                }
            }

            // Process jumps from this instruction - synthesize calls for cross-boundary jumps
            // Only for jumps that were identified as crossing function call targets
            for (const auto& jump_entry : cost.jumps) {
                uint32_t target_addr = jump_entry.first;
                const auto& jump_info = jump_entry.second;

                // Check if this specific jump (source, target) crossed a boundary
                if (cross_boundary_jumps.count(std::make_pair(addr, target_addr)) == 0) {
                    continue;  // Skip jumps that didn't cross function call targets
                }

                // Find which function the target belongs to
                auto it = instruction_to_function.find(target_addr);
                if (it != instruction_to_function.end()) {
                    uint32_t target_func = it->second;

                    FunctionCall fc;
                    fc.caller_address = addr;
                    fc.target_function = target_func;
                    fc.call_count = jump_info.call_count;
                    fc.inclusive_instructions = jump_info.inclusive_instructions;
                    fc.inclusive_instr_fetches = jump_info.inclusive_instr_fetches;
                    fc.inclusive_data_reads = jump_info.inclusive_data_reads;
                    fc.inclusive_data_writes = jump_info.inclusive_data_writes;
                    func.calls.push_back(std::move(fc));
                }
            }
        }

        grouped.AddFunction(func_addr, std::move(func));
    }

    return grouped;
}

} // namespace Profiler
