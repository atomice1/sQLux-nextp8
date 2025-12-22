// Profiler data structures implementation

#include "profiler_data.h"
#include <algorithm>

namespace Profiler {

ProfilerData::ProfilerData()
    : current_pc_(0), total_instructions_(0) {
}

ProfilerData::~ProfilerData() {
}

void ProfilerData::Clear() {
    instruction_costs_.clear();
    call_stack_.clear();
    current_pc_ = 0;
    total_instructions_ = 0;
}

void ProfilerData::ProcessEvent(uint32_t event) {
    EventType type = GetEventType(event);
    uint32_t address = GetEventAddress(event);

    switch (type) {
        case EventType::INSTR_EXECUTE:
            ProcessInstructionExecute(address);
            break;
        case EventType::JUMP:
            ProcessJump(address);
            break;
        case EventType::CALL:
            ProcessCall(address, GetReturnOffset(event));
            break;
        case EventType::RETURN:
            ProcessReturn(address);
            break;
        case EventType::DATA_READ:
            ProcessDataRead(address);
            break;
        case EventType::DATA_WRITE:
            ProcessDataWrite(address);
            break;
        case EventType::INSTR_READ:
            ProcessInstrRead(address);
            break;
    }
}

void ProfilerData::ProcessInstructionExecute(uint32_t address) {
    current_pc_ = address;
    instruction_costs_[address].self_cost++;
    total_instructions_++;

    // Create top-level call frame if stack is empty
    if (call_stack_.empty()) {
        // Push a top-level frame with no caller or return address
        // Use a dummy CallInfo for top-level frames
        auto& dummy_call_info = instruction_costs_[0].calls[address];
        dummy_call_info.call_count++;
        call_stack_.emplace_back(address, 0, 0, dummy_call_info);
    }

    // Increment inclusive costs for ALL frames in the stack
    for (auto& frame : call_stack_) {
        frame.call_info.inclusive_instructions++;
        // Also increment for all active jumps in this frame
        for (auto& jump_entry : frame.jump_refs) {
            jump_entry.second.get().inclusive_instructions++;
        }
    }
}

void ProfilerData::ProcessJump(uint32_t address) {
    // Record the jump from current_pc_ to address and get reference to CallInfo
    auto& jump_info = instruction_costs_[current_pc_].jumps[address];
    jump_info.call_count++;

    // Add jump reference ONLY to the current (top) frame, not all frames
    // This ensures jump costs only accumulate while in the function where the jump occurred
    if (!call_stack_.empty()) {
        call_stack_.back().jump_refs.insert_or_assign(std::make_pair(current_pc_, address), std::ref(jump_info));
    }

    // Jumps don't change the call stack
    current_pc_ = address;
}

void ProfilerData::ProcessCall(uint32_t address, uint32_t return_offset) {
    // Record the call from current_pc_ to address
    auto& call_info = instruction_costs_[current_pc_].calls[address];
    if (current_pc_ != 0)
        call_info.call_count++;

    // Push a new call frame with expected return address and reference to CallInfo
    // Return address is the PC after the call instruction (current_pc_ + 2)
    // caller_pc is the call instruction itself (current_pc_)
    call_stack_.emplace_back(address, current_pc_, current_pc_ + 2 + return_offset, call_info);

    current_pc_ = address;
}

void ProfilerData::ProcessReturn(uint32_t address) {
    if (call_stack_.empty()) {
        current_pc_ = address;
        return;
    }

    // Check if this is a normal return or longjmp
    // Normal return: address matches the expected return address of top frame
    if (call_stack_.back().return_address == address) {
        // Normal return - pop one frame
        CallFrame frame = call_stack_.back();
        call_stack_.pop_back();
    } else {
        printf("DEBUG: returning to %x (expected %x) from %x\n", address, call_stack_.back().return_address, current_pc_);
        printf("LONGJMP DETECTED\n");
        // Longjmp detected - search for matching return address and unwind
        bool found = false;
        size_t frames_to_pop = 0;

        // Search backwards through stack for matching return address
        for (size_t i = call_stack_.size(); i > 0; --i) {
            if (call_stack_[i - 1].return_address == address) {
                frames_to_pop = call_stack_.size() - (i - 1);
                found = true;
                break;
            }
        }

        if (found) {
            // Unwind multiple frames (longjmp)
            for (size_t i = 0; i < frames_to_pop; ++i) {
                CallFrame frame = call_stack_.back();
                call_stack_.pop_back();
            }
        } else {
            // Return address not found in stack - corrupted or bottom of stack
            // Pop all frames
            while (!call_stack_.empty())
                call_stack_.pop_back();
        }
    }

    current_pc_ = address;
}

void ProfilerData::ProcessDataRead(uint32_t address) {
    if (current_pc_ != 0) {
        instruction_costs_[current_pc_].data_reads++;
    }
    for (auto& frame : call_stack_) {
        frame.call_info.inclusive_data_reads++;
        // Also increment for all active jumps in this frame
        for (auto& jump_entry : frame.jump_refs) {
            jump_entry.second.get().inclusive_data_reads++;
        }
    }
}

void ProfilerData::ProcessDataWrite(uint32_t address) {
    if (current_pc_ != 0) {
        instruction_costs_[current_pc_].data_writes++;
    }

    for (auto& frame : call_stack_) {
        frame.call_info.inclusive_data_writes++;
        // Also increment for all active jumps in this frame
        for (auto& jump_entry : frame.jump_refs) {
            jump_entry.second.get().inclusive_data_writes++;
        }
    }
}

void ProfilerData::ProcessInstrRead(uint32_t address) {
    if (current_pc_ != 0) {
        instruction_costs_[current_pc_].instr_fetches++;
    }

    for (auto& frame : call_stack_) {
        frame.call_info.inclusive_instr_fetches++;
        // Also increment for all active jumps in this frame
        for (auto& jump_entry : frame.jump_refs) {
            jump_entry.second.get().inclusive_instr_fetches++;
        }
    }
}

void ProfilerData::Finalize() {
}

} // namespace Profiler
