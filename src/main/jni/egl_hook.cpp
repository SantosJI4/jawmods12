/*
 * egl_hook.cpp — Hook de eglSwapBuffers para renderização ImGui unificada
 * ============================================================
 *
 * Funcionamento:
 *   - Hookea eglSwapBuffers() da libEGL.so usando Dobby
 *   - No primeiro frame: inicializa ImGui com o contexto EGL do jogo
 *   - Todo frame: desenha Key UI (se key inválida) ou ESP+Menu
 *   - Touch: thread separada lê /dev/input/event* para o ImGui
 *   - Volume DOWN: abre/fecha o menu (igual ao overlay externo)
 *
 * Compilado apenas com -DUNIFIED_BUILD (libgl2_unified.so)
 * No modo root/Zygisk padrão, o overlay APK externo é usado.
 * ============================================================
 */

#ifdef UNIFIED_BUILD

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <atomic>
#include <linux/input.h>
#include <sys/prctl.h>
#include <sys/ioctl.h>

#include "obfuscate.h"
#include "SharedData.h"
#include "key_validator.h"

// Dobby para hook inline
#include "dobby.h"

// ImGui
#include "imgui.h"
#include "imgui_impl_opengl3.h"

// ── Declarações externas ──────────────────────────────────────────────────────
// GameHook.cpp exporta estes dados diretamente (mesma lib, mesmo processo)
extern SharedESPData* sharedData;

// main.cpp exporta DrawESP e DrawMenu (recompilado sem SHM reader)
extern void DrawESP(int screenW, int screenH);
extern void DrawMenu();

// ── Estado do EGL hook ────────────────────────────────────────────────────────
static std::atomic<bool>  g_eglInitialized{false};
static std::atomic<bool>  g_menuOpen{false};
static int                g_screenW = 0, g_screenH = 0;

// Ponteiro original de eglSwapBuffers
typedef EGLBoolean (*eglSwapBuffers_fn)(EGLDisplay, EGLSurface);
static eglSwapBuffers_fn  orig_eglSwapBuffers = nullptr;

// ── Touch state (thread-safe) ─────────────────────────────────────────────────
static std::atomic<float> g_touchX{-1.0f};
static std::atomic<float> g_touchY{-1.0f};
static std::atomic<int>   g_touchEvent{-1}; // 0=down, 1=up, 2=move
static std::atomic<bool>  g_touchRunning{false};

// ── Thread de leitura de input (touch + volume) ───────────────────────────────
static void* inputReaderThread(void*) {
    prctl(PR_SET_NAME, (char*)OBFUSCATE("InputReader"), 0, 0, 0);

    // Encontrar dispositivos de touch e teclado
    int touchFd  = -1;
    int keyFd    = -1;
    int touchRangeX = 1080, touchRangeY = 1920;

    for (int i = 0; i < 32; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;

        // Verificar capacidades
        uint8_t evbits[EV_MAX / 8 + 1] = {};
        ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits);

        // Touch: tem EV_ABS + ABS_MT_POSITION_X
        if ((evbits[EV_ABS / 8] & (1 << (EV_ABS % 8))) && touchFd < 0) {
            uint8_t absbits[ABS_MAX / 8 + 1] = {};
            ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits);
            if (absbits[ABS_MT_POSITION_X / 8] & (1 << (ABS_MT_POSITION_X % 8))) {
                // Obter range de coordenadas
                struct input_absinfo aix{}, aiy{};
                ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &aix);
                ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &aiy);
                if (aix.maximum > 0) touchRangeX = aix.maximum;
                if (aiy.maximum > 0) touchRangeY = aiy.maximum;
                touchFd = fd;
                continue;
            }
        }

        // Volume: tem EV_KEY + KEY_VOLUMEDOWN
        if ((evbits[EV_KEY / 8] & (1 << (EV_KEY % 8))) && keyFd < 0) {
            uint8_t keybits[KEY_MAX / 8 + 1] = {};
            ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits);
            if (keybits[KEY_VOLUMEDOWN / 8] & (1 << (KEY_VOLUMEDOWN % 8))) {
                keyFd = fd;
                continue;
            }
        }

        close(fd);
    }

    struct input_event ev{};
    float curX = 0, curY = 0;
    bool touching = false;
    int volPressCount = 0;
    double lastVolPress = 0.0;

    while (g_touchRunning.load()) {
        bool anyRead = false;

        // ── Touch ──
        if (touchFd >= 0) {
            while (read(touchFd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
                anyRead = true;
                if (ev.type == EV_ABS) {
                    if (ev.code == ABS_MT_POSITION_X) {
                        curX = (float)ev.value / touchRangeX *
                               (g_screenW > 0 ? g_screenW : touchRangeX);
                    } else if (ev.code == ABS_MT_POSITION_Y) {
                        curY = (float)ev.value / touchRangeY *
                               (g_screenH > 0 ? g_screenH : touchRangeY);
                    }
                } else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
                    if (ev.value == 1) {
                        touching = true;
                        g_touchX.store(curX); g_touchY.store(curY);
                        g_touchEvent.store(0); // down
                    } else {
                        touching = false;
                        g_touchEvent.store(1); // up
                    }
                } else if (ev.type == EV_SYN && touching) {
                    g_touchX.store(curX); g_touchY.store(curY);
                    g_touchEvent.store(2); // move
                }
            }
        }

        // ── Volume keys ──
        if (keyFd >= 0) {
            while (read(keyFd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
                anyRead = true;
                if (ev.type == EV_KEY && ev.code == KEY_VOLUMEDOWN && ev.value == 1) {
                    // Toggle menu
                    g_menuOpen.store(!g_menuOpen.load());
                }
                // Triple-press volume UP = sair / desativar (opcional)
            }
        }

        if (!anyRead) usleep(4000); // 4ms sleep quando não há eventos
    }

    if (touchFd >= 0) close(touchFd);
    if (keyFd >= 0) close(keyFd);
    return nullptr;
}

