/*
 * ============================================================
 * EXTERNAL OVERLAY - Processo separado do jogo
 * ============================================================
 *
 * Arquitetura HÃBRIDA:
 *   - APK roda como processo independente (NÃƒO injeta no jogo)
 *   - Overlay via WindowManager com FLAG_SECURE (exclusÃ£o de captura)
 *   - DADOS vÃªm do GameHook.cpp via SharedMemory (arquivo mmap)
 *   - GameHook.cpp Ã© injetado no jogo via script root â€” faz VMT hook
 *   - O hook escreve posiÃ§Ãµes de tela â†’ overlay lÃª e desenha
 *   - ImGui renderiza em EGL context prÃ³prio sobre o overlay
 *
 * main.cpp = APENAS LEITURA + DESENHO (zero acesso ao jogo)
 * GameHook.cpp = VMT hook dentro do jogo (coleta dados)
 *
 * CÃ³digo antigo salvo em: main_injected.cpp.bak
 * ============================================================
 */

#include <jni.h>
#include <pthread.h>
#include <android/native_window_jni.h>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <unistd.h>
#include <cerrno>
#include <ctime>
#include <GLES3/gl3.h>
#include <EGL/egl.h>

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_opengl3.h"

#include "Overlay.h"
#include "SharedData.h"

#include <android/log.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <signal.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <linux/input.h>
#define MTAG "MeowESP"
#define MLOGI(...) __android_log_print(ANDROID_LOG_INFO, MTAG, __VA_ARGS__)

// ============================================================
// ESP State
// ============================================================
static bool esp = true;   // ligado por padrao
static bool drawEnemyBox = true;
static bool drawSnapLine = true;
static bool drawDistance = false;
static ImVec4 espLineColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
static float espMaxDistance = 999.0f;
static float linePositionX = 0.5f;

// ============================================================
// Safe overlay state
// ============================================================
static bool drawNickName      = true;

// ============================================================
// Captura de Tela (v51)
// Usa su + screencap/screenrecord â€” requer root no overlay APK
// Arquivos salvos em /sdcard/DCIM/jawmods/
// ============================================================
static pid_t g_recordPid       = -1;
static bool  g_recordingActive = false;
static char  g_captureMsg[64]  = "";
static float g_captureMsgTimer = 0.0f;

static void setCaptureMsg(const char* msg) {
    snprintf(g_captureMsg, sizeof(g_captureMsg), "%s", msg);
    g_captureMsgTimer = 2.0f; // exibe por 2 segundos
}

static void doScreenshot() {
    char cmd[320];
    time_t t = time(nullptr);
    snprintf(cmd, sizeof(cmd),
        "su -c \"mkdir -p /sdcard/DCIM/jawmods && screencap -p /sdcard/DCIM/jawmods/ss_%ld.png\"",
        (long)t);
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execl("/system/bin/sh", "sh", "-c", cmd, nullptr);
        _exit(0);
    } else if (pid > 0) {
        waitpid(pid, nullptr, 0);
        setCaptureMsg("Screenshot salvo!");
    } else {
        setCaptureMsg("Erro: sem root?");
    }
}

static void startRecording() {
    char cmd[320];
    time_t t = time(nullptr);
    snprintf(cmd, sizeof(cmd),
        "su -c \"mkdir -p /sdcard/DCIM/jawmods && screenrecord --bit-rate 8000000 /sdcard/DCIM/jawmods/rec_%ld.mp4\"",
        (long)t);
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execl("/system/bin/sh", "sh", "-c", cmd, nullptr);
        _exit(0);
    } else if (pid > 0) {
        g_recordPid = pid;
        g_recordingActive = true;
        setCaptureMsg("Gravando...");
    } else {
        setCaptureMsg("Erro: sem root?");
    }
}

static void stopRecording() {
    if (g_recordPid > 0) {
        kill(g_recordPid, SIGINT); // screenrecord salva o MP4 ao receber SIGINT
        waitpid(g_recordPid, nullptr, WNOHANG);
        g_recordPid = -1;
    }
    g_recordingActive = false;
    setCaptureMsg("Gravacao salva!");
}

// ============================================================
// Hotkeys State  (Linux keycodes: Vol- = 114, Vol+ = 115)
// ============================================================
static int hotkeyEsp    = 0;    // 0 = desativado

// â”€â”€ Aimbot State â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
static bool aimbotEnabled     = false;
static float aimbotFovDeg     = 60.0f;
static float aimbotSmooth     = 0.0f;
static bool aimbotIgnoreKnocked = true;
static bool aimbotThroughWalls  = false;
static bool aimbotAlwaysTrack   = false;
static bool showFovCircle       = true;
static int32_t triggerKey       = 0;

// â”€â”€ Advanced ESP State â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
static bool drawHeadDot         = true;

// SharedMemory (leitura do hook injetado no jogo)
static SharedESPData* sharedData = nullptr;
static int shmFd = -1;
static std::atomic<bool> shmConnected{false};
static uint32_t lastWriteSeq = 0;

// Game package â€” para encontrar SHM no data dir do jogo
#define GAME_PACKAGE "com.dts.freefireth"

// ============================================================
// SharedMemory Reader â€” Conecta ao arquivo criado pelo hook
// Tenta: 1) /data/data/<game>/.gl_cache, 2) /data/local/tmp/, 3) /sdcard/
// ============================================================
static std::atomic<bool> readerRunning{false};
static pthread_t readerThread = 0;
static char shmStatus[256] = "Iniciando...";

