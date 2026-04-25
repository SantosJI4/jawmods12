/*
 * ============================================================
 * injector.c — ptrace-based .so injector for ARM64
 * ============================================================
 *
 * Injects a shared library into a running process using ptrace.
 * Similar to how GameGuardian, LibTool, and linux-inject work.
 *
 * METHOD:
 *   1. ptrace(ATTACH) to target PID
 *   2. Save original registers
 *   3. Find dlopen() address in target process via /proc/PID/maps
 *   4. Write .so path string to target's stack
 *   5. Set registers: x0=path, x1=RTLD_NOW, pc=dlopen, lr=0 (trap)
 *   6. ptrace(CONT) — target calls dlopen(), hits SIGSEGV at lr=0
 *   7. Restore original registers
 *   8. ptrace(DETACH)
 *
 * The loaded .so's __attribute__((constructor)) runs automatically.
 *
 * USAGE: ./injector <PID> <path_to_so>
 *
 * Based on techniques from:
 *   - https://github.com/gaffe23/linux-inject
 *   - https://github.com/topjohnwu/MagiskHide (ptrace)
 *   - LibTool / GameGuardian injection method
 * ============================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <signal.h>
#include <dirent.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <elf.h>
#include <linux/elf.h>

#define LOG_TAG "injector"
#define LOGI(...) fprintf(stdout, __VA_ARGS__); fprintf(stdout, "\n"); fflush(stdout)
#define LOGE(...) fprintf(stderr, "ERROR: " __VA_ARGS__); fprintf(stderr, "\n"); fflush(stderr)

/* ARM64 user_regs_struct layout */
struct arm64_regs {
    unsigned long long regs[31]; /* x0-x30 */
    unsigned long long sp;
    unsigned long long pc;
    unsigned long long pstate;
};

/*
 * Read registers from target via ptrace
 */