// ── Inicialização do ImGui ────────────────────────────────────────────────────
static bool initImGui(EGLDisplay dpy, EGLSurface surf) {
    // Obter tamanho da tela via EGL
    int w = 0, h = 0;
    eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
    if (w <= 0 || h <= 0) return false;
    g_screenW = w; g_screenH = h;

    // Verificar que há um contexto EGL válido
    if (eglGetCurrentContext() == EGL_NO_CONTEXT) return false;

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize    = ImVec2((float)w, (float)h);
    io.IniFilename    = nullptr;
    io.LogFilename    = nullptr;

    // Escala para mobile (telas de alta densidade)
    float scale = (float)w / 1080.0f;
    if (scale < 0.5f) scale = 0.5f;
    if (scale > 3.0f) scale = 3.0f;
    io.FontGlobalScale = scale;
    ImGui::GetStyle().ScaleAllSizes(scale);

    // Estilo cyberpunk (mesma paleta do overlay externo)
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg]          = ImVec4(0.04f, 0.04f, 0.07f, 0.96f);
    colors[ImGuiCol_Border]            = ImVec4(0.30f, 0.18f, 0.60f, 0.60f);
    colors[ImGuiCol_TitleBg]           = ImVec4(0.06f, 0.06f, 0.10f, 1.0f);
    colors[ImGuiCol_TitleBgActive]     = ImVec4(0.10f, 0.06f, 0.20f, 1.0f);
    colors[ImGuiCol_Header]            = ImVec4(0.30f, 0.18f, 0.60f, 0.45f);
    colors[ImGuiCol_HeaderHovered]     = ImVec4(0.45f, 0.28f, 0.85f, 0.65f);
    colors[ImGuiCol_HeaderActive]      = ImVec4(0.60f, 0.38f, 1.0f, 0.80f);
    colors[ImGuiCol_Button]            = ImVec4(0.18f, 0.10f, 0.38f, 0.80f);
    colors[ImGuiCol_ButtonHovered]     = ImVec4(0.35f, 0.20f, 0.70f, 1.0f);
    colors[ImGuiCol_ButtonActive]      = ImVec4(0.50f, 0.30f, 0.90f, 1.0f);
    colors[ImGuiCol_SliderGrab]        = ImVec4(0.50f, 0.30f, 0.90f, 1.0f);
    colors[ImGuiCol_SliderGrabActive]  = ImVec4(0.70f, 0.50f, 1.0f, 1.0f);
    colors[ImGuiCol_CheckMark]         = ImVec4(0.50f, 0.30f, 0.90f, 1.0f);
    colors[ImGuiCol_FrameBg]           = ImVec4(0.08f, 0.08f, 0.13f, 1.0f);
    colors[ImGuiCol_FrameBgHovered]    = ImVec4(0.12f, 0.12f, 0.20f, 1.0f);
    colors[ImGuiCol_Tab]               = ImVec4(0.10f, 0.06f, 0.20f, 1.0f);
    colors[ImGuiCol_TabSelected]       = ImVec4(0.30f, 0.18f, 0.60f, 1.0f);
    colors[ImGuiCol_Separator]         = ImVec4(0.30f, 0.18f, 0.60f, 0.50f);
    colors[ImGuiCol_Text]              = ImVec4(0.82f, 0.83f, 0.86f, 1.0f);
    style.WindowRounding    = 10.0f;
    style.FrameRounding     = 5.0f;
    style.GrabRounding      = 4.0f;
    style.ScrollbarRounding = 5.0f;
    style.WindowBorderSize  = 1.0f;

    // Backend OpenGL ES 3.0
    ImGui_ImplOpenGL3_Init("#version 300 es");

    // Iniciar thread de input
    g_touchRunning.store(true);
    pthread_t t;
    pthread_create(&t, nullptr, inputReaderThread, nullptr);
    pthread_detach(t);

    // Iniciar validação de key (background)
    startKeyValidation();

    return true;
}

