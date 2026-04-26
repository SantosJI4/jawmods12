/*
 * ============================================================
 * GameHook.cpp - Injetado no processo do JOGO via script root
 * ============================================================
 *
 * TÃ‰CNICA: VMT Hook (Virtual Method Table)
 *   - NÃƒO usa Dobby/Inline Hook (sem JMP no cÃ³digo)
 *   - Troca o ponteiro methodPointer dentro do MethodInfo
 *   - O anti-cheat escaneia cÃ³digo por patches â†’ nÃ³s nÃ£o patcheamos cÃ³digo
 *   - SÃ³ trocamos um ponteiro na estrutura de dados do il2cpp
 *
 * FLUXO:
 *   1. lib_main() â†’ aguarda libil2cpp.so carregar
 *   2. base + offset â†’ resolve funÃ§Ãµes direto (zero il2cpp API)
 *   3. dlsym real â†’ resolve Player::LateUpdate MethodInfo
 *   4. VMT hook no OnUpdate â†’ jogo chama nosso hook automaticamente
 *   5. No hook: chama get_main, get_transform, get_position, WorldToScreenPoint
 *   6. Escreve resultado no SharedMemory
 *   7. Overlay externo lÃª e desenha
 *
 * ZERO busca de ponteiros - o jogo entrega tudo no hook
 * ============================================================
 */

#include <jni.h>
#include <pthread.h>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <cstring>
#include <cmath>
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <ctime>
#include <fcntl.h>
#include <elf.h>
#include <sys/prctl.h>
#include <android/log.h>

#include "Utils.h"
#include "Vector3.h"
#include "obfuscate.h"
#include "SharedData.h"
// ============================================================
// STEALTH CONFIG â€” desativar logging e traces em release
// Comentar a linha abaixo para build de RELEASE (sem log)
// ============================================================
#define STEALTH_DEBUG  // â† DESCOMENTADO = modo debug com logs

#ifdef STEALTH_DEBUG
  #define HOOK_TAG "GameHook"
  #define LOGI(...) __android_log_print(ANDROID_LOG_INFO, HOOK_TAG, __VA_ARGS__)
  #define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, HOOK_TAG, __VA_ARGS__)
#else
  #define LOGI(...) ((void)0)
  #define LOGE(...) ((void)0)
#endif

#define HOOK_BUILD_VER "v60"

// ============================================================
// Hook Log File â€” SOMENTE em modo debug
// ============================================================
#ifdef STEALTH_DEBUG
#define HOOK_LOG_PATH_1 "/data/local/tmp/.gl_log"
#define HOOK_LOG_PATH_2 "/sdcard/.gl_log"

static void hookLogWrite(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    const char* gamePath = getGameLogPath();
    if (gamePath && gamePath[0]) {
        int fd = open(gamePath, O_CREAT | O_WRONLY | O_APPEND, 0666);
        if (fd >= 0) {
            write(fd, buf, strlen(buf));
            write(fd, "\n", 1);
            close(fd);
        }
    }

    const char* paths[] = { HOOK_LOG_PATH_1, HOOK_LOG_PATH_2 };
    for (int i = 0; i < 2; i++) {
        int fd = open(paths[i], O_CREAT | O_WRONLY | O_APPEND, 0666);
        if (fd >= 0) {
            write(fd, buf, strlen(buf));
            write(fd, "\n", 1);
            close(fd);
        }
    }
    __android_log_print(ANDROID_LOG_INFO, "GameHook", "%s", buf);
}
#else
static void hookLogWrite(const char*, ...) { /* NO-OP em release */ }
#endif

// ============================================================
// STEALTH: Esconder libHook.so do /proc/self/maps
//
// TÃ©cnica: Para cada segmento file-backed da nossa .so em maps,
// cria um mmap anÃ´nimo MAP_FIXED no mesmo endereÃ§o, copia o
// conteÃºdo, e restaura as permissÃµes. O kernel substitui o VMA
// file-backed por anÃ´nimo â†’ some do maps.
// ============================================================
static void hideFromMaps() {
    char line[1024];
    struct { uintptr_t start; uintptr_t end; int prot; } segs[8];
    int nsegs = 0;

    FILE *maps = fopen("/proc/self/maps", "r");
    if (!maps) return;

    while (fgets(line, sizeof(line), maps) && nsegs < 8) {
        // Procurar por libgl2.so (nosso .so injetado)
        if (!strstr(line, "libgl2.so")) continue;

        uintptr_t start, end;
        char perms[5] = {0};
        if (sscanf(line, "%lx-%lx %4s", &start, &end, perms) != 3) continue;

        // CRITICO: NÃƒO remap segmentos r-xp (executÃ¡veis).
        // Estamos rodando DE DENTRO desse segmento â€” mmap(MAP_FIXED) sobre
        // ele zera as pÃ¡ginas enquanto a CPU executa aqui â†’ freeze/crash.
        // Apenas r-- e rw- sÃ£o seguros de remap.
        if (perms[2] == 'x') continue;

        int prot = 0;
        if (perms[0] == 'r') prot |= PROT_READ;
        if (perms[1] == 'w') prot |= PROT_WRITE;

        segs[nsegs].start = start;
        segs[nsegs].end = end;
        segs[nsegs].prot = prot;
        nsegs++;
    }
    fclose(maps);

    for (int i = 0; i < nsegs; i++) {
        size_t size = segs[i].end - segs[i].start;
        if (size == 0) continue;

        // 1. Copiar conteÃºdo do segmento
        void *backup = malloc(size);
        if (!backup) continue;
        memcpy(backup, (void*)segs[i].start, size);

        // 2. Sobrescrever com mapeamento anÃ´nimo (MAP_FIXED)
        // Isso substitui o VMA file-backed por anÃ´nimo
        void *anon = mmap((void*)segs[i].start, size,
                          PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
                          -1, 0);
        if (anon == MAP_FAILED) {
            free(backup);
            continue;
        }

        // 3. Copiar conteÃºdo de volta
        memcpy(anon, backup, size);
        free(backup);

        // 4. Restaurar permissÃµes originais
        mprotect(anon, size, segs[i].prot);
    }

    LOGI("hideFromMaps: %d segmentos remapeados como anonimos", nsegs);
}

// ============================================================
// ELF Symbol Resolver â€” resolve symbols direto da MEMÃ“RIA
// Usa DT_HASH (SysV) ou DT_GNU_HASH com lookup O(1) correto.
// Sem scan linear â€” nÃ£o trava em libil2cpp com milhoes de simbolos.
// ============================================================

#ifndef DT_GNU_HASH
#define DT_GNU_HASH 0x6ffffef5
#endif

// FunÃ§Ã£o de hash ELF padrÃ£o (SysV)
static uint32_t elf_sysv_hash(const char *name) {
    uint32_t h = 0, g;
    for (; *name; ++name) {
        h = (h << 4) + (uint8_t)*name;
        if ((g = h & 0xf0000000u)) h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

// FunÃ§Ã£o de hash GNU (djb2 modificado)
static uint32_t elf_gnu_hash(const char *name) {
    uint32_t h = 5381;
    for (; *name; ++name) h = h * 33 + (uint8_t)*name;
    return h;
}

static uintptr_t resolveElfSymbol(uintptr_t loadBase, const char *symName) {
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)loadBase;

    // Validar ELF magic
    if (ehdr->e_ident[0] != 0x7f || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L'  || ehdr->e_ident[3] != 'F') {
        LOGE("resolveElfSymbol: ELF invalido em 0x%lx", (unsigned long)loadBase);
        return 0;
    }

    Elf64_Phdr *phdrs = (Elf64_Phdr *)(loadBase + ehdr->e_phoff);

    // Calcular bias (loadBase - primeiro PT_LOAD vaddr)
    uintptr_t bias = loadBase;
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type == PT_LOAD) { bias = loadBase - phdrs[i].p_vaddr; break; }
    }

    // Achar PT_DYNAMIC
    Elf64_Dyn *dyn = nullptr;
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type == PT_DYNAMIC) { dyn = (Elf64_Dyn*)(bias + phdrs[i].p_vaddr); break; }
    }
    if (!dyn) { LOGE("resolveElfSymbol: PT_DYNAMIC nao encontrado"); return 0; }

    // Extrair ponteiros da tabela dinÃ¢mica
    Elf64_Sym  *symtab  = nullptr;
    const char *strtab  = nullptr;
    uint32_t   *hashTab = nullptr; // DT_HASH  (SysV)
    uint32_t   *gnuHash = nullptr; // DT_GNU_HASH

    for (Elf64_Dyn *d = dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
            case DT_SYMTAB:   symtab  = (Elf64_Sym*)(bias + d->d_un.d_ptr);  break;
            case DT_STRTAB:   strtab  = (const char*)(bias + d->d_un.d_ptr); break;
            case DT_HASH:     hashTab = (uint32_t*)(bias + d->d_un.d_ptr);   break;
            case DT_GNU_HASH: gnuHash = (uint32_t*)(bias + d->d_un.d_ptr);   break;
        }
    }

    if (!symtab || !strtab) {
        LOGE("resolveElfSymbol: symtab=%p strtab=%p", (void*)symtab, (void*)strtab);
        return 0;
    }

    // â”€â”€ Tentativa 1: SysV hash â€” O(1) lookup via chain â”€â”€
    if (hashTab) {
        uint32_t nbuckets = hashTab[0];
        uint32_t nchain   = hashTab[1];
        uint32_t *buckets = hashTab + 2;
        uint32_t *chains  = buckets + nbuckets;
        uint32_t h = elf_sysv_hash(symName) % nbuckets;
        for (uint32_t i = buckets[h]; i != 0 && i < nchain; i = chains[i]) {
            if (symtab[i].st_value &&
                strcmp(strtab + symtab[i].st_name, symName) == 0) {
                LOGI("resolveElfSymbol: '%s' -> 0x%lx (SysV)", symName,
                     (unsigned long)(bias + symtab[i].st_value));
                return bias + symtab[i].st_value;
            }
        }
        LOGE("resolveElfSymbol: '%s' nao encontrado via SysV (nbuckets=%u nchain=%u)",
             symName, nbuckets, nchain);
        // Nao retorna 0 aqui â€” tenta GNU hash se disponivel
    }

    // â”€â”€ Tentativa 2: GNU hash â€” O(1) lookup via bloom + chain â”€â”€
    if (gnuHash) {
        uint32_t nbuckets    = gnuHash[0];
        uint32_t symoffset   = gnuHash[1];
        uint32_t bloom_size  = gnuHash[2];
        uint32_t bloom_shift = gnuHash[3];
        // Bloom filter: array de uint64_t para Elf64 (8 bytes por entrada)
        uint64_t *bloom  = (uint64_t*)((uint8_t*)gnuHash + 16);
        uint32_t *bkts   = (uint32_t*)(bloom + bloom_size);
        uint32_t *chains = bkts + nbuckets;

        uint32_t h = elf_gnu_hash(symName);

        // Bloom filter: descarta simbolos definitivamente ausentes
        uint64_t bw   = bloom[(h >> 6) % bloom_size];
        uint32_t bit1 = h & 63;
        uint32_t bit2 = (h >> bloom_shift) & 63;
        if (!((bw >> bit1) & 1u) || !((bw >> bit2) & 1u)) {
            LOGE("resolveElfSymbol: '%s' rejeitado bloom GNU", symName);
            return 0;
        }

        uint32_t symIdx = bkts[h % nbuckets];
        if (symIdx < symoffset) {
            LOGE("resolveElfSymbol: '%s' GNU bucket vazio (symIdx=%u off=%u)",
                 symName, symIdx, symoffset);
            return 0;
        }

        uint32_t *c = chains + (symIdx - symoffset);
        for (uint32_t idx = symIdx; ; idx++, c++) {
            if (((*c ^ h) & ~1u) == 0 &&
                symtab[idx].st_value &&
                strcmp(strtab + symtab[idx].st_name, symName) == 0) {
                LOGI("resolveElfSymbol: '%s' -> 0x%lx (GNU)", symName,
                     (unsigned long)(bias + symtab[idx].st_value));
                return bias + symtab[idx].st_value;
            }
            if (*c & 1u) break; // fim da chain
        }
        LOGE("resolveElfSymbol: '%s' nao encontrado na GNU chain", symName);
        return 0;
    }

    LOGE("resolveElfSymbol: '%s' sem DT_HASH nem DT_GNU_HASH", symName);
    return 0;
}

