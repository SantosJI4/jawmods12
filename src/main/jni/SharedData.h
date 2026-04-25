#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/ashmem.h>
#include <sys/ioctl.h>

/*
 * SharedData - Comunicação entre o HOOK (no jogo) e o OVERLAY (externo)
 *
 * Usa ashmem (Android Shared Memory) — memória compartilhada entre processos.
 * O hook escreve os dados ESP, o overlay lê e desenha.
 *
 * Fluxo:
 *   Hook (jogo) → escreve posições de tela no SharedData
 *   Overlay (APK) → lê SharedData e desenha ImGui
 */

#define SHARED_MEM_NAME  "gl_cache"
#define SHARED_MEM_PATH  "/dev/ashmem"
#define SHARED_MEM_SIZE  8192
#define MAX_ESP_PLAYERS  32

struct ESPEntry {
    float topX, topY, topZ;          // Posição de tela do topo do player
    float bottomX, bottomY, bottomZ; // Posição de tela da base do player
    float distance;                  // Distância do player
    int   team;                      // ID do time
    float health;                    // Vida (legado)
    bool  valid;                     // Entrada válida
    int   curHp;                     // HP atual
    int   maxHp;                     // HP máximo
    bool  knocked;                   // Player está derrubado/sangrando
    char  nick[24];                  // Nome do player (UTF-8, truncado em 23 chars)
    bool  isLocked;                  // Aimbot atualmente travado neste player
};

struct SharedESPData {
    uint32_t magic;               // 0xDEADF00D = SHM criado (hack_thread rodando)
    uint32_t hookApplied;         // 0xBEEF1234 = VmtHook OK, hookActive=true
    std::atomic<uint32_t> writeSeq; // Sequência de escrita (detectar update)
    int screenW;
    int screenH;
    int playerCount;
    volatile int espEnabled;          // Overlay seta 1/0 → hook verifica

    // ── Aimbot ──────────────────────────────────────────────────────────────
    volatile int   aimbotEnabled;         // Liga/desliga aimbot
    volatile int   aimbotIgnoreKnocked;   // Ignorar alvos knocked/sangrando
    volatile float aimbotFovDeg;          // Cone de FOV (graus)
    volatile float aimbotSmooth;          // Suavidade (0=snap, 1=muito suave)
    volatile int   aimbotHasTarget;       // Hook reporta: alvo travado
    volatile int   aimbotThroughWalls;    // Ignorar verificação de visibilidade
    volatile int   aimbotAlwaysTrack;     // Rastrear sempre (sem precisar atirar)
    volatile int   aimbotLockedIdx;       // Índice do player travado (-1=nenhum)
    volatile int   triggerKey;            // Keycode do trigger (hold-to-aim)
    volatile int   triggerHeld;           // 1 = tecla trigger pressionada

    volatile int debugLastCall;     // Diagnóstico: último il2cpp call completado
    volatile int resetSelf;         // Overlay seta 1 → hook reseta cache do self player

    ESPEntry players[MAX_ESP_PLAYERS];
};

// ============================================================
// Helpers para criar/abrir shared memory
// ============================================================

// Cria shared memory (chamado pelo HOOK no jogo)
static int shm_create() {
    int fd = open(SHARED_MEM_PATH, O_RDWR);
    if (fd < 0) return -1;

    // Dá nome à região
    ioctl(fd, ASHMEM_SET_NAME, SHARED_MEM_NAME);
    ioctl(fd, ASHMEM_SET_SIZE, SHARED_MEM_SIZE);

    return fd;
}

// Mapeia shared memory
static SharedESPData* shm_map(int fd) {
    void* ptr = mmap(nullptr, SHARED_MEM_SIZE, PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) return nullptr;
    return static_cast<SharedESPData*>(ptr);
}

// Desmapeia
static void shm_unmap(SharedESPData* data) {
    if (data) munmap(data, SHARED_MEM_SIZE);
}

// ============================================================
// Alternativa: Arquivo simples no /data/local/tmp/
// Mais simples que ashmem, funciona com root
// ============================================================

// Paths para shared memory (o hook tenta em ordem)
// Nomes ofuscados para evitar deteccao por file scanning
#define SHM_PATH_1 "/data/local/tmp/.gl_cache"
#define SHM_PATH_2 "/sdcard/.gl_cache"
#define SHM_FILENAME ".gl_cache"
#define HOOKLOG_FILENAME ".gl_log"

static const char* shmActivePath = nullptr;

// fd pre-aberto em preAppSpecialize (root context) pelo modulo Zygisk.
// SELinux bloqueia untrusted_app de criar shell_data_file,
// mas um fd aberto como root sobrevive ao setuid e pode ser reutilizado.
#ifdef ZYGISK_BUILD
extern int g_zygisk_shm_fd;
#else
static int g_zygisk_shm_fd = -1;
#endif

