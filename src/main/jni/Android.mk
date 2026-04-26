LOCAL_PATH := $(call my-dir)

# ============================================================
# PREBUILT: libdobby.a — Inline hook library
# ============================================================
include $(CLEAR_VARS)
LOCAL_MODULE := dobby
LOCAL_SRC_FILES := lib/$(TARGET_ARCH_ABI)/libdobby.a
include $(PREBUILT_STATIC_LIBRARY)

# ============================================================
# MODULE 1: libMEOW.so — OVERLAY EXTERNO (APK)
# Processo separado - NÃO injeta no jogo
# Lê dados do SharedMemory e desenha ImGui
# ============================================================

include $(CLEAR_VARS)
LOCAL_MODULE := MEOW

LOCAL_CFLAGS := -w -s -Wno-error=format-security -fvisibility=hidden -fpermissive -fexceptions
LOCAL_CPPFLAGS := -w -s -Wno-error=format-security -fvisibility=hidden -Werror -std=c++17
LOCAL_CPPFLAGS += -Wno-error=c++11-narrowing -fpermissive -Wall -fexceptions
LOCAL_LDFLAGS += -Wl,--gc-sections,--strip-all, -llog
LOCAL_LDLIBS := -llog -landroid -lEGL -lGLESv3
LOCAL_ARM_MODE := arm

LOCAL_C_INCLUDES := $(LOCAL_PATH)
LOCAL_C_INCLUDES += $(LOCAL_PATH)/include/ImGui
LOCAL_C_INCLUDES += $(LOCAL_PATH)/include/ImGui/backends
LOCAL_C_INCLUDES += $(LOCAL_PATH)/include/ImGui/font
LOCAL_C_INCLUDES += $(LOCAL_PATH)/include/Utils
LOCAL_C_INCLUDES += $(LOCAL_PATH)/include/Utils/Unity