// ============================================================
// OFFSETS DO DUMP (Free Fire v12 â€” il2cppdumper RVA)
// Atualizar quando o jogo atualizar
// ============================================================
#define OFF_LateUpdate              0x67BE774
#define OFF_Camera_get_main         0x9C0764C
#define OFF_WorldToScreenPoint      0x9C072D0
#define OFF_get_worldToCameraMatrix 0x9C06D7C
#define OFF_get_projectionMatrix    0x9C06E2C
#define OFF_get_transform           0x9C4DF54
#define OFF_get_position            0x9C5C0F4
#define OFF_Transform_get_eulerAngles 0x0
#define OFF_Transform_set_eulerAngles 0x0
#define OFF_Screen_get_width        0x9C14D60
#define OFF_Screen_get_height       0x9C14D88
#define OFF_IsLocalPlayer           0x67558A4
#define OFF_IsLocalTeammate         0x6789BF8
#define OFF_get_CurHP               0x67CDD24
#define OFF_get_MaxHP               0x67CDE1C
#define OFF_IsKnockedDownBleed      0x1150  // offset do campo bool no objeto Player

// Animator.GetBoneTransform(HumanBodyBones) â€” para pegar o Transform do bone da cabeÃ§a
// HumanBodyBones.Head = 10
// Player layout: offset 0x700 = NewPlayerAnimationSystemComponent*
// NewPlayerAnimationSystemComponent base (GCommon.AnimationSystemComponent): offset 0x28 = Animator*
#define OFF_Animator_GetBoneTransform 0x9BEE970
#define PLAYER_ANIM_COMPONENT_OFFSET  0x700  // campo HFKJCLHCBGB na classe Player (dump L650012)
#define ANIM_COMPONENT_ANIMATOR_OFFSET 0x20   // campo KFGPIOMOLHI (Animator) na base GCommon.AnimationSystemComponent (dump L644587/644603)
#define HUMAN_BODY_BONE_HEAD           10    // UnityEngine.HumanBodyBones.Head

// Aimbot 2 â€” GetHeadTF() direto (v59-ab2)
// Player::GetHeadTF() (dump L652224) @ 0x67E689C â€” retorna UnityEngine.Transform* da cabeÃ§a
// IsCrouching() (dump L651872) @ 0x67566AC â€” detectar agachado (bloquear se deitado)
// get_IsDieing() (dump L651508) @ 0x675B1FC â€” detectar derrubado/sangrando
#define OFF_GetHeadTF          0x67E689C
#define OFF_IsCrouching        0x67566AC
#define OFF_get_IsDieing       0x675B1FC

// Aimbot 2 â€” campo legado (removido, mantido por documentaÃ§Ã£o):
// OFF_PLAYER_HEAD_NODE e OFF_TRANSFORMNODE_TRANSFORM eram field offsets supostos, substituÃ­dos
// por chamada direta a GetHeadTF() que Ã© mais confiÃ¡vel.

// PlayerColliderChecker â€” hitbox real da cabeÃ§a (igual o servidor usa para dano)
// Collider.get_bounds() retorna Bounds por valor: { m_Center @ +0x10, m_Extents @ +0x1C }
// List<ColliderInfo> layout IL2CPP: _items (array*) @ +0x10, _size @ +0x18
// array de objetos: elemento[i] em _items[i] (ponteiro), cada objeto: m_collider@0x10, m_hitBoxType@0x18
// HitPart.Head = 0
#define OFF_get_HeadCollider               0x676FEB4
// get_bounds() retorna Bounds (24 bytes) via SRET x8 â€” ABI incorreta para ponteiro direto.
// get_bounds_Injected(out Bounds& ret) recebe o ponteiro em x1 como parÃ¢metro normal â†’ correto.
#define OFF_Collider_get_bounds_Injected  0x9CB794C

// (offsets GetPartByCollider e IsHeadShotCheck removidos em v31 â€” server-side validates)

// Aimbot â€” Player::SetAimRotation(Quaternion q, bool forceUpdate) (v40)
// MÃ©todo oficial do jogo para definir para onde o player mira.
// Dump L651569: public System.Void SetAimRotation(Quaternion, bool) // 0x67718B8
#define OFF_SetAimRotation          0x67718B8
// m_CurrentAimRotation @ 0x1834 â€” Quaternion atual (para smooth/slerp)
#define OFF_m_CurrentAimRotation    0x1834
// Wall check â€” AttackableEntity::IsVisible() â€” game-native visibility query
// Dump L645952: public virtual System.Boolean IsVisible() // 0x68C83F8
// Player herda de AttackableEntity e NAO faz override â†’ chamar direto por offset Ã© correto.
#define OFF_IsVisible               0x68C83F8
// ADS check â€” Player::get_IsSighting() â€” true quando o player estÃ¡ mirando (ADS)
// Dump L653775: public System.Boolean get_IsSighting() // 0x676689C
#define OFF_get_IsSighting          0x676689C
// IsFiring â€” Player::IsFiring() â€” true quando o player estÃ¡ atirando
// Dump L652303: public System.Boolean IsFiring() // 0x675D420
#define OFF_IsFiring                0x675D420

// â”€â”€ Player Hacks (v49) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// NickName â€” Player::get_NickName() â€” string IL2CPP do nome do player
// Dump L651414: public System.String get_NickName() // 0x676CDF0
#define OFF_get_NickName              0x676CDF0
// Aimbot busy-guard: bloquear aimbot durante granada e skill ativa (parede de gelo)
// get_IsGrenadeStart() - true quando player esta armando/jogando granada (Dump L651458)
#define OFF_get_IsGrenadeStart    0x676DDE4
// get_IsRunningActSkill() - true quando skill ativa em uso (gelo, etc.) (Dump L652819)
#define OFF_get_IsRunningActSkill 0x68242E8

// ============================================================
// Function Pointers â€” resolvidos via base + offset direto
// ============================================================
// Il2Cpp ABI: TODOS os metodos recebem MethodInfo* como ultimo param
// Estaticos: fn(MethodInfo*), Instancia: fn(self, MethodInfo*)
// Se a assinatura nao bater, ARM64 passa lixo nos registros â†’ SIGSEGV
// ============================================================
static void*   (*fn_Camera_get_main)(void* method) = nullptr;
static void*   (*fn_get_transform)(void* component, void* method) = nullptr;
static Vector3 (*fn_get_position)(void* transform, void* method) = nullptr;
static int     (*fn_Screen_get_width)(void* method) = nullptr;
static int     (*fn_Screen_get_height)(void* method) = nullptr;

// Para VP Matrix (Camera)
struct Matrix4x4 { float m[16]; };
static Matrix4x4 (*fn_get_worldToCameraMatrix)(void* camera, void* method) = nullptr;
static Matrix4x4 (*fn_get_projectionMatrix)(void* camera, void* method) = nullptr;

// WorldToScreenPoint(self, Vector3 pos, MethodInfo*) â€” 1 arg version
// O dump mostra: WorldToScreenPoint(Vector3 position) â†’ 1 arg
static Vector3 (*fn_WorldToScreenPoint)(void* camera, Vector3 pos, void* method) = nullptr;
static bool useManualW2S = true;

// IsLocalPlayer â€” filtra o proprio jogador do ESP
static bool (*fn_IsLocalPlayer)(void* player, void* method) = nullptr;
// IsLocalTeammate(bool includeLocalPlayer) â€” true = e aliado (ou o proprio player se bool=true)
// Offset: COW.GamePlay.Player::IsLocalTeammate(bool) = 0x6789BF8
static bool (*fn_IsLocalTeammate)(void* player, bool includeLocal, void* method) = nullptr;
static int  (*fn_get_CurHP)(void* player, void* method) = nullptr;
static int  (*fn_get_MaxHP)(void* player, void* method) = nullptr;

// Aim Assist â€” UnityEngine.Transform
static Vector3 (*fn_get_eulerAngles)(void* transform, void* method) = nullptr;
static void    (*fn_set_eulerAngles)(void* transform, Vector3 euler, void* method) = nullptr;
// Animator.GetBoneTransform(HumanBodyBones) â€” retorna Transform do bone da cabeÃ§a
static void*   (*fn_GetBoneTransform)(void* animator, int32_t boneId, void* method) = nullptr;
// Collider.get_bounds() â€” retorna Bounds por valor (passado por ponteiro hidden em x8 na ABI ARM64)
typedef struct { float cx, cy, cz; float ex, ey, ez; } BoundsVal;
static void*   (*fn_get_HeadCollider)(void* player, void* method) = nullptr;
// _Injected: assinatura nativa real â€” sem SRET, ponteiro de saÃ­da em x1
static void    (*fn_Collider_get_bounds_Injected)(void* collider, BoundsVal* outBounds, void* method) = nullptr;

