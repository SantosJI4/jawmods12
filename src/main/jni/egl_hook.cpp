/*
 * egl_hook.cpp — Hook de eglSwapBuffers para renderização ImGui unificada
 * ============================================================
 *
 * Menu toggle: toque 3x rápido no CANTO SUPERIOR DIREITO (80x80px)
 *   - Mais confiável que leitura de volume (que é bloqueada por SELinux)
 *   - Não depende de permissão especial de input
 *
 * Compilado apenas com -DUNIFIED_BUILD
 * ============================================================
 */

#ifdef UNIFIED_BUILD

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <ctime>
#include <atomic>
#include <linux/input.h>
#include <sys/prctl.h>
#include <sys/ioctl.h>
#include <sys/time.h>

#include "obfuscate.h"
#include "SharedData.h"
#include "key_validator.h"
#include "dobby.h"
#include "imgui.h"
#include "imgui_impl_opengl3.h"

// ── Declarações externas ──────────────────────────────────────────────────────
extern SharedESPData* sharedData;
extern void DrawESP(int screenW, int screenH);
extern void DrawMenu();

// ── Estado global ─────────────────────────────────────────────────────────────
static std::atomic<bool>  g_eglInitialized{false};
static std::atomic<bool>  g_menuOpen{false};
static int                g_screenW = 0, g_screenH = 0;

typedef EGLBoolean (*eglSwapBuffers_fn)(EGLDisplay, EGLSurface);
static eglSwapBuffers_fn  orig_eglSwapBuffers = nullptr;

// ── Touch state (thread-safe, atualizado pela thread de input) ────────────────
static std::atomic<float> g_touchX{-1.0f};
static std::atomic<float> g_touchY{-1.0f};
// touchEvent: 0=down, 1=up, 2=move, -1=nenhum
static std::atomic<int>   g_touchEvent{-1};
static std::atomic<bool>  g_touchRunning{false};

// ── Triple-tap corner para toggle do menu ─────────────────────────────────────
// Toque 3x em menos de 1.5s no canto superior direito (ou qualquer canto) = toggle
static double getTimeSec() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void handleMenuToggle(float tx, float ty) {
    if (g_screenW <= 0 || g_screenH <= 0) return;

    // Definir zona do canto superior direito (últimos 12% da largura, primeiros 8% da altura)
    float zoneW = g_screenW * 0.12f;
    float zoneH = g_screenH * 0.08f;
    bool inCorner = (tx >= g_screenW - zoneW) && (ty <= zoneH);

    if (!inCorner) return;

    static int tapCount = 0;
    static double firstTapTime = 0.0;
    double now = getTimeSec();

    if (tapCount == 0 || now - firstTapTime > 1.5) {
        tapCount = 1;
        firstTapTime = now;
    } else {
        tapCount++;
        if (tapCount >= 3) {
            g_menuOpen.store(!g_menuOpen.load());
            tapCount = 0;
        }
    }
}

// ── Thread de leitura de touch via /dev/input/event* ─────────────────────────
// Apenas para touch (coordenadas para ImGui)
// Volume keys via SELinux bloqueado — usar triple-tap no canto como alternativa
static void* inputReaderThread(void*) {
    // Usar nome de thread genérico do sistema (não "SurfaceFlinger" fixo)
    prctl(PR_SET_NAME, (char*)OBFUSCATE("android.display"), 0, 0, 0);

    int touchFd  = -1;
    int touchRangeX = 1080, touchRangeY = 1920;

    for (int i = 0; i < 32 && touchFd < 0; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;

        uint8_t evbits[(EV_MAX / 8) + 1] = {};
        ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits);

        bool hasAbs = (evbits[EV_ABS / 8] & (1 << (EV_ABS % 8))) != 0;
        if (hasAbs) {
            uint8_t absbits[(ABS_MAX / 8) + 1] = {};
            ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits);
            bool hasMT = (absbits[ABS_MT_POSITION_X / 8] &
                          (1 << (ABS_MT_POSITION_X % 8))) != 0;
            if (hasMT) {
                struct input_absinfo aix{}, aiy{};
                ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &aix);
                ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &aiy);
                if (aix.maximum > 0) touchRangeX = aix.maximum;
                if (aiy.maximum > 0) touchRangeY = aiy.maximum;
                touchFd = fd;
                continue;
            }
        }
        close(fd);
    }

    struct input_event ev{};
    float curX = 0.0f, curY = 0.0f;
    bool  touching = false;

    while (g_touchRunning.load()) {
        if (touchFd < 0) { usleep(50000); continue; }

        bool anyRead = false;
        while (read(touchFd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
            anyRead = true;
            if (ev.type == EV_ABS) {
                if (ev.code == ABS_MT_POSITION_X) {
                    float w = (float)(g_screenW > 0 ? g_screenW : touchRangeX);
                    curX = (float)ev.value / (float)touchRangeX * w;
                } else if (ev.code == ABS_MT_POSITION_Y) {
                    float h = (float)(g_screenH > 0 ? g_screenH : touchRangeY);
                    curY = (float)ev.value / (float)touchRangeY * h;
                }
            } else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
                if (ev.value == 1) {
                    touching = true;
                    g_touchX.store(curX); g_touchY.store(curY);
                    g_touchEvent.store(0); // DOWN
                } else {
                    touching = false;
                    g_touchEvent.store(1); // UP
                }
            } else if (ev.type == EV_SYN && touching) {
                g_touchX.store(curX); g_touchY.store(curY);
                g_touchEvent.store(2); // MOVE
            }
        }
        // ~60 Hz quando ocioso — menos read()/syscall que 4ms em loop apertado
        if (!anyRead) usleep(16666);
    }

    if (touchFd >= 0) close(touchFd);
    return nullptr;
}

