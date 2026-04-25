#pragma once

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/uio.h>
#include <cstdlib>
#include <ctime>

/*
 * MemoryReader - Leitura externa STEALTH via /proc/<pid>/mem
 * 
 * REGRAS:
 *   - APENAS leitura (RPM) - NUNCA escrita (WPM)
 *   - Lê em bulk (uma chamada grande) ao invés de muitas pequenas
 *   - Jitter no timing para evitar padrões detectáveis
 *   - Usa process_vm_readv (syscall direto, menos rastreável)
 *   - Cache de moduleBase para evitar re-scan de /proc/maps
 *   - Requer ROOT
 */
class MemoryReader {
public:
    static MemoryReader& get() {
        static MemoryReader instance;
        return instance;
    }

    bool attach(const char* packageName) {
        pid = findPidByName(packageName);
        if (pid <= 0) return false;
        attached = true;
        cachedIl2cppBase = 0; // reset cache
        return true;
    }

    void detach() {
        pid = -1;
        attached = false;
        cachedIl2cppBase = 0;
    }

    bool isAttached() const { return attached && pid > 0; }
    pid_t getPid() const { return pid; }

    // ========================================
    // RPM - ReadProcessMemory via process_vm_readv
    // Uma única syscall, sem abrir /proc/<pid>/mem como fd
    // ========================================
    bool rpm(uintptr_t address, void* buffer, size_t size) {
        if (!isAttached() || !address || !buffer || !size) return false;

        struct iovec local;
        struct iovec remote;

        local.iov_base = buffer;
        local.iov_len = size;
        remote.iov_base = reinterpret_cast<void*>(address);
        remote.iov_len = size;

        ssize_t nread = process_vm_readv(pid, &local, 1, &remote, 1, 0);
        return nread == static_cast<ssize_t>(size);
    }

    // ========================================
    // Bulk RPM - Lê múltiplas regiões em UMA syscall
    // Muito mais stealth que várias chamadas individuais
    // ========================================
    bool rpmBulk(uintptr_t* addresses, void** buffers, size_t* sizes, int count) {
        if (!isAttached() || count <= 0 || count > 16) return false;

        struct iovec local[16];
        struct iovec remote[16];

        for (int i = 0; i < count; i++) {
            local[i].iov_base = buffers[i];
            local[i].iov_len = sizes[i];
            remote[i].iov_base = reinterpret_cast<void*>(addresses[i]);
            remote[i].iov_len = sizes[i];
        }

        ssize_t nread = process_vm_readv(pid, local, count, remote, count, 0);
        return nread > 0;
    }

    // Lê um tipo genérico
    template<typename T>
    T read(uintptr_t address) {
        T result{};
        rpm(address, &result, sizeof(T));
        return result;
    }

    // Lê um ponteiro (seguir cadeia de ponteiros)
    uintptr_t readPtr(uintptr_t address) {
        return read<uintptr_t>(address);
    }

    // Segue cadeia de ponteiros: base → [+off1] → [+off2] → ... → [+offN]
    uintptr_t readChain(uintptr_t base, const std::vector<uintptr_t>& offsets) {
        uintptr_t addr = base;
        for (size_t i = 0; i < offsets.size(); i++) {
            addr = readPtr(addr + offsets[i]);
            if (addr == 0) return 0;
        }
        return addr;
    }

    // ========================================
    // Module Base com cache
    // ========================================
    uintptr_t getModuleBase(const char* moduleName) {
        // Cache para libil2cpp.so (módulo mais acessado)
        if (cachedIl2cppBase != 0 && strcmp(moduleName, "libil2cpp.so") == 0) {
            return cachedIl2cppBase;
        }

        uintptr_t base = scanModuleBase(moduleName);

        if (strcmp(moduleName, "libil2cpp.so") == 0) {
            cachedIl2cppBase = base;
        }

        return base;
    }

    uintptr_t getAbsolute(const char* moduleName, uintptr_t offset) {
        uintptr_t base = getModuleBase(moduleName);
        return base ? (base + offset) : 0;
    }

    bool isModuleLoaded(const char* moduleName) {
        return scanModuleBase(moduleName) != 0;
    }

    // ========================================
    // Verifica se o processo ainda existe
    // ========================================
    bool isProcessAlive() {
        if (pid <= 0) return false;
        char path[32];
        snprintf(path, sizeof(path), "/proc/%d", pid);
        return access(path, F_OK) == 0;
    }

    // ========================================
    // Stealth: jitter no delay (evita padrão fixo)
    // ========================================
    static void stealthSleep(int baseUs) {
        int jitter = (rand() % (baseUs / 4)) - (baseUs / 8);
        usleep(baseUs + jitter);
    }

private:
    MemoryReader() { srand(time(nullptr)); }
    ~MemoryReader() { detach(); }
    MemoryReader(const MemoryReader&) = delete;
    MemoryReader& operator=(const MemoryReader&) = delete;

    uintptr_t scanModuleBase(const char* moduleName) {
        if (pid <= 0) return 0;

        char mapsPath[64];
        snprintf(mapsPath, sizeof(mapsPath), "/proc/%d/maps", pid);

        FILE* fp = fopen(mapsPath, "r");
        if (!fp) return 0;

        char line[512];
        uintptr_t base = 0;

        while (fgets(line, sizeof(line), fp)) {
            if (strstr(line, moduleName) && strstr(line, "r-xp")) {
                base = static_cast<uintptr_t>(strtoull(line, nullptr, 16));
                break;
            }
        }

        fclose(fp);
        return base;
    }

    pid_t findPidByName(const char* packageName) {
        DIR* dir = opendir("/proc");
        if (!dir) return -1;

        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (entry->d_type != DT_DIR) continue;

            bool isNumeric = true;
            for (const char* p = entry->d_name; *p; ++p) {
                if (*p < '0' || *p > '9') { isNumeric = false; break; }
            }
            if (!isNumeric) continue;

            char cmdlinePath[64];
            snprintf(cmdlinePath, sizeof(cmdlinePath), "/proc/%s/cmdline", entry->d_name);

            FILE* fp = fopen(cmdlinePath, "r");
            if (!fp) continue;

            char cmdline[256] = {0};
            fgets(cmdline, sizeof(cmdline), fp);
            fclose(fp);

            if (strcmp(cmdline, packageName) == 0) {
                closedir(dir);
                return atoi(entry->d_name);
            }
        }

        closedir(dir);
        return -1;
    }

    pid_t pid = -1;
    bool attached = false;
    uintptr_t cachedIl2cppBase = 0;
};