// Wall check â€” AttackableEntity::IsVisible() (v42)
static bool (*fn_IsVisible)(void* self, void* method) = nullptr;
// ADS check â€” Player::get_IsSighting() (v43)
static bool (*fn_get_IsSighting)(void* self, void* method) = nullptr;
// Aimbot 2 â€” cabeÃ§a direta + filtros
static void* (*fn_GetHeadTF)(void* player, void* method)   = nullptr;  // GetHeadTF() â†’ Transform*
static bool  (*fn_IsCrouching)(void* player, void* method) = nullptr;  // agachado/deitado
static bool  (*fn_get_IsDieing)(void* player, void* method)= nullptr;  // derrubado/sangrando
// IsFiring â€” Player::IsFiring() (v44)
static bool (*fn_IsFiring)(void* self, void* method) = nullptr;
// Busy-guard: granada e skill ativa (v61)
static bool (*fn_get_IsGrenadeStart)(void* self, void* method)    = nullptr;
static bool (*fn_get_IsRunningActSkill)(void* self, void* method)  = nullptr;
// â”€â”€ Player Hacks (v49) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// NickName â€” Player::get_NickName() â€” sÃ³ chamado, nÃ£o hookeado
static void* (*fn_get_NickName)(void* self, void* method) = nullptr;

// Aimbot â€” Player::SetAimRotation
struct Quaternion { float x, y, z, w; };
typedef void (*SetAimRotationFn)(void* self, Quaternion q, bool forceUpdate, void* method);
static SetAimRotationFn fn_SetAimRotation = nullptr;

// LookRotation simples e correta: camPos -> targetPos -> pitch/yaw -> Quaternion
// Sem matriz 3x3, sem risco de degenerado ou flip.
static Quaternion LookQuatFromDir(float dx, float dy, float dz) {
    float hd = sqrtf(dx*dx + dz*dz);
    float pitch = -atan2f(dy, hd);          // rad, negativo = olha pra cima
    float yaw   =  atan2f(dx, dz);          // rad
    // Quaternion de Euler XY: Q = Qyaw(Y) * Qpitch(X)
    float hp = pitch * 0.5f, hy = yaw * 0.5f;
    float sp = sinf(hp), cp = cosf(hp);
    float sy = sinf(hy), cy = cosf(hy);
    return {
        sp * cy,   // x
        cp * sy,   // y
       -sp * sy,   // z
        cp * cy    // w
    };
}

// ============================================================
// Aimbot state (v31: direct aim, camera moves visibly to enemy head)
// ============================================================
static uintptr_t g_il2cpp_base = 0;

// Original LateUpdate
static void (*orig_OnUpdate)(void* self, void* methodInfo) = nullptr;

// Shared memory
static SharedESPData* sharedData = nullptr;
static int shmFd = -1;

// Frame sync
static std::atomic<bool> hookActive{false};

// Cache por frame
static void* g_cachedCamera = nullptr;
static Matrix4x4 g_vpMatrix;
static bool g_vpValid = false;
static int g_frameId = 0;

// â”€â”€ Aim Assist â€” estado por frame â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
static void*   g_camTransform      = nullptr;      // Transform da cÃ¢mera (atualizado por frame)
static Vector3 g_aimTargetWorld{0.0f, 0.0f, 0.0f}; // PosiÃ§Ã£o 3D da cabeÃ§a do melhor alvo
static float   g_aimBestScreenDist = 1e9f;         // prioridade 0: menor distÃ¢ncia ao centro
static int     g_aimBestHp         = 0x7fffffff;   // prioridade 1: menor HP (Lowest HP mode)
static float   g_aimBestDepth      = 1e9f;         // prioridade 2: menor clipW (Nearest Distance)
static bool    g_aimHasTarget      = false;        // alvo vÃ¡lido neste frame
static bool    g_camEulerValid     = false;
static Vector3 g_cachedCamEuler{};
// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

// ============================================================
// VMT HOOK â€” Troca methodPointer no MethodInfo
// ============================================================
//
// MethodInfo (il2cpp internals):
//   struct MethodInfo {
//       Il2CppMethodPointer methodPointer; // offset 0x00 â† trocamos isso
//       void* invoker_method;              // offset 0x08
//       const char* name;                  // offset 0x10
//       ...
//   };
//
// il2cpp_class_get_method_from_name retorna MethodInfo*
// O primeiro campo Ã© o ponteiro da funÃ§Ã£o
// Trocar esse campo = VMT Hook
//   â†’ Sem patchear cÃ³digo executÃ¡vel
//   â†’ SÃ³ muda um ponteiro em data section
//   â†’ Muito mais difÃ­cil de detectar

static bool VmtHook(void* methodInfo, void* newFunc, void** origFunc) {
    if (!methodInfo) return false;

    // MethodInfo->methodPointer estÃ¡ no offset 0
    void** methodPtr = (void**)methodInfo;

    // Salvar original
    *origFunc = *methodPtr;

    // Tornar a pÃ¡gina writable (pode cruzar boundary de pÃ¡gina)
    uintptr_t pageSize = sysconf(_SC_PAGESIZE);
    uintptr_t pageAddr = (uintptr_t)methodPtr & ~(pageSize - 1);
    if (mprotect((void*)pageAddr, pageSize * 2, PROT_READ | PROT_WRITE) != 0) {
        LOGE("mprotect falhou: %s", strerror(errno));
        return false;
    }

    // Trocar o ponteiro (VMT swap)
    *methodPtr = newFunc;

    // Restaurar proteÃ§Ã£o (read-only)
    mprotect((void*)pageAddr, pageSize * 2, PROT_READ);

    LOGI("VMT Hook aplicado: orig=%p â†’ new=%p", *origFunc, newFunc);
    return true;
}

// ============================================================
// Manual WorldToScreen â€” Evita chamar Camera.WorldToScreenPoint
// que pode causar deadlock no Update loop
// ============================================================
static void MultiplyMatrix(const Matrix4x4& a, const Matrix4x4& b, Matrix4x4& out) {
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            out.m[j * 4 + i] = 0;
            for (int k = 0; k < 4; k++) {
                out.m[j * 4 + i] += a.m[k * 4 + i] * b.m[j * 4 + k];
            }
        }
    }
}

static Vector3 ManualWorldToScreen(Vector3 world, const Matrix4x4& vp, int sw, int sh) {
    const float* m = vp.m;
    // Unity Matrix4x4 is column-major: m[col*4+row]
    float clipX = world.x * m[0] + world.y * m[4] + world.z * m[8]  + m[12];
    float clipY = world.x * m[1] + world.y * m[5] + world.z * m[9]  + m[13];
    float clipW = world.x * m[3] + world.y * m[7] + world.z * m[11] + m[15];

    if (clipW < 0.001f) return Vector3(0, 0, -1);

    float ndcX = clipX / clipW;
    float ndcY = clipY / clipW;

    float screenX = (ndcX * 0.5f + 0.5f) * (float)sw;
    float screenY = (1.0f - (ndcY * 0.5f + 0.5f)) * (float)sh;

    return Vector3(screenX, screenY, clipW);
}

// ============================================================
// HOOK DO OnUpdate â€” Chamado pelo jogo para CADA player
// ============================================================

static Vector3 g_aimCandTarget{0.0f, 0.0f, 0.0f};  // melhor alvo deste frame
static float   g_aimCandDist  = 1e9f;
static bool    g_aimCandValid = false;

// â”€â”€ Aimbot state (lock persistence) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
static void*   g_aimbotLockedTarget  = nullptr;   // alvo atualmente travado
static Vector3 g_aimbotLockedHeadPos{};           // Ãºltima posiÃ§Ã£o vÃ¡lida da cabeÃ§a
static bool    g_aimbotHasLock       = false;     // true = aimbot travado num alvo
static float   g_aimbotBestDist      = 1e9f;      // melhor candidato deste frame
static bool    g_aimbotJustLocked    = false;     // true = primeiro frame de novo lock -> snap instantaneo
static bool    g_wasFiring          = false;     // estado anterior de IsFiring (detectar borda de disparo)
// Predicao de movimento (velocity prediction)
static Vector3   g_predSmoothVel{};            // velocidade suavizada por EMA (m/ms)
static Vector3   g_predPrevHeadPos{};          // posicao anterior da cabeca do alvo travado
static long long g_predPrevTime    = 0;        // timestamp do frame anterior (ms)
static long long g_frameCurrentMs  = 0;        // timestamp do frame atual (atualizado no newFrame)
// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

// ============================================================
// PLAYER HACKS (v49)
// ============================================================