// Game data dir — hardcode o package pois /proc/self/cmdline nao e confiavel
// (no constructor ainda e 'app_process64' do zygote)
#ifndef HOOK_GAME_PACKAGE
#define HOOK_GAME_PACKAGE "com.dts.freefireth"
#endif

static char g_gameDataDir[256] = {0};
static char g_shmGamePath[512] = {0};
static char g_logGamePath[512] = {0};

static const char* getGameDataDir() {
    if (g_gameDataDir[0]) return g_gameDataDir;
    snprintf(g_gameDataDir, sizeof(g_gameDataDir), "/data/data/%s", HOOK_GAME_PACKAGE);
    return g_gameDataDir;
}

static const char* getGameShmPath() {
    if (g_shmGamePath[0]) return g_shmGamePath;
    const char* dir = getGameDataDir();
    if (dir[0]) snprintf(g_shmGamePath, sizeof(g_shmGamePath), "%s/%s", dir, SHM_FILENAME);
    return g_shmGamePath;
}

static const char* getGameLogPath() {
    if (g_logGamePath[0]) return g_logGamePath;
    const char* dir = getGameDataDir();
    if (dir[0]) snprintf(g_logGamePath, sizeof(g_logGamePath), "%s/%s", dir, HOOKLOG_FILENAME);
    return g_logGamePath;
}

// Cria/abre shared memory (hook no jogo — roda como UID do app)
// Tenta: 1) /data/local/tmp/ (PROVADO funcionar), 2) data dir do jogo, 3) /sdcard/
static int shm_create_file() {
    // ZYGISK: fd pre-aberto em root context (preAppSpecialize antes do setuid)
    // Este fd bypass SELinux pois foi aberto como root e sobrevive ao setuid.
    if (g_zygisk_shm_fd >= 0) {
        ftruncate(g_zygisk_shm_fd, SHARED_MEM_SIZE);
        fchmod(g_zygisk_shm_fd, 0666);
        shmActivePath = SHM_PATH_1;
        return g_zygisk_shm_fd;
    }

    // PRIMARIO: /data/local/tmp/ — pre-criado pelo overlay com chmod 666
    // Este e o path que o hook COMPROVOU funcionar (fd=196 no logcat)
    int fd = open(SHM_PATH_1, O_RDWR);
    if (fd >= 0) {
        ftruncate(fd, SHARED_MEM_SIZE);
        fchmod(fd, 0666);
        shmActivePath = SHM_PATH_1;
        return fd;
    }
    fd = open(SHM_PATH_1, O_CREAT | O_RDWR, 0666);
    if (fd >= 0) {
        ftruncate(fd, SHARED_MEM_SIZE);
        fchmod(fd, 0666);
        shmActivePath = SHM_PATH_1;
        return fd;
    }

    // Fallback 1: diretorio de dados do jogo
    const char* gamePath = getGameShmPath();
    if (gamePath[0]) {
        fd = open(gamePath, O_RDWR);
        if (fd < 0) fd = open(gamePath, O_CREAT | O_RDWR, 0666);
        if (fd >= 0) {
            ftruncate(fd, SHARED_MEM_SIZE);
            fchmod(fd, 0666);
            shmActivePath = gamePath;
            return fd;
        }
    }

    // Fallback 2: /sdcard/
    fd = open(SHM_PATH_2, O_CREAT | O_RDWR, 0666);
    if (fd >= 0) {
        ftruncate(fd, SHARED_MEM_SIZE);
        fchmod(fd, 0666);
        shmActivePath = SHM_PATH_2;
        return fd;
    }

    return -1;
}

// Abre shared memory para leitura (overlay)
// Tenta: 1) data dir do jogo, 2) /data/local/tmp/, 3) /sdcard/
static int shm_open_file(const char* gamePackage) {
    // Tentar data dir do jogo especifico
    if (gamePackage && gamePackage[0]) {
        char path[512];
        snprintf(path, sizeof(path), "/data/data/%s/%s", gamePackage, SHM_FILENAME);
        int fd = open(path, O_RDWR);
        if (fd >= 0) {
            off_t sz = lseek(fd, 0, SEEK_END);
            lseek(fd, 0, SEEK_SET);
            if (sz >= (off_t)SHARED_MEM_SIZE) return fd;
            close(fd);
        }
    }
    // Fallback paths
    int fd = open(SHM_PATH_1, O_RDWR);
    if (fd >= 0) {
        off_t sz = lseek(fd, 0, SEEK_END);
        lseek(fd, 0, SEEK_SET);
        if (sz >= (off_t)SHARED_MEM_SIZE) return fd;
        close(fd);
    }
    fd = open(SHM_PATH_2, O_RDWR);
    if (fd >= 0) {
        off_t sz = lseek(fd, 0, SEEK_END);
        lseek(fd, 0, SEEK_SET);
        if (sz >= (off_t)SHARED_MEM_SIZE) return fd;
        close(fd);
    }
    return -1;
}