void* shmReaderLoop(void*) {
    int attempt = 0;
    MLOGI("shmReaderLoop started (build: v7-fixed-paths)");

    // Paths para tentar: /data/local/tmp/ PRIMEIRO (o hook comprovou que funciona la)
    // game dir como fallback (pode falhar por SELinux/namespace)
    char gameShmPath[512];
    snprintf(gameShmPath, sizeof(gameShmPath), "/data/data/%s/%s", GAME_PACKAGE, SHM_FILENAME);
    const char* paths[] = { SHM_PATH_1, gameShmPath, SHM_PATH_2 };
    const int numPaths = 3;

    while (readerRunning.load()) {
        attempt++;

        int fd = -1;
        const char* usedPath = nullptr;
        for (int p = 0; p < numPaths; p++) {
            fd = open(paths[p], O_RDWR);
            if (fd >= 0) {
                usedPath = paths[p];
                MLOGI("Tentativa %d: aberto %s (fd=%d)", attempt, usedPath, fd);
                break;
            }
        }
        if (fd < 0) {
            snprintf(shmStatus, sizeof(shmStatus),
                "Tentativa %d: nenhum shm acessivel (errno=%d: %s)",
                attempt, errno, strerror(errno));
            MLOGI("%s", shmStatus);
            sleep(1);
            continue;
        }

        // Verificar tamanho
        off_t sz = lseek(fd, 0, SEEK_END);
        lseek(fd, 0, SEEK_SET);
        MLOGI("Tentativa %d: tamanho=%ld", attempt, (long)sz);

        if (sz < (off_t)SHARED_MEM_SIZE) {
            snprintf(shmStatus, sizeof(shmStatus),
                "Tentativa %d: arquivo pequeno (%ld < %d)",
                attempt, (long)sz, SHARED_MEM_SIZE);
            MLOGI("%s", shmStatus);
            close(fd);
            sleep(1);
            continue;
        }

        // Mapear
        SharedESPData* data = shm_map(fd);
        if (!data) {
            snprintf(shmStatus, sizeof(shmStatus),
                "Tentativa %d: mmap falhou (errno=%d: %s)",
                attempt, errno, strerror(errno));
            MLOGI("%s", shmStatus);
            close(fd);
            sleep(1);
            continue;
        }

        // Log magic value
        MLOGI("Tentativa %d: mmap OK, magic=0x%08X (esperado 0xDEADF00D)", attempt, data->magic);

        // FORÃ‡AR CONEXÃƒO: conecta se o arquivo existe e Ã© mapeÃ¡vel
        // NÃ£o espera magic â€” o hook pode nÃ£o ter escrito ainda
        shmFd = fd;
        sharedData = data;
        shmConnected.store(true);
        snprintf(shmStatus, sizeof(shmStatus),
            "Conectado! magic=0x%08X", data->magic);
        MLOGI("SharedMemory CONECTADO (forÃ§ado). magic=0x%08X", data->magic);
        break;
    }

    // Manter thread viva para monitoramento
    while (readerRunning.load()) {
        if (sharedData) {
            // Atualizar status periodicamente
            snprintf(shmStatus, sizeof(shmStatus),
                "magic=0x%08X seq=%u players=%d esp=%d dbg=%d",
                sharedData->magic,
                sharedData->writeSeq.load(std::memory_order_relaxed),
                sharedData->playerCount,
                sharedData->espEnabled,
                sharedData->debugLastCall);
        }
        usleep(500000);
    }

    // Cleanup
    if (sharedData) {
        shm_unmap(sharedData);
        sharedData = nullptr;
    }
    if (shmFd >= 0) {
        close(shmFd);
        shmFd = -1;
    }
    shmConnected.store(false);
    return nullptr;
}

