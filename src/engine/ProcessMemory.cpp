#include "ProcessMemory.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h> // GetProcessMemoryInfo is K32GetProcessMemoryInfo in kernel32 (PSAPI_VERSION 2)
#elif defined(__APPLE__)
#include <mach/mach.h>
#else
#include <cstdio>
#include <unistd.h>
#endif

namespace processMemory {

uint64_t residentBytes()
{
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS c{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &c, sizeof c)) return uint64_t(c.WorkingSetSize);
    return 0;
#elif defined(__APPLE__)
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS)
        return uint64_t(info.resident_size);
    return 0;
#else
    // Linux: /proc/self/statm, second field = resident pages
    FILE *f = std::fopen("/proc/self/statm", "r");
    if (!f) return 0;
    unsigned long long size = 0, resident = 0;
    const int n = std::fscanf(f, "%llu %llu", &size, &resident);
    std::fclose(f);
    const long page = sysconf(_SC_PAGESIZE);
    return n == 2 && page > 0 ? uint64_t(resident) * uint64_t(page) : 0;
#endif
}

} // namespace processMemory
