/*
 * key_validator.cpp — Validação de licença JAWMODS (modo unificado)
 * ============================================================
 *
 * Roda 100% em C++ dentro do processo do Free Fire.
 * Usa AttachCurrentThread para acessar o JVM do jogo e fazer
 * requisições HTTPS sem precisar de libcurl ou outro APK.
 *
 * Fluxo:
 *   startKeyValidation()
 *     → tenta carregar key salva do disco
 *     → se válida: KeyState::VALID (menu aparece)
 *     → se não: KeyState::INPUT (DrawKeyUI mostra tela de key)
 *   submitKey(key)
 *     → KeyState::VALIDATING → thread HTTP POST /validate
 *     → se ok: salva key+expiry → KeyState::VALID
 *     → se erro: KeyState::INVALID com mensagem
 */

#include "key_validator.h"
#include "obfuscate.h"
#include "imgui.h"

#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cmath>
#include <string>
#include <sys/stat.h>
#include <android/log.h>

// ── Globals ──────────────────────────────────────────────────────────────────
std::atomic<KeyState> g_keyState{KeyState::IDLE};
char                  g_keyErrorMsg[128] = {};
long long             g_keyExpiresUnix   = 0;
JavaVM*               g_jvm              = nullptr;

// Key sendo processada (copiada de DrawKeyUI para o thread de validação)
static char g_pendingKey[64] = {};

// ── Paths ofuscados ──────────────────────────────────────────────────────────
// Arquivo de key salva dentro do data dir do jogo (sem root, processo tem acesso)
static const char* keyFilePath() {
    static char path[256] = {};
    if (!path[0]) {
        snprintf(path, sizeof(path), "/data/data/%s/.%s",
                 (char*)OBFUSCATE("com.dts.freefireth"),
                 (char*)OBFUSCATE("gl_key"));
    }
    return path;
}