// ── Salvar/restaurar estado OpenGL ───────────────────────────────────────────
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
    if (s.blend)       glEnable(GL_BLEND);       else glDisable(GL_BLEND);
    if (s.depthTest)   glEnable(GL_DEPTH_TEST);  else glDisable(GL_DEPTH_TEST);
    if (s.scissorTest) glEnable(GL_SCISSOR_TEST);else glDisable(GL_SCISSOR_TEST);
    if (s.cullFace)    glEnable(GL_CULL_FACE);   else glDisable(GL_CULL_FACE);
    glBlendFuncSeparate(s.blendSrcRGB, s.blendDstRGB, s.blendSrcAlpha, s.blendDstAlpha);
}

// ── Hook principal: substituição de eglSwapBuffers ────────────────────────────
static EGLBoolean hooked_eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {
    // Inicializar ImGui na primeira chamada
    if (!g_eglInitialized.load()) {
        if (initImGui(dpy, surface)) {
            g_eglInitialized.store(true);
        } else {
            return orig_eglSwapBuffers(dpy, surface);
        }
    }

    // Atualizar tamanho caso orientação mude
    {
        int w = 0, h = 0;
        eglQuerySurface(dpy, surface, EGL_WIDTH, &w);
        eglQuerySurface(dpy, surface, EGL_HEIGHT, &h);
        if (w > 0 && h > 0 && (w != g_screenW || h != g_screenH)) {
            g_screenW = w; g_screenH = h;
            ImGui::GetIO().DisplaySize = ImVec2((float)w, (float)h);
        }
    }

    // ── Alimentar ImGui com eventos de touch ──
    ImGuiIO& io = ImGui::GetIO();
    int tevt = g_touchEvent.exchange(-1);
    float tx = g_touchX.load(), ty = g_touchY.load();

    if (tevt == 0) {
        // DOWN: apenas feed ao ImGui se o menu estiver aberto
        if (g_menuOpen.load() || !isKeyValid()) {
            io.AddMousePosEvent(tx, ty);
            io.AddMouseButtonEvent(0, true);
        }
    } else if (tevt == 1) {
        io.AddMouseButtonEvent(0, false);
    } else if (tevt == 2 && (g_menuOpen.load() || !isKeyValid())) {
        io.AddMousePosEvent(tx, ty);
    }

    // ── Nova frame ImGui ──
    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();

    // Salvar estado GL do jogo para restaurar depois
    GLState glState;
    saveGLState(glState);

    // Configurar viewport para tela inteira
    glViewport(0, 0, g_screenW, g_screenH);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                        GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    // ── Desenhar conteúdo ──
    if (!isKeyValid()) {
        // Tela de validação de key (antes de qualquer feature)
        DrawKeyUI();
    } else {
        // Key válida: desenhar ESP sempre visível
        DrawESP(g_screenW, g_screenH);

        // Desenhar menu apenas quando aberto
        if (g_menuOpen.load()) {
            DrawMenu();
        }
    }

    // ── Finalizar frame ImGui e renderizar ──
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    // Restaurar estado GL do jogo
    restoreGLState(glState);

    // Chamar original para apresentar o frame na tela
    return orig_eglSwapBuffers(dpy, surface);
}

// ── Instalar o hook via Dobby ─────────────────────────────────────────────────
void installEglHook() {
    // Aguardar um momento para garantir que libEGL.so está carregada
    usleep(200000); // 200ms

    void* libEGL = dlopen(OBFUSCATE("libEGL.so"), RTLD_NOW | RTLD_GLOBAL);
    if (!libEGL) {
        // Tentar via RTLD_DEFAULT (já carregada pelo processo)
        libEGL = RTLD_DEFAULT;
    }

    void* sym = dlsym(libEGL, OBFUSCATE("eglSwapBuffers"));
    if (!sym) return;

    // Usar Dobby para hook inline (mais estável que PLT hook para este caso)
    DobbyHook(sym, (void*)hooked_eglSwapBuffers, (void**)&orig_eglSwapBuffers);
}

#endif // UNIFIED_BUILD