// ============================================================
// Draw ESP â€” LÃª direto do SharedMemory (sem cÃ³pia)
// ============================================================
void DrawESP(int screenW, int screenH) {
    if (!sharedData || !shmConnected.load()) return;

    // Sincronizar configuraÃ§Ãµes de aimbot com shared memory
    sharedData->espEnabled            = 1;
    sharedData->aimbotEnabled         = aimbotEnabled ? 1 : 0;
    sharedData->aimbotFovDeg          = aimbotFovDeg;
    sharedData->aimbotSmooth          = aimbotSmooth;
    sharedData->aimbotIgnoreKnocked   = aimbotIgnoreKnocked ? 1 : 0;
    sharedData->aimbotThroughWalls    = aimbotThroughWalls ? 1 : 0;
    sharedData->aimbotAlwaysTrack     = aimbotAlwaysTrack ? 1 : 0;
    sharedData->triggerKey            = triggerKey;

    if (!esp) return;
    if (sharedData->magic != 0xDEADF00D) return;

    uint32_t seq = sharedData->writeSeq.load(std::memory_order_acquire);
    int count = sharedData->playerCount;
    if (count <= 0 || count > MAX_ESP_PLAYERS) return;

    int gameW = sharedData->screenW;
    int gameH = sharedData->screenH;
    if (gameW <= 0 || gameH <= 0) return;
    float scaleX = (float)screenW / (float)gameW;
    float scaleY = (float)screenH / (float)gameH;

    ImVec2 snapOrigin = ImVec2((float)screenW * 0.5f, (float)screenH);
    auto* draw = ImGui::GetBackgroundDrawList();
    float t = (float)(clock() % 1000) / 1000.0f;  // [0,1) pulsation timer

    for (int i = 0; i < count; i++) {
        const ESPEntry& entry = sharedData->players[i];
        if (!entry.valid) continue;
        if (entry.distance > espMaxDistance) continue;
        if (entry.curHp <= 0) continue;

        float rawTopX    = entry.topX    * scaleX;
        float rawTopY    = entry.topY    * scaleY;
        float rawBottomX = entry.bottomX * scaleX;
        float rawBottomY = entry.bottomY * scaleY;

        float top = fminf(rawTopY, rawBottomY);
        float bot = fmaxf(rawTopY, rawBottomY);
        float boxHeight = bot - top;
        if (boxHeight < 2.0f) continue;

        float boxWidth  = boxHeight * 0.45f;
        float centerX   = (rawTopX + rawBottomX) * 0.5f;
        float padY      = boxHeight * 0.03f;
        float halfW     = boxWidth * 0.5f;
        ImVec2 boxMin   = ImVec2(centerX - halfW, top - padY);
        ImVec2 boxMax   = ImVec2(centerX + halfW, bot + padY);
        float W = boxMax.x - boxMin.x;
        float H = boxMax.y - boxMin.y;

        bool isKnocked  = entry.knocked;
        bool isLocked   = entry.isLocked;

        // â”€â”€ Corner Box â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        if (drawEnemyBox) {
            float cs = fminf(W * 0.25f, 8.0f);  // tamanho do canto
            ImU32 bCol = isKnocked
                ? IM_COL32(255, 50, 50, 230)
                : (isLocked ? IM_COL32(255, 180, 0, 255) : IM_COL32(255, 255, 255, 200));
            ImU32 shadow = IM_COL32(0, 0, 0, 100);
            float th = 1.8f;
            // Sombra fina
            auto Corner = [&](float x, float y, float dx1, float dy1, float dx2, float dy2) {
                draw->AddLine(ImVec2(x+1,y+1), ImVec2(x+dx1+1,y+dy1+1), shadow, th+0.5f);
                draw->AddLine(ImVec2(x+1,y+1), ImVec2(x+dx2+1,y+dy2+1), shadow, th+0.5f);
                draw->AddLine(ImVec2(x,y), ImVec2(x+dx1,y+dy1), bCol, th);
                draw->AddLine(ImVec2(x,y), ImVec2(x+dx2,y+dy2), bCol, th);
            };
            Corner(boxMin.x,     boxMin.y,      cs,  0,  0,  cs);  // top-left
            Corner(boxMax.x,     boxMin.y,     -cs,  0,  0,  cs);  // top-right
            Corner(boxMin.x,     boxMax.y,      cs,  0,  0, -cs);  // bot-left
            Corner(boxMax.x,     boxMax.y,     -cs,  0,  0, -cs);  // bot-right
        }

        // â”€â”€ Lock Ring (pulsating) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        if (isLocked && aimbotEnabled) {
            float pulse = 0.6f + 0.4f * sinf(t * 6.28f);
            float rr = halfW * 0.55f;
            draw->AddCircle(
                ImVec2(centerX, top - padY - rr * 0.2f),
                rr, IM_COL32(255, 165, 0, (int)(200 * pulse)), 32, 2.0f);
        }

        // â”€â”€ Head Dot â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        if (drawHeadDot) {
            float headR = fmaxf(boxWidth * 0.12f, 3.0f);
            ImU32 hdCol = isKnocked ? IM_COL32(255, 50, 50, 200) : IM_COL32(255, 255, 255, 220);
            draw->AddCircle(ImVec2(rawTopX, rawTopY), headR, hdCol, 20, 1.5f);
        }

        // â”€â”€ Knocked Badge â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        if (isKnocked) {
            float pulse2 = 0.5f + 0.5f * sinf(t * 4.0f);
            float bw = 36.0f, bh = 12.0f;
            ImVec2 bc(centerX, top - padY - 8.0f);
            draw->AddRectFilled(
                ImVec2(bc.x - bw*0.5f, bc.y - bh*0.5f),
                ImVec2(bc.x + bw*0.5f, bc.y + bh*0.5f),
                IM_COL32(200, 0, 0, (int)(180 * pulse2)), 3.0f);
            draw->AddText(
                ImVec2(bc.x - ImGui::CalcTextSize("DOWN").x * 0.5f, bc.y - 5.5f),
                IM_COL32(255, 255, 255, 240), "DOWN");
        }

        // â”€â”€ Nick Pill â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        if (drawNickName && entry.nick[0] != '\0') {
            ImVec2 tsz = ImGui::CalcTextSize(entry.nick);
            float  tx  = centerX - tsz.x * 0.5f;
            float  ty  = boxMin.y - tsz.y - 4.0f;
            draw->AddRectFilled(
                ImVec2(tx - 4, ty - 1), ImVec2(tx + tsz.x + 4, ty + tsz.y + 1),
                IM_COL32(0, 0, 0, 140), 3.0f);
            draw->AddText(ImVec2(tx+1, ty+1), IM_COL32(0,0,0,160), entry.nick);
            ImU32 nCol = isLocked ? IM_COL32(255,180,0,255) : IM_COL32(220,220,220,230);
            draw->AddText(ImVec2(tx, ty), nCol, entry.nick);
        }

        // â”€â”€ HP Bar (horizontal, baixo do box) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        {
            float hpR = (entry.maxHp > 0) ? (float)entry.curHp / (float)entry.maxHp : 1.0f;
            if (hpR < 0.0f) hpR = 0.0f;
            if (hpR > 1.0f) hpR = 1.0f;
            float barY  = boxMax.y + 3.0f;
            float barH2 = 3.5f;
            draw->AddRectFilled(ImVec2(boxMin.x, barY), ImVec2(boxMax.x, barY + barH2),
                                IM_COL32(30,30,30,180), 2.0f);
            ImU32 hpCol = (hpR > 0.5f)
                ? IM_COL32((int)((1.0f-hpR)*2.0f*255), 220, 0, 220)
                : IM_COL32(255, (int)(hpR*2.0f*255), 0, 220);
            draw->AddRectFilled(ImVec2(boxMin.x, barY),
                                ImVec2(boxMin.x + W * hpR, barY + barH2), hpCol, 2.0f);
            char hpTxt[12]; snprintf(hpTxt, sizeof(hpTxt), "%d", entry.curHp);
            ImVec2 hpTs = ImGui::CalcTextSize(hpTxt);
            draw->AddText(ImVec2(centerX - hpTs.x * 0.5f + 1, barY + barH2 + 2),
                          IM_COL32(0,0,0,160), hpTxt);
            draw->AddText(ImVec2(centerX - hpTs.x * 0.5f, barY + barH2 + 1),
                          IM_COL32(220,220,220,200), hpTxt);
        }

        // â”€â”€ Snapline com ponta â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        if (drawSnapLine) {
            float bodyCY = (top + bot) * 0.5f;
            ImU32 slColor = isLocked
                ? IM_COL32(255,180,0,180)
                : IM_COL32(180,180,180,120);
            draw->AddLine(snapOrigin, ImVec2(centerX, bodyCY), slColor, 1.2f);
            // ponta triangular
            float dx = centerX - snapOrigin.x;
            float dy = bodyCY  - snapOrigin.y;
            float dl = sqrtf(dx*dx+dy*dy);
            if (dl > 10.0f) {
                float nx = dx/dl, ny = dy/dl;
                float px = -ny, py = nx;
                float tipLen = 6.0f;
                ImVec2 tip(centerX, bodyCY);
                ImVec2 p1(tip.x - nx*tipLen + px*3.5f, tip.y - ny*tipLen + py*3.5f);
                ImVec2 p2(tip.x - nx*tipLen - px*3.5f, tip.y - ny*tipLen - py*3.5f);
                draw->AddTriangleFilled(tip, p1, p2, slColor);
            }
        }

        // â”€â”€ DistÃ¢ncia â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        if (drawDistance) {
            char distText[16];
            snprintf(distText, sizeof(distText), "%.0fm", entry.distance);
            ImVec2 dts = ImGui::CalcTextSize(distText);
            float dx2 = centerX - dts.x * 0.5f;
            float dy2 = bot + padY + 7.0f;
            draw->AddText(ImVec2(dx2+1,dy2+1), IM_COL32(0,0,0,150), distText);
            draw->AddText(ImVec2(dx2,  dy2),   IM_COL32(200,200,200,180), distText);
        }
    }

    // â”€â”€ FOV Circle â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    if (showFovCircle && aimbotEnabled) {
        float fovRadiusPx = (float)screenW * (aimbotFovDeg / 90.0f) * 0.5f;
        bool locked = sharedData && sharedData->aimbotHasTarget;
        ImU32 circleColor = locked
            ? IM_COL32(255, 140, 0, 180)
            : IM_COL32(200, 200, 200, 50);
        draw->AddCircle(
            ImVec2(screenW * 0.5f, screenH * 0.5f),
            fovRadiusPx, circleColor, 64, 1.5f);
    }

    lastWriteSeq = seq;
}

