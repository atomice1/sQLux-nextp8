// Profiler data structures for intermediate representation

#ifndef PROFILER_DATA_H
#define PROFILER_DATA_H

#include <cstdint>
#include <map>
#include <vector>
#include <string>
#include <set>
#include <functional>

namespace Profiler {

// Event types (top 8 bits of 32-bit event)
enum class EventType : uint8_t {
    INSTR_EXECUTE = 0x00,
    JUMP = 0x01,
    CALL = 0x02,
    RETURN = 0x03,
    DATA_READ = 0x04,
    DATA_WRITE = 0x05,
    INSTR_READ = 0x06
};

// Extract event type from 32-bit event
inline EventType GetEventType(uint32_t event) {
    return static_cast<EventType>((event >> 28) & 0xF);
}

// Extract return offset from CALL event
inline uint32_t GetReturnOffset(uint32_t event) {
    return (event >> 24) & 0xF;
}

// Extract address from 32-bit event
inline uint32_t GetEventAddress(uint32_t event) {
    return event & 0x00FFFFFF;
}

// Create a 32-bit event
inline uint32_t MakeEvent(EventType type, uint32_t address) {
    return (static_cast<uint32_t>(type) << 24) | (address & 0x00FFFFFF);
}

// Cost information for a single instruction address
struct InstructionCost {
    uint64_t self_cost;           // Instructions executed at this address
    uint64_t instr_fetches;       // Instruction fetches
    uint64_t data_reads;          // Data reads
    uint64_t data_writes;         // Data writes

    // Calls from this instruction
    struct CallInfo {
        uint64_t call_count;
        uint64_t inclusive_instructions;
        uint64_t inclusive_instr_fetches;
        uint64_t inclusive_data_reads;
        uint64_t inclusive_data_writes;

        CallInfo() : call_count(0),
                     inclusive_instructions(0), inclusive_instr_fetches(0),
                     inclusive_data_reads(0), inclusive_data_writes(0) {}
    };

    std::map<uint32_t, CallInfo> calls;  // target_address -> CallInfo
    std::map<uint32_t, CallInfo> jumps;  // target_address -> CallInfo for jumps

    InstructionCost() : self_cost(0), instr_fetches(0), data_reads(0), data_writes(0) {}
};


// Call context (stack frame)
struct CallFrame {
    uint32_t address;                      // Function entry address
    uint32_t caller_pc;                    // PC of call instruction (call site)
    uint32_t return_address;               // Expected return address
    InstructionCost::CallInfo& call_info;  // Reference to CallInfo in ProfilerData
    std::map<std::pair<uint32_t, uint32_t>, std::reference_wrapper<InstructionCost::CallInfo>> jump_refs;  // (source, target) -> CallInfo ref for active jumps

    CallFrame(uint32_t addr, uint32_t caller, uint32_t ret_addr, InstructionCost::CallInfo& ci)
        : address(addr), caller_pc(caller), return_address(ret_addr), call_info(ci) {}
};


// Main profiler data structure
class ProfilerData {
public:
    ProfilerData();
    ~ProfilerData();

    // Process a single event
    void ProcessEvent(uint32_t event);

    // Clear all data
    void Clear();

    // Get the instruction cost map
    const std::map<uint32_t, InstructionCost>& GetInstructionCosts() const {
        return instruction_costs_;
    }

    // Get total instruction count
    uint64_t GetTotalInstructions() const {
        return total_instructions_;
    }

    // Finalize profiling data
    void Finalize();

private:
    std::map<uint32_t, InstructionCost> instruction_costs_;
    std::vector<CallFrame> call_stack_;
    uint32_t current_pc_;      // Current instruction address
    uint64_t total_instructions_;

    void ProcessInstructionExecute(uint32_t address);
    void ProcessJump(uint32_t address);
    void ProcessCall(uint32_t address, uint32_t return_offset);
    void ProcessReturn(uint32_t address);
    void ProcessDataRead(uint32_t address);
    void ProcessDataWrite(uint32_t address);
    void ProcessInstrRead(uint32_t address);
};

} // namespace Profiler

#endif // PROFILER_DATA_H