// Helper: lÃª string IL2CPP (UTF-16LE) e converte para UTF-8 simples (ASCII subset)
// Estrutura do objeto System.String no IL2CPP:
//   +0x10: int32_t length (nÂº de chars UTF-16)
//   +0x14: uint16_t[] chars
static void il2cppStringToUtf8(void* strObj, char* out, int maxOut) {
    if (!strObj || !out || maxOut <= 0) { if (out) out[0] = '\0'; return; }
    out[0] = '\0';
    int32_t len = *(int32_t*)((uint8_t*)strObj + 0x10);
    if (len <= 0 || len > 128) return;
    uint16_t* chars = (uint16_t*)((uint8_t*)strObj + 0x14);
    int written = 0;
    for (int i = 0; i < len && written < maxOut - 1; i++) {
        uint16_t c = chars[i];
        if (c < 0x80) {
            out[written++] = (char)c;
        } else if (c < 0x800) {
            if (written + 1 >= maxOut - 1) break;
            out[written++] = (char)(0xC0 | (c >> 6));
            out[written++] = (char)(0x80 | (c & 0x3F));
        } else {
            if (written + 2 >= maxOut - 1) break;
            out[written++] = (char)(0xE0 | (c >> 12));
            out[written++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[written++] = (char)(0x80 | (c & 0x3F));
        }
    }
    out[written] = '\0';
}

static void Hook_OnUpdate(void* self, void* methodInfo) {
    // â”€â”€ Player local: SetAimRotation â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    if (self && sharedData && hookActive.load() &&
        fn_IsLocalPlayer && fn_IsLocalPlayer(self, nullptr)) {
        // Liberar lock se nenhum alvo vÃ¡lido foi encontrado no frame anterior.
        if (g_aimbotBestDist >= 9e8f) { g_aimbotHasLock = false; g_aimbotLockedTarget = nullptr; }

        // â”€â”€ Aimbot: Head direct snap + SLERP suave â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        // Ativa se: (alwaysTrack ligado) OU (triggerHeld com triggerKey configurado)
        bool triggerActive;
        if (sharedData->triggerKey == 0) {
            // Sem tecla configurada: ativa quando o player esta atirando no jogo
            triggerActive = fn_IsFiring ? fn_IsFiring(self, nullptr) : true;
        } else {
            // Com tecla configurada: IsFiring OU tecla fisica segurada
            bool isFiring = fn_IsFiring ? fn_IsFiring(self, nullptr) : false;
            triggerActive = isFiring || (sharedData->triggerHeld > 0);
        }
        bool shouldAim = sharedData->aimbotAlwaysTrack || triggerActive;

        // Magic Bullet: detectar borda de disparo (false->true) e snap instantaneo para cabeca
        bool isFiringNow  = fn_IsFiring ? fn_IsFiring(self, nullptr) : false;
        bool firingEdge   = isFiringNow && !g_wasFiring;
        g_wasFiring       = isFiringNow;

        // Busy-guard: nao mover mira durante granada ou skill ativa (gelo, etc.)
        bool isBusy = false;
        if (fn_get_IsGrenadeStart    && fn_get_IsGrenadeStart(self, nullptr))    isBusy = true;
        if (fn_get_IsRunningActSkill && fn_get_IsRunningActSkill(self, nullptr)) isBusy = true;

        // Magic Bullet: snap para cabeca no frame exato do inicio do disparo
        // Independe do smooth/alwaysTrack -- garante hit na cabeca ao atirar
        // Ativa somente se: aimbot ligado + tem lock + nao busy + firingEdge
        bool magicBulletFired = false;
        if (firingEdge && sharedData->magicBulletEnabled && sharedData->aimbotEnabled &&
            g_aimbotHasLock && fn_SetAimRotation && g_camTransform && fn_get_position && !isBusy) {
            Vector3 mbCamPos = fn_get_position(g_camTransform, nullptr);
            if (!std::isnan(mbCamPos.x) && !std::isnan(mbCamPos.y) && !std::isnan(mbCamPos.z)) {
                float mbDx = g_aimbotLockedHeadPos.x - mbCamPos.x;
                float mbDy = g_aimbotLockedHeadPos.y - mbCamPos.y;
                float mbDz = g_aimbotLockedHeadPos.z - mbCamPos.z;
                float mbLen = sqrtf(mbDx*mbDx + mbDy*mbDy + mbDz*mbDz);
                if (mbLen >= 0.1f) {
                    Quaternion mbQ = LookQuatFromDir(mbDx, mbDy, mbDz);
                    fn_SetAimRotation(self, mbQ, true, nullptr);
                    magicBulletFired = true; // snap aplicado, pular aimbot suave neste frame
                }
            }
        }

        // Aimbot suave: nao rodar se Magic Bullet ja aplicou rotacao neste frame
        if (!magicBulletFired && g_aimbotHasLock && fn_SetAimRotation && g_camTransform && fn_get_position &&
            sharedData->aimbotEnabled && shouldAim && !isBusy) {
            Vector3 camPos = fn_get_position(g_camTransform, nullptr);
            if (!std::isnan(camPos.x) && !std::isnan(camPos.y) && !std::isnan(camPos.z)) {
                float dx = g_aimbotLockedHeadPos.x - camPos.x;
                float dy = g_aimbotLockedHeadPos.y - camPos.y;
                float dz = g_aimbotLockedHeadPos.z - camPos.z;
                float len = sqrtf(dx*dx + dy*dy + dz*dz);
                if (len >= 0.1f) {
                    Quaternion targetQ = LookQuatFromDir(dx, dy, dz);
                    float smooth = sharedData->aimbotSmooth;
                    // Snap instantaneo na primeira frame de novo lock (AWM/12-gauge)
                    bool doSnap = g_aimbotJustLocked;
                    g_aimbotJustLocked = false;
                    if (!doSnap && smooth > 0.0f && smooth < 1.0f) {
                        Quaternion curQ = *(Quaternion*)((uint8_t*)self + OFF_m_CurrentAimRotation);
                        float dot = curQ.x*targetQ.x + curQ.y*targetQ.y
                                  + curQ.z*targetQ.z + curQ.w*targetQ.w;
                        if (dot < 0.0f) {
                            targetQ.x=-targetQ.x; targetQ.y=-targetQ.y;
                            targetQ.z=-targetQ.z; targetQ.w=-targetQ.w;
                        }
                        // Smooth adaptativo: rapido quando longe, preciso perto do alvo
                        float absDot = (dot < 0.0f ? -dot : dot);
                        float angleErr = 1.0f - absDot;  // 0=no alvo, 1=longe
                        float baseT = 1.0f - smooth;     // velocidade base
                        float t = baseT + angleErr * baseT * 2.5f; // ate 3.5x mais rapido ao longe
                        if (t > 1.0f) t = 1.0f;
                        targetQ.x = curQ.x + (targetQ.x - curQ.x) * t;
                        targetQ.y = curQ.y + (targetQ.y - curQ.y) * t;
                        targetQ.z = curQ.z + (targetQ.z - curQ.z) * t;
                        targetQ.w = curQ.w + (targetQ.w - curQ.w) * t;
                        float qlen = sqrtf(targetQ.x*targetQ.x + targetQ.y*targetQ.y
                                         + targetQ.z*targetQ.z + targetQ.w*targetQ.w);
                        if (qlen > 0.001f) {
                            targetQ.x/=qlen; targetQ.y/=qlen;
                            targetQ.z/=qlen; targetQ.w/=qlen;
                        }
                    }
                    fn_SetAimRotation(self, targetQ, true, nullptr);
                }
            }
        }

        if (orig_OnUpdate) orig_OnUpdate(self, methodInfo);

        return;
    }
    // â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

    // Chamar original (jogo processa lÃ³gica, fÃ­sica) para nÃ£o-local players
    if (orig_OnUpdate) {
        orig_OnUpdate(self, methodInfo);
    }

    // Se shared memory nÃ£o estÃ¡ pronto ou hook desativado, retorna
    if (!sharedData || !self || !hookActive.load()) return;

    // Sempre manter magic ativo para overlay detectar
    sharedData->magic = 0xDEADF00D;

    // resetSelf reservado (nao necessario com IsLocalTeammate)

    // espEnabled reservado para futuro; hook sempre processa players

    // â”€â”€ Detectar inÃ­cio de novo frame por tempo â”€â”€
    // Unity chama Update() para todos os objetos em sequÃªncia dentro do mesmo frame.
    // Se passaram >2ms desde a Ãºltima chamada, Ã© um novo frame.
    // Isso Ã© independente do overlay â€” VP matrix SEMPRE atualiza a cada frame.
    static long long g_lastCallMs = 0;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long long nowMs = ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
    bool newFrame = (nowMs - g_lastCallMs) > 2;
    g_lastCallMs = nowMs;

    if (newFrame) {
        g_frameId++;
        sharedData->debugLastCall = 10;

        // Camera.get_main â€” UMA VEZ por frame
        g_cachedCamera = fn_Camera_get_main ? fn_Camera_get_main(nullptr) : nullptr;
        sharedData->debugLastCall = 11;

        // CRITICO: sÃ³ resetar playerCount quando temos cÃ¢mera vÃ¡lida.
        // Se Camera.get_main retorna null neste frame, mantemos os dados
        // do frame anterior visÃ­veis no overlay em vez de zerar tudo.
        if (g_cachedCamera) {
            sharedData->playerCount = 0;
        } else {
            // sem cÃ¢mera: retornar cedo, dados do frame anterior permanecem
            return;
        }

        // ResoluÃ§Ã£o interna do jogo â€” pode ser menor que a surface do overlay
        // (Free Fire reduz res interna em grafico baixo/medio)
        // Atualiza sharedData->screenW/H para o overlay escalar as coordenadas W2S
        if (fn_Screen_get_width && fn_Screen_get_height) {
            int gw = fn_Screen_get_width(nullptr);
            int gh = fn_Screen_get_height(nullptr);
            if (gw > 0 && gh > 0) {
                sharedData->screenW = gw;
                sharedData->screenH = gh;
            }
        }

        // VP Matrix â€” SEMPRE atualiza (captura FOV atual: scope vs hip-fire)
        g_vpValid = false;
        if (useManualW2S && fn_get_worldToCameraMatrix && fn_get_projectionMatrix) {
            Matrix4x4 viewMat = fn_get_worldToCameraMatrix(g_cachedCamera, nullptr);
            sharedData->debugLastCall = 12;
            Matrix4x4 projMat = fn_get_projectionMatrix(g_cachedCamera, nullptr);
            sharedData->debugLastCall = 13;
            MultiplyMatrix(projMat, viewMat, g_vpMatrix);
            g_vpValid = true;
        }

        // â”€â”€ Aim: g_camTransform (necessÃ¡rio para silentAim no prÃ³ximo frame) â”€â”€â”€
        g_camTransform = fn_get_transform ? fn_get_transform(g_cachedCamera, nullptr) : nullptr;

        // Cache euler prÃ©-frame para anti-recoil em Hook_CameraLateUpdate.
        // Lido AQUI (antes do camera controller rodar) para capturar o estado
        // sem recoil. Guard NaN: se invÃ¡lido, anti-recoil fica desativado neste frame.
        g_camEulerValid = false;
        if (g_camTransform && fn_get_eulerAngles) {
            Vector3 e = fn_get_eulerAngles(g_camTransform, nullptr);
            if (!std::isnan(e.x) && !std::isnan(e.y) && !std::isnan(e.z)) {
                g_cachedCamEuler = e;
                g_camEulerValid  = true;
            }
        }

        // Camera-moving aimbot REMOVIDO (v29).
        // aimbotHasTarget reflete se aimbot tem alvo para o HUD do overlay.
        sharedData->aimbotHasTarget = g_aimbotHasLock ? 1 : 0;
        g_frameCurrentMs = nowMs; // atualiza timestamp do frame para predicao

        // Resetar candidatos para este frame
        g_aimBestScreenDist = 1e9f;
        g_aimBestHp         = 0x7fffffff;
        g_aimBestDepth      = 1e9f;
        g_aimCandValid      = false;
        g_aimCandDist       = 1e9f;
        // Resetar candidato aimbot (lock Ã© preservado entre frames)
        g_aimbotBestDist    = 1e9f;
        // Se aimbot foi desativado, libera o lock
        if (!sharedData->aimbotEnabled) {
            g_aimbotLockedTarget = nullptr;
            g_aimbotHasLock      = false;
        }
    }

    int idx = sharedData->playerCount;
    if (idx >= MAX_ESP_PLAYERS || idx >= 20) return;

    void* camera = g_cachedCamera;
    if (!camera) return;

    // Precisamos de VP matrix OU WorldToScreenPoint fallback
    if (!g_vpValid && !fn_WorldToScreenPoint) return;

    int screenH = sharedData->screenH;
    int screenW = sharedData->screenW;
    if (screenW <= 0 || screenH <= 0) return;

    // â”€â”€ Filtrar eu mesmo + aliados â”€â”€
    // IsLocalTeammate(true) = inclui o proprio jogador + todos os aliados
    // (o player local jÃ¡ foi tratado acima e retornou; aqui sÃ³ aliados)
    if (fn_IsLocalTeammate && fn_IsLocalTeammate(self, true, nullptr)) return;
    // Fallback: filtra self (caso IsLocalTeammate nÃ£o disponÃ­vel)
    if (!fn_IsLocalTeammate && fn_IsLocalPlayer && fn_IsLocalPlayer(self, nullptr)) return;

    // â”€â”€ Pegar transform do player â”€â”€
    sharedData->debugLastCall = 20 + idx;
    void* transform = fn_get_transform ? fn_get_transform(self, nullptr) : nullptr;
    if (!transform) return;

    // â”€â”€ Pegar posiÃ§Ã£o 3D do player â”€â”€
    sharedData->debugLastCall = 40 + idx;
    Vector3 worldPos = fn_get_position(transform, nullptr);

    // Sanity check
    if (std::isnan(worldPos.x) || std::isnan(worldPos.y) || std::isnan(worldPos.z)) return;

    // â”€â”€ PosiÃ§Ã£o real da cabeÃ§a: 3 camadas de fallback â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    // P1: get_HeadCollider() â†’ bounds.center  (hitbox exata registrada pelo servidor)
    // P2: Animator.GetBoneTransform(Head=10) â†’ position (bone visual, muito preciso)
    // P3: worldPos.y + 1.75 â€” estimativa fixa
    Vector3 bottomWorld(worldPos.x, worldPos.y - 0.05f, worldPos.z);
    Vector3 topWorld;
    bool gotHead = false;

    // â”€â”€ Aimbot: GetHeadTF() â€” posiÃ§Ã£o real da cabeÃ§a via mÃ©todo do jogo â”€â”€â”€â”€â”€
    // Filtros aplicados ANTES de usar como candidato:
    //   â€¢ IsVisible()         â€” ignora inimigos atrÃ¡s de paredes (se aimbotThroughWalls=0)
    //   â€¢ get_IsDieing()      â€” ignora derrubados/sangrando
    //   â€¢ IsCrouching() + altura â€” ignora deitados (prone)
    Vector3 aimbotHeadPos{};
    bool    aimbotHeadValid = false;
    if (sharedData->aimbotEnabled && fn_GetHeadTF && fn_get_position) {
        // Filtro: derrubado/sangrando
        bool isKnockedAb2 = *(bool*)((uint8_t*)self + OFF_IsKnockedDownBleed);
        bool isDying = isKnockedAb2 || (fn_get_IsDieing && fn_get_IsDieing(self, nullptr));
        if (isDying && self == g_aimbotLockedTarget) {
            g_aimbotHasLock = false; g_aimbotLockedTarget = nullptr;
        }
        if (!isDying || !sharedData->aimbotIgnoreKnocked) {
            if (!isDying) {
                // Filtro: deitado (prone)
                bool isProneSkip = false;
                if (fn_IsCrouching) {
                    bool crouching = fn_IsCrouching(self, nullptr);
                    if (crouching) {
                        void* htf = fn_GetHeadTF(self, nullptr);
                        if (htf) {
                            Vector3 hp = fn_get_position(htf, nullptr);
                            if (!std::isnan(hp.y) && (hp.y - worldPos.y) < 0.75f)
                                isProneSkip = true;
                        }
                    }
                }
                if (!isProneSkip) {
                    void* headTF = fn_GetHeadTF(self, nullptr);
                    if (headTF) {
                        Vector3 hp = fn_get_position(headTF, nullptr);
                        if (!std::isnan(hp.x) && !std::isnan(hp.y) && !std::isnan(hp.z) &&
                            (hp.x != 0.0f || hp.y != 0.0f || hp.z != 0.0f)) {
                            aimbotHeadPos   = hp;
                            aimbotHeadValid = true;
                        }
                    }
                }
            }
        }
    }

    // â”€â”€ P1: Head collider â€” hitbox real de colisÃ£o/dano â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    // Usa get_bounds_Injected (out Bounds& ret) â€” ponteiro de saÃ­da em x1, sem SRET.
    if (!gotHead && fn_get_HeadCollider && fn_Collider_get_bounds_Injected) {
        void* headCollider = fn_get_HeadCollider(self, nullptr);
        if (headCollider) {
            BoundsVal bounds{};
            fn_Collider_get_bounds_Injected(headCollider, &bounds, nullptr);
            if (!std::isnan(bounds.cx) && !std::isnan(bounds.cy) && !std::isnan(bounds.cz) &&
                (bounds.cx != 0.0f || bounds.cy != 0.0f || bounds.cz != 0.0f)) {
                topWorld = Vector3(bounds.cx, bounds.cy, bounds.cz);
                gotHead = true;
            }
        }
    }

    // â”€â”€ P2: Head bone via Animator (bone visual, fallback do collider) â”€â”€â”€â”€â”€â”€â”€
    // Guard: validar que animComp parece um ponteiro heap vÃ¡lido (> 0x1000)
    // antes de desreferenciar â€” offset 0x700 pode estar errado para alguns players.
    if (!gotHead && fn_GetBoneTransform && fn_get_position) {
        void* animComp = *(void**)((uint8_t*)self + PLAYER_ANIM_COMPONENT_OFFSET);
        if (animComp && (uintptr_t)animComp > 0x1000) {
            void* animator = *(void**)((uint8_t*)animComp + ANIM_COMPONENT_ANIMATOR_OFFSET);
            if (animator && (uintptr_t)animator > 0x1000) {
                void* headTransform = fn_GetBoneTransform(animator, HUMAN_BODY_BONE_HEAD, nullptr);
                if (headTransform) {
                    Vector3 headPos = fn_get_position(headTransform, nullptr);
                    if (!std::isnan(headPos.x) && !std::isnan(headPos.y) && !std::isnan(headPos.z)) {
                        topWorld = headPos;
                        gotHead = true;
                    }
                }
            }
        }
    }

    if (!gotHead) {
        // â”€â”€ P3: Estimativa fixa â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        topWorld = Vector3(worldPos.x, worldPos.y + 1.75f, worldPos.z);
    }

    Vector3 screenBottom, screenTop;

    if (g_vpValid) {
        // â”€â”€ Modo VP Matrix â€” MATH PURA, zero chamada il2cpp â”€â”€
        sharedData->debugLastCall = 60 + idx;
        screenBottom = ManualWorldToScreen(bottomWorld, g_vpMatrix, screenW, screenH);
        screenTop = ManualWorldToScreen(topWorld, g_vpMatrix, screenW, screenH);
    } else {
        // â”€â”€ Fallback: WorldToScreenPoint direto â”€â”€
        sharedData->debugLastCall = 80 + idx;
        screenBottom = fn_WorldToScreenPoint(camera, bottomWorld, nullptr);
        screenTop = fn_WorldToScreenPoint(camera, topWorld, nullptr);
        // Unity Y invertido
        screenBottom.y = (float)screenH - screenBottom.y;
        screenTop.y = (float)screenH - screenTop.y;
    }

    // Verificar se estÃ¡ na frente da cÃ¢mera
    if (screenBottom.z < 0.1f || screenTop.z < 0.1f) return;

    // HP check comum (evita alocar alvo morto/derrubado)
    int chp = fn_get_CurHP ? fn_get_CurHP(self, nullptr) : 1;

    float dX = screenTop.x - screenW * 0.5f;
    float dY = screenTop.y - screenH * 0.5f;
    float screenDist = sqrtf(dX * dX + dY * dY);

    // â”€â”€ Aimbot: seleÃ§Ã£o de candidato (head direto, lock com histerese) â”€â”€â”€â”€â”€â”€â”€â”€â”€
    // Seleciona melhor candidato por distÃ¢ncia da tela ao centro.
    // Filtros jÃ¡ aplicados acima (isDying, prone, via aimbotHeadValid).
    if (sharedData->aimbotEnabled && aimbotHeadValid && chp > 0) {
        // Filtro visibilidade: ignorar paredes SOMENTE se aimbotThroughWalls desativado
        bool aimbotVis = (sharedData->aimbotThroughWalls)
                         ? true
                         : (fn_IsVisible ? fn_IsVisible(self, nullptr) : true);
        if (!aimbotVis && self == g_aimbotLockedTarget) {
            g_aimbotHasLock = false; g_aimbotLockedTarget = nullptr;
        }
        if (aimbotVis && screenTop.z <= 120.0f) {
            float fovDeg = sharedData->aimbotFovDeg;
            if (fovDeg < 1.0f || fovDeg > 180.0f) fovDeg = 60.0f;
            float fovRadPx = (float)screenW * (fovDeg / 90.0f) * 0.5f;

            // Histerese: alvo travado usa raio 35% maior para nÃ£o soltar ao mover
            float effectiveRadius = fovRadPx;
            if (self == g_aimbotLockedTarget && g_aimbotHasLock) {
                effectiveRadius = fovRadPx * 1.35f;
            }

            if (screenDist < effectiveRadius) {
                // Bonus sticky: alvo ja travado tem 55% de desconto na comparacao
                // Evita troca rapida entre inimigos proximos (causa do tremido)
                bool isCurrent = (self == g_aimbotLockedTarget && g_aimbotHasLock);
                float compareDist = isCurrent ? screenDist * 0.45f : screenDist;
                if (compareDist < g_aimbotBestDist) {
                    bool isNew = (self != g_aimbotLockedTarget || !g_aimbotHasLock);
                    g_aimbotBestDist     = compareDist;
                    g_aimbotLockedTarget = self;
                    g_aimbotHasLock      = true;
                    if (isNew) {
                        // Novo alvo: resetar estado de predicao, snap imediato
                        g_aimbotJustLocked  = true;
                        g_predSmoothVel     = Vector3{0.0f, 0.0f, 0.0f};
                        g_predPrevHeadPos   = aimbotHeadPos;
                        g_predPrevTime      = g_frameCurrentMs;
                        g_aimbotLockedHeadPos = aimbotHeadPos;
                    } else {
                        // Alvo atual: calcular velocidade suavizada (EMA alpha=0.4)
                        float dtMs = (float)(g_frameCurrentMs - g_predPrevTime);
                        if (dtMs > 0.5f && dtMs < 60.0f) {
                            float vx = (aimbotHeadPos.x - g_predPrevHeadPos.x) / dtMs;
                            float vy = (aimbotHeadPos.y - g_predPrevHeadPos.y) / dtMs;
                            float vz = (aimbotHeadPos.z - g_predPrevHeadPos.z) / dtMs;
                            const float alpha = 0.4f;
                            g_predSmoothVel.x = g_predSmoothVel.x * (1.0f - alpha) + vx * alpha;
                            g_predSmoothVel.y = g_predSmoothVel.y * (1.0f - alpha) + vy * alpha;
                            g_predSmoothVel.z = g_predSmoothVel.z * (1.0f - alpha) + vz * alpha;
                        }
                        g_predPrevHeadPos = aimbotHeadPos;
                        g_predPrevTime    = g_frameCurrentMs;
                        // Aplicar predicao: pos_futura = pos_atual + vel * tempo_previsto
                        float predMs = sharedData->aimbotPrediction;
                        if (predMs > 0.0f && predMs <= 200.0f) {
                            g_aimbotLockedHeadPos.x = aimbotHeadPos.x + g_predSmoothVel.x * predMs;
                            g_aimbotLockedHeadPos.y = aimbotHeadPos.y + g_predSmoothVel.y * predMs;
                            g_aimbotLockedHeadPos.z = aimbotHeadPos.z + g_predSmoothVel.z * predMs;
                        } else {
                            g_aimbotLockedHeadPos = aimbotHeadPos;
                        }
                    }
                }
            }
        }
    }
    // â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

    // â”€â”€ Escrever no SharedMemory â”€â”€
    ESPEntry& entry = sharedData->players[idx];
    entry.topX = screenTop.x;
    entry.topY = screenTop.y;
    entry.topZ = screenTop.z;
    entry.bottomX = screenBottom.x;
    entry.bottomY = screenBottom.y;
    entry.bottomZ = screenBottom.z;
    entry.distance = screenBottom.z;
    entry.team = 0;
    entry.health = 100.0f;
    entry.valid = true;

    // HP via chamada direta il2cpp
    int curHp = fn_get_CurHP ? fn_get_CurHP(self, nullptr) : 100;
    int maxHp = fn_get_MaxHP ? fn_get_MaxHP(self, nullptr) : 100;
    if (maxHp <= 0) maxHp = 100;
    entry.curHp  = curHp;
    entry.maxHp  = maxHp;

    // Knocked: campo bool IsKnockedDownBleed a offset 0x1150 do objeto Player
    entry.knocked = *(bool*)((uint8_t*)self + OFF_IsKnockedDownBleed);

    // Nick name: Player::get_NickName() â†’ string IL2CPP (v49)
    entry.nick[0] = '\0';
    if (fn_get_NickName) {
        void* strObj = fn_get_NickName(self, nullptr);
        if (strObj) il2cppStringToUtf8(strObj, entry.nick, sizeof(entry.nick));
    }

    // Marcar se aimbot estÃ¡ travado neste player
    entry.isLocked = (self == g_aimbotLockedTarget) && g_aimbotHasLock;

    sharedData->playerCount = idx + 1;
    sharedData->magic = 0xDEADF00D;
    sharedData->writeSeq.fetch_add(1, std::memory_order_release);
}