# Sources - overlay + ImGui
FILE_LIST := $(LOCAL_PATH)/main.cpp
FILE_LIST += $(wildcard $(LOCAL_PATH)/src/ImGui/*.cpp*)
FILE_LIST += $(wildcard $(LOCAL_PATH)/src/ImGui/backends/*.cpp*)

LOCAL_SRC_FILES := $(FILE_LIST:$(LOCAL_PATH)/%=%)

include $(BUILD_SHARED_LIBRARY)

# ============================================================
# MODULE 2: libHook.so — INJETADO NO JOGO (via script root)
# VMT Hook: troca methodPointer no MethodInfo do il2cpp
# Coleta dados e escreve no SharedMemory
# ============================================================

include $(CLEAR_VARS)
LOCAL_MODULE := gl2

LOCAL_CFLAGS := -w -Wno-error=format-security -fvisibility=hidden -fpermissive -fexceptions
LOCAL_CPPFLAGS := -w -Wno-error=format-security -fvisibility=hidden -Werror -std=c++17
LOCAL_CPPFLAGS += -Wno-error=c++11-narrowing -fpermissive -Wall -fexceptions
LOCAL_LDFLAGS += -Wl,--gc-sections,--strip-debug
LOCAL_LDLIBS := -llog -landroid
LOCAL_ARM_MODE := arm

LOCAL_C_INCLUDES := $(LOCAL_PATH)
LOCAL_C_INCLUDES += $(LOCAL_PATH)/include/Utils
LOCAL_C_INCLUDES += $(LOCAL_PATH)/include/Utils/Unity
LOCAL_C_INCLUDES += $(LOCAL_PATH)/include/Hook/Dobby

# Sources - GameHook + inject entry point
HOOK_FILES := $(LOCAL_PATH)/GameHook.cpp
HOOK_FILES += $(LOCAL_PATH)/inject.cpp

LOCAL_SRC_FILES := $(HOOK_FILES:$(LOCAL_PATH)/%=%)
LOCAL_STATIC_LIBRARIES := dobby

include $(BUILD_SHARED_LIBRARY)

# ============================================================
# MODULE 2b: libgl2_noroot.so — VERSÃO NO-ROOT do HOOK
# Para injetar direto no APK do jogo sem Magisk/Zygisk
# Compilar com -DNOROOT_BUILD para ativar o constructor de inject.cpp
# ============================================================

include $(CLEAR_VARS)
LOCAL_MODULE := gl2_noroot

LOCAL_CFLAGS := -w -Wno-error=format-security -fvisibility=hidden -fpermissive -fexceptions -DNOROOT_BUILD
LOCAL_CPPFLAGS := -w -Wno-error=format-security -fvisibility=hidden -Werror -std=c++17
LOCAL_CPPFLAGS += -Wno-error=c++11-narrowing -fpermissive -Wall -fexceptions -DNOROOT_BUILD
LOCAL_LDFLAGS += -Wl,--gc-sections,--strip-debug
LOCAL_LDLIBS := -llog -landroid
LOCAL_ARM_MODE := arm

LOCAL_C_INCLUDES := $(LOCAL_PATH)
LOCAL_C_INCLUDES += $(LOCAL_PATH)/include/Utils
LOCAL_C_INCLUDES += $(LOCAL_PATH)/include/Utils/Unity
LOCAL_C_INCLUDES += $(LOCAL_PATH)/include/Hook/Dobby

NOROOT_FILES := $(LOCAL_PATH)/GameHook.cpp
NOROOT_FILES += $(LOCAL_PATH)/inject.cpp

LOCAL_SRC_FILES := $(NOROOT_FILES:$(LOCAL_PATH)/%=%)
LOCAL_STATIC_LIBRARIES := dobby

include $(BUILD_SHARED_LIBRARY)

# ============================================================
# MODULE 3: libzygisk.so — ZYGISK MODULE (Magisk)
# Carregado pelo Magisk antes do anti-cheat inicializar.
# ZERO ptrace, ZERO arquivo externo no game dir.
# Entry point: postAppSpecialize() em zygisk_main.cpp
# Instalar em: /data/adb/modules/jawmods/zygisk/arm64-v8a.so
# ============================================================

include $(CLEAR_VARS)
LOCAL_MODULE := zygisk

LOCAL_CFLAGS   := -w -Wno-error=format-security -fvisibility=hidden -fpermissive -fexceptions -DZYGISK_BUILD -DSTEALTH_DEBUG
LOCAL_CPPFLAGS := -w -Wno-error=format-security -fvisibility=hidden -Werror -std=c++17
LOCAL_CPPFLAGS += -Wno-error=c++11-narrowing -fpermissive -Wall -fexceptions -DZYGISK_BUILD -DSTEALTH_DEBUG
LOCAL_LDFLAGS  += -Wl,--gc-sections,--strip-debug
LOCAL_LDLIBS   := -llog -landroid
LOCAL_ARM_MODE := arm

LOCAL_C_INCLUDES := $(LOCAL_PATH)
LOCAL_C_INCLUDES += $(LOCAL_PATH)/include/Utils
LOCAL_C_INCLUDES += $(LOCAL_PATH)/include/Utils/Unity
LOCAL_C_INCLUDES += $(LOCAL_PATH)/include/Hook/Dobby

# zygisk_main.cpp (Zygisk entry) + GameHook.cpp (hook logic)
ZYGISK_FILES := $(LOCAL_PATH)/zygisk_main.cpp
ZYGISK_FILES += $(LOCAL_PATH)/GameHook.cpp

LOCAL_SRC_FILES := $(ZYGISK_FILES:$(LOCAL_PATH)/%=%)
LOCAL_STATIC_LIBRARIES := dobby

include $(BUILD_SHARED_LIBRARY)

# ============================================================
# MODULE 3b: libopus.so — SISTEMA UNIFICADO (no-root)
# ============================================================
# Nome "opus" = codec de áudio legítimo, passa despercebido.
# Contém TUDO: GameHook (VMT) + EGL hook + ImGui + Key Validator
# Injetar no APK do Free Fire via patch smali:
#   const-string v0, "opus"
#   invoke-static {v0}, Ljava/lang/System;->loadLibrary(Ljava/lang/String;)V
# Sem APK de overlay separado. Menu abre com Volume DOWN.
# ============================================================

include $(CLEAR_VARS)
LOCAL_MODULE := opus

UNIFIED_CFLAGS := -w -Wno-error=format-security -fvisibility=hidden -fpermissive -fexceptions
UNIFIED_CFLAGS += -DNOROOT_BUILD -DUNIFIED_BUILD

LOCAL_CFLAGS   := $(UNIFIED_CFLAGS)
LOCAL_CPPFLAGS := -w -Wno-error=format-security -fvisibility=hidden -Werror -std=c++17
LOCAL_CPPFLAGS += -Wno-error=c++11-narrowing -fpermissive -Wall -fexceptions
LOCAL_CPPFLAGS += $(UNIFIED_CFLAGS)
LOCAL_LDFLAGS  += -Wl,--gc-sections,--strip-debug
LOCAL_LDLIBS   := -llog -landroid -lEGL -lGLESv3
LOCAL_ARM_MODE := arm

LOCAL_C_INCLUDES := $(LOCAL_PATH)
LOCAL_C_INCLUDES += $(LOCAL_PATH)/include/ImGui
LOCAL_C_INCLUDES += $(LOCAL_PATH)/include/ImGui/backends
LOCAL_C_INCLUDES += $(LOCAL_PATH)/include/ImGui/font
LOCAL_C_INCLUDES += $(LOCAL_PATH)/include/Utils
LOCAL_C_INCLUDES += $(LOCAL_PATH)/include/Utils/Unity
LOCAL_C_INCLUDES += $(LOCAL_PATH)/include/Hook/Dobby

# GameHook: VMT hook + SharedMemory + Aimbot + ESP data collector
# inject.cpp: JNI_OnLoad + anti-debug + EGL hook startup
# egl_hook.cpp: eglSwapBuffers hook + ImGui render + touch input
# key_validator.cpp: HTTP key validation via JNI + UI
# main.cpp: DrawESP + DrawMenu (sem shmReaderLoop em UNIFIED_BUILD)
# ImGui: rendering backend
UNIFIED_FILES := $(LOCAL_PATH)/GameHook.cpp
UNIFIED_FILES += $(LOCAL_PATH)/inject.cpp
UNIFIED_FILES += $(LOCAL_PATH)/egl_hook.cpp
UNIFIED_FILES += $(LOCAL_PATH)/key_validator.cpp
UNIFIED_FILES += $(LOCAL_PATH)/main.cpp
UNIFIED_FILES += $(wildcard $(LOCAL_PATH)/src/ImGui/*.cpp*)
# Apenas opengl3 backend — imgui_impl_android.cpp NÃO é usado no EGL hook
UNIFIED_FILES += $(LOCAL_PATH)/src/ImGui/backends/imgui_impl_opengl3.cpp

LOCAL_SRC_FILES := $(UNIFIED_FILES:$(LOCAL_PATH)/%=%)
LOCAL_STATIC_LIBRARIES := dobby

include $(BUILD_SHARED_LIBRARY)

# ============================================================
# MODULE 4: libinjector.so — PTRACE INJECTOR (fallback)
# ============================================================

include $(CLEAR_VARS)
LOCAL_MODULE := injector

LOCAL_CFLAGS := -w -fvisibility=hidden -O2 -DNDEBUG -fPIC
LOCAL_LDFLAGS += -Wl,--gc-sections,--strip-all
LOCAL_LDLIBS := -llog -ldl
LOCAL_ARM_MODE := arm

LOCAL_SRC_FILES := injector.c

include $(BUILD_SHARED_LIBRARY)