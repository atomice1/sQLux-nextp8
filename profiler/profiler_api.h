// C glue header for profiler - C-compatible interface

#ifndef PROFILER_API_H
#define PROFILER_API_H

#ifdef __cplusplus
extern "C" {
#endif

// Initialize the profiler system
void Profiler_Initialize(void);

// Shutdown the profiler system
void Profiler_Shutdown(void);

#ifdef __cplusplus
}
#endif

#endif // PROFILER_API_H