// ── Inicialização do ImGui ────────────────────────────────────────────────────
static bool initImGui(EGLDisplay dpy, EGLSurface surf) {
    int w = 0, h = 0;
    eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
    if (w <= 0 || h <= 0) return false;
    if (eglGetCurrentContext() == EGL_NO_CONTEXT) return false;

    g_screenW = w; g_screenH = h;

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize  = ImVec2((float)w, (float)h);
    io.IniFilename  = nullptr;
    io.LogFilename  = nullptr;

    float scale = (float)w / 1080.0f;
    if (scale < 0.5f) scale = 0.5f;
    if (scale > 3.0f) scale = 3.0f;
    io.FontGlobalScale = scale;
    ImGui::GetStyle().ScaleAllSizes(scale);

    // Estilo visual
    ImGuiStyle& sty = ImGui::GetStyle();
    ImVec4* c = sty.Colors;
    c[ImGuiCol_WindowBg]         = ImVec4(0.04f,0.04f,0.07f,0.96f);
    c[ImGuiCol_Border]           = ImVec4(0.30f,0.18f,0.60f,0.60f);
    c[ImGuiCol_TitleBg]          = ImVec4(0.06f,0.06f,0.10f,1.0f);
    c[ImGuiCol_TitleBgActive]    = ImVec4(0.10f,0.06f,0.20f,1.0f);
    c[ImGuiCol_Header]           = ImVec4(0.30f,0.18f,0.60f,0.45f);
    c[ImGuiCol_HeaderHovered]    = ImVec4(0.45f,0.28f,0.85f,0.65f);
    c[ImGuiCol_HeaderActive]     = ImVec4(0.60f,0.38f,1.0f,0.80f);
    c[ImGuiCol_Button]           = ImVec4(0.18f,0.10f,0.38f,0.80f);
    c[ImGuiCol_ButtonHovered]    = ImVec4(0.35f,0.20f,0.70f,1.0f);
    c[ImGuiCol_ButtonActive]     = ImVec4(0.50f,0.30f,0.90f,1.0f);
    c[ImGuiCol_SliderGrab]       = ImVec4(0.50f,0.30f,0.90f,1.0f);
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.70f,0.50f,1.0f,1.0f);
    c[ImGuiCol_CheckMark]        = ImVec4(0.50f,0.30f,0.90f,1.0f);
    c[ImGuiCol_FrameBg]          = ImVec4(0.08f,0.08f,0.13f,1.0f);
    c[ImGuiCol_FrameBgHovered]   = ImVec4(0.12f,0.12f,0.20f,1.0f);
    c[ImGuiCol_Tab]              = ImVec4(0.10f,0.06f,0.20f,1.0f);
    c[ImGuiCol_TabSelected]      = ImVec4(0.30f,0.18f,0.60f,1.0f);
    c[ImGuiCol_Separator]        = ImVec4(0.30f,0.18f,0.60f,0.50f);
    c[ImGuiCol_Text]             = ImVec4(0.82f,0.83f,0.86f,1.0f);
    sty.WindowRounding    = 10.0f;
    sty.FrameRounding     = 5.0f;
    sty.GrabRounding      = 4.0f;
    sty.ScrollbarRounding = 5.0f;
    sty.WindowBorderSize  = 1.0f;

    ImGui_ImplOpenGL3_Init("#version 300 es");

    // Iniciar thread de touch
    g_touchRunning.store(true);
    pthread_t t;
    pthread_create(&t, nullptr, inputReaderThread, nullptr);
    pthread_detach(t);

    // Iniciar validação de key
    startKeyValidation();

    return true;
}

// ── Salvar/restaurar estado OpenGL ────────────────────────────────────────────
struct GLState {
    GLint fbo, viewport[4], prog;
    GLboolean blend, depthTest, scissorTest, cullFace;
    GLint blendSrcRGB, blendDstRGB, blendSrcAlpha, blendDstAlpha;
};

static void saveGLState(GLState& s) {
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &s.fbo);
    glGetIntegerv(GL_VIEWPORT, s.viewport);
    glGetIntegerv(GL_CURRENT_PROGRAM, &s.prog);
    s.blend       = glIsEnabled(GL_BLEND);
    s.depthTest   = glIsEnabled(GL_DEPTH_TEST);
    s.scissorTest = glIsEnabled(GL_SCISSOR_TEST);
    s.cullFace    = glIsEnabled(GL_CULL_FACE);
    glGetIntegerv(GL_BLEND_SRC_RGB,   &s.blendSrcRGB);
    glGetIntegerv(GL_BLEND_DST_RGB,   &s.blendDstRGB);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &s.blendSrcAlpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &s.blendDstAlpha);
}

