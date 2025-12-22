// Callgrind file format serializer implementation

#include "profiler_callgrind.h"
#include "profiler_grouped.h"
#include <cassert>
#include <iomanip>
#include <iostream>

namespace Profiler {

CallgrindSerializer::CallgrindSerializer() {
}

CallgrindSerializer::~CallgrindSerializer() {
}

bool CallgrindSerializer::WriteToFile(const std::string& filename, const GroupedProfilerData& data) {
    std::ofstream out(filename);
    if (!out.is_open()) {
        std::cerr << "Failed to open callgrind output file: " << filename << std::endl;
        return false;
    }

    WriteHeader(out, data);
    WriteBody(out, data);

    out.close();
    return true;
}

void CallgrindSerializer::WriteHeader(std::ofstream& out, const GroupedProfilerData& data) {
    // Format marker
    out << "# callgrind format\n";
    out << "version: 1\n";
    out << "creator: sqlux-profiler\n";
    out << "cmd: sqlux\n";
    out << "\n";

    // Position specification - we only track instruction addresses
    out << "positions: instr\n";

    // Event types - Cycles, Instructions, DataReads, DataWrites
    out << "events: Cycles Instructions DataReads DataWrites\n";

    // Calculate totals across all functions
    uint64_t total_instructions = 0;
    uint64_t total_instr_fetches = 0;
    uint64_t total_data_reads = 0;
    uint64_t total_data_writes = 0;

    for (const auto& func_entry : data.GetFunctions()) {
        const GroupedFunction& func = func_entry.second;
        total_instructions += func.total_self_instructions;
        total_instr_fetches += func.total_self_instr_fetches;
        total_data_reads += func.total_self_data_reads;
        total_data_writes += func.total_self_data_writes;
    }

    // Cycles = Instructions + InstructionFetch * 3 + DataReads * 3 + DataWrites * 3
    uint64_t total_cycles = total_instructions + (total_instr_fetches * 3) +
                           (total_data_reads * 3) + (total_data_writes * 3);

    // Summary
    out << "summary: " << total_cycles << " " << total_instructions << " "
        << total_data_reads << " " << total_data_writes << "\n";
    out << "\n";
}

void CallgrindSerializer::WriteBody(std::ofstream& out, const GroupedProfilerData& data) {
    const auto& functions = data.GetFunctions();

    if (functions.empty()) {
        return;
    }

    // Write each function's costs
    for (const auto& func_entry : functions) {
        uint32_t func_addr = func_entry.first;
        const GroupedFunction& func = func_entry.second;

        // Write function header
        out << "fn=0x" << std::hex << func_addr << std::dec << "\n";

        uint32_t last_address = 0;

        // Build a map of caller addresses to calls for this function
        std::map<uint32_t, std::vector<const FunctionCall*>> calls_by_address;
        for (const auto& call : func.calls) {
            calls_by_address[call.caller_address].push_back(&call);
        }

        // Write costs for all instructions in this function
        for (const auto& instr : func.instructions) {
            uint32_t address = instr.address;
            const InstructionCost& cost = instr.cost;

            // Write self-cost line with address (absolute or relative)
            if (last_address == 0) {
                // First address - write absolute
                out << "0x" << std::hex << address << std::dec;
            } else {
                // Use relative addressing if close enough
                int64_t diff = static_cast<int64_t>(address) - static_cast<int64_t>(last_address);
                if (diff > 0 && diff <= 1000) {
                    out << "+" << diff;
                } else if (diff < 0 && diff >= -1000) {
                    out << diff;
                } else if (diff == 0) {
                    out << "*";
                } else {
                    // Large jump - use absolute
                    out << "0x" << std::hex << address << std::dec;
                }
            }

            // Write self cost values: Cycles Instructions DataReads DataWrites
            // Cycles = Instructions + InstructionFetch * 3 + DataReads * 3 + DataWrites * 3
            uint64_t cycles = cost.self_cost + (cost.instr_fetches * 3) + (cost.data_reads * 3) + (cost.data_writes * 3);
            out << " " << cycles
                << " " << cost.self_cost
                << " " << cost.data_reads
                << " " << cost.data_writes
                << "\n";

            last_address = address;

            // Write any calls from this instruction
            auto calls_it = calls_by_address.find(address);
            if (calls_it != calls_by_address.end()) {
                for (const FunctionCall* call : calls_it->second) {
                    // cfn= line (called function)
                    out << "cfn=0x" << std::hex << call->target_function << std::dec << "\n";

                    // calls= line (count and target position)
                    out << "calls=" << call->call_count
                        << " 0x" << std::hex << call->target_function << std::dec
                        << "\n";

                    // Cost line for the call (source position and inclusive costs)
                    // The position must be the caller address
                    uint64_t incl_cycles = call->inclusive_instructions +
                                          (call->inclusive_instr_fetches * 3) +
                                          (call->inclusive_data_reads * 3) +
                                          (call->inclusive_data_writes * 3);
                    out << "0x" << std::hex << call->caller_address << std::dec
                        << " " << incl_cycles
                        << " " << call->inclusive_instructions
                        << " " << call->inclusive_data_reads
                        << " " << call->inclusive_data_writes
                        << "\n";

                    // Update last_address since we just wrote the caller address
                    last_address = call->caller_address;
                }
            }
        }

        out << "\n";
    }
}

} // namespace Profiler