// ── Persistência local da key ─────────────────────────────────────────────────
// Formato: KEY\nEXPIRY_UNIX_TS\n
static void saveKeyLocal(const char* key, long long expiresUnix) {
    int fd = open(keyFilePath(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (fd < 0) return;
    char buf[128];
    int len = snprintf(buf, sizeof(buf), "%s\n%lld\n", key, expiresUnix);
    write(fd, buf, len);
    close(fd);
}

static bool loadKeyLocal(char* outKey, int keyMaxLen, long long* outExpires) {
    int fd = open(keyFilePath(), O_RDONLY);
    if (fd < 0) return false;
    char buf[128] = {};
    int rd = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (rd <= 0) return false;
    // Parsear KEY\nEXPIRY\n
    char* nl = strchr(buf, '\n');
    if (!nl) return false;
    int keyLen = (int)(nl - buf);
    if (keyLen <= 0 || keyLen >= keyMaxLen) return false;
    strncpy(outKey, buf, keyLen);
    outKey[keyLen] = '\0';
    *outExpires = atoll(nl + 1);
    return true;
}

// ── JNI helpers ───────────────────────────────────────────────────────────────

// Obtém JNIEnv para o thread atual (attach se necessário)
static JNIEnv* getEnv(bool* attached) {
    *attached = false;
    if (!g_jvm) return nullptr;
    JNIEnv* env = nullptr;
    int res = g_jvm->GetEnv((void**)&env, JNI_VERSION_1_6);
    if (res == JNI_EDETACHED) {
        if (g_jvm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
            *attached = true;
        } else {
            return nullptr;
        }
    }
    return env;
}

// Converte jstring → std::string
static std::string jstr2str(JNIEnv* env, jstring js) {
    if (!js) return "";
    const char* cstr = env->GetStringUTFChars(js, nullptr);
    std::string s = cstr ? cstr : "";
    if (cstr) env->ReleaseStringUTFChars(js, cstr);
    return s;
}


static jobject getAppContext(JNIEnv* env) {
    jclass clsAT = env->FindClass("android/app/ActivityThread");
    if (!clsAT) return nullptr;
    jmethodID midCAT = env->GetStaticMethodID(clsAT, "currentActivityThread",
                                               "()Landroid/app/ActivityThread;");
    if (!midCAT) { env->DeleteLocalRef(clsAT); return nullptr; }
    jobject at = env->CallStaticObjectMethod(clsAT, midCAT);
    if (!at) { env->DeleteLocalRef(clsAT); return nullptr; }
    jmethodID midGetApp = env->GetMethodID(clsAT, "getApplication",
                                            "()Landroid/app/Application;");
    env->DeleteLocalRef(clsAT);
    if (!midGetApp) return nullptr;
    return env->CallObjectMethod(at, midGetApp);
}

// Obtém android_id via Settings.Secure.getString
static std::string getAndroidId(JNIEnv* env) {
    jobject ctx = getAppContext(env);
    if (!ctx) return "unknown";
    jclass clsCtx = env->FindClass("android/content/Context");
    jmethodID midGetCR = env->GetMethodID(clsCtx, "getContentResolver",
                                           "()Landroid/content/ContentResolver;");
    jobject cr = env->CallObjectMethod(ctx, midGetCR);
    env->DeleteLocalRef(clsCtx);

    jclass clsSec = env->FindClass("android/provider/Settings$Secure");
    jmethodID midGetStr = env->GetStaticMethodID(
        clsSec, "getString",
        "(Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/lang/String;");
    jstring keyName = env->NewStringUTF("android_id");
    jstring result = (jstring)env->CallStaticObjectMethod(clsSec, midGetStr, cr, keyName);
    env->DeleteLocalRef(keyName);
    env->DeleteLocalRef(clsSec);
    return jstr2str(env, result);
}

// Obtém Build.MODEL
static std::string getDeviceModel(JNIEnv* env) {
    jclass clsBuild = env->FindClass("android/os/Build");
    jfieldID fidModel = env->GetStaticFieldID(clsBuild, "MODEL", "Ljava/lang/String;");
    jstring jModel = (jstring)env->GetStaticObjectField(clsBuild, fidModel);
    env->DeleteLocalRef(clsBuild);
    return jstr2str(env, jModel);
}

// HTTP POST via java.net.HttpURLConnection (HTTPS suportado pelo JVM do jogo)

// Lê texto do clipboard via ClipboardManager JNI
static std::string getClipboardText() {
    bool attached = false;
    JNIEnv* env = getEnv(&attached);
    if (!env) return "";

    std::string result;
    jobject ctx = getAppContext(env);
    if (!ctx) { if (attached) g_jvm->DetachCurrentThread(); return ""; }

    // ctx.getSystemService("clipboard")
    jclass clsCtx = env->FindClass("android/content/Context");
    jmethodID midGetSys = env->GetMethodID(clsCtx, "getSystemService",
                                            "(Ljava/lang/String;)Ljava/lang/Object;");
    jstring jSvcName = env->NewStringUTF("clipboard");
    jobject clipMgr = env->CallObjectMethod(ctx, midGetSys, jSvcName);
    env->DeleteLocalRef(jSvcName);
    env->DeleteLocalRef(clsCtx);

    if (!clipMgr || env->ExceptionCheck()) {
        env->ExceptionClear();
        if (attached) g_jvm->DetachCurrentThread();
        return "";
    }

    // clipMgr.getPrimaryClip()
    jclass clsClipMgr = env->GetObjectClass(clipMgr);
    jmethodID midGetPrimary = env->GetMethodID(clsClipMgr, "getPrimaryClip",
                                                "()Landroid/content/ClipData;");
    jobject clipData = env->CallObjectMethod(clipMgr, midGetPrimary);
    env->DeleteLocalRef(clsClipMgr);

    if (!clipData || env->ExceptionCheck()) {
        env->ExceptionClear();
        if (attached) g_jvm->DetachCurrentThread();
        return "";
    }

    // clipData.getItemAt(0)
    jclass clsClipData = env->GetObjectClass(clipData);
    jmethodID midGetItem = env->GetMethodID(clsClipData, "getItemAt",
                                             "(I)Landroid/content/ClipData$Item;");
    jobject item = env->CallObjectMethod(clipData, midGetItem, (jint)0);
    env->DeleteLocalRef(clsClipData);

    if (!item || env->ExceptionCheck()) {
        env->ExceptionClear();
        if (attached) g_jvm->DetachCurrentThread();
        return "";
    }

    // item.getText().toString()
    jclass clsItem = env->GetObjectClass(item);
    jmethodID midGetText = env->GetMethodID(clsItem, "getText",
                                             "()Ljava/lang/CharSequence;");
    jobject charSeq = env->CallObjectMethod(item, midGetText);
    env->DeleteLocalRef(clsItem);

    if (!charSeq || env->ExceptionCheck()) {
        env->ExceptionClear();
        if (attached) g_jvm->DetachCurrentThread();
        return "";
    }

    jclass clsCS = env->GetObjectClass(charSeq);
    jmethodID midToStr = env->GetMethodID(clsCS, "toString", "()Ljava/lang/String;");
    jstring jText = (jstring)env->CallObjectMethod(charSeq, midToStr);
    env->DeleteLocalRef(clsCS);
    env->DeleteLocalRef(charSeq);

    if (jText && !env->ExceptionCheck()) {
        result = jstr2str(env, jText);
    } else {
        env->ExceptionClear();
    }

    if (attached) g_jvm->DetachCurrentThread();
    return result;
}
 da Application do jogo via ActivityThread reflection

// Lê o texto do clipboard do Android via ClipboardManager
// Funciona pois estamos no processo principal do FF (foreground)

static std::string httpPost(JNIEnv* env, const char* url, const char* jsonBody) {
    std::string result;

    // java.net.URL url = new URL(urlStr)
    jclass clsURL = env->FindClass("java/net/URL");
    jmethodID midURLInit = env->GetMethodID(clsURL, "<init>", "(Ljava/lang/String;)V");
    jstring jUrl = env->NewStringUTF(url);
    jobject objURL = env->NewObject(clsURL, midURLInit, jUrl);
    env->DeleteLocalRef(jUrl);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return ""; }

    // HttpURLConnection conn = (HttpURLConnection) url.openConnection()
    jmethodID midOpenConn = env->GetMethodID(clsURL, "openConnection",
                                              "()Ljava/net/URLConnection;");
    jobject objConn = env->CallObjectMethod(objURL, midOpenConn);
    env->DeleteLocalRef(objURL);
    if (!objConn || env->ExceptionCheck()) { env->ExceptionClear(); return ""; }
    jclass clsConn = env->GetObjectClass(objConn);

    // Configurar método e headers
    jmethodID midSetMethod = env->GetMethodID(clsConn, "setRequestMethod",
                                               "(Ljava/lang/String;)V");
    jstring jPost = env->NewStringUTF("POST");
    env->CallVoidMethod(objConn, midSetMethod, jPost);
    env->DeleteLocalRef(jPost);

    jmethodID midSetProp = env->GetMethodID(clsConn, "setRequestProperty",
                                             "(Ljava/lang/String;Ljava/lang/String;)V");
    jstring jCT = env->NewStringUTF("Content-Type");
    jstring jCTV = env->NewStringUTF("application/json");
    env->CallVoidMethod(objConn, midSetProp, jCT, jCTV);
    env->DeleteLocalRef(jCT); env->DeleteLocalRef(jCTV);

    jstring jUA = env->NewStringUTF("User-Agent");
    jstring jUAV = env->NewStringUTF("Mozilla/5.0");
    env->CallVoidMethod(objConn, midSetProp, jUA, jUAV);
    env->DeleteLocalRef(jUA); env->DeleteLocalRef(jUAV);

    // Timeouts
    jmethodID midSetConn = env->GetMethodID(clsConn, "setConnectTimeout", "(I)V");
    jmethodID midSetRead = env->GetMethodID(clsConn, "setReadTimeout", "(I)V");
    env->CallVoidMethod(objConn, midSetConn, (jint)8000);
    env->CallVoidMethod(objConn, midSetRead, (jint)8000);

    // setDoOutput(true)
    jmethodID midSetDO = env->GetMethodID(clsConn, "setDoOutput", "(Z)V");
    env->CallVoidMethod(objConn, midSetDO, JNI_TRUE);

    // Escrever body
    jmethodID midGetOut = env->GetMethodID(clsConn, "getOutputStream",
                                            "()Ljava/io/OutputStream;");
    jobject objOs = env->CallObjectMethod(objConn, midGetOut);
    if (objOs && !env->ExceptionCheck()) {
        jclass clsOs = env->GetObjectClass(objOs);
        jmethodID midWrite = env->GetMethodID(clsOs, "write", "([B)V");
        int bodyLen = (int)strlen(jsonBody);
        jbyteArray ba = env->NewByteArray(bodyLen);
        env->SetByteArrayRegion(ba, 0, bodyLen, (const jbyte*)jsonBody);
        env->CallVoidMethod(objOs, midWrite, ba);
        env->DeleteLocalRef(ba);
        // os.close()
        jmethodID midClose = env->GetMethodID(clsOs, "close", "()V");
        env->CallVoidMethod(objOs, midClose);
        env->DeleteLocalRef(clsOs);
        env->DeleteLocalRef(objOs);
    }
    if (env->ExceptionCheck()) { env->ExceptionClear(); return ""; }

    // Ler response
    jmethodID midGetIn = env->GetMethodID(clsConn, "getInputStream",
                                           "()Ljava/io/InputStream;");
    jobject objIs = env->CallObjectMethod(objConn, midGetIn);
    if (!objIs || env->ExceptionCheck()) {
        env->ExceptionClear();
        // Try error stream
        jmethodID midGetErr = env->GetMethodID(clsConn, "getErrorStream",
                                                "()Ljava/io/InputStream;");
        objIs = env->CallObjectMethod(objConn, midGetErr);
        if (!objIs || env->ExceptionCheck()) {
            env->ExceptionClear();
            env->DeleteLocalRef(clsConn);
            env->DeleteLocalRef(objConn);
            return "";
        }
    }

    // BufferedReader para ler linha a linha
    jclass clsISR = env->FindClass("java/io/InputStreamReader");
    jmethodID midISRInit = env->GetMethodID(clsISR, "<init>",
                                             "(Ljava/io/InputStream;)V");
    jobject objISR = env->NewObject(clsISR, midISRInit, objIs);
    env->DeleteLocalRef(clsISR);
    env->DeleteLocalRef(objIs);

    jclass clsBR = env->FindClass("java/io/BufferedReader");
    jmethodID midBRInit = env->GetMethodID(clsBR, "<init>",
                                            "(Ljava/io/Reader;)V");
    jobject objBR = env->NewObject(clsBR, midBRInit, objISR);
    env->DeleteLocalRef(objISR);

    jmethodID midRL = env->GetMethodID(clsBR, "readLine", "()Ljava/lang/String;");
    while (true) {
        jstring line = (jstring)env->CallObjectMethod(objBR, midRL);
        if (!line || env->ExceptionCheck()) { env->ExceptionClear(); break; }
        result += jstr2str(env, line);
        env->DeleteLocalRef(line);
    }
    // close BufferedReader
    jmethodID midClose = env->GetMethodID(clsBR, "close", "()V");
    env->CallVoidMethod(objBR, midClose);
    env->DeleteLocalRef(clsBR);
    env->DeleteLocalRef(objBR);

    // conn.disconnect()
    jmethodID midDisconn = env->GetMethodID(clsConn, "disconnect", "()V");
    env->CallVoidMethod(objConn, midDisconn);
    env->DeleteLocalRef(clsConn);
    env->DeleteLocalRef(objConn);

    if (env->ExceptionCheck()) env->ExceptionClear();
    return result;
}

// Parseia campo string de JSON simples: "key":"value"
static std::string jsonStr(const std::string& json, const char* key) {
    std::string search = std::string("\"") + key + "\":\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) {
        search = std::string("\"") + key + "\": \"";
        pos = json.find(search);
    }
    if (pos == std::string::npos) return "";
    size_t start = pos + search.size();
    size_t end = json.find('"', start);
    if (end == std::string::npos) return "";
    return json.substr(start, end - start);
}