// ============================================================
// Hook Log Reader â€” lÃª o arquivo de log do hook para diagnostico
// Tenta: game dir, /data/local/tmp/, /sdcard/
// ============================================================
#define HOOK_LOG_PATH_1 "/data/local/tmp/.gl_log"
#define HOOK_LOG_PATH_2 "/sdcard/.gl_log"
static char hookLogBuf[2048] = "Nenhum log do hook";
static time_t hookLogLastRead = 0;

static void readHookLog() {
    // Ler no maximo a cada 2 segundos
    time_t now = time(nullptr);
    if (now - hookLogLastRead < 2) return;
    hookLogLastRead = now;

    // Paths: /data/local/tmp/ PRIMEIRO (onde o hook escreve)
    char gameLogPath[512];
    snprintf(gameLogPath, sizeof(gameLogPath), "/data/data/%s/%s", GAME_PACKAGE, HOOKLOG_FILENAME);
    const char* paths[] = { HOOK_LOG_PATH_1, gameLogPath, HOOK_LOG_PATH_2 };
    for (int i = 0; i < 3; i++) {
        int fd = open(paths[i], O_RDONLY);
        if (fd >= 0) {
            off_t sz = lseek(fd, 0, SEEK_END);
            if (sz > 0) {
                // Ler ultimos 2000 bytes
                off_t start = (sz > 2000) ? sz - 2000 : 0;
                lseek(fd, start, SEEK_SET);
                int toRead = (sz - start < (off_t)sizeof(hookLogBuf) - 1) ? (int)(sz - start) : (int)sizeof(hookLogBuf) - 1;
                int rd = read(fd, hookLogBuf, toRead);
                if (rd > 0) hookLogBuf[rd] = '\0';
            }
            close(fd);
            return; // Encontrou, para
        }
    }
}

// ============================================================
// Config â€” persiste em /data/local/tmp/.jawmods_cfg
// ============================================================
#define JAW_CONFIG_PATH  "/data/local/tmp/.jawmods_cfg"
#define JAW_CONFIG_MAGIC 0x4A41570Cu  // "JAW" v12 - advanced aimbot

#pragma pack(push, 1)
struct JawConfig {
    uint32_t magic;
    uint8_t  esp, drawBox, drawSnap, drawDist, drawNick, drawHead;
    float    espMaxDist;
    int32_t  hotkeyEsp;
    // Aimbot
    uint8_t  aimbotOn;
    float    aimbotFov, aimbotSmooth;
    uint8_t  aimbotIgnoreKnocked, aimbotThroughWalls, aimbotAlwaysTrack;
    int32_t  triggerKey;
    uint8_t  showFovCircle;
};
#pragma pack(pop)

static void saveConfig() {
    JawConfig c{};
    c.magic              = JAW_CONFIG_MAGIC;
    c.esp                = esp;
    c.drawBox            = drawEnemyBox;
    c.drawSnap           = drawSnapLine;
    c.drawDist           = drawDistance;
    c.drawNick           = drawNickName;
    c.drawHead           = drawHeadDot;
    c.espMaxDist         = espMaxDistance;
    c.hotkeyEsp          = hotkeyEsp;
    c.aimbotOn           = aimbotEnabled;
    c.aimbotFov          = aimbotFovDeg;
    c.aimbotSmooth       = aimbotSmooth;
    c.aimbotIgnoreKnocked= aimbotIgnoreKnocked;
    c.aimbotThroughWalls = aimbotThroughWalls;
    c.aimbotAlwaysTrack  = aimbotAlwaysTrack;
    c.triggerKey         = triggerKey;
    c.showFovCircle      = showFovCircle;
    int fd = open(JAW_CONFIG_PATH, O_CREAT | O_WRONLY | O_TRUNC, 0666);
    if (fd >= 0) { write(fd, &c, sizeof(c)); close(fd); }
}

static void loadConfig() {
    JawConfig c{};
    int fd = open(JAW_CONFIG_PATH, O_RDONLY);
    if (fd < 0) return;
    ssize_t rd = read(fd, &c, sizeof(c));
    close(fd);
    if (rd != (ssize_t)sizeof(c) || c.magic != JAW_CONFIG_MAGIC) return;
    esp                 = c.esp;
    drawEnemyBox        = c.drawBox;
    drawSnapLine        = c.drawSnap;
    drawDistance        = c.drawDist;
    drawNickName        = c.drawNick;
    drawHeadDot         = c.drawHead;
    espMaxDistance      = c.espMaxDist;
    hotkeyEsp           = c.hotkeyEsp;
    aimbotEnabled       = c.aimbotOn;
    aimbotFovDeg        = c.aimbotFov;
    aimbotSmooth        = c.aimbotSmooth;
    aimbotIgnoreKnocked = c.aimbotIgnoreKnocked;
    aimbotThroughWalls  = c.aimbotThroughWalls;
    aimbotAlwaysTrack   = c.aimbotAlwaysTrack;
    triggerKey          = c.triggerKey;
    showFovCircle       = c.showFovCircle;
}

// ============================================================
// Server Status Check â€” TCP connect ao servidor jawmods
// Atualizar SERVER_CHECK_HOST se a URL do SquareCloud mudar.
// ============================================================
#define SERVER_CHECK_HOST "jawmods-key-server.squareweb.app"
#define SERVER_CHECK_PORT "443"

static std::atomic<bool> g_serverOnline{false};
static char g_serverStatus[64] = "Verificando...";

static void* serverCheckThread(void*) {
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(SERVER_CHECK_HOST, SERVER_CHECK_PORT, &hints, &res) != 0 || !res) {
        snprintf(g_serverStatus, sizeof(g_serverStatus), "Offline (DNS)");
        g_serverOnline.store(false);
        return nullptr;
    }
    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        freeaddrinfo(res);
        snprintf(g_serverStatus, sizeof(g_serverStatus), "Offline");
        g_serverOnline.store(false);
        return nullptr;
    }
    struct timeval tv{};
    tv.tv_sec = 5;
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int r = connect(sock, res->ai_addr, res->ai_addrlen);
    close(sock);
    freeaddrinfo(res);
    if (r == 0) {
        g_serverOnline.store(true);
        snprintf(g_serverStatus, sizeof(g_serverStatus), "Online");
    } else {
        g_serverOnline.store(false);
        snprintf(g_serverStatus, sizeof(g_serverStatus), "Offline");
    }
    return nullptr;
}

// ============================================================
// Hotkeys â€” lÃª eventos de volume do /dev/input/event*
// MÃºltiplos processos podem ler o mesmo eventX sem interferir.
// ============================================================
static std::atomic<bool> g_hotkeyRunning{false};
static pthread_t         g_hotkeyThread = 0;