static int get_regs(pid_t pid, struct arm64_regs *regs) {
    struct iovec iov;
    iov.iov_base = regs;
    iov.iov_len = sizeof(*regs);
    if (ptrace(PTRACE_GETREGSET, pid, (void*)NT_PRSTATUS, &iov) < 0) {
        LOGE("GETREGSET failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

/*
 * Write registers to target via ptrace
 */
static int set_regs(pid_t pid, struct arm64_regs *regs) {
    struct iovec iov;
    iov.iov_base = regs;
    iov.iov_len = sizeof(*regs);
    if (ptrace(PTRACE_SETREGSET, pid, (void*)NT_PRSTATUS, &iov) < 0) {
        LOGE("SETREGSET failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

/*
 * Read memory from target process via /proc/PID/mem
 * More reliable than PTRACE_PEEKDATA for large reads
 */
static int read_mem(pid_t pid, unsigned long long addr, void *buf, size_t len) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/mem", pid);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    if (lseek64(fd, (off64_t)addr, SEEK_SET) == (off64_t)-1) {
        close(fd);
        return -1;
    }
    ssize_t n = read(fd, buf, len);
    close(fd);
    return (n == (ssize_t)len) ? 0 : -1;
}

/*
 * Write memory to target process via ptrace (word at a time)
 */
static int write_mem(pid_t pid, unsigned long long addr, const void *buf, size_t len) {
    const unsigned long long *src = (const unsigned long long *)buf;
    size_t i;

    /* Write 8 bytes at a time */
    for (i = 0; i + 8 <= len; i += 8) {
        if (ptrace(PTRACE_POKEDATA, pid, (void*)(addr + i), (void*)src[i/8]) < 0) {
            LOGE("POKEDATA at %llx failed: %s", addr + i, strerror(errno));
            return -1;
        }
    }

    /* Handle remaining bytes (read-modify-write) */
    if (i < len) {
        unsigned long long word = 0;
        errno = 0;
        word = ptrace(PTRACE_PEEKDATA, pid, (void*)(addr + i), NULL);
        if (errno) {
            LOGE("PEEKDATA at %llx failed: %s", addr + i, strerror(errno));
            return -1;
        }
        memcpy(&word, (const char*)buf + i, len - i);
        if (ptrace(PTRACE_POKEDATA, pid, (void*)(addr + i), (void*)word) < 0) {
            LOGE("POKEDATA tail at %llx failed: %s", addr + i, strerror(errno));
            return -1;
        }
    }

    return 0;
}

/*
 * Find the base address of a library in a process's memory map.
 * Returns the FIRST r-xp mapping — used for dlsym offset calculations.
 */
static unsigned long long find_lib_base(pid_t pid, const char *lib_name) {
    char path[64], line[512];
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;

    unsigned long long addr = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, lib_name) && strstr(line, "r-xp")) {
            /* First r-xp mapping of the library = text segment base */
            sscanf(line, "%llx-", &addr);
            break;
        }
        /* Fallback: any readable mapping */
        if (strstr(line, lib_name) && strstr(line, "r--p") && addr == 0) {
            sscanf(line, "%llx-", &addr);
        }
    }
    fclose(f);
    return addr;
}

/*
 * Find the ELF LOAD base of a library — the LOWEST address mapping
 * with file offset 0. This is where st_value=0 would map to.
 *
 * CRITICAL: ELF st_value is relative to this address, NOT to the r-xp segment.
 * Example from maps:
 *   78b142a000 r--p 00000000 linker64  <- THIS is the load base (offset=0)
 *   78b1461000 r-xp 00037000 linker64  <- r-xp is at +0x37000
 *
 * st_value=0x371b0 -> real addr = 78b142a000 + 0x371b0 = 78b14611b0
 * NOT 78b1461000 + 0x371b0 (which would be 0x37000 too far!)
 */
static unsigned long long find_lib_load_base(pid_t pid, const char *lib_name) {
    char path[64], line[512];
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;

    unsigned long long addr = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, lib_name)) {
            /* Find mapping with file offset 0 (= ELF load base).
             * Format: addr-end perms offset dev inode path */
            unsigned long long start, end, offset;
            char perms[8];
            if (sscanf(line, "%llx-%llx %s %llx", &start, &end, perms, &offset) == 4) {
                if (offset == 0) {
                    addr = start;
                    break;
                }
                /* If no offset=0 found yet, take lowest address as fallback */
                if (addr == 0 || start < addr) {
                    addr = start;
                }
            }
        }
    }
    fclose(f);
    return addr;
}

/*
 * Find the offset of a symbol within a library by parsing its ELF
 * Reads the library file directly (not from process memory)
 */
static unsigned long long find_symbol_offset(const char *lib_path, const char *sym_name) {
    int fd = open(lib_path, O_RDONLY);
    if (fd < 0) return 0;

    /* Read ELF header */
    Elf64_Ehdr ehdr;
    if (read(fd, &ehdr, sizeof(ehdr)) != sizeof(ehdr)) { close(fd); return 0; }

    /* Read section headers */
    Elf64_Shdr *shdrs = malloc(ehdr.e_shnum * sizeof(Elf64_Shdr));
    if (!shdrs) { close(fd); return 0; }
    lseek(fd, ehdr.e_shoff, SEEK_SET);
    read(fd, shdrs, ehdr.e_shnum * sizeof(Elf64_Shdr));

    /* Find .dynsym and .dynstr */
    unsigned long long result = 0;
    for (int i = 0; i < ehdr.e_shnum; i++) {
        if (shdrs[i].sh_type == SHT_DYNSYM) {
            Elf64_Shdr *symtab = &shdrs[i];
            Elf64_Shdr *strtab = &shdrs[symtab->sh_link];

            /* Read string table */
            char *strings = malloc(strtab->sh_size);
            if (!strings) break;
            lseek(fd, strtab->sh_offset, SEEK_SET);
            read(fd, strings, strtab->sh_size);

            /* Read symbols */
            int nsyms = symtab->sh_size / sizeof(Elf64_Sym);
            Elf64_Sym *syms = malloc(symtab->sh_size);
            if (!syms) { free(strings); break; }
            lseek(fd, symtab->sh_offset, SEEK_SET);
            read(fd, syms, symtab->sh_size);

            for (int j = 0; j < nsyms; j++) {
                if (syms[j].st_name && strcmp(strings + syms[j].st_name, sym_name) == 0) {
                    result = syms[j].st_value;
                    break;
                }
            }

            free(syms);
            free(strings);
            break;
        }
    }

    free(shdrs);
    close(fd);
    return result;
}

