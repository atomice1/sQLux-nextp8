// Profiler data invariant checker

#ifndef PROFILER_INVARIANTS_H
#define PROFILER_INVARIANTS_H

namespace Profiler {

class ProfilerData;
class GroupedProfilerData;

// Check profiler data invariants and report violations
bool CheckProfilerInvariants(const ProfilerData& data);

// Check grouped profiler data invariants (after function grouping)
bool CheckGroupedInvariants(const GroupedProfilerData& data);

} // namespace Profiler

#endif // PROFILER_INVARIANTS_H