// ============================================================
// INIT â€” Resolve funÃ§Ãµes + aplica VMT hook
// ============================================================

void* hack_thread(void*) {
    // Esconder nome da thread (anti-cheat enumera threads)
    prctl(PR_SET_NAME, "Binder:default", 0, 0, 0);

    // â”€â”€ Criar SHM IMEDIATAMENTE (Zygisk: fd jÃ¡ disponÃ­vel como root) â”€â”€
    // Garante sharedData != null durante todos os stage markers abaixo.
    if (!sharedData) {
        shmFd = shm_create_file();
        if (shmFd >= 0) {
            sharedData = shm_map(shmFd);
            if (sharedData) {
                memset(sharedData, 0, sizeof(SharedESPData));
                sharedData->magic        = 0xDEADF00D;
                sharedData->debugLastCall = 1; // stage 1: SHM criado, thread rodando
            } else {
                close(shmFd);
            }
        }
    }

    hookLogWrite("Thread iniciada, aguardando libil2cpp.so... shm=%p", sharedData);
    LOGI("Hook thread iniciada, aguardando libil2cpp.so...");

    // â”€â”€ Aguardar il2cpp carregar â”€â”€
    while (!isLibraryLoaded("libil2cpp.so")) {
        if (sharedData) sharedData->debugLastCall = 2; // stage 2: waiting libil2cpp
        sleep(1);
    }
    LOGI("libil2cpp.so detectada");
    hookLogWrite("libil2cpp.so detectada");

    // â”€â”€ Pegar base address do libil2cpp.so â”€â”€
    // findLibrary retorna o PRIMEIRO mapeamento que contÃ©m o nome.
    // No Android ARM64, o primeiro mapeamento Ã© r--p offset=0 = load base.
    // Os offsets do il2cppdumper (RVA) sÃ£o relativos a esse endereÃ§o.
    if (sharedData) sharedData->debugLastCall = 3; // stage 3: libil2cpp found
    uintptr_t il2cpp_base = findLibrary("libil2cpp.so");
    g_il2cpp_base = il2cpp_base;
    if (!il2cpp_base) {
        if (sharedData) sharedData->debugLastCall = 93; // stage 93: base=0 error
        LOGE("findLibrary(libil2cpp.so) retornou 0!");
        hookLogWrite("ERRO: il2cpp base = 0");
        return nullptr;
    }
    LOGI("libil2cpp.so base = 0x%lx", (unsigned long)il2cpp_base);
    hookLogWrite("il2cpp base = 0x%lx", (unsigned long)il2cpp_base);

    // Validar que a base Ã© realmente o inicio do ELF (magic: 7f 45 4c 46)
    {
        unsigned char elfMagic[4] = {0};
        memcpy(elfMagic, (void*)il2cpp_base, 4);
        if (elfMagic[0] != 0x7f || elfMagic[1] != 'E' || elfMagic[2] != 'L' || elfMagic[3] != 'F') {
            LOGE("ERRO: base 0x%lx NAO Ã© ELF! magic=%02x%02x%02x%02x",
                 (unsigned long)il2cpp_base, elfMagic[0], elfMagic[1], elfMagic[2], elfMagic[3]);
            hookLogWrite("ERRO: base nao e ELF! magic=%02x%02x%02x%02x",
                         elfMagic[0], elfMagic[1], elfMagic[2], elfMagic[3]);
            return nullptr;
        }
        LOGI("ELF magic validado: OK (7f454c46)");
    }

    // â”€â”€ Resolver TODAS as funÃ§Ãµes via base + offset direto â”€â”€
    // ZERO dependÃªncia de il2cpp_domain_get, fake_dlfcn, ByNameModding
    #define RESOLVE_OFFSET(type, offset) (type)(il2cpp_base + offset)

    fn_Camera_get_main         = RESOLVE_OFFSET(void*(*)(void*),                   OFF_Camera_get_main);
    fn_get_worldToCameraMatrix = RESOLVE_OFFSET(Matrix4x4(*)(void*, void*),        OFF_get_worldToCameraMatrix);
    fn_get_projectionMatrix    = RESOLVE_OFFSET(Matrix4x4(*)(void*, void*),        OFF_get_projectionMatrix);
    fn_WorldToScreenPoint      = RESOLVE_OFFSET(Vector3(*)(void*, Vector3, void*), OFF_WorldToScreenPoint);
    fn_get_transform           = RESOLVE_OFFSET(void*(*)(void*, void*),            OFF_get_transform);
    fn_get_position            = RESOLVE_OFFSET(Vector3(*)(void*, void*),          OFF_get_position);
    fn_Screen_get_width        = RESOLVE_OFFSET(int(*)(void*),                     OFF_Screen_get_width);
    fn_Screen_get_height       = RESOLVE_OFFSET(int(*)(void*),                     OFF_Screen_get_height);
    fn_IsLocalPlayer           = RESOLVE_OFFSET(bool(*)(void*, void*),             OFF_IsLocalPlayer);
    fn_IsLocalTeammate         = RESOLVE_OFFSET(bool(*)(void*, bool, void*),        OFF_IsLocalTeammate);
    fn_get_CurHP               = RESOLVE_OFFSET(int(*)(void*, void*),               OFF_get_CurHP);
    fn_get_MaxHP               = RESOLVE_OFFSET(int(*)(void*, void*),               OFF_get_MaxHP);
    // Aim Assist
    fn_get_eulerAngles = OFF_Transform_get_eulerAngles
        ? RESOLVE_OFFSET(Vector3(*)(void*, void*),       OFF_Transform_get_eulerAngles) : nullptr;
    fn_set_eulerAngles = OFF_Transform_set_eulerAngles
        ? RESOLVE_OFFSET(void(*)(void*, Vector3, void*), OFF_Transform_set_eulerAngles) : nullptr;
    fn_GetBoneTransform        = RESOLVE_OFFSET(void*(*)(void*, int32_t, void*),    OFF_Animator_GetBoneTransform);
    fn_get_HeadCollider              = RESOLVE_OFFSET(void*(*)(void*, void*),             OFF_get_HeadCollider);
    fn_Collider_get_bounds_Injected  = RESOLVE_OFFSET(void(*)(void*, BoundsVal*, void*),  OFF_Collider_get_bounds_Injected);
    // Aimbot SetAimRotation
    fn_SetAimRotation = RESOLVE_OFFSET(SetAimRotationFn, OFF_SetAimRotation);
    fn_IsVisible      = RESOLVE_OFFSET(bool(*)(void*, void*),              OFF_IsVisible);
    fn_get_IsSighting = RESOLVE_OFFSET(bool(*)(void*, void*),              OFF_get_IsSighting);
    fn_IsFiring       = RESOLVE_OFFSET(bool(*)(void*, void*),              OFF_IsFiring);
    // Aimbot 2 â€” GetHeadTF + filtros
    fn_GetHeadTF      = RESOLVE_OFFSET(void*(*)(void*, void*),             OFF_GetHeadTF);
    fn_IsCrouching    = RESOLVE_OFFSET(bool(*)(void*, void*),              OFF_IsCrouching);
    fn_get_IsDieing   = RESOLVE_OFFSET(bool(*)(void*, void*),              OFF_get_IsDieing);
    // Player Hacks (v49) â€” get_NickName sÃ³ chamado (nÃ£o hookeado)
    fn_get_NickName        = RESOLVE_OFFSET(void*(*)(void*, void*), OFF_get_NickName);
    // Busy-guard: desativa aimbot durante granada/skill ativa
    fn_get_IsGrenadeStart    = OFF_get_IsGrenadeStart    ? RESOLVE_OFFSET(bool(*)(void*, void*), OFF_get_IsGrenadeStart)    : nullptr;
    fn_get_IsRunningActSkill = OFF_get_IsRunningActSkill ? RESOLVE_OFFSET(bool(*)(void*, void*), OFF_get_IsRunningActSkill) : nullptr;
    // Speed Hack â€” resolvido via VmtHook em UpdateVelocity (ver adiante)

    LOGI("Offsets resolvidos:");
    LOGI("  get_main          = %p (base+0x%X)", fn_Camera_get_main, OFF_Camera_get_main);
    LOGI("  worldToCam        = %p (base+0x%X)", fn_get_worldToCameraMatrix, OFF_get_worldToCameraMatrix);
    LOGI("  projMatrix        = %p (base+0x%X)", fn_get_projectionMatrix, OFF_get_projectionMatrix);
    LOGI("  WorldToScreenPoint= %p (base+0x%X)", fn_WorldToScreenPoint, OFF_WorldToScreenPoint);
    LOGI("  get_transform     = %p (base+0x%X)", fn_get_transform, OFF_get_transform);
    LOGI("  get_position      = %p (base+0x%X)", fn_get_position, OFF_get_position);
    LOGI("  Screen_get_width  = %p (base+0x%X)", fn_Screen_get_width, OFF_Screen_get_width);
    LOGI("  Screen_get_height = %p (base+0x%X)", fn_Screen_get_height, OFF_Screen_get_height);
    hookLogWrite("Offsets: cam=%p trans=%p pos=%p", fn_Camera_get_main, fn_get_transform, fn_get_position);

    #undef RESOLVE_OFFSET

    // Usar WorldToScreenPoint direto (mais preciso â€” evita erro de VP matrix manual)
    // Estamos na Unity thread (LateUpdate hook), sem risco de deadlock.
    useManualW2S = false;
    LOGI("Usando WorldToScreenPoint direto para W2S (posicionamento preciso)");

    // Verificar se tudo resolveu
    if (!fn_Camera_get_main || !fn_get_transform || !fn_get_position) {
        LOGE("Falha nos offsets: cam=%p trans=%p pos=%p",
             fn_Camera_get_main, fn_get_transform, fn_get_position);
        hookLogWrite("ERRO: offsets invalidos");
        return nullptr;
    }

    hookLogWrite("Game data dir: %s", getGameDataDir());

    // SHM jÃ¡ criado no inÃ­cio do hack_thread.
    // Se por algum motivo falhou lÃ¡, tenta novamente agora.
    if (!sharedData) {
        shmFd = shm_create_file();
        if (shmFd < 0) {
            LOGE("Falha ao criar shared memory: errno=%d (%s)", errno, strerror(errno));
            hookLogWrite("FALHA shm_create_file: errno=%d (%s) uid=%d", errno, strerror(errno), getuid());
            return nullptr;
        }
        sharedData = shm_map(shmFd);
        if (!sharedData) {
            close(shmFd);
            LOGE("Falha ao mapear shared memory: errno=%d (%s)", errno, strerror(errno));
            hookLogWrite("FALHA shm_map: errno=%d (%s)", errno, strerror(errno));
            return nullptr;
        }
        memset(sharedData, 0, sizeof(SharedESPData));
        sharedData->magic = 0xDEADF00D;
    }
    LOGI("SHM OK: path=%s mmap=%p", shmActivePath ? shmActivePath : "???", sharedData);
    hookLogWrite("SHM OK: path=%s mmap=%p", shmActivePath ? shmActivePath : "???", sharedData);

    // NOTA: Screen::get_width/height chamado DEPOIS do domain pronto
    // (il2cpp runtime precisa estar inicializado antes de chamar metodos)

    // â”€â”€ VMT Hook no LateUpdate â”€â”€
    // Precisamos do MethodInfo* do LateUpdate pra trocar o methodPointer.
    // dlopen/dlsym NAO funciona (namespace do linker Android 7+).
    // Solucao: resolver simbolos il2cpp lendo o ELF do disco direto.
    LOGI("[1/5] Resolvendo il2cpp API via ELF (memoria)...");
    hookLogWrite("[1/5] Resolvendo il2cpp API via ELF (memoria)...");

    // Resolver as funcoes il2cpp via ELF com retry.
    // libil2cpp.so pode estar parcialmente mapeada logo apos ser detectada â€”
    // o linker faz mmap lazy dos segmentos. Retentativas com sleep garantem
    // que todas as paginas PT_DYNAMIC estao presentes antes da leitura.
    uintptr_t addr_domain_get = 0, addr_assemblies = 0, addr_get_image = 0;
    uintptr_t addr_image_name = 0, addr_class_from_name2 = 0, addr_class_method = 0;
    uintptr_t addr_class_get_methods = 0;

    for (int elfTry = 0; elfTry < 6; elfTry++) {
        if (elfTry > 0) {
            LOGI("ELF retry %d/5 (aguardando mapeamento completo)...", elfTry);
            hookLogWrite("ELF retry %d/5...", elfTry);
            if (sharedData) sharedData->debugLastCall = 3; // volta a stage 3
            sleep(2);
            // Re-buscar base por seguranca
            uintptr_t newBase = findLibrary("libil2cpp.so");
            if (newBase) il2cpp_base = newBase;
        }
        addr_domain_get      = resolveElfSymbol(il2cpp_base, "il2cpp_domain_get");
        addr_assemblies      = resolveElfSymbol(il2cpp_base, "il2cpp_domain_get_assemblies");
        addr_get_image       = resolveElfSymbol(il2cpp_base, "il2cpp_assembly_get_image");
        addr_image_name      = resolveElfSymbol(il2cpp_base, "il2cpp_image_get_name");
        addr_class_from_name2= resolveElfSymbol(il2cpp_base, "il2cpp_class_from_name");
        addr_class_method    = resolveElfSymbol(il2cpp_base, "il2cpp_class_get_method_from_name");
        addr_class_get_methods = resolveElfSymbol(il2cpp_base, "il2cpp_class_get_methods");

        LOGI("ELF try %d: domain=%p asm=%p img=%p name=%p class=%p method=%p",
             elfTry + 1,
             (void*)addr_domain_get, (void*)addr_assemblies, (void*)addr_get_image,
             (void*)addr_image_name, (void*)addr_class_from_name2, (void*)addr_class_method);
        hookLogWrite("ELF try %d: domain=%p class=%p method=%p",
             elfTry + 1, (void*)addr_domain_get, (void*)addr_class_from_name2, (void*)addr_class_method);

        if (addr_domain_get && addr_assemblies && addr_get_image &&
            addr_image_name && addr_class_from_name2 && addr_class_method) break;
    }

    auto p_domain_get            = (void*(*)())                              addr_domain_get;
    auto p_domain_get_assemblies = (void**(*)(const void*, size_t*))         addr_assemblies;
    auto p_assembly_get_image    = (const void*(*)(const void*))             addr_get_image;
    auto p_image_get_name        = (const char*(*)(void*))                   addr_image_name;
    auto p_class_from_name       = (void*(*)(const void*, const char*, const char*)) addr_class_from_name2;
    auto p_class_get_method      = (void*(*)(void*, const char*, int))       addr_class_method;
    // il2cpp_class_get_methods(class, &iter) â€” itera metodos; iter=nullptr inicia
    auto p_class_get_methods     = (void*(*)(void*, void**))                 addr_class_get_methods;

    if (sharedData) sharedData->debugLastCall = 4; // stage 4: ELF resolve done
    if (!p_domain_get || !p_domain_get_assemblies || !p_assembly_get_image ||
        !p_image_get_name || !p_class_from_name || !p_class_get_method) {
        if (sharedData) sharedData->debugLastCall = 94; // stage 94: ELF resolve failed
        LOGE("ELF resolver falhou para uma ou mais funcoes il2cpp");
        hookLogWrite("ERRO: ELF resolver falhou apos 6 tentativas");
        return nullptr;
    }

    // Validar que domain_get aponta pra codigo ARM64 valido
    {
        uint32_t firstInst = *(uint32_t*)((uintptr_t)p_domain_get);
        LOGI("domain_get first instruction: 0x%08x", firstInst);
        hookLogWrite("domain_get inst: 0x%08x", firstInst);
        if (firstInst == 0 || firstInst == 0xffffffff) {
            LOGE("ERRO: domain_get aponta pra codigo invalido!");
            hookLogWrite("ERRO: domain_get codigo invalido");
            return nullptr;
        }
    }

    // Aguardar il2cpp_init() completar.
    // IMPORTANTE: NAO usar sigsetjmp/siglongjmp â€” quebra o ART (signal chain).
    // Em vez disso, esperar global-metadata.dat aparecer em maps.
    // il2cpp_init() faz mmap do metadata file â†’ quando aparece em maps,
    // o runtime ja esta inicializado o suficiente pra domain_get funcionar.
    LOGI("[2/5] Aguardando il2cpp runtime inicializar...");
    hookLogWrite("[2/5] Aguardando il2cpp init...");

    if (sharedData) sharedData->debugLastCall = 5; // stage 5: waiting metadata
    // Fase 1: Esperar global-metadata.dat em /proc/self/maps
    // Timeout reduzido para 30s (era 60s) â€” FF carrega rapidamente
    bool metadataFound = false;
    for (int i = 0; i < 30; i++) {
        FILE *maps = fopen("/proc/self/maps", "r");
        if (maps) {
            char line[1024];
            while (fgets(line, sizeof(line), maps)) {
                if (strstr(line, "global-metadata.dat")) {
                    metadataFound = true;
                    break;
                }
            }
            fclose(maps);
        }
        if (metadataFound) {
            LOGI("  global-metadata.dat encontrado (tentativa %d)", i+1);
            hookLogWrite("metadata encontrado (tentativa %d)", i+1);
            break;
        }
        LOGI("  aguardando global-metadata.dat... (%d/30)", i+1);
        sleep(1);
    }
    if (!metadataFound) {
        if (sharedData) sharedData->debugLastCall = 95; // stage 95: metadata timeout
        LOGE("global-metadata.dat timeout 30s");
        hookLogWrite("ERRO: metadata timeout 30s");
        return nullptr;
    }
    if (sharedData) sharedData->debugLastCall = 6; // stage 6: metadata found

    // Fase 2: Esperar domain ficar pronto.
    // FIX stage-6 crash: metadata mapeado != runtime inicializado.
    // p_domain_get() chamado cedo demais crashava dentro do il2cpp.
    // Aguarda 3s fixo para dar tempo ao runtime completar a init.
    LOGI("  Metadata encontrado. Aguardando runtime init (3s)...");
    hookLogWrite("metadata ok â€” aguardando runtime 3s...");
    sleep(3);
    LOGI("  Tentando domain_get...");

    void *domain = nullptr;
    for (int i = 0; i < 15; i++) {
        domain = p_domain_get();
        if (domain) {
            size_t sz = 0;
            void **assemblies = p_domain_get_assemblies(domain, &sz);
            if (assemblies && sz > 0) {
                LOGI("il2cpp domain PRONTO (tentativa %d): domain=%p assemblies=%zu", i+1, domain, sz);
                hookLogWrite("domain PRONTO (tentativa %d) assemblies=%zu", i+1, sz);
                break;
            }
        }
        if (sharedData) sharedData->debugLastCall = 7; // stage 7: waiting domain
        if (i >= 14) {
            if (sharedData) sharedData->debugLastCall = 97; // stage 97: domain timeout
            LOGE("il2cpp domain timeout (15s apos metadata)");
            hookLogWrite("ERRO: domain timeout 15s");
            return nullptr;
        }
        LOGI("  domain wait... (%d/15)", i+1);
        sleep(1);
    }

    // Agora il2cpp runtime esta pronto â€” seguro chamar metodos
    // NOTA: Screen::get_width/height NAO e chamado aqui.
    // O overlay conhece as dimensoes da tela via Android window system
    // e as escreve no SHM a cada frame (onOverlayDraw). Isso e mais
    // confiavel que chamar il2cpp de uma thread nao-Unity.
    LOGI("[3/5] il2cpp runtime pronto (screenW/H via overlay)");
    hookLogWrite("[3/5] il2cpp runtime pronto (screenW/H escrito pelo overlay)");

    // Achar Assembly-CSharp image
    LOGI("[3/5] Buscando Assembly-CSharp...");
    hookLogWrite("[3/5] Buscando Assembly-CSharp...");
    size_t asmCount = 0;
    void **assemblies = p_domain_get_assemblies(domain, &asmCount);
    void *cs_image = nullptr;
    for (size_t i = 0; i < asmCount; i++) {
        void *img = (void*)p_assembly_get_image(assemblies[i]);
        const char *name = p_image_get_name(img);
        if (name && strcmp(name, "Assembly-CSharp.dll") == 0) {
            cs_image = img;
            break;
        }
    }
    if (!cs_image) {
        if (sharedData) sharedData->debugLastCall = 96; // stage 96: assembly not found
        LOGE("Assembly-CSharp.dll nao encontrada!");
        hookLogWrite("ERRO: Assembly-CSharp.dll nao encontrada");
        return nullptr;
    }
    if (sharedData) sharedData->debugLastCall = 8; // stage 8: Assembly-CSharp found
    LOGI("Assembly-CSharp.dll = %p", cs_image);

    // Achar classe Player
    LOGI("[4/5] Buscando Player class...");
    hookLogWrite("[4/5] Buscando Player class...");
    if (sharedData) sharedData->debugLastCall = 81; // stage 81: finding Player class
    void *playerClass = p_class_from_name(cs_image, "COW.GamePlay", "Player");
    if (!playerClass) {
        if (sharedData) sharedData->debugLastCall = 98; // stage 98: Player class not found
        LOGE("Classe Player nao encontrada");
        hookLogWrite("ERRO: classe Player nao encontrada");
        return nullptr;
    }
    if (sharedData) sharedData->debugLastCall = 9; // stage 9: Player class found
    LOGI("playerClass = %p", playerClass);
    hookLogWrite("playerClass = %p", playerClass);

    // Pegar MethodInfo* do LateUpdate
    LOGI("  Chamando class_get_method_from_name(LateUpdate, 0)...");
    hookLogWrite("Buscando LateUpdate MethodInfo...");
    void *onUpdateMethodInfo = p_class_get_method(playerClass, "LateUpdate", 0);
    if (!onUpdateMethodInfo) {
        if (sharedData) sharedData->debugLastCall = 99; // stage 99: LateUpdate not found
        LOGE("LateUpdate nao encontrado em Player");
        hookLogWrite("ERRO: LateUpdate nao encontrado");
        return nullptr;
    }
    LOGI("LateUpdate MethodInfo = %p", onUpdateMethodInfo);
    hookLogWrite("LateUpdate MethodInfo = %p", onUpdateMethodInfo);

    // CRÃTICO: verificar que methodPointer bate com nosso offset.
    // p_class_get_method pode retornar um LateUpdate errado (hÃ¡ vÃ¡rios no dump).
    // Se nÃ£o bater, itera todos os mÃ©todos para achar o correto.
    // Hooking o mÃ©todo errado = Hook_OnUpdate recebe self invÃ¡lido = crash.
    void *currentMethodPtr = *(void**)onUpdateMethodInfo;
    void *expectedPtr = (void*)(il2cpp_base + OFF_LateUpdate);
    LOGI("  methodPointer atual = %p, esperado (base+off) = %p, match=%s",
         currentMethodPtr, expectedPtr,
         (currentMethodPtr == expectedPtr) ? "SIM" : "NAO");
    hookLogWrite("LateUpdate ptr=%p expected=%p match=%s",
         currentMethodPtr, expectedPtr,
         (currentMethodPtr == expectedPtr) ? "SIM" : "NAO");

    if (currentMethodPtr != expectedPtr) {
        LOGE("  MISMATCH â€” iterando metodos do Player para achar OFF_LateUpdate...");
        hookLogWrite("MISMATCH â€” buscando LateUpdate por endereco...");
        void *correctMethodInfo = nullptr;
        if (p_class_get_methods) {
            void *iter2 = nullptr;
            void *mi2   = nullptr;
            while ((mi2 = p_class_get_methods(playerClass, &iter2)) != nullptr) {
                if ((uintptr_t)*(void**)mi2 == (uintptr_t)expectedPtr) {
                    correctMethodInfo = mi2;
                    break;
                }
            }
        }
        if (!correctMethodInfo) {
            if (sharedData) sharedData->debugLastCall = 99;
            LOGE("  LateUpdate correto nao encontrado! Abortando hook.");
            hookLogWrite("ERRO: LateUpdate correto nao encontrado");
            return nullptr;
        }
        onUpdateMethodInfo = correctMethodInfo;
        LOGI("  LateUpdate correto encontrado via iteracao: %p", onUpdateMethodInfo);
        hookLogWrite("LateUpdate correto: %p", onUpdateMethodInfo);
    }

    // Aplicar VMT Hook
    LOGI("[5/5] Aplicando VMT Hook...");
    hookLogWrite("[5/5] Aplicando VMT Hook...");
    if (!VmtHook(onUpdateMethodInfo, (void*)Hook_OnUpdate, (void**)&orig_OnUpdate)) {
        LOGE("VMT Hook falhou");
        hookLogWrite("ERRO: VMT Hook falhou");
        return nullptr;
    }

    hookActive.store(true);
    sharedData->hookApplied = 0xBEEF1234;  // sinaliza overlay que VMT hook esta ativo
    LOGI("=== HOOK ATIVO === VMT hook em Player::LateUpdate");
    hookLogWrite("=== HOOK ATIVO === VMT em Player::LateUpdate");

    return nullptr;
}