/*
 * Resolve symbol address in target process via offset calculation.
 * CRITICAL: validates that local_func is actually WITHIN lib_name
 * to prevent negative offsets (e.g., dlopen in libdl.so vs linker64).
 */
static unsigned long long resolve_remote_func(pid_t pid, const char *lib_name,
                                               const char *func_name) {
    unsigned long long local_base = find_lib_base(getpid(), lib_name);
    unsigned long long remote_base = find_lib_base(pid, lib_name);

    if (!local_base || !remote_base) {
        LOGI("  resolve_remote_func: %s not found (local=%llx remote=%llx)",
             lib_name, local_base, remote_base);
        return 0;
    }

    void *local_func = dlsym(RTLD_DEFAULT, func_name);
    if (!local_func) {
        LOGI("  resolve_remote_func: dlsym(%s) failed", func_name);
        return 0;
    }

    unsigned long long local_addr = (unsigned long long)local_func;

    /* CRITICAL: validate that local_func is ABOVE local_base.
     * If dlsym returns an address in a different library, the offset
     * will be negative (huge unsigned) → wrong remote address → crash.
     * Also sanity check: offset should be < 64MB (reasonable lib size). */
    if (local_addr < local_base) {
        LOGI("  resolve_remote_func: %s (%llx) is BELOW %s base (%llx) — wrong library!",
             func_name, local_addr, lib_name, local_base);
        return 0;
    }
    unsigned long long offset = local_addr - local_base;
    if (offset > 0x4000000ULL) { /* 64MB sanity limit */
        LOGI("  resolve_remote_func: offset %llx too large for %s — wrong library?",
             offset, lib_name);
        return 0;
    }

    unsigned long long remote_addr = remote_base + offset;
    LOGI("  %s: local=%llx remote=%llx (base: local=%llx remote=%llx offset=%llx)",
         func_name, local_addr, remote_addr, local_base, remote_base, offset);
    return remote_addr;
}

/*
 * Resolve symbol in target process by parsing ELF on disk.
 * More reliable than offset calculation — doesn't depend on
 * the symbol being in the same library in our process.
 *
 * CRITICAL: Uses find_lib_load_base (offset=0 mapping), NOT find_lib_base
 * (r-xp mapping). ELF st_value is relative to the load base.
 */
static unsigned long long resolve_remote_func_elf(pid_t pid, const char *lib_path,
                                                   const char *lib_name,
                                                   const char *func_name) {
    unsigned long long load_base = find_lib_load_base(pid, lib_name);
    if (!load_base) {
        LOGI("  resolve_elf: %s not in target maps", lib_name);
        return 0;
    }

    unsigned long long sym_offset = find_symbol_offset(lib_path, func_name);
    if (!sym_offset) {
        LOGI("  resolve_elf: %s not found in %s", func_name, lib_path);
        return 0;
    }

    unsigned long long remote_addr = load_base + sym_offset;

    /* Log both bases for debugging */
    unsigned long long rxp_base = find_lib_base(pid, lib_name);
    LOGI("  %s (ELF): remote=%llx (load_base=%llx + st_value=%llx) rxp_base=%llx from %s",
         func_name, remote_addr, load_base, sym_offset, rxp_base, lib_path);

    /* Sanity: remote_addr should be within the r-xp segment */
    if (rxp_base && remote_addr < load_base) {
        LOGE("  resolve_elf: computed addr %llx < load_base %llx — BUG!", remote_addr, load_base);
        return 0;
    }

    return remote_addr;
}

