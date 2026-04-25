/*
 * ============================================================
 * zygisk_main.cpp - Entry point do módulo Zygisk para JawMods
 * ============================================================
 *
 * VANTAGENS sobre ptrace injection:
 *   - Carregado pelo Magisk/Zygisk ANTES do anti-cheat inicializar
 *   - .so não precisa estar no game dir (sem arquivo externo detectável)
 *   - TracerPid = 0 (sem ptrace)
 *   - Aparece como mapeamento Magisk no /proc/maps (pode ser ocultado
 *     pelo próprio Magisk com DenyList)
 *
 * FLUXO:
 *   Zygote fork → postAppSpecialize(nice_name=com.dts.freefireth)
 *   → pthread_create(hack_thread) → aguarda libil2cpp.so
 *   → VMT hook → SharedMemory → Overlay lê e desenha
 * ============================================================
 */

#include "zygisk.hpp"
#include <pthread.h>
#include <cstring>
#include <cerrno>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <android/log.h>
#include "SharedData.h"   // SHARED_MEM_SIZE, g_zygisk_shm_fd

// hack_thread é definido em GameHook.cpp (mesmo módulo compilado junto)
extern void* hack_thread(void*);
extern bool  g_hookStarted;
extern int   g_zygisk_shm_fd;  // definido em GameHook.cpp (#ifdef ZYGISK_BUILD)

#define ZTAG "JawMods-Zygisk"
// Em release não loga nada — mesmo conceito do STEALTH_DEBUG
#ifdef STEALTH_DEBUG
  #define ZLOGI(...) __android_log_print(ANDROID_LOG_INFO,  ZTAG, __VA_ARGS__)
  #define ZLOGE(...) __android_log_print(ANDROID_LOG_ERROR, ZTAG, __VA_ARGS__)
#else
  #define ZLOGI(...) ((void)0)
  #define ZLOGE(...) ((void)0)
#endif

// ============================================================
// Módulo Zygisk
// ============================================================
class JawModsModule : public zygisk::ModuleBase {

public:
    void onLoad(zygisk::Api *api, JNIEnv *env) override {
        this->api = api;
        this->env = env;
    }

    // preAppSpecialize: roda ANTES do setuid (ainda somos root/zygote).
    // Abrimos o SHM file aqui para contornar SELinux:
    //   untrusted_app nao pode criar/abrir shell_data_file (/data/local/tmp/)
    //   mas root pode. O fd aberto como root sobrevive ao setuid().
    //   api->exemptFd() avisa o Magisk para nao fechar o fd durante a transicao.
    void preAppSpecialize(zygisk::AppSpecializeArgs *args) override {
        if (!args || !args->nice_name) return;

        const char *process = env->GetStringUTFChars(args->nice_name, nullptr);
        if (!process) return;

        bool isTarget = (strstr(process, "freefireth") != nullptr ||
                         strstr(process, "freefire")   != nullptr);

        env->ReleaseStringUTFChars(args->nice_name, process);

        if (!isTarget) return;

        // Criar/abrir o arquivo SHM como root
        int fd = open(SHM_PATH_1, O_RDWR);
        if (fd < 0) fd = open(SHM_PATH_1, O_CREAT | O_RDWR, 0666);
        if (fd >= 0) {
            ftruncate(fd, SHARED_MEM_SIZE);
            fchmod(fd, 0666);
            chmod(SHM_PATH_1, 0666);
            api->exemptFd(fd); // nao fechar durante unmount do Magisk
            preShmFd = fd;
            ZLOGI("preAppSpecialize: SHM criado fd=%d path=%s", fd, SHM_PATH_1);
        } else {
            ZLOGE("preAppSpecialize: FALHA ao criar SHM errno=%d", errno);
        }
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs *args) override {
        if (!args || !args->nice_name) return;

        // Pegar o nome do processo que acabou de ser especializado
        const char *process = env->GetStringUTFChars(args->nice_name, nullptr);
        if (!process) return;

        bool isTarget = (strstr(process, "freefireth") != nullptr ||
                         strstr(process, "freefire")   != nullptr);

        env->ReleaseStringUTFChars(args->nice_name, process);

        if (!isTarget) return;

        ZLOGI("postAppSpecialize: target process detectado, iniciando hook...");

        // Passar o fd pre-aberto (root) para shm_create_file via global
        if (preShmFd >= 0) {
            g_zygisk_shm_fd = preShmFd;
            ZLOGI("postAppSpecialize: g_zygisk_shm_fd = %d", preShmFd);
        }

        if (g_hookStarted) return;
        g_hookStarted = true;

        // Lançar hack_thread — mesmo que no modo ptrace
        // hack_thread aguarda libil2cpp.so aparecer nos maps e aplica o hook
        pthread_t t;
        pthread_create(&t, nullptr, hack_thread, nullptr);
        pthread_detach(t);
    }

private:
    zygisk::Api *api     = nullptr;
    JNIEnv      *env     = nullptr;
    int          preShmFd = -1;   // fd aberto em preAppSpecialize (root context)
};

// Registrar o módulo (expõe zygisk_module_entry como símbolo público)
REGISTER_ZYGISK_MODULE(JawModsModule)