// ============================================================
// ENTRY POINT: CONSTRUCTOR (ptrace + dlopen)
// Quando o injector faz dlopen() no processo do jogo,
// esta funcao roda automaticamente via __attribute__((constructor))
// ============================================================

bool g_hookStarted = false;

// fd do SHM pre-aberto em preAppSpecialize (root context) pelo modulo Zygisk.
// Usado por shm_create_file() para contornar SELinux no contexto untrusted_app.
#ifdef ZYGISK_BUILD
int g_zygisk_shm_fd = -1;
#endif

// lib_main() Ã© o entry point para injeÃ§Ã£o via ptrace (libgl2.so).
// Em modo Zygisk, o entry point Ã© postAppSpecialize() em zygisk_main.cpp.
#ifndef ZYGISK_BUILD
__attribute__((constructor))
void lib_main() {
    LOGI("lib_main() LOADED [%s] pid=%d uid=%d", HOOK_BUILD_VER, getpid(), getuid());

    // hideFromMaps() DESATIVADO: causa crash por race condition.
    // mmap(MAP_FIXED) sobre r-- zera rodata durante a janela de restauracao.
    // Codigo acessando string literals nesse instante = leitura de zeros = crash.
    // O rename libgl2.so ja e suficiente contra maps scan.
    // hideFromMaps();

    // Verificar se estamos no processo do jogo
    char cmdline[256] = {0};
    FILE *f = fopen("/proc/self/cmdline", "r");
    if (f) { fread(cmdline, 1, 255, f); fclose(f); }

    LOGI("lib_main() cmdline=[%s]", cmdline);

    if (strstr(cmdline, "freefireth") == nullptr &&
        strstr(cmdline, "freefire") == nullptr &&
        strstr(cmdline, "sniper3d") == nullptr) {
        LOGI("lib_main() skipping (not game process)");
        return;
    }

    if (g_hookStarted) return;
    g_hookStarted = true;

    // Limpar arquivos de log antigos (evidÃªncia forense)
#ifndef STEALTH_DEBUG
    unlink("/data/local/tmp/.hook_log");
    unlink("/sdcard/.hook_log");
    // Limpar SHM antigo (nome mudou)
    unlink("/data/local/tmp/.esp_shm");
    unlink("/sdcard/.esp_shm");
    const char* gameLog = getGameLogPath();
    if (gameLog && gameLog[0]) unlink(gameLog);
    // Limpar game-dir .esp_shm antigo
    char oldShm[512];
    snprintf(oldShm, sizeof(oldShm), "/data/data/%s/.esp_shm", HOOK_GAME_PACKAGE);
    unlink(oldShm);
    char oldLog2[512];
    snprintf(oldLog2, sizeof(oldLog2), "/data/data/%s/.hook_log", HOOK_GAME_PACKAGE);
    unlink(oldLog2);
#endif

    LOGI("loaded (ptrace) [%s] pid=%d", HOOK_BUILD_VER, getpid());
    hookLogWrite("=== HOOK CARREGADO (ptrace) [%s] === pid=%d uid=%d", HOOK_BUILD_VER, getpid(), getuid());
    pthread_t t;
    pthread_create(&t, nullptr, hack_thread, nullptr);
    pthread_detach(t);
}
#endif // ZYGISK_BUILD

// ============================================================
// Agent_OnAttach â€” Fallback (mantido por compatibilidade)
// ============================================================
extern "C" JNIEXPORT jint JNICALL
Agent_OnAttach(JavaVM* vm, char* options, void* reserved) {
    if (g_hookStarted) return 0;
    g_hookStarted = true;

    LOGI("Agent_OnAttach [%s] pid=%d", HOOK_BUILD_VER, getpid());
    hookLogWrite("=== HOOK CARREGADO (Agent) [%s] === pid=%d uid=%d",
                 HOOK_BUILD_VER, getpid(), getuid());
    pthread_t t;
    pthread_create(&t, nullptr, hack_thread, nullptr);
    pthread_detach(t);
    return 0;
}