// Parseia campo long de JSON simples: "key":1234
static long long jsonLong(const std::string& json, const char* key) {
    std::string search = std::string("\"") + key + "\":";
    size_t pos = json.find(search);
    if (pos == std::string::npos) {
        search = std::string("\"") + key + "\": ";
        pos = json.find(search);
    }
    if (pos == std::string::npos) return 0;
    size_t start = pos + search.size();
    while (start < json.size() && json[start] == ' ') start++;
    return atoll(json.c_str() + start);
}

// ── Thread de validação HTTP ──────────────────────────────────────────────────
struct ValidateArgs {
    char key[64];
};

static void* validateThread(void* arg) {
    ValidateArgs* va = (ValidateArgs*)arg;
    char key[64];
    strncpy(key, va->key, sizeof(key) - 1);
    delete va;

    bool attached = false;
    JNIEnv* env = getEnv(&attached);
    if (!env) {
        snprintf(g_keyErrorMsg, sizeof(g_keyErrorMsg), "JVM indisponivel");
        g_keyState.store(KeyState::INVALID);
        return nullptr;
    }

    std::string androidId   = getAndroidId(env);
    std::string deviceModel = getDeviceModel(env);

    // Montar JSON
    char jsonBody[256];
    snprintf(jsonBody, sizeof(jsonBody),
             "{\"key\":\"%s\",\"android_id\":\"%s\",\"device_model\":\"%s\"}",
             key, androidId.c_str(), deviceModel.c_str());

    // URL do servidor (ofuscada)
    char serverUrl[128];
    snprintf(serverUrl, sizeof(serverUrl), "https://%s/validate",
             (char*)OBFUSCATE("jawmods.squareweb.app"));

    std::string resp = httpPost(env, serverUrl, jsonBody);

    if (attached) g_jvm->DetachCurrentThread();

    if (resp.empty()) {
        snprintf(g_keyErrorMsg, sizeof(g_keyErrorMsg), "Sem conexao com servidor");
        g_keyState.store(KeyState::NO_NETWORK);
        return nullptr;
    }

    bool valid = (resp.find("\"valid\":true") != std::string::npos ||
                  resp.find("\"valid\": true") != std::string::npos);

    if (valid) {
        long long remaining = jsonLong(resp, "remaining_seconds");
        long long expiresUnix = (long long)time(nullptr) + remaining;
        g_keyExpiresUnix = expiresUnix;
        saveKeyLocal(key, expiresUnix);
        g_keyState.store(KeyState::VALID);
    } else {
        std::string errMsg = jsonStr(resp, "error");
        if (errMsg.empty()) errMsg = "Key invalida";
        snprintf(g_keyErrorMsg, sizeof(g_keyErrorMsg), "%s", errMsg.c_str());
        g_keyState.store(KeyState::INVALID);
    }

    return nullptr;
}

// ── API pública ───────────────────────────────────────────────────────────────

void submitKey(const char* key) {
    if (g_keyState.load() == KeyState::VALIDATING) return;
    g_keyState.store(KeyState::VALIDATING);
    g_keyErrorMsg[0] = '\0';
    strncpy(g_pendingKey, key, sizeof(g_pendingKey) - 1);

    ValidateArgs* va = new ValidateArgs();
    strncpy(va->key, key, sizeof(va->key) - 1);
    pthread_t t;
    pthread_create(&t, nullptr, validateThread, va);
    pthread_detach(t);
}

// Obtém path do diretório externo da app via JNI
// Retorna ex: /sdcard/Android/data/com.dts.freefireth/files
// Garantidamente acessível pelo processo do jogo em todas as versões do Android
static std::string getExternalFilesDir() {
    bool attached = false;
    JNIEnv* env = getEnv(&attached);
    if (!env) return "";

    std::string result;
    jobject ctx = getAppContext(env);
    if (ctx) {
        jclass clsCtx = env->FindClass("android/content/Context");
        if (clsCtx) {
            jmethodID midGetExt = env->GetMethodID(clsCtx, "getExternalFilesDir",
                                                    "(Ljava/lang/String;)Ljava/io/File;");
            if (midGetExt) {
                jobject fileObj = env->CallObjectMethod(ctx, midGetExt, nullptr);
                if (fileObj && !env->ExceptionCheck()) {
                    jclass clsFile = env->GetObjectClass(fileObj);
                    jmethodID midGetPath = env->GetMethodID(clsFile, "getAbsolutePath",
                                                             "()Ljava/lang/String;");
                    jstring jPath = (jstring)env->CallObjectMethod(fileObj, midGetPath);
                    result = jstr2str(env, jPath);
                    env->DeleteLocalRef(fileObj);
                    env->DeleteLocalRef(clsFile);
                }
            }
            env->DeleteLocalRef(clsCtx);
        }
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (attached) g_jvm->DetachCurrentThread();
    return result;
}

// Cache do external files dir (evita chamar JNI todo frame)
static std::string g_externalFilesDir;
static bool        g_externalFilesDirLoaded = false;

static const std::string& cachedExternalFilesDir() {
    if (!g_externalFilesDirLoaded && g_jvm) {
        g_externalFilesDir = getExternalFilesDir();
        if (!g_externalFilesDir.empty()) g_externalFilesDirLoaded = true;
    }
    return g_externalFilesDir;
}

// Tenta ler uma linha limpa de um arquivo (remove newline/espaços, uppercase)
static bool tryReadKeyFile(const char* path, char* outKey, int maxLen) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    char buf[64] = {};
    int rd = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (rd <= 0) return false;
    int len = 0;
    for (int i = 0; i < rd && len < maxLen - 1; i++) {
        char c = buf[i];
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
        if (c >= 'a' && c <= 'z') c -= 32;
        outKey[len++] = c;
    }
    outKey[len] = '\0';
    return len >= 10;
}

// Lê a key de arquivo externo (colocado pelo usuário)
// Paths em ordem de prioridade:
//   1. /sdcard/Android/data/com.dts.freefireth/files/jawm.key  (GARANTIDO - via JNI)
//   2. /sdcard/Download/jawm.key
//   3. /sdcard/jawm.key
//   4. /storage/emulated/0/jawm.key
static bool readKeyFromFile(char* outKey, int maxLen) {
    // 1. Path do external files dir da app (acessível sem root em qualquer Android)
    const std::string& extDir = cachedExternalFilesDir();
    if (!extDir.empty()) {
        char path[512];
        snprintf(path, sizeof(path), "%s/jawm.key", extDir.c_str());
        if (tryReadKeyFile(path, outKey, maxLen)) return true;
    }

    // 2-4. Fallbacks para caminhos comuns
    char dl[256], sdcard[256], emu[256];
    snprintf(dl,     sizeof(dl),     "/sdcard/Download/jawm.key");
    snprintf(sdcard, sizeof(sdcard), "/sdcard/jawm.key");
    snprintf(emu,    sizeof(emu),    "/storage/emulated/0/jawm.key");

    if (tryReadKeyFile(dl,     outKey, maxLen)) return true;
    if (tryReadKeyFile(sdcard, outKey, maxLen)) return true;
    if (tryReadKeyFile(emu,    outKey, maxLen)) return true;

    return false;
}

