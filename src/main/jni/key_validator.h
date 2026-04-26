#pragma once
/*
 * key_validator.h — Validação de licença em C++ puro (sem Java)
 *
 * Usa JNI para acessar java.net.HttpURLConnection dentro do processo do jogo.
 * Funciona no modo UNIFIED_BUILD: toda a validação acontece dentro da libgl2.so
 * sem depender do overlay APK externo.
 *
 * API do servidor (/validate):
 *   POST {"key":"JAWM-XXXX-XXXX-XXXX","android_id":"...","device_model":"..."}
 *   Resp {"valid":true,"remaining_seconds":N,"expires_at":"...","note":"..."}
 *        {"valid":false,"error":"..."}
 */

#include <jni.h>
#include <atomic>
#include <cstdint>

// ── Estado global da validação ───────────────────────────────────────────────

enum class KeyState {
    IDLE,           // Ainda não começou
    INPUT,          // Aguardando usuário digitar a key
    VALIDATING,     // Requisição em andamento
    VALID,          // Key válida e ativa
    INVALID,        // Key inválida ou expirada
    NO_NETWORK,     // Sem internet
};

extern std::atomic<KeyState> g_keyState;
extern char                  g_keyErrorMsg[128];    // mensagem de erro/status
extern long long             g_keyExpiresUnix;      // timestamp unix de expiração

// JavaVM salvo em JNI_OnLoad — necessário para HTTP do background thread
extern JavaVM* g_jvm;

// ── API pública ──────────────────────────────────────────────────────────────

// Inicia thread de validação (lê key salva ou entra em modo INPUT)
void startKeyValidation();

// Valida uma key string (chamado da UI quando usuário clica em Validar)
void submitKey(const char* key);

// Desenha a UI de key usando ImGui (fullscreen antes do menu aparecer)
void DrawKeyUI();

// Retorna true se key está válida e ativa
inline bool isKeyValid() {
    return g_keyState.load() == KeyState::VALID;
}
