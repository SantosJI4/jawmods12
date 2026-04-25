#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <string>
#include <string.h>
#include <limits.h>
#include <inttypes.h>
#include <vector>
#include <sstream>
#include "dobby.h"

uintptr_t libBaseAddress = 0;
uintptr_t libBaseEndAddress = 0;

#if defined(__aarch64__)
#define SplitApk "split_config.arm64_v8a.apk"
#else
#define SplitApk "split_config.armeabi_v7a.apk"
#endif

uintptr_t findLibrary(const char *library) {
    char filename[0xFF] = {0},
            buffer[1024] = {0};
    FILE *fp = NULL;
    uintptr_t address = 0, start = 0, end = 0;
    char flags[7], path[PATH_MAX];
    pid_t pid = getpid();
    sprintf(filename, "/proc/%d/maps", pid);
    fp = fopen(filename, "r");
    if (fp == NULL) {
        perror("fopen");
        goto done;
    }
    while (fgets(buffer, sizeof(buffer), fp)) {
        strcpy(path, "");
        sscanf(buffer, "%" PRIxPTR "-%" PRIxPTR " %s %*x %*x:%*x %*u %s\n", &start, &end, flags, path);
        if (strstr(flags, "r-xp") == 0) continue;
        if (strstr(path, library)) {
            address = start;
            goto done;
        }
    }
    done:
    if (fp) {
        fclose(fp);
    }
    return address;
}

bool isLibraryLoaded(const char *libraryName) {
    char line[512] = {0};
    FILE *fp = fopen("/proc/self/maps", "rt");
    if (fp != NULL) {
        while (fgets(line, sizeof(line), fp)) {
            std::string a = line;
            if (strstr(line, libraryName)) {
                return true;
            }
        }
        fclose(fp);
    }
    return false;
}

uintptr_t getAbsoluteAddress(uintptr_t addr) {
    if (addr == 0)
        return 0;
    return (libBaseAddress + addr);
}

#define LIB "libil2cpp.so"
#define gAA getAbsoluteAddress
#define DHOOK(offset, ptr, orig) DobbyHook((void*)gAA(offset), (void*)ptr, (void **)&orig)
#define MHOOK(offset, ptr, orig) DobbyHook((void*)offset, (void*)ptr, (void **)&orig)