/*
 * ============================================================
 * inject.cpp — Entry Point Universal (Root + No-Root)
 * ============================================================
 *
 * ARQUITETURA SEM ROOT:
 *   A injeção sem root funciona em duas etapas:
 *
 *   Etapa 1 — Preparação (feita uma vez, offline):
 *     a) Extrair o APK do Free Fire original
 *     b) Copiar libgl2.so para lib/arm64-v8a/ do APK
 *     c) Patchear o DEX (smali) da classe Application do FF
 *        para adicionar System.loadLibrary("gl2") no onCreate()
 *     d) Repackage + assinar o APK com nossa chave
 *     e) Distribuir o APK modificado
 *
 *   Etapa 2 — Runtime (automático ao abrir o jogo):
 *     - Android linka libgl2.so (agora está no APK)
 *     - __attribute__((constructor)) → lib_main_inject() executa
 *     - lib_main_inject() verifica: processo certo? sem debugger?
 *     - Inicia hack_thread() que aplica o VMT hook
 *
 * VANTAGEM SOBRE ROOT:
 *   - Funciona em qualquer dispositivo (sem Magisk/Zygisk)
 *   - A lib é carregada ANTES do próprio jogo inicializar
 *   - Sem rastro em /proc/self/maps (nome é libgl2.so, legítimo)
 *
 * SEGURANÇA:
 *   - Anti-debug antes de qualquer ação
 *   - Strings críticas ofuscadas via OBFUSCATE()
 *   - Sem logs em build de produção
 *   - Thread com nome camuflado
 *
 * COMO USAR:
 *   Compilar com -DNOROOT_BUILD para ativar este entry point.
 *   Em modo Zygisk (root), use zygisk_main.cpp em vez deste.
 * ============================================================
 */

#include <jni.h>
#include <pthread.h>
#include <unistd.h>
#include <dlfcn.h>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <android/log.h>

#include "obfuscate.h"
#include "SharedData.h"

// ── Log Control ──────────────────────────────────────────────────────────────
// Produção: zero logs. Descomentar apenas para debug temporário.
// #define INJECT_DEBUG
#ifdef INJECT_DEBUG
  #define ILOG(...) __android_log_print(ANDROID_LOG_INFO, "INJ", __VA_ARGS__)
#else
  #define ILOG(...) ((void)0)
#endif

// Declarações externas: funções do GameHook.cpp
extern void* hack_thread(void*);
extern bool  g_hookStarted;

// ── JavaVM global — necessário para key_validator.cpp fazer HTTP via JNI ──────
// Declarado em key_validator.h, definido lá. Preenchido em JNI_OnLoad abaixo.
#ifdef UNIFIED_BUILD
#include "key_validator.h"
// installEglHook() declarado em egl_hook.cpp
extern void installEglHook();
#endif

// ── Shared SHM fd (preenchido pelo Zygisk em root mode) ──────────────────────
// Em no-root, este fd fica -1 e shm_create_file() cria o arquivo diretamente.
#ifndef ZYGISK_BUILD
extern int g_zygisk_shm_fd;
#endif

// ============================================================
// Anti-Debug — detecta debugger e Frida antes de iniciar
// ============================================================

static bool inj_isBeingDebugged() {
    FILE* f = fopen(OBFUSCATE("/proc/self/status"), "r");
    if (!f) return false;
    char buf[128];
    bool traced = false;
    while (fgets(buf, sizeof(buf), f)) {
        if (strncmp(buf, "TracerPid:", 10) == 0) {
            traced = (atoi(buf + 10) != 0);
            break;
        }
    }
    fclose(f);
    return traced;
}

static bool inj_detectFrida() {
    FILE* f = fopen(OBFUSCATE("/proc/self/maps"), "r");
    if (!f) return false;
    char buf[512];
    bool found = false;
    while (fgets(buf, sizeof(buf), f)) {
        if (strstr(buf, OBFUSCATE("frida-agent")) ||
            strstr(buf, OBFUSCATE("frida-gadget")) ||
            strstr(buf, OBFUSCATE("re.frida"))) {
            found = true;
            break;
        }
    }
    fclose(f);
    return found;
}

// ============================================================
// Verificação de processo
// Retorna true se estamos dentro do processo do Free Fire
// ============================================================
static bool inj_isGameProcess() {
    char cmdline[256] = {0};
    // /proc/self/cmdline: nome do processo separado por \0
    int fd = open(OBFUSCATE("/proc/self/cmdline"), O_RDONLY);
    if (fd >= 0) {
        read(fd, cmdline, sizeof(cmdline) - 1);
        close(fd);
    }
    // Verificar se contém identificadores do Free Fire
    // Strings ofuscadas — não aparecem no binário
    return strstr(cmdline, OBFUSCATE("freefireth")) != nullptr ||
           strstr(cmdline, OBFUSCATE("freefire"))   != nullptr;
}

// ============================================================
// Thread de inicialização: espera il2cpp + inicia hook
// ============================================================
static void* inj_startupThread(void*) {
    // Camuflar nome da thread
    prctl(PR_SET_NAME, (char*)OBFUSCATE("HeapTaskDaemon"), 0, 0, 0);

    // Pequeno delay para garantir que o processo está totalmente inicializado
    // Importante no modo no-root: libgl2.so carrega antes do Unity inicializar
    usleep(500000); // 500ms

    // Verificação de segurança (pode ser bypassada em emuladores)
    if (inj_isBeingDebugged() || inj_detectFrida()) {
        ILOG("inj_startupThread: ambiente inseguro, abortando");
        return nullptr;
    }

    if (!inj_isGameProcess()) {
        ILOG("inj_startupThread: processo incorreto, abortando");
        return nullptr;
    }

    // Iniciar o hack principal
    if (!g_hookStarted) {
        g_hookStarted = true;
        hack_thread(nullptr); // executa em linha (já estamos em thread separada)
    }
    return nullptr;
}

// ============================================================
// JNI_OnLoad — chamado quando Java faz System.loadLibrary("gl2")
// Este é o entry point principal no modo NO-ROOT
// ============================================================
extern "C" JNIEXPORT jint JNICALL
JNI_OnLoad(JavaVM* vm, void* /*reserved*/) {
    ILOG("JNI_OnLoad_inject: pid=%d uid=%d", getpid(), getuid());

    // Anti-debug imediato (antes de qualquer coisa)
    if (inj_isBeingDebugged() || inj_detectFrida()) {
        return JNI_VERSION_1_6; // retorno silencioso, sem hook
    }

    if (!inj_isGameProcess()) {
        return JNI_VERSION_1_6;
    }

#ifdef UNIFIED_BUILD
    // Salvar JavaVM para key_validator.cpp usar nas requisições HTTP
    g_jvm = vm;
#endif

    if (!g_hookStarted) {
        pthread_t t;
        pthread_create(&t, nullptr, inj_startupThread, nullptr);
        pthread_detach(t);
    }

#ifdef UNIFIED_BUILD
    // Instalar EGL hook em thread separada (após o jogo carregar OpenGL)
    {
        pthread_t eglT;
        pthread_create(&eglT, nullptr, [](void*) -> void* {
            installEglHook();
            return nullptr;
        }, nullptr);
        pthread_detach(eglT);
    }
#endif

    return JNI_VERSION_1_6;
}

// ============================================================
// Constructor attribute — executa imediatamente ao ser linkado
// Funciona tanto em dlopen (root/Zygisk) quanto em loadLibrary
// Este método complementa o JNI_OnLoad (um dos dois sempre dispara)
// ============================================================
#ifdef NOROOT_BUILD
__attribute__((constructor))
static void inj_constructor() {
    ILOG("inj_constructor: pid=%d uid=%d", getpid(), getuid());

    // Anti-debug: se debugger ativo, não inicializar
    // (verifica via ptrace — técnica diferente do TracerPid para redundância)
    if (ptrace(PTRACE_TRACEME, 0, nullptr, nullptr) < 0) {
        // ptrace retornou erro = já existe um tracer = sendo debugado
        return;
    }
    // ptrace funcionou = não há debugger. Revogar para não bloquear uso legítimo.
    ptrace(PTRACE_DETACH, 0, nullptr, nullptr);

    // Verificação de Frida
    if (inj_detectFrida()) return;

    if (!inj_isGameProcess()) return;

    if (!g_hookStarted) {
        g_hookStarted = true;
        pthread_t t;
        pthread_create(&t, nullptr, inj_startupThread, nullptr);
        pthread_detach(t);
    }
}
#endif // NOROOT_BUILD

// ============================================================
// GUIA DE IMPLEMENTAÇÃO NO-ROOT (passos offline)
// ============================================================
//
// 1. DESCOMPILAR APK:
//    apktool d freefireth.apk -o ff_out/
//
// 2. COPIAR NOSSA LIB:
//    cp libgl2.so ff_out/lib/arm64-v8a/libgl2.so
//
// 3. PATCHEAR DEX (adicionar loadLibrary no Application.onCreate):
//    Editar smali/com/dts/freefireth/MyApplication.smali (ou equivalente)
//    Adicionar antes do return-void em onCreate():
//
//      const-string v0, "gl2"
//      invoke-static {v0}, Ljava/lang/System;->loadLibrary(Ljava/lang/String;)V
//
// 4. REPACKAGE + ASSINAR:
//    apktool b ff_out/ -o ff_modded.apk
//    zipalign -v 4 ff_modded.apk ff_aligned.apk
//    apksigner sign --ks nossa_chave.jks ff_aligned.apk
//
// 5. RESULTADO:
//    ff_aligned.apk = APK do FF com mod embutido
//    Distribuir este APK para os clientes (sem root necessário)
//
// NOTA: A validação de key via servidor continua funcionando
//       pois main.cpp (overlay) faz a verificação na UI.
//       Para arquitetura totalmente unificada (sem overlay APK),
//       implementar a validação de key aqui via libcurl/socket.
// ============================================================