void startKeyValidation() {
    // 1. Tentar carregar key salva localmente (cache)
    char savedKey[64] = {};
    long long expiresUnix = 0;
    if (loadKeyLocal(savedKey, sizeof(savedKey), &expiresUnix)) {
        long long now = (long long)time(nullptr);
        if (expiresUnix > now + 60) { // válida por mais de 1 minuto
            g_keyExpiresUnix = expiresUnix;
            g_keyState.store(KeyState::VALID);
            // Revalidação silenciosa no background (verificar revogação)
            ValidateArgs* va = new ValidateArgs();
            strncpy(va->key, savedKey, sizeof(va->key) - 1);
            pthread_t t;
            pthread_create(&t, nullptr, validateThread, va);
            pthread_detach(t);
            return;
        }
    }

    // 2. Tentar ler key de arquivo no sdcard (ativação sem teclado)
    char fileKey[64] = {};
    if (readKeyFromFile(fileKey, sizeof(fileKey))) {
        // Arquivo encontrado: validar automaticamente
        submitKey(fileKey);
        return;
    }

    // 3. Nenhuma key: aguardar UI (usuário coloca arquivo no sdcard)
    g_keyState.store(KeyState::INPUT);
}

// Verifica periodicamente se o arquivo de key foi criado (polling a cada 2s)
static void checkKeyFile() {
    static long long lastCheck = 0;
    long long now = (long long)time(nullptr);
    if (now - lastCheck < 2) return;
    lastCheck = now;

    KeyState st = g_keyState.load();
    if (st == KeyState::VALIDATING || st == KeyState::VALID) return;

    char fileKey[64] = {};
    if (readKeyFromFile(fileKey, sizeof(fileKey))) {
        submitKey(fileKey);
    }
}

