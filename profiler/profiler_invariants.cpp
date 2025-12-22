// Profiler data invariant checker implementation

#include "profiler_invariants.h"
#include "profiler_data.h"
#include "profiler_grouped.h"
#include <iostream>
#include <iomanip>
#include <set>
#include <map>

namespace Profiler {

bool CheckProfilerInvariants(const ProfilerData& data) {
    const auto& costs = data.GetInstructionCosts();
    uint64_t total_instructions = data.GetTotalInstructions();

    std::cout << "\n=== Profiler Invariant Check ===\n";
    std::cout << "Total instructions: " << total_instructions << "\n";
    std::cout << "Total instruction addresses: " << costs.size() << "\n";

    bool valid = true;

    // Invariant: Sum of all self costs should equal total instructions
    uint64_t sum_self = 0;
    for (const auto& entry : costs) {
        sum_self += entry.second.self_cost;
    }

    if (sum_self != total_instructions) {
        std::cerr << "ERROR: Sum of self costs (" << sum_self
                  << ") != total_instructions (" << total_instructions << ")\n";
        valid = false;
    }

    // Basic sanity: check for negative or unreasonably large values
    for (const auto& entry : costs) {
        uint32_t addr = entry.first;
        const InstructionCost& cost = entry.second;

        if (cost.self_cost > total_instructions) {
            std::cerr << "ERROR: Address 0x" << std::hex << addr << std::dec
                      << " has self_cost (" << cost.self_cost
                      << ") > total_instructions (" << total_instructions << ")\n";
            valid = false;
        }

        // Check calls have non-negative inclusive costs
        for (const auto& call_entry : cost.calls) {
            const auto& call_info = call_entry.second;
            if (call_info.inclusive_instructions < 0 || call_info.inclusive_instr_fetches < 0 ||
                call_info.inclusive_data_reads < 0 || call_info.inclusive_data_writes < 0) {
                std::cerr << "ERROR: Address 0x" << std::hex << addr << std::dec
                          << " has call with negative inclusive costs\n";
                valid = false;
            }
        }
    }

    std::cout << "Invariant check: " << (valid ? "PASSED" : "FAILED") << "\n";
    std::cout << "=================================\n\n";

    return valid;
}

bool CheckGroupedInvariants(const GroupedProfilerData& data) {
    bool valid = true;
    const auto& functions = data.GetFunctions();
    uint64_t total_instructions = data.GetTotalInstructions();

    std::cout << "\n=== Grouped Data Invariant Check ===\n";
    std::cout << "Total instructions: " << total_instructions << "\n";
    std::cout << "Total functions: " << functions.size() << "\n";

    // Build map of incoming calls to each function
    std::map<uint32_t, std::vector<const FunctionCall*>> incoming_calls;
    for (const auto& entry : functions) {
        for (const auto& call : entry.second.calls) {
            incoming_calls[call.target_function].push_back(&call);
        }
    }

    // Check each function
    for (const auto& entry : functions) {
        uint32_t func_addr = entry.first;
        const GroupedFunction& func = entry.second;

        // Invariant 1: self costs should sum correctly
        uint64_t expected_self = 0;
        for (const auto& instr : func.instructions) {
            expected_self += instr.cost.self_cost;
        }

        if (expected_self != func.total_self_instructions) {
            std::cerr << "ERROR: Function 0x" << std::hex << func_addr << std::dec
                      << " total_self_instructions (" << func.total_self_instructions
                      << ") != sum of instruction costs (" << expected_self << ")\n";
            valid = false;
        }

        // Invariant 2: each call's inclusive should be non-negative and reasonable
        for (const auto& call : func.calls) {
            if (call.inclusive_instructions < 0 || call.inclusive_instr_fetches < 0 ||
                call.inclusive_data_reads < 0 || call.inclusive_data_writes < 0) {
                std::cerr << "ERROR: Function 0x" << std::hex << func_addr << std::dec
                          << " has call to 0x" << std::hex << call.target_function << std::dec
                          << " with negative inclusive costs\n";
                valid = false;
            }

            // Verify target function exists
            if (functions.find(call.target_function) == functions.end()) {
                std::cerr << "ERROR: Function 0x" << std::hex << func_addr << std::dec
                          << " calls unknown function 0x" << std::hex << call.target_function << std::dec << "\n";
                valid = false;
            }
        }

        // Invariant 3: sum of incoming calls should approximately equal function's total inclusive
        auto incoming_it = incoming_calls.find(func_addr);
        if (incoming_it != incoming_calls.end()) {
            uint64_t sum_incoming = 0;
            for (const auto* call : incoming_it->second) {
                sum_incoming += call->inclusive_instructions;
            }

            // Calculate this function's total inclusive (self + all outgoing calls)
            uint64_t func_inclusive = func.total_self_instructions;
            for (const auto& call : func.calls) {
                func_inclusive += call.inclusive_instructions;
            }

            // Allow 10% tolerance for recursion and rounding
            if (sum_incoming < func_inclusive * 0.9 || sum_incoming > func_inclusive * 1.1) {
                std::cerr << "WARNING: Function 0x" << std::hex << func_addr << std::dec
                          << ": sum of incoming calls (" << sum_incoming
                          << ") differs significantly from function total inclusive (" << func_inclusive << ")\n";
                std::cerr << "  This may indicate recursion or missing call tracking\n";
            }
        }
    }

    // Invariant 4: verify sum of all self costs equals total
    uint64_t sum_all_self = 0;
    for (const auto& entry : functions) {
        sum_all_self += entry.second.total_self_instructions;
    }

    if (sum_all_self != total_instructions) {
        std::cerr << "ERROR: Sum of all function self costs (" << sum_all_self
                  << ") != total_instructions (" << total_instructions << ")\n";
        valid = false;
    }

    // Invariant 5: check entry points (tracked from empty call stack execution)
    const auto& entry_points = data.GetEntryPoints();
    std::cout << "Found " << entry_points.size() << " entry points (executed with empty stack):\n";

    uint64_t entry_point_total = 0;
    for (uint32_t addr : entry_points) {
        auto func_it = functions.find(addr);
        if (func_it == functions.end()) {
            std::cerr << "ERROR: Entry point 0x" << std::hex << addr << std::dec
                      << " is not mapped to any function!\n";
            valid = false;
            continue;
        }

        const auto& func = func_it->second;
        uint64_t func_inclusive = func.total_self_instructions;
        for (const auto& call : func.calls) {
            func_inclusive += call.inclusive_instructions;
        }
        entry_point_total += func_inclusive;
        std::cout << "  0x" << std::hex << addr << std::dec
                  << ": " << func_inclusive << " instructions ("
                  << std::fixed << std::setprecision(2)
                  << (100.0 * func_inclusive / total_instructions) << "%)\n";
    }

    std::cout << "Total from entry points: " << entry_point_total
              << " (" << std::fixed << std::setprecision(2)
              << (100.0 * entry_point_total / total_instructions) << "%)\n";

    // Check that entry points account for ~100% of costs
    if (entry_point_total < total_instructions * 0.99) {
        std::cerr << "ERROR: Entry points account for " << entry_point_total
                  << " (" << std::fixed << std::setprecision(2)
                  << (100.0 * entry_point_total / total_instructions)
                  << "%) of costs, expected ~100%!\n";
        valid = false;
    } else if (entry_point_total > total_instructions * 1.01) {
        std::cerr << "ERROR: Entry points account for " << entry_point_total
                  << " (" << std::fixed << std::setprecision(2)
                  << (100.0 * entry_point_total / total_instructions)
                  << "%) of costs, expected ~100% (possible recursion in entry points)\n";
        valid = false;
    }

    std::cout << "Grouped invariant check: " << (valid ? "PASSED" : "FAILED") << "\n";
    std::cout << "=====================================\n\n";

    return valid;
}

} // namespace Profiler
