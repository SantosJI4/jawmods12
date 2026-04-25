#pragma once

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#ifndef EGL_OPENGL_ES3_BIT
#define EGL_OPENGL_ES3_BIT 0x0040
#endif
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <pthread.h>
#include <unistd.h>
#include <atomic>
#include <mutex>

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_opengl3.h"
#include "Roboto-Regular.h"

#define OVERLAY_TAG "OverlayRender"
#define OLOGI(...) __android_log_print(ANDROID_LOG_INFO, OVERLAY_TAG, __VA_ARGS__)
#define OLOGE(...) __android_log_print(ANDROID_LOG_ERROR, OVERLAY_TAG, __VA_ARGS__)

class Overlay {
public:
    static Overlay& get() {
        static Overlay instance;
        return instance;
    }

    bool init(JNIEnv* env, jobject surface, int w, int h) {
        std::lock_guard<std::mutex> lock(mtx);
        if (running.load()) { OLOGI("init: already running"); return false; }

        OLOGI("init: surface=%p w=%d h=%d", surface, w, h);
        nativeWindow = ANativeWindow_fromSurface(env, surface);
        if (!nativeWindow) { OLOGE("init: ANativeWindow_fromSurface failed"); return false; }

        screenW = w;
        screenH = h;

        if (!initEGL()) {
            OLOGE("init: initEGL failed");
            ANativeWindow_release(nativeWindow);
            nativeWindow = nullptr;
            return false;
        }

        setupImGui();

        // CRITICO: Liberar o contexto EGL deste thread (Java UI)
        // O render thread vai pegar o contexto com eglMakeCurrent
        eglMakeCurrent(eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

        running.store(true);
        pthread_create(&renderThread, nullptr, renderThreadEntry, this);
        OLOGI("init: SUCCESS, render thread started");
        return true;
    }

    void destroy() {
        running.store(false);
        if (renderThread) {
            pthread_join(renderThread, nullptr);
            renderThread = 0;
        }

        std::lock_guard<std::mutex> lock(mtx);

        if (imguiInitialized) {
            ImGui_ImplOpenGL3_Shutdown();
            ImGui::DestroyContext();
            imguiInitialized = false;
        }

        if (eglDisplay != EGL_NO_DISPLAY) {
            eglMakeCurrent(eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (eglSurface != EGL_NO_SURFACE) eglDestroySurface(eglDisplay, eglSurface);
            if (eglContext != EGL_NO_CONTEXT) eglDestroyContext(eglDisplay, eglContext);
            eglTerminate(eglDisplay);
        }

        eglDisplay = EGL_NO_DISPLAY;
        eglSurface = EGL_NO_SURFACE;
        eglContext = EGL_NO_CONTEXT;

        if (nativeWindow) {
            ANativeWindow_release(nativeWindow);
            nativeWindow = nullptr;
        }
    }

    void setScreenSize(int w, int h) {
        screenW = w;
        screenH = h;
    }

    void handleTouch(int action, float x, float y) {
        std::lock_guard<std::mutex> lock(touchMtx);
        touchX = x;
        touchY = y;
        if (action == 0) { // ACTION_DOWN
            pendingDown   = true;
            pendingUp     = false;
            touchPrevY    = y;
            touchStartY   = y;
            totalDragY    = 0.0f;
            pendingWheel  = 0.0f;
            dragMode      = 0;  // 0=undecided, 1=scroll, 2=window-drag
        } else if (action == 1 || action == 3) { // ACTION_UP / CANCEL
            pendingUp     = true;
            touchPrevY    = -1.0f;
            totalDragY    = 0.0f;
            dragMode      = 0;
        } else if (action == 2) { // ACTION_MOVE
            if (touchPrevY >= 0.0f) {
                float dy    = touchPrevY - y; // positivo = dedo subiu = scroll down
                totalDragY += dy;
                pendingWheel += dy / 60.0f;   // acumulado; só consumido se dragMode==1
                touchPrevY  = y;
            }
        }
    }

    int getScreenW() const { return screenW; }
    int getScreenH() const { return screenH; }
    bool isRunning() const { return running.load(); }

    // Callback de draw externo - setado pelo main.cpp
    void (*onDraw)(int screenW, int screenH) = nullptr;

private:
    Overlay() = default;
    ~Overlay() { destroy(); }
    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;

    bool initEGL() {
        OLOGI("initEGL: start");
        eglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (eglDisplay == EGL_NO_DISPLAY) { OLOGE("initEGL: no display"); return false; }

        EGLint major, minor;
        if (!eglInitialize(eglDisplay, &major, &minor)) { OLOGE("initEGL: eglInitialize failed"); return false; }
        OLOGI("initEGL: EGL %d.%d", major, minor);

        const EGLint configAttribs[] = {
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RED_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, 8,
            EGL_DEPTH_SIZE, 0,
            EGL_STENCIL_SIZE, 0,
            EGL_NONE
        };

        EGLConfig config;
        EGLint numConfigs;
        if (!eglChooseConfig(eglDisplay, configAttribs, &config, 1, &numConfigs) || numConfigs == 0) {
            // Fallback: try ES2
            OLOGI("initEGL: ES3 config failed, trying ES2");
            const EGLint fallbackAttribs[] = {
                EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                EGL_RED_SIZE, 8,
                EGL_GREEN_SIZE, 8,
                EGL_BLUE_SIZE, 8,
                EGL_ALPHA_SIZE, 8,
                EGL_NONE
            };
            if (!eglChooseConfig(eglDisplay, fallbackAttribs, &config, 1, &numConfigs) || numConfigs == 0) {
                OLOGE("initEGL: no config found");
                return false;
            }
        }
        OLOGI("initEGL: config found, numConfigs=%d", numConfigs);

        ANativeWindow_setBuffersGeometry(nativeWindow, 0, 0, EGL_NATIVE_VISUAL_ID);

        // Obtem o formato nativo
        EGLint format;
        eglGetConfigAttrib(eglDisplay, config, EGL_NATIVE_VISUAL_ID, &format);
        ANativeWindow_setBuffersGeometry(nativeWindow, 0, 0, format);

        eglSurface = eglCreateWindowSurface(eglDisplay, config, nativeWindow, nullptr);
        if (eglSurface == EGL_NO_SURFACE) { OLOGE("initEGL: no surface"); return false; }
        OLOGI("initEGL: surface created");

        const EGLint contextAttribs[] = {
            EGL_CONTEXT_CLIENT_VERSION, 3,
            EGL_NONE
        };

        eglContext = eglCreateContext(eglDisplay, config, EGL_NO_CONTEXT, contextAttribs);
        if (eglContext == EGL_NO_CONTEXT) { OLOGE("initEGL: no context"); return false; }

        if (!eglMakeCurrent(eglDisplay, eglSurface, eglSurface, eglContext)) {
            OLOGE("initEGL: eglMakeCurrent failed");
            return false;
        }

        OLOGI("initEGL: SUCCESS");
        return true;
    }

    void setupImGui() {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();

        io.DisplaySize = ImVec2((float)screenW, (float)screenH);
        // ── JawMods Dark + Green Theme ──
        ImGui::StyleColorsDark();
        ImGuiStyle* style = &ImGui::GetStyle();
        ImVec4* c = style->Colors;

        // Main green accent
        ImVec4 green     = ImVec4(0.00f, 0.90f, 0.46f, 1.00f); // #00E676
        ImVec4 greenDim  = ImVec4(0.00f, 0.55f, 0.28f, 1.00f);
        ImVec4 greenDark = ImVec4(0.00f, 0.35f, 0.18f, 1.00f);
        ImVec4 bg        = ImVec4(0.05f, 0.05f, 0.05f, 0.94f);
        ImVec4 bgChild   = ImVec4(0.08f, 0.08f, 0.08f, 1.00f);
        ImVec4 bgFrame   = ImVec4(0.12f, 0.12f, 0.12f, 1.00f);
        ImVec4 border    = ImVec4(0.16f, 0.16f, 0.16f, 1.00f);
        ImVec4 textMain  = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
        ImVec4 textDim   = ImVec4(0.60f, 0.60f, 0.60f, 1.00f);

        // Window
        c[ImGuiCol_WindowBg]       = bg;
        c[ImGuiCol_ChildBg]        = bgChild;
        c[ImGuiCol_PopupBg]        = ImVec4(0.08f, 0.08f, 0.08f, 0.96f);
        c[ImGuiCol_Border]         = border;
        c[ImGuiCol_BorderShadow]   = ImVec4(0, 0, 0, 0);

        // Title bar
        c[ImGuiCol_TitleBg]        = ImVec4(0.04f, 0.04f, 0.04f, 1.00f);
        c[ImGuiCol_TitleBgActive]  = ImVec4(0.00f, 0.18f, 0.09f, 1.00f);
        c[ImGuiCol_TitleBgCollapsed] = ImVec4(0.02f, 0.02f, 0.02f, 0.50f);

        // Text
        c[ImGuiCol_Text]           = textMain;
        c[ImGuiCol_TextDisabled]   = textDim;

        // Frame (inputs, checkboxes)
        c[ImGuiCol_FrameBg]        = bgFrame;
        c[ImGuiCol_FrameBgHovered] = ImVec4(0.00f, 0.30f, 0.15f, 0.40f);
        c[ImGuiCol_FrameBgActive]  = ImVec4(0.00f, 0.45f, 0.23f, 0.60f);

        // Button
        c[ImGuiCol_Button]         = greenDark;
        c[ImGuiCol_ButtonHovered]  = greenDim;
        c[ImGuiCol_ButtonActive]   = green;

        // Check, slider
        c[ImGuiCol_CheckMark]      = green;
        c[ImGuiCol_SliderGrab]     = greenDim;
        c[ImGuiCol_SliderGrabActive] = green;

        // Header (collapsing, selectable)
        c[ImGuiCol_Header]         = ImVec4(0.00f, 0.25f, 0.13f, 0.50f);
        c[ImGuiCol_HeaderHovered]  = ImVec4(0.00f, 0.40f, 0.20f, 0.60f);
        c[ImGuiCol_HeaderActive]   = greenDim;

        // Separator
        c[ImGuiCol_Separator]      = border;
        c[ImGuiCol_SeparatorHovered] = greenDim;
        c[ImGuiCol_SeparatorActive]  = green;

        // Scrollbar
        c[ImGuiCol_ScrollbarBg]    = ImVec4(0.03f, 0.03f, 0.03f, 0.50f);
        c[ImGuiCol_ScrollbarGrab]  = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
        c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
        c[ImGuiCol_ScrollbarGrabActive]  = greenDim;

        // Tab
        c[ImGuiCol_Tab]            = greenDark;
        c[ImGuiCol_TabHovered]     = greenDim;

        // Resize grip
        c[ImGuiCol_ResizeGrip]         = ImVec4(0.00f, 0.30f, 0.15f, 0.25f);
        c[ImGuiCol_ResizeGripHovered]   = greenDim;
        c[ImGuiCol_ResizeGripActive]    = green;

        // Style metrics
        style->Alpha = 1.0f;
        style->WindowTitleAlign = ImVec2(0.5f, 0.5f);
        style->WindowPadding = ImVec2(16, 12);
        style->FramePadding = ImVec2(10, 6);
        style->ItemSpacing = ImVec2(10, 8);
        style->ScrollbarSize = 14;
        style->WindowBorderSize = 1;
        style->ChildBorderSize = 1;
        style->PopupBorderSize = 1;
        style->FrameBorderSize = 0;
        style->WindowRounding = 10;
        style->ChildRounding = 8;
        style->FrameRounding = 8;
        style->PopupRounding = 8;
        style->ScrollbarRounding = 6;
        style->GrabRounding = 6;
        style->GrabMinSize = 10;

        ImGui_ImplOpenGL3_Init("#version 300 es");

        ImFontConfig fontCfg;
        io.Fonts->AddFontFromMemoryTTF(&Roboto_Regular, sizeof(Roboto_Regular),
                                        40.0f, &fontCfg, io.Fonts->GetGlyphRangesCyrillic());

        ImGui::GetStyle().ScaleAllSizes(3.0f);
        imguiInitialized = true;
    }

    void processTouch() {
        std::lock_guard<std::mutex> lock(touchMtx);

        ImGuiIO& io = ImGui::GetIO();

        // Sempre atualizar posição do mouse
        if (touchX >= 0.0f && touchY >= 0.0f)
            io.MousePos = ImVec2(touchX, touchY);

        // DOWN tem prioridade: garante que ImGui vê o click antes de qualquer UP
        if (pendingDown) {
            io.MouseDown[0] = true;
            pendingDown = false;
            return;
        }

        if (pendingUp) {
            io.MouseDown[0] = false;
            pendingUp    = false;
            pendingWheel = 0.0f;
            clearMousePosNextFrame = true;
            return;
        }

        // Movimento em andamento — decidir entre drag de janela/resize e scroll de conteúdo.
        // Verificação usa estado do FRAME ANTERIOR do ImGui (processTouch roda antes de NewFrame).
        if (io.MouseDown[0]) {
            bool imguiDragging = ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f);

            if (dragMode != 2 && imguiDragging)
                dragMode = 2;  // ImGui está arrastando janela ou resize handle

            if (dragMode == 2) {
                // Drag de janela/resize: manter mouse pressionado, descartar scroll
                pendingWheel = 0.0f;
            } else if (dragMode == 0 && fabsf(totalDragY) > 40.0f) {
                // Sem drag de janela e deslocamento vertical grande → scroll de conteúdo
                dragMode = 1;
                io.MouseDown[0] = false;
            }

            if (dragMode == 1 && fabsf(pendingWheel) > 0.001f) {
                io.AddMouseWheelEvent(0.0f, pendingWheel);
                pendingWheel = 0.0f;
            }
        }
    }

    void renderFrame() {
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2((float)screenW, (float)screenH);
        io.DeltaTime = 1.0f / 60.0f;

        processTouch();

        // Limpa com fundo totalmente transparente
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        // Habilita blending para transparência
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();

        // Callback de draw externo
        if (onDraw) {
            onDraw(screenW, screenH);
        }

        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        eglSwapBuffers(eglDisplay, eglSurface);

        if (clearMousePosNextFrame) {
            io.MousePos = ImVec2(-1, -1);
            clearMousePosNextFrame = false;
        }
    }

    static void* renderThreadEntry(void* arg) {
        Overlay* self = static_cast<Overlay*>(arg);

        OLOGI("renderThread: started, making EGL current");
        // Re-bind EGL context nesta thread
        if (!eglMakeCurrent(self->eglDisplay, self->eglSurface,
                        self->eglSurface, self->eglContext)) {
            OLOGE("renderThread: eglMakeCurrent FAILED");
            return nullptr;
        }
        OLOGI("renderThread: EGL current OK, entering loop");

        int frameCount = 0;
        while (self->running.load()) {
            self->renderFrame();
            frameCount++;
            if (frameCount == 1 || frameCount == 60 || frameCount % 300 == 0) {
                OLOGI("renderThread: frame %d", frameCount);
            }
            usleep(16000); // ~60 FPS
        }

        eglMakeCurrent(self->eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        return nullptr;
    }

    // EGL
    EGLDisplay eglDisplay = EGL_NO_DISPLAY;
    EGLSurface eglSurface = EGL_NO_SURFACE;
    EGLContext eglContext = EGL_NO_CONTEXT;
    ANativeWindow* nativeWindow = nullptr;

    // Screen
    std::atomic<int> screenW{0};
    std::atomic<int> screenH{0};

    // State
    std::atomic<bool> running{false};
    pthread_t renderThread = 0;
    bool imguiInitialized = false;
    bool clearMousePosNextFrame = false;
    std::mutex mtx;

    // Touch
    std::mutex touchMtx;
    float touchX      = -1.0f;
    float touchY      = -1.0f;
    bool  pendingDown = false;
    bool  pendingUp   = false;
    // Drag/scroll detection (resolvido em processTouch via estado do ImGui)
    float touchPrevY  = -1.0f;
    float touchStartY = 0.0f;
    float totalDragY  = 0.0f;
    float pendingWheel= 0.0f;
    int   dragMode    = 0;    // 0=undecided, 1=scroll-conteúdo, 2=window-drag/resize
};