static void* hotkeyThread(void*) {
    // Encontrar device com KEY_VOLUMEUP (bit 115)
    int evfd = -1;
    for (int i = 0; i < 20 && evfd < 0; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        uint8_t keybits[128] = {};  // 1024 bits, cobre KEY_VOLUMEUP=115
        if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits) >= 0 &&
            (keybits[115 / 8] & (1u << (115 % 8)))) {
            evfd = fd;
            break;
        }
        close(fd);
    }
    if (evfd < 0) return nullptr;

    struct input_event ev{};
    while (g_hotkeyRunning.load()) {
        ssize_t rd = read(evfd, &ev, sizeof(ev));
        if (rd < (ssize_t)sizeof(ev)) { usleep(8000); continue; }
        if (ev.type != EV_KEY) continue;
        // â”€â”€ Trigger key: detectar HOLD/RELEASE em tempo real â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        // Se triggerKey estÃ¡ configurado, essa tecla faz "hold-to-aim".
        // Tecla de trigger nÃ£o aciona toggles de features.
        // â”€â”€ Toggles normais sÃ³ em key-press (value==1) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        // Trigger key: press/hold=1 ativa, release=0 desativa
        if (triggerKey > 0 && ev.code == (uint16_t)triggerKey) {
            if (sharedData) sharedData->triggerHeld = (ev.value >= 1) ? 1 : 0;
            continue;
        }
        if (ev.value != 1) continue;
        if (hotkeyEsp > 0 && ev.code == (uint16_t)hotkeyEsp)
            esp = !esp;
    }
    close(evfd);
    return nullptr;
}

// ============================================================
// Animated Toggle Switch â€” iOS-style, suave, usa ImGui storage
// ============================================================
static bool AnimatedToggle(const char* id, bool* v) {
    ImGuiStorage* storage = ImGui::GetStateStorage();
    ImGuiID       sid     = ImGui::GetID(id);
    float anim = storage->GetFloat(sid, *v ? 1.0f : 0.0f);
    anim += ((*v ? 1.0f : 0.0f) - anim) * ImGui::GetIO().DeltaTime * 14.0f;
    if (anim > 0.99f) anim = 1.0f;
    if (anim < 0.01f) anim = 0.0f;
    storage->SetFloat(sid, anim);

    const float H = 18.0f, W = 34.0f, R = H * 0.5f;
    ImVec2 p  = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    ImGui::InvisibleButton(id, ImVec2(W, H));
    bool changed = false;
    if (ImGui::IsItemClicked()) { *v = !*v; changed = true; }

    // Track: lerp de cinza para verde
    int tr = (int)(50  + anim * (-50));
    int tg = (int)(55  + anim * (195 - 55));
    int tb = (int)(60  + anim * (95  - 60));
    dl->AddRectFilled(p, ImVec2(p.x + W, p.y + H), IM_COL32(tr, tg, tb, 255), R);

    // Knob deslizante
    float knobX = p.x + R + anim * (W - R * 2.0f);
    dl->AddCircleFilled(ImVec2(knobX, p.y + R), R - 2.5f, IM_COL32(230, 232, 235, 255));

    return changed;
}