/*
 * Find the linker library name in a process
 * Could be /system/bin/linker64 or /apex/com.android.runtime/bin/linker64
 */
static int find_linker_name(pid_t pid, char *out, size_t out_len) {
    char path[64], line[512];
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "linker64")) {
            /* Extract the path portion */
            char *p = strchr(line, '/');
            if (p) {
                char *nl = strchr(p, '\n');
                if (nl) *nl = '\0';
                strncpy(out, p, out_len - 1);
                out[out_len - 1] = '\0';
                fclose(f);
                return 0;
            }
        }
    }
    fclose(f);
    return -1;
}

/*
 * Attach to all threads of a process (required for multi-threaded apps)
 * Returns number of threads attached
 */
static int ptrace_attach_all(pid_t pid) {
    /* Attach to main thread first */
    if (ptrace(PTRACE_ATTACH, pid, NULL, NULL) < 0) {
        LOGE("PTRACE_ATTACH(%d) failed: %s", pid, strerror(errno));
        return -1;
    }
    int status;
    waitpid(pid, &status, 0);
    LOGI("  Attached to main thread %d", pid);
    return 1;
}

/*
 * ================================================================
 * MAIN INJECTION LOGIC
 * ================================================================
 */
static int do_inject(pid_t pid, const char *so_path) {
    struct arm64_regs orig_regs, mod_regs;
    int ret = -1;

    LOGI("[*] Target PID: %d", pid);
    LOGI("[*] Library: %s", so_path);

    /* Verify .so exists and is readable */
    if (access(so_path, R_OK) != 0) {
        LOGE("Cannot access %s: %s", so_path, strerror(errno));
        return -1;
    }

    /* Verify target process exists */
    char proc_path[64];
    snprintf(proc_path, sizeof(proc_path), "/proc/%d/status", pid);
    if (access(proc_path, F_OK) != 0) {
        LOGE("Process %d does not exist", pid);
        return -1;
    }

    /* 1. Attach via ptrace */
    LOGI("[1] Attaching to process...");
    if (ptrace_attach_all(pid) < 0) {
        return -1;
    }

    /* 2. Save original registers */
    LOGI("[2] Saving registers...");
    if (get_regs(pid, &orig_regs) < 0) {
        goto detach;
    }
    memcpy(&mod_regs, &orig_regs, sizeof(orig_regs));

    /* 3. Find dlopen in target process */
    LOGI("[3] Resolving dlopen...");

    /* Try multiple methods to find dlopen in target process */
    unsigned long long remote_dlopen = 0;
    int is_loader_dlopen = 0; /* __loader_dlopen takes 3 args: (path, flags, caller_addr) */
    char linker_path[256] = {0};
    find_linker_name(pid, linker_path, sizeof(linker_path));
    LOGI("  Linker: %s", linker_path[0] ? linker_path : "(not found)");

    /* Method A: Parse linker64 ELF on disk for __loader_dlopen
     * Most reliable on Android 7+ — direct symbol, no guessing */
    if (linker_path[0]) {
        remote_dlopen = resolve_remote_func_elf(pid, linker_path, "linker64",
                                                 "__loader_dlopen");
        if (remote_dlopen) is_loader_dlopen = 1;
    }

    /* Method B: Parse linker64 ELF for dlopen */
    if (!remote_dlopen && linker_path[0]) {
        remote_dlopen = resolve_remote_func_elf(pid, linker_path, "linker64",
                                                 "dlopen");
    }

    /* Method C: libdl.so offset calculation (dlopen lives here on older Android) */
    if (!remote_dlopen) {
        remote_dlopen = resolve_remote_func(pid, "libdl.so", "dlopen");
    }

    /* Method D: Parse libdl.so ELF on disk */
    if (!remote_dlopen) {
        remote_dlopen = resolve_remote_func_elf(pid, "/system/lib64/libdl.so",
                                                 "libdl.so", "dlopen");
    }

    /* Method E: last resort — linker offset calc */
    if (!remote_dlopen) {
        remote_dlopen = resolve_remote_func(pid, "linker64", "__loader_dlopen");
        if (remote_dlopen) is_loader_dlopen = 1;
    }

    if (!remote_dlopen) {
        LOGE("Could not resolve dlopen in target process");
        goto restore;
    }
    LOGI("  dlopen at: 0x%llx", remote_dlopen);

    /* Diagnostic: read first 2 instructions at dlopen address.
     * If it's a bare RET (0xd65f03c0), the symbol is a stub and
     * x0 will be unchanged after call → false "x0=path_addr" failure. */
    {
        unsigned int instrs[2] = {0, 0};
        if (read_mem(pid, remote_dlopen, instrs, 8) == 0) {
            LOGI("  dlopen code: [0]=%08x [1]=%08x", instrs[0], instrs[1]);
            if (instrs[0] == 0xd65f03c0) {
                LOGE("  WARNING: dlopen is a bare RET stub! Address may be wrong.");
            }
        }
    }

    /* 4. Write .so path to target's stack */
    LOGI("[4] Writing path to target stack...");
    {
        size_t path_len = strlen(so_path) + 1;
        /* Align path_len to 16 bytes (ARM64 ABI) */
        size_t aligned_len = (path_len + 15) & ~15ULL;

        /* Use space below current SP */
        unsigned long long path_addr = (orig_regs.sp - aligned_len - 256) & ~15ULL;

        if (write_mem(pid, path_addr, so_path, path_len) < 0) {
            LOGE("Failed to write path to target memory");
            goto restore;
        }
        LOGI("  Path written at: 0x%llx (%zu bytes)", path_addr, path_len);

        /* 5. Set up call to dlopen(path, RTLD_NOW) or
         * __loader_dlopen(path, RTLD_NOW, caller_addr) */
        LOGI("[5] Setting up dlopen call (loader_variant=%d)...", is_loader_dlopen);
        mod_regs.regs[0] = path_addr;      /* x0 = path string */
        mod_regs.regs[1] = RTLD_NOW;       /* x1 = flags */
        if (is_loader_dlopen) {
            /* __loader_dlopen needs caller_addr in x2 to determine namespace.
             * Use a code address from the game's own libil2cpp.so (or any game lib)
             * so dlopen resolves in the game's linker namespace. */
            unsigned long long caller = find_lib_base(pid, "libil2cpp.so");
            if (!caller) caller = find_lib_base(pid, "libunity.so");
            if (!caller) caller = find_lib_base(pid, "libmain.so");
            if (!caller) caller = orig_regs.pc; /* fallback: where target was executing */
            mod_regs.regs[2] = caller;
            LOGI("  caller_addr (x2) = 0x%llx", caller);
        }
        mod_regs.pc = remote_dlopen;        /* pc = dlopen */
        mod_regs.regs[30] = 0;             /* lr = 0 (will SIGSEGV after return) */
        /* Keep SP aligned */
        mod_regs.sp = path_addr - 16;

        if (set_regs(pid, &mod_regs) < 0) {
            goto restore;
        }
    }

    /* 6. Resume execution — target calls dlopen() */
    LOGI("[6] Executing dlopen in target...");
    if (ptrace(PTRACE_CONT, pid, NULL, NULL) < 0) {
        LOGE("PTRACE_CONT failed: %s", strerror(errno));
        goto restore;
    }

    /* Wait for the SIGSEGV from jumping to lr=0 after dlopen returns */
    {
        int status;
        int wait_count = 0;
        while (1) {
            pid_t w = waitpid(pid, &status, 0);
            if (w < 0) {
                LOGE("waitpid failed: %s", strerror(errno));
                goto restore;
            }

            if (WIFSTOPPED(status)) {
                int sig = WSTOPSIG(status);
                LOGI("  Process stopped by signal %d", sig);

                if (sig == SIGSEGV || sig == SIGBUS) {
                    /* Check return value in x0 and where PC crashed */
                    struct arm64_regs after_regs;
                    get_regs(pid, &after_regs);
                    LOGI("  after: x0=%llx pc=%llx sp=%llx",
                         after_regs.regs[0], after_regs.pc, after_regs.sp);

                    /* If PC is 0 (our lr trap), dlopen returned normally.
                     * If PC is elsewhere, something crashed before dlopen ran. */
                    if (after_regs.pc == 0 && after_regs.regs[0] != 0
                        && after_regs.regs[0] != mod_regs.regs[0]) {
                        /* PC=0 (lr trap) + x0 != 0 + x0 != path_addr = real handle */
                        LOGI("  dlopen handle: 0x%llx (SUCCESS — pc=0 trap)", after_regs.regs[0]);
                        ret = 0;
                    } else if (after_regs.pc == 0 && after_regs.regs[0] == mod_regs.regs[0]) {
                        /* PC=0 but x0 == path_addr = dlopen returned NULL or SELinux blocked */
                        LOGE("  dlopen FAILED: x0=path_addr (SELinux? file permissions?)");
                        LOGE("  The .so path may not be executable by the target process");
                        ret = -1;
                    } else if (after_regs.pc != 0 && after_regs.regs[0] == mod_regs.regs[0]) {
                        /* x0 unchanged + PC != 0 = crashed at bad dlopen address */
                        LOGE("  CRASH at pc=%llx — dlopen address is WRONG!", after_regs.pc);
                        LOGE("  x0 unchanged (=path_addr) — dlopen never executed");
                        ret = -1;
                    } else if (after_regs.regs[0] != 0) {
                        /* x0 changed to non-zero = probably success */
                        LOGI("  dlopen handle: 0x%llx (likely SUCCESS)", after_regs.regs[0]);
                        ret = 0;
                    } else {
                        LOGE("  dlopen returned NULL (FAILED) pc=%llx", after_regs.pc);
                        ret = -1;
                    }
                    break;
                } else if (sig == SIGSTOP || sig == SIGTSTP || sig == SIGTTIN || sig == SIGTTOU) {
                    /* Ignore stop signals, continue */
                    ptrace(PTRACE_CONT, pid, NULL, NULL);
                    continue;
                } else {
                    /* Unexpected signal — forward it and continue */
                    LOGI("  Forwarding signal %d", sig);
                    ptrace(PTRACE_CONT, pid, NULL, (void*)(long)sig);
                    continue;
                }
            }

            if (WIFEXITED(status) || WIFSIGNALED(status)) {
                LOGE("  Process exited/killed during injection!");
                goto detach_only;
            }

            wait_count++;
            if (wait_count > 100) {
                LOGE("  Too many wait iterations, aborting");
                break;
            }
        }
    }

restore:
    /* 7. Restore original registers */
    LOGI("[7] Restoring original registers...");
    set_regs(pid, &orig_regs);

detach:
    /* 8. Detach */
    LOGI("[8] Detaching...");
    ptrace(PTRACE_DETACH, pid, NULL, NULL);

detach_only:
    if (ret == 0) {
        LOGI("[+] INJECTION SUCCESSFUL into PID %d", pid);
        /* Verify: check if .so actually appears in /proc/PID/maps */
        char maps_path[64];
        snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps", pid);
        FILE *mf = fopen(maps_path, "r");
        if (mf) {
            char mline[512];
            int found = 0;
            while (fgets(mline, sizeof(mline), mf)) {
                if (strstr(mline, "libHook") || strstr(mline, "Hook.so")) {
                    LOGI("  Verified: %s", mline);
                    found = 1;
                }
            }
            fclose(mf);
            if (!found) {
                LOGE("  WARNING: .so NOT found in /proc/%d/maps after SUCCESS!", pid);
                LOGE("  dlopen handle may be invalid or .so already unloaded");
            }
        }
    } else {
        LOGE("[-] INJECTION FAILED for PID %d", pid);
        /* Diagnostic: dump relevant maps regions */
        char maps_path[64];
        snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps", pid);
        FILE *mf = fopen(maps_path, "r");
        if (mf) {
            char mline[512];
            LOGI("  --- /proc/%d/maps (linker + hook entries) ---", pid);
            while (fgets(mline, sizeof(mline), mf)) {
                if (strstr(mline, "linker") || strstr(mline, "libHook")
                    || strstr(mline, "Hook.so") || strstr(mline, "libdl")) {
                    LOGI("  %s", mline);
                }
            }
            fclose(mf);
        }
    }
    return ret;
}