static void restoreGLState(const GLState& s) {
    glBindFramebuffer(GL_FRAMEBUFFER, s.fbo);
    glViewport(s.viewport[0], s.viewport[1], s.viewport[2], s.viewport[3]);
    glUseProgram(s.prog);
    if (s.blend)       glEnable(GL_BLEND);        else glDisable(GL_BLEND);
    if (s.depthTest)   glEnable(GL_DEPTH_TEST);   else glDisable(GL_DEPTH_TEST);
    if (s.scissorTest) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (s.cullFace)    glEnable(GL_CULL_FACE);    else glDisable(GL_CULL_FACE);
    glBlendFuncSeparate(s.blendSrcRGB, s.blendDstRGB,
                        s.blendSrcAlpha, s.blendDstAlpha);
}

// ── Hook de eglSwapBuffers ────────────────────────────────────────────────────
static EGLBoolean hooked_eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {

    // Inicializar ImGui na primeira chamada válida
    if (!g_eglInitialized.load()) {
        if (initImGui(dpy, surface)) {
            g_eglInitialized.store(true);
        } else {
            return orig_eglSwapBuffers(dpy, surface);
        }
    }

    // Sempre atualizar dimensões (leve) — necessário antes do fast-path de lobby
    {
        int w = 0, h = 0;
        eglQuerySurface(dpy, surface, EGL_WIDTH, &w);
        eglQuerySurface(dpy, surface, EGL_HEIGHT, &h);
        if (w > 0 && h > 0 && (w != g_screenW || h != g_screenH)) {
            g_screenW = w; g_screenH = h;
            ImGui::GetIO().DisplaySize = ImVec2((float)w, (float)h);
        }
    }

    const bool keyValid = isKeyValid();
    const bool inMatch = sharedData && sharedData->magic == 0xDEADF00D
                         && sharedData->playerCount > 0;

    // Touch: triple-tap do menu precisa ser processado mesmo no fast-path
    int tevt = g_touchEvent.exchange(-1);
    float tx  = g_touchX.load();
    float ty  = g_touchY.load();
    if (tevt == 0) {
        handleMenuToggle(tx, ty);
    }

    bool menuOpen = g_menuOpen.load();

    // Fast-path: key OK, menu fechado, fora de partida — sem ImGui/GL state churn.
    // Reduz drasticamente assinatura de hook (glGet*, blend, ImGui) no lobby.
    if (keyValid && !menuOpen && !inMatch) {
        return orig_eglSwapBuffers(dpy, surface);
    }

    ImGuiIO& io = ImGui::GetIO();

    if (tevt == 0) { // TOUCH DOWN
        if (menuOpen || !keyValid) {
            io.AddMousePosEvent(tx, ty);
            io.AddMouseButtonEvent(0, true);
        }
    } else if (tevt == 1) { // TOUCH UP
        io.AddMouseButtonEvent(0, false);
    } else if (tevt == 2) { // TOUCH MOVE
        if (menuOpen || !keyValid) {
            io.AddMousePosEvent(tx, ty);
        }
    }

    // ── Nova frame ImGui ──────────────────────────────────────────────────────
    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();

    // Salvar estado GL para não corromper o render do jogo
    GLState glState;
    saveGLState(glState);
    glViewport(0, 0, g_screenW, g_screenH);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                        GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    // ── Desenhar conteúdo ──────────────────────────────────────────────────────
    if (!keyValid) {
        DrawKeyUI();
    } else {
        if (inMatch) {
            DrawESP(g_screenW, g_screenH);
        }

        if (g_menuOpen.load()) {
            DrawMenu();
        }
        // Indicador removido no lobby: qualquer overlay persistente no swap chain
        // aumenta superfície de detecção; menu/ESP só quando necessário.
    }

    // ── Render ────────────────────────────────────────────────────────────────
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    restoreGLState(glState);

    return orig_eglSwapBuffers(dpy, surface);
}

// ── Instalar hook ─────────────────────────────────────────────────────────────
void installEglHook() {
    // Delay aleatório entre 800ms e 2800ms
    // Fingerprint fixo de inicialização é detectável pelo anti-cheat
    srand((unsigned int)time(nullptr) ^ (unsigned int)(uintptr_t)&installEglHook);
    int delayMs = 800 + (rand() % 2000);
    usleep((useconds_t)delayMs * 1000);

    void* sym = dlsym(RTLD_DEFAULT, OBFUSCATE("eglSwapBuffers"));
    if (!sym) {
        void* libEGL = dlopen(OBFUSCATE("libEGL.so"), RTLD_NOW | RTLD_GLOBAL);
        if (libEGL) sym = dlsym(libEGL, OBFUSCATE("eglSwapBuffers"));
    }
    if (!sym) return;

    DobbyHook(sym, (void*)hooked_eglSwapBuffers, (void**)&orig_eglSwapBuffers);
}

#endif // UNIFIED_BUILD