// ── ImGui UI da key ───────────────────────────────────────────────────────────
// Ativação via arquivo: usuario cria /sdcard/jawm.key com a key
// O mod detecta automaticamente a cada 2 segundos sem precisar de teclado.

void DrawKeyUI() {
    ImGuiIO& io = ImGui::GetIO();
    float W = io.DisplaySize.x;
    float H = io.DisplaySize.y;

    // Verificar arquivo de key periodicamente
    checkKeyFile();

    // Fundo escuro
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    bg->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), IM_COL32(4, 4, 8, 230));

    // Painel central
    float panW = W * 0.88f;
    if (panW > 460.0f) panW = 460.0f;
    float panH = 360.0f;
    float panX = (W - panW) * 0.5f;
    float panY = (H - panH) * 0.5f;

    // Borda animada
    static float phase = 0.0f;
    phase += io.DeltaTime * 2.0f;
    float glow = 0.5f + 0.5f * sinf(phase);
    ImU32 borderCol = IM_COL32((int)(80 + 50*glow), (int)(30 + 20*glow),
                                (int)(200 + 55*glow), 255);

    bg->AddRectFilled(ImVec2(panX, panY), ImVec2(panX+panW, panY+panH),
                      IM_COL32(10, 11, 18, 255), 14.0f);
    bg->AddRect(ImVec2(panX, panY), ImVec2(panX+panW, panY+panH),
                borderCol, 14.0f, 0, 1.8f);
    bg->AddRectFilled(ImVec2(panX+20, panY+1), ImVec2(panX+panW*0.55f, panY+3.0f),
                      borderCol);

    ImGui::SetNextWindowPos(ImVec2(panX, panY), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(panW, panH), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(26.0f, 22.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 11.0f));
    ImGui::Begin("##keyui", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoCollapse);

    // Logo
    ImGui::SetCursorPosY(18.0f);
    ImGui::TextColored(ImVec4(0.51f, 0.31f, 1.0f, 1.0f), "JAW");
    ImGui::SameLine(0, 4);
    ImGui::TextColored(ImVec4(0.82f, 0.83f, 0.86f, 1.0f), "MODS");
    ImGui::SameLine(0, 8);
    ImGui::TextColored(ImVec4(0.39f, 0.41f, 0.47f, 1.0f), "Ativacao de Licenca");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    KeyState st = g_keyState.load();

    if (st == KeyState::VALID) {
        // Key validada com sucesso
        long long rem = g_keyExpiresUnix - (long long)time(nullptr);
        int days = (int)(rem / 86400);
        ImGui::TextColored(ImVec4(0.0f, 0.90f, 0.50f, 1.0f), "  KEY ATIVADA!");
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                           "  %d dias restantes", days);

    } else if (st == KeyState::VALIDATING) {
        // Aguardando servidor
        static float dots = 0.0f;
        dots += io.DeltaTime * 3.0f;
        int d = (int)dots % 4;
        char dotStr[8] = {};
        for (int i = 0; i < d; i++) dotStr[i] = '.';
        ImGui::TextColored(ImVec4(0.9f, 0.75f, 0.1f, 1.0f),
                           "  Verificando%s", dotStr);
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.55f, 0.57f, 0.64f, 1.0f),
                           "  Conectando ao servidor...");

    } else {
        // Instruções de ativação via arquivo
        ImGui::TextColored(ImVec4(0.9f, 0.75f, 0.1f, 1.0f),
                           "  Como ativar:");
        ImGui::Spacing();

        // ── Botão COLAR (método principal: clipboard) ──
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.10f, 0.36f, 0.10f, 0.90f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.15f, 0.55f, 0.15f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.20f, 0.70f, 0.20f, 1.00f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 7.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 11.0f));
        if (ImGui::Button("  COLAR KEY DO CLIPBOARD  ", ImVec2(panW - 52.0f, 0.0f))) {
            std::string clip = getClipboardText();
            // Limpar e deixar só uppercase sem espaços
            std::string key;
            for (char c : clip) {
                if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
                if (c >= 'a' && c <= 'z') c -= 32;
                key += c;
            }
            if (key.size() >= 10) {
                submitKey(key.c_str());
            }
        }
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(3);

        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.39f, 0.41f, 0.47f, 1.0f),
                           "  Copie sua key e toque no botao acima.");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // ── Alternativa: arquivo no sdcard ──
        ImGui::TextColored(ImVec4(0.55f, 0.57f, 0.64f, 1.0f),
                           "  Ou coloque o arquivo jawm.key em:");
        {
            const std::string& extDir2 = cachedExternalFilesDir();
            if (!extDir2.empty()) {
                char keyPath[512];
                snprintf(keyPath, sizeof(keyPath), "%s/jawm.key", extDir2.c_str());
                ImGui::TextColored(ImVec4(0.0f, 0.65f, 0.85f, 1.0f), "  %s", keyPath);
            } else {
                ImGui::TextColored(ImVec4(0.0f, 0.65f, 0.85f, 1.0f),
                                   "  /sdcard/Download/jawm.key");
            }
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Passo 1 (dica compacta)
        ImGui::TextColored(ImVec4(0.51f, 0.31f, 1.0f, 1.0f), "  1.");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.82f, 0.83f, 0.86f, 1.0f),
                           "Crie um arquivo de texto no celular");

        // Passo 2
        ImGui::TextColored(ImVec4(0.51f, 0.31f, 1.0f, 1.0f), "  2.");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.82f, 0.83f, 0.86f, 1.0f),
                           "Salve como:");
        // Mostrar o caminho real obtido via JNI (mais confiável)
        {
            const std::string& extDir3 = cachedExternalFilesDir();
            if (!extDir3.empty()) {
                char keyPath[512];
                snprintf(keyPath, sizeof(keyPath), "%s/jawm.key", extDir3.c_str());
                ImGui::TextColored(ImVec4(0.0f, 0.80f, 1.0f, 1.0f),
                                   "  %s", keyPath);
            } else {
                ImGui::TextColored(ImVec4(0.0f, 0.80f, 1.0f, 1.0f),
                                   "  /sdcard/Download/jawm.key");
            }
        }

        // Passo 3
        ImGui::TextColored(ImVec4(0.51f, 0.31f, 1.0f, 1.0f), "  3.");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.82f, 0.83f, 0.86f, 1.0f),
                           "Cole sua key dentro do arquivo:");
        ImGui::TextColored(ImVec4(0.0f, 0.80f, 1.0f, 1.0f),
                           "       JAWM-XXXX-XXXX-XXXX");

        // Passo 4
        ImGui::TextColored(ImVec4(0.51f, 0.31f, 1.0f, 1.0f), "  4.");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.82f, 0.83f, 0.86f, 1.0f),
                           "O mod ativa automaticamente!");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Polling indicator
        static float scanAnim = 0.0f;
        scanAnim += io.DeltaTime * 1.5f;
        int dot = (int)scanAnim % 4;
        char dotStr[8] = {};
        for (int i = 0; i < dot; i++) dotStr[i] = '.';
        ImGui::TextColored(ImVec4(0.39f, 0.41f, 0.47f, 1.0f),
                           "  Aguardando arquivo%s", dotStr);

        // Erro anterior
        if ((st == KeyState::INVALID || st == KeyState::NO_NETWORK) && g_keyErrorMsg[0]) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                               "  Erro: %s", g_keyErrorMsg);
            ImGui::TextColored(ImVec4(0.55f, 0.57f, 0.64f, 1.0f),
                               "  Coloque o arquivo novamente para tentar.");
        }
    }

    ImGui::End();
    ImGui::PopStyleVar(2);
}