// ============================================================
// Draw Menu â€” Cyberpunk Dark v60
// ============================================================
void DrawMenu() {
    // â”€â”€ Paleta Cyberpunk â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    const ImU32 cBg        = IM_COL32(10,  11,  15,  245);
    const ImU32 cPanel     = IM_COL32(16,  17,  22,  255);
    const ImU32 cBorder    = IM_COL32(38,  42,  55,  255);
    const ImU32 cViolet    = IM_COL32(130, 80,  255, 255);
    const ImU32 cVioletDim = IM_COL32(80,  50,  160, 200);
    const ImU32 cGreen     = IM_COL32(0,   225, 120, 255);
    const ImU32 cGreenDim  = IM_COL32(0,   130, 70,  200);
    const ImU32 cRed       = IM_COL32(255, 80,  80,  255);
    const ImU32 cText      = IM_COL32(210, 212, 220, 255);
    const ImU32 cTextDim   = IM_COL32(100, 105, 120, 255);
    const ImU32 cYellow    = IM_COL32(255, 200, 50,  255);

    const ImVec4 cv4Violet  = ImVec4(0.51f, 0.31f, 1.00f, 1.00f);
    const ImVec4 cv4Green   = ImVec4(0.00f, 0.88f, 0.47f, 1.00f);
    const ImVec4 cv4Red     = ImVec4(1.00f, 0.31f, 0.31f, 1.00f);
    const ImVec4 cv4Dim     = ImVec4(0.39f, 0.41f, 0.47f, 1.00f);
    const ImVec4 cv4Text    = ImVec4(0.82f, 0.83f, 0.86f, 1.00f);
    const ImVec4 cv4Yellow  = ImVec4(1.00f, 0.78f, 0.20f, 1.00f);

    bool shmReady   = shmConnected.load() && sharedData && sharedData->magic == 0xDEADF00D;
    bool vmtApplied = shmReady && sharedData->hookApplied == 0xBEEF1234;

    static float fadeAlpha   = 0.0f;
    static bool menuMinimized = false;
    if (fadeAlpha < 1.0f) {
        fadeAlpha += ImGui::GetIO().DeltaTime * 5.0f;
        if (fadeAlpha > 1.0f) fadeAlpha = 1.0f;
    }

    // â”€â”€ Window Styles â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,  ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,    ImVec2(8.0f, 5.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,   ImVec2(6.0f, 3.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,  5.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize,  6.0f);

    ImGui::PushStyleColor(ImGuiCol_WindowBg,             ImVec4(0.04f, 0.04f, 0.06f, 0.96f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg,              ImVec4(0.00f, 0.00f, 0.00f, 0.00f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab,           cv4Violet);
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive,     ImVec4(0.70f, 0.45f, 1.00f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_CheckMark,            cv4Violet);
    ImGui::PushStyleColor(ImGuiCol_FrameBg,              ImVec4(0.10f, 0.10f, 0.14f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,       ImVec4(0.14f, 0.14f, 0.20f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive,        ImVec4(0.18f, 0.18f, 0.26f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Header,               ImVec4(0.30f, 0.18f, 0.60f, 0.40f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered,        ImVec4(0.40f, 0.25f, 0.75f, 0.55f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,         ImVec4(0.51f, 0.31f, 1.00f, 0.70f));
    ImGui::PushStyleColor(ImGuiCol_Button,               ImVec4(0.30f, 0.18f, 0.60f, 0.75f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,        ImVec4(0.45f, 0.28f, 0.85f, 0.90f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,         ImVec4(0.60f, 0.38f, 1.00f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_PopupBg,              ImVec4(0.07f, 0.07f, 0.10f, 0.97f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg,          ImVec4(0.04f, 0.04f, 0.06f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab,        ImVec4(0.30f, 0.18f, 0.60f, 0.80f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, cv4Violet);
    ImGui::PushStyleColor(ImGuiCol_Separator,            ImVec4(0.15f, 0.15f, 0.22f, 1.00f));

    const float FULL_H  = 500.0f;
    const float MINI_H  = 46.0f;
    const float HEADER_H = 46.0f;
    const float FOOTER_H = 28.0f;
    const float TAB_H    = 36.0f;

    static const float kMenuWidths[] = { 290.0f, 340.0f, 400.0f };
    static int  menuSizeLevel   = 1;
    static bool prevMinimized   = false;
    float menuW    = kMenuWidths[menuSizeLevel];
    float targetH  = menuMinimized ? MINI_H : FULL_H;

    ImGui::SetNextWindowPos(ImVec2(8.0f, 8.0f), ImGuiCond_Once);
    bool sizeTransition = (menuMinimized != prevMinimized);
    if (sizeTransition) {
        ImGui::SetNextWindowSize(ImVec2(menuW, targetH), ImGuiCond_Always);
    } else if (!menuMinimized) {
        ImGui::SetNextWindowSizeConstraints(ImVec2(260.0f, 320.0f), ImVec2(600.0f, 800.0f));
    } else {
        ImGui::SetNextWindowSize(ImVec2(menuW, MINI_H), ImGuiCond_Always);
    }
    prevMinimized = menuMinimized;

    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::Begin("##jaw_menu", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse);

    ImDrawList* dl  = ImGui::GetWindowDrawList();
    ImVec2 wpos     = ImGui::GetWindowPos();
    ImVec2 wsz      = ImGui::GetWindowSize();
    float  W        = wsz.x;
    float  winH     = wsz.y;

    // â”€â”€ Background â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    dl->AddRectFilled(wpos, ImVec2(wpos.x+W, wpos.y+winH), cBg, 12.0f);
    dl->AddRect(wpos, ImVec2(wpos.x+W, wpos.y+winH), cBorder, 12.0f, 0, 1.0f);
    // Accent line top
    dl->AddRectFilled(
        ImVec2(wpos.x+12, wpos.y+1),
        ImVec2(wpos.x+W*0.45f, wpos.y+2),
        cViolet);

    // â”€â”€ HEADER â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    dl->AddRectFilled(wpos, ImVec2(wpos.x+W, wpos.y+HEADER_H), cPanel, 12.0f,
                      ImDrawFlags_RoundCornersTop);
    dl->AddLine(ImVec2(wpos.x,wpos.y+HEADER_H), ImVec2(wpos.x+W,wpos.y+HEADER_H), cBorder);

    // Status dot animado
    static float dotPhase = 0.0f;
    dotPhase += ImGui::GetIO().DeltaTime * 2.5f;
    float dotAlpha = 0.5f + 0.5f * sinf(dotPhase);
    ImU32 dotColor = vmtApplied ? IM_COL32(0,225,120,(int)(200*dotAlpha))
                    : shmReady  ? IM_COL32(255,200,50,(int)(200*dotAlpha))
                    :             IM_COL32(180,60,60,(int)(200*dotAlpha));
    dl->AddCircleFilled(ImVec2(wpos.x+16, wpos.y+HEADER_H*0.5f), 5.0f, dotColor);

    // TÃ­tulo
    ImGui::SetCursorPos(ImVec2(26.0f, 12.0f));
    ImGui::TextColored(cv4Violet, "JAW");
    ImGui::SameLine(0.0f, 4.0f);
    ImGui::TextColored(cv4Text, "MODS");
    ImGui::SameLine(0.0f, 6.0f);
    ImGui::TextColored(cv4Dim, "v60");

    // BotÃµes direita
    float btnY = 8.0f;
    float btnX = W - 68.0f;
    ImGui::SetCursorPos(ImVec2(btnX, btnY));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f,0.18f,0.26f,0.9f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f,0.18f,0.60f,1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.51f,0.31f,1.0f,1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f,4.0f));
    const char* sizeLabel = (menuSizeLevel==0?"P":(menuSizeLevel==1?"M":"G"));
    if (ImGui::Button(sizeLabel, ImVec2(22,22))) {
        menuSizeLevel = (menuSizeLevel+1)%3;
        menuW = kMenuWidths[menuSizeLevel];
        ImGui::SetNextWindowSize(ImVec2(menuW, winH), ImGuiCond_Always);
    }
    ImGui::SameLine(0.0f, 4.0f);
    if (ImGui::Button(menuMinimized ? "+" : "-", ImVec2(22,22)))
        menuMinimized = !menuMinimized;
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);

    if (menuMinimized) {
        ImGui::End();
        ImGui::PopStyleColor(19);
        ImGui::PopStyleVar(6);
        return;
    }

    // â”€â”€ TABS â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    static int activeTab = 0;
    const char* tabNames[] = { "ESP", "AIM", "MISC" };
    const int   numTabs    = 3;

    dl->AddRectFilled(
        ImVec2(wpos.x, wpos.y+HEADER_H),
        ImVec2(wpos.x+W, wpos.y+HEADER_H+TAB_H), cPanel);

    float tabW = W / numTabs;
    for (int i = 0; i < numTabs; i++) {
        bool isActive = (i == activeTab);
        ImVec2 tl(wpos.x + i*tabW, wpos.y+HEADER_H);
        ImVec2 br(tl.x+tabW, tl.y+TAB_H);
        if (isActive) {
            dl->AddRectFilled(tl, br, IM_COL32(26,22,40,255));
            dl->AddRectFilled(
                ImVec2(tl.x+4, br.y-2),
                ImVec2(br.x-4, br.y),
                cViolet, 1.0f);
        }
        ImGui::SetCursorPos(ImVec2(i*tabW, HEADER_H));
        ImGui::InvisibleButton(tabNames[i], ImVec2(tabW, TAB_H));
        if (ImGui::IsItemClicked()) activeTab = i;
        ImVec2 tsz = ImGui::CalcTextSize(tabNames[i]);
        ImU32 tCol = isActive ? cViolet : cTextDim;
        dl->AddText(ImVec2(tl.x+(tabW-tsz.x)*0.5f, tl.y+(TAB_H-tsz.y)*0.5f), tCol, tabNames[i]);
    }
    dl->AddLine(
        ImVec2(wpos.x, wpos.y+HEADER_H+TAB_H),
        ImVec2(wpos.x+W, wpos.y+HEADER_H+TAB_H), cBorder);

    // â”€â”€ CONTENT â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    float contentTop  = HEADER_H + TAB_H + 2.0f;
    float contentBotY = winH - FOOTER_H - 4.0f;
    float contentH    = contentBotY - contentTop;

    ImGui::SetCursorPos(ImVec2(0.0f, contentTop));
    ImGui::BeginChild("##content", ImVec2(W, contentH), false,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::SetCursorPos(ImVec2(12.0f, 8.0f));

    // Helper: Section header
    auto SectionHeader = [&](const char* title) {
        ImGui::Spacing();
        ImVec2 cp = ImGui::GetCursorScreenPos();
        ImDrawList* cdl = ImGui::GetWindowDrawList();
        float fw = ImGui::GetContentRegionAvail().x - 12.0f;
        cdl->AddRectFilled(ImVec2(cp.x-4,cp.y), ImVec2(cp.x+fw,cp.y+18), IM_COL32(26,22,40,255), 4.0f);
        cdl->AddRectFilled(ImVec2(cp.x-4,cp.y), ImVec2(cp.x+3,cp.y+18), cViolet, 2.0f);
        ImGui::SetCursorScreenPos(ImVec2(cp.x+8, cp.y+2));
        ImGui::TextColored(cv4Violet, "%s", title);
        ImGui::SetCursorScreenPos(ImVec2(cp.x, cp.y+22));
        ImGui::Spacing();
    };

    // Helper: Toggle Row com AnimatedToggle
    auto ToggleRow = [&](const char* label, bool* val) {
        float fw = ImGui::GetContentRegionAvail().x - 12.0f;
        ImGui::PushID(label);
        ImGui::TextColored(cv4Text, "%s", label);
        ImGui::SameLine(fw - 34.0f);
        AnimatedToggle("##tog", val);
        ImGui::PopID();
    };

    // Helper: Pill Status
    auto Pill = [&](const char* txt, ImU32 col, ImU32 textCol) {
        ImDrawList* pdl = ImGui::GetWindowDrawList();
        ImVec2 cp = ImGui::GetCursorScreenPos();
        ImVec2 tsz = ImGui::CalcTextSize(txt);
        float pw = tsz.x + 14.0f, ph = tsz.y + 6.0f;
        pdl->AddRectFilled(cp, ImVec2(cp.x+pw, cp.y+ph), col, ph*0.5f);
        pdl->AddText(ImVec2(cp.x+7, cp.y+3), textCol, txt);
        ImGui::Dummy(ImVec2(pw, ph));
    };

    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x - 12.0f);

    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    // TAB: ESP
    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    if (activeTab == 0) {
        SectionHeader("VISIBILIDADE");
        ToggleRow("ESP Ligado",     &esp);
        ImGui::Spacing();
        ToggleRow("Caixas",         &drawEnemyBox);
        ImGui::Spacing();
        ToggleRow("Snaplines",      &drawSnapLine);
        ImGui::Spacing();
        ToggleRow("Distancia",      &drawDistance);
        ImGui::Spacing();
        ToggleRow("Nick Name",      &drawNickName);
        ImGui::Spacing();
        ToggleRow("Ponto na Cabeca",&drawHeadDot);
        ImGui::Spacing();
        ToggleRow("Circulo FOV",    &showFovCircle);

        SectionHeader("FILTROS");
        ImGui::TextColored(cv4Dim, "Distancia Max");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 12.0f);
        ImGui::SliderFloat("##dist", &espMaxDistance, 50.0f, 999.0f, "%.0fm");

        ImGui::Spacing(); ImGui::Spacing();
        // Status pills
        if (shmReady) {
            int cnt = sharedData->playerCount;
            char buf[32]; snprintf(buf, sizeof(buf), "%d PLAYERS", cnt);
            Pill(buf, IM_COL32(26,22,40,255), cText);
        }
    }

    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    // TAB: AIM
    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    if (activeTab == 1) {
        SectionHeader("AIMBOT");
        ToggleRow("Aimbot Ligado", &aimbotEnabled);
        ImGui::Spacing();

        if (aimbotEnabled) {
            // Status
            bool hasLock = shmReady && sharedData->aimbotHasTarget;
            if (hasLock) {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 4.0f);
                Pill("HEAD LOCKED", IM_COL32(255,140,0,200), cBg);
            } else if (aimbotAlwaysTrack) {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 4.0f);
                Pill("BUSCANDO ALVO", IM_COL32(50,50,80,200), cTextDim);
            } else {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 4.0f);
                Pill("HOLD TRIGGER", IM_COL32(50,50,80,200), cTextDim);
            }
            ImGui::Spacing();
        }

        SectionHeader("CONFIGURACOES");
        ImGui::TextColored(cv4Dim, "FOV (graus)");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 12.0f);
        ImGui::SliderFloat("##afov", &aimbotFovDeg, 5.0f, 180.0f, "%.0f");

        ImGui::Spacing();
        ImGui::TextColored(cv4Dim, "Suavidade");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 12.0f);
        ImGui::SliderFloat("##asmooth", &aimbotSmooth, 0.0f, 0.95f, "%.2f");

        SectionHeader("OPCOES");
        ToggleRow("Rastrear Sempre", &aimbotAlwaysTrack);
        ImGui::Spacing();
        ToggleRow("Ignorar Knocked", &aimbotIgnoreKnocked);
        ImGui::Spacing();
        ToggleRow("Atravessar Paredes", &aimbotThroughWalls);
    }

    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    // TAB: MISC
    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    if (activeTab == 2) {
        SectionHeader("CAPTURA");
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.30f,0.18f,0.60f,0.75f));
        float bw2 = (ImGui::GetContentRegionAvail().x - 20.0f) * 0.5f;
        if (ImGui::Button("Screenshot", ImVec2(bw2, 26)))  doScreenshot();
        ImGui::SameLine(0.0f, 6.0f);
        if (!g_recordingActive) {
            if (ImGui::Button("Gravar", ImVec2(bw2, 26))) startRecording();
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f,0.15f,0.15f,0.9f));
            if (ImGui::Button("Parar", ImVec2(bw2, 26)))  stopRecording();
            ImGui::PopStyleColor();
        }
        ImGui::PopStyleColor();

        SectionHeader("STATUS");
        // ConexÃ£o
        ImGui::TextColored(cv4Dim, "Hook:");
        ImGui::SameLine();
        if (vmtApplied) ImGui::TextColored(cv4Green, "ATIVO");
        else if (shmReady) ImGui::TextColored(cv4Yellow, "AGUARDANDO VMT");
        else ImGui::TextColored(cv4Red, "SEM CONEXAO");

        ImGui::TextColored(cv4Dim, "Servidor:");
        ImGui::SameLine();
        bool online = g_serverOnline.load();
        ImGui::TextColored(online ? cv4Green : cv4Red, "%s", g_serverStatus);

        if (shmReady) {
            ImGui::Spacing();
            ImGui::TextColored(cv4Dim, "DebugCall: %d", sharedData->debugLastCall);
            ImGui::TextColored(cv4Dim, "Players:   %d", sharedData->playerCount);
        }

        SectionHeader("DIAGNOSTICO");
        readHookLog();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 12.0f);
        ImGui::InputTextMultiline("##hooklog", hookLogBuf, sizeof(hookLogBuf),
            ImVec2(-1.0f, 100.0f), ImGuiInputTextFlags_ReadOnly);
    }

    ImGui::PopItemWidth();
    ImGui::EndChild();

    // â”€â”€ FOOTER â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    dl->AddRectFilled(
        ImVec2(wpos.x+1, wpos.y+winH-FOOTER_H),
        ImVec2(wpos.x+W-1, wpos.y+winH),
        cPanel, 12.0f, ImDrawFlags_RoundCornersBottom);
    dl->AddLine(
        ImVec2(wpos.x+1, wpos.y+winH-FOOTER_H),
        ImVec2(wpos.x+W-1, wpos.y+winH-FOOTER_H), cBorder);

    ImGui::SetCursorPos(ImVec2(14.0f, winH - FOOTER_H + 7.0f));
    ImGui::TextColored(cv4Dim, "jawmods.app");
    ImGui::SameLine();
    // ESP status pill no footer
    if (esp) {
        ImGui::TextColored(cv4Green, "ESP");
    } else {
        ImGui::TextColored(cv4Dim, "esp off");
    }
    ImGui::SameLine();
    if (aimbotEnabled) {
        ImGui::TextColored(ImVec4(1.0f,0.78f,0.20f,1.0f), "AIM");
    }

    // Salvar config ao fechar
    static float saveTimer = 0.0f;
    saveTimer += ImGui::GetIO().DeltaTime;
    if (saveTimer > 5.0f) { saveTimer = 0.0f; saveConfig(); }

    ImGui::End();

    ImGui::PopStyleColor(19);
    ImGui::PopStyleVar(6);
}