/*
 * ================================================================
 * ENTRY POINT: __attribute__((constructor))
 * ================================================================
 *
 * Compilado como BUILD_SHARED_LIBRARY (libinjector.so) para o APK
 * empacotar. Invocado via LD_PRELOAD em um shell root:
 *
 *   su -c "LD_PRELOAD=/data/local/tmp/libinjector.so cat /dev/null"
 *
 * O constructor le PID e path de /data/local/tmp/.inject_config
 * (escrito pelo Java antes do LD_PRELOAD), faz a injecao, e _exit().
 *
 * Se o config nao existe, retorna silenciosamente (nao interfere
 * com outros processos caso .so seja carregado acidentalmente).
 */
#define INJECT_CONFIG "/data/local/tmp/.inject_config"

__attribute__((constructor))
static void injector_entry(void) {
    /* Somente executar se o config file existe */
    FILE *cfg = fopen(INJECT_CONFIG, "r");
    if (!cfg) return;

    /* Ler PID e caminho do .so */
    char pid_str[32] = {0};
    char so_path[256] = {0};

    if (!fgets(pid_str, sizeof(pid_str), cfg) ||
        !fgets(so_path, sizeof(so_path), cfg)) {
        LOGE("Failed to read config file");
        fclose(cfg);
        return;
    }
    fclose(cfg);

    /* Remover config imediatamente (evita re-execucao) */
    unlink(INJECT_CONFIG);

    /* Parse */
    pid_t target_pid = atoi(pid_str);
    char *nl = strchr(so_path, '\n');
    if (nl) *nl = '\0';
    nl = strchr(so_path, '\r');
    if (nl) *nl = '\0';

    if (target_pid <= 0 || so_path[0] == '\0') {
        LOGE("Invalid config: pid=%d path=[%s]", target_pid, so_path);
        _exit(1);
    }

    if (getuid() != 0) {
        LOGE("Must run as root (uid=0). Current uid=%d", getuid());
        _exit(1);
    }

    LOGI("=== JawMods ptrace injector v15 ===");
    LOGI("Config: PID=%d SO=%s", target_pid, so_path);
    int result = do_inject(target_pid, so_path);

    /* Verificar injecao nos maps */
    if (result == 0) {
        usleep(500000);
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "grep -c '%s' /proc/%d/maps 2>/dev/null",
                 so_path, target_pid);
        FILE *p = popen(cmd, "r");
        if (p) {
            char out[32] = {0};
            fgets(out, sizeof(out), p);
            pclose(p);
            int count = atoi(out);
            if (count > 0) {
                LOGI("[+] Verified: .so in /proc/%d/maps (%d entries)", target_pid, count);
            } else {
                LOGI("[!] .so NOT found in maps after injection");
            }
        }
    }

    LOGI(result == 0 ? "[+] INJECTION SUCCESSFUL" : "[-] INJECTION FAILED");
    fflush(stdout);
    fflush(stderr);
    _exit(result == 0 ? 0 : 1);
}