// ============================================================
// Overlay Draw Callback - Chamado a cada frame pelo Overlay
// ============================================================
void onOverlayDraw(int screenW, int screenH) {
    // Gravar dimensoes como fallback SOMENTE se o hook ainda nao inicializou.
    if (sharedData && shmConnected.load() && sharedData->screenW <= 0) {
        sharedData->screenW = screenW;
        sharedData->screenH = screenH;
    }

    // Atualizar timer do toast de captura
    static uint64_t lastFrameUs = 0;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t nowUs = (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
    float dt = lastFrameUs ? (float)(nowUs - lastFrameUs) / 1000000.0f : 0.016f;
    lastFrameUs = nowUs;
    if (g_captureMsgTimer > 0.0f) g_captureMsgTimer -= dt;

    // Toast de captura (aparece sobre tudo)
    if (g_captureMsgTimer > 0.0f && g_captureMsg[0]) {
        ImDrawList* bg = ImGui::GetBackgroundDrawList();
        ImVec2 center((float)screenW * 0.5f, (float)screenH * 0.85f);
        ImVec2 tsz = ImGui::CalcTextSize(g_captureMsg);
        float alpha = (g_captureMsgTimer > 0.5f) ? 1.0f : (g_captureMsgTimer / 0.5f);
        bg->AddRectFilled(
            ImVec2(center.x - tsz.x * 0.5f - 10, center.y - tsz.y * 0.5f - 6),
            ImVec2(center.x + tsz.x * 0.5f + 10, center.y + tsz.y * 0.5f + 6),
            IM_COL32(0, 0, 0, (int)(180 * alpha)), 8.0f);
        bg->AddText(ImVec2(center.x - tsz.x * 0.5f, center.y - tsz.y * 0.5f),
            IM_COL32(255, 255, 255, (int)(255 * alpha)), g_captureMsg);
    }

    DrawESP(screenW, screenH);
    DrawMenu();
}

// ============================================================
// JNI - Chamados pelo OverlayService.java
// ============================================================
extern "C" {

JNIEXPORT void JNICALL
Java_com_android_support_OverlayService_nativeOnSurfaceCreated(
        JNIEnv* env, jclass, jobject surface, jint width, jint height) {

    MLOGI("nativeOnSurfaceCreated: w=%d h=%d surface=%p", width, height, surface);

    Overlay& overlay = Overlay::get();
    overlay.onDraw = onOverlayDraw;
    bool ok = overlay.init(env, surface, width, height);
    MLOGI("nativeOnSurfaceCreated: init=%s", ok ? "OK" : "FAILED");

    // Inicia thread de leitura SharedMemory
    if (!readerRunning.load()) {
        readerRunning.store(true);
        pthread_create(&readerThread, nullptr, shmReaderLoop, nullptr);
        MLOGI("shmReaderLoop started");
    }

    // Carregar configuracoes salvas
    loadConfig();

    // Verificar status do servidor (background)
    {
        pthread_t t;
        pthread_create(&t, nullptr, serverCheckThread, nullptr);
        pthread_detach(t);
    }

    // Iniciar thread de hotkeys (volume keys)
    if (!g_hotkeyRunning.load()) {
        g_hotkeyRunning.store(true);
        pthread_create(&g_hotkeyThread, nullptr, hotkeyThread, nullptr);
    }
}

JNIEXPORT void JNICALL
Java_com_android_support_OverlayService_nativeOnSurfaceDestroyed(
        JNIEnv*, jclass) {

    // Para hotkey thread
    g_hotkeyRunning.store(false);
    if (g_hotkeyThread) {
        pthread_join(g_hotkeyThread, nullptr);
        g_hotkeyThread = 0;
    }

    // Para reader thread
    readerRunning.store(false);
    if (readerThread) {
        pthread_join(readerThread, nullptr);
        readerThread = 0;
    }

    Overlay::get().destroy();
}

JNIEXPORT void JNICALL
Java_com_android_support_OverlayService_nativeOnTouch(
        JNIEnv*, jclass, jint action, jfloat x, jfloat y) {

    Overlay::get().handleTouch(action, x, y);
}

JNIEXPORT void JNICALL
Java_com_android_support_OverlayService_nativeSetScreenSize(
        JNIEnv*, jclass, jint width, jint height) {

    Overlay::get().setScreenSize(width, height);
}

JNIEXPORT jint JNICALL
JNI_OnLoad(JavaVM* vm, void*) {
    JNIEnv* env;
    if (vm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
        return JNI_ERR;
    }
    return JNI_VERSION_1_6;
}

} // extern "C"
