/*
 * key_validator.cpp — Validação de licença JAWMODS (modo unificado)
 * ============================================================
 * Roda 100% em C++ dentro do processo do Free Fire.
 * Usa AttachCurrentThread para acessar o JVM do jogo e fazer
 * requisições HTTPS sem precisar de libcurl ou outro APK.
 *
 * Ativação do cliente:
 *   1. Copiar a key (JAWM-XXXX-XXXX-XXXX) e tocar em "COLAR KEY"
 *   2. Ou criar arquivo jawm.key no diretório externo da app
 * ============================================================
 */

#include "key_validator.h"
#include "obfuscate.h"
#include "imgui.h"

#include <jni.h>
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

// ── Globals definidos aqui, declarados extern em key_validator.h ──────────────
std::atomic<KeyState> g_keyState{KeyState::IDLE};
char                  g_keyErrorMsg[128] = {};
long long             g_keyExpiresUnix   = 0;
JavaVM*               g_jvm              = nullptr;

static char g_pendingKey[64] = {};

// ── Persistência local ────────────────────────────────────────────────────────
// Arquivo salvo no data dir do jogo (processo tem acesso garantido)
// Formato: KEY\nEXPIRY_UNIX_TIMESTAMP\n

static const char* keyFilePath() {
    static char path[256] = {};
    if (!path[0]) {
        snprintf(path, sizeof(path), "/data/data/%s/.%s",
                 (char*)OBFUSCATE("com.dts.freefireth"),
                 (char*)OBFUSCATE("gl_key"));
    }
    return path;
}

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
    char* nl = strchr(buf, '\n');
    if (!nl) return false;
    int keyLen = (int)(nl - buf);
    if (keyLen <= 0 || keyLen >= keyMaxLen) return false;
    strncpy(outKey, buf, keyLen);
    outKey[keyLen] = '\0';
    *outExpires = atoll(nl + 1);
    return true;
}

// ── JNI helpers (ordem de dependência: getEnv → jstr2str → getAppContext → rest) ──

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

static std::string jstr2str(JNIEnv* env, jstring js) {
    if (!js) return "";
    const char* cstr = env->GetStringUTFChars(js, nullptr);
    std::string s = cstr ? cstr : "";
    if (cstr) env->ReleaseStringUTFChars(js, cstr);
    return s;
}

// Obtém o Application context do jogo via ActivityThread reflection
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

// Obtém android_id via Settings.Secure
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
    if (env->ExceptionCheck()) { env->ExceptionClear(); return "unknown"; }
    return jstr2str(env, result);
}

// Obtém Build.MODEL
static std::string getDeviceModel(JNIEnv* env) {
    jclass clsBuild = env->FindClass("android/os/Build");
    if (!clsBuild) return "unknown";
    jfieldID fidModel = env->GetStaticFieldID(clsBuild, "MODEL", "Ljava/lang/String;");
    jstring jModel = (jstring)env->GetStaticObjectField(clsBuild, fidModel);
    env->DeleteLocalRef(clsBuild);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return "unknown"; }
    return jstr2str(env, jModel);
}

// Lê texto do clipboard via ClipboardManager
static std::string getClipboardText() {
    bool attached = false;
    JNIEnv* env = getEnv(&attached);
    if (!env) return "";

    std::string result;
    jobject ctx = getAppContext(env);
    if (!ctx) { if (attached) g_jvm->DetachCurrentThread(); return ""; }

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

    jclass clsClipMgr = env->GetObjectClass(clipMgr);
    jmethodID midGetPrimary = env->GetMethodID(clsClipMgr, "getPrimaryClip",
                                                "()Landroid/content/ClipData;");
    jobject clipData = env->CallObjectMethod(clipMgr, midGetPrimary);
    env->DeleteLocalRef(clsClipMgr);
    env->DeleteLocalRef(clipMgr);

    if (!clipData || env->ExceptionCheck()) {
        env->ExceptionClear();
        if (attached) g_jvm->DetachCurrentThread();
        return "";
    }

    jclass clsClipData = env->GetObjectClass(clipData);
    jmethodID midGetItem = env->GetMethodID(clsClipData, "getItemAt",
                                             "(I)Landroid/content/ClipData$Item;");
    jobject item = env->CallObjectMethod(clipData, midGetItem, (jint)0);
    env->DeleteLocalRef(clsClipData);
    env->DeleteLocalRef(clipData);

    if (!item || env->ExceptionCheck()) {
        env->ExceptionClear();
        if (attached) g_jvm->DetachCurrentThread();
        return "";
    }

    jclass clsItem = env->GetObjectClass(item);
    jmethodID midGetText = env->GetMethodID(clsItem, "getText",
                                             "()Ljava/lang/CharSequence;");
    jobject charSeq = env->CallObjectMethod(item, midGetText);
    env->DeleteLocalRef(clsItem);
    env->DeleteLocalRef(item);

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

// Obtém o external files dir da app via JNI (ex: /sdcard/Android/data/com.dts.freefireth/files)
// Acessível em qualquer Android sem root
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

// Cache do external files dir
static std::string g_externalFilesDir;
static bool        g_externalFilesDirLoaded = false;

static const std::string& cachedExternalFilesDir() {
    if (!g_externalFilesDirLoaded && g_jvm) {
        g_externalFilesDir = getExternalFilesDir();
        if (!g_externalFilesDir.empty()) g_externalFilesDirLoaded = true;
    }
    return g_externalFilesDir;
}

// ── HTTP via JNI ──────────────────────────────────────────────────────────────

// HTTP POST via java.net.HttpURLConnection (HTTPS suportado pelo JVM do jogo)
static std::string httpPost(JNIEnv* env, const char* url, const char* jsonBody) {
    std::string result;

    jclass clsURL = env->FindClass("java/net/URL");
    if (!clsURL) return "";
    jmethodID midURLInit = env->GetMethodID(clsURL, "<init>", "(Ljava/lang/String;)V");
    jstring jUrl = env->NewStringUTF(url);
    jobject objURL = env->NewObject(clsURL, midURLInit, jUrl);
    env->DeleteLocalRef(jUrl);
    if (!objURL || env->ExceptionCheck()) { env->ExceptionClear(); return ""; }

    jmethodID midOpenConn = env->GetMethodID(clsURL, "openConnection",
                                              "()Ljava/net/URLConnection;");
    jobject objConn = env->CallObjectMethod(objURL, midOpenConn);
    env->DeleteLocalRef(objURL);
    if (!objConn || env->ExceptionCheck()) { env->ExceptionClear(); return ""; }
    jclass clsConn = env->GetObjectClass(objConn);

    jmethodID midSetMethod = env->GetMethodID(clsConn, "setRequestMethod",
                                               "(Ljava/lang/String;)V");
    jstring jPost = env->NewStringUTF("POST");
    env->CallVoidMethod(objConn, midSetMethod, jPost);
    env->DeleteLocalRef(jPost);

    jmethodID midSetProp = env->GetMethodID(clsConn, "setRequestProperty",
                                             "(Ljava/lang/String;Ljava/lang/String;)V");
    { jstring a = env->NewStringUTF("Content-Type"), b = env->NewStringUTF("application/json");
      env->CallVoidMethod(objConn, midSetProp, a, b);
      env->DeleteLocalRef(a); env->DeleteLocalRef(b); }
    { jstring a = env->NewStringUTF("User-Agent"), b = env->NewStringUTF("Mozilla/5.0");
      env->CallVoidMethod(objConn, midSetProp, a, b);
      env->DeleteLocalRef(a); env->DeleteLocalRef(b); }

    env->CallVoidMethod(objConn,
        env->GetMethodID(clsConn, "setConnectTimeout", "(I)V"), (jint)8000);
    env->CallVoidMethod(objConn,
        env->GetMethodID(clsConn, "setReadTimeout", "(I)V"), (jint)8000);
    env->CallVoidMethod(objConn,
        env->GetMethodID(clsConn, "setDoOutput", "(Z)V"), JNI_TRUE);

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
        env->CallVoidMethod(objOs, env->GetMethodID(clsOs, "close", "()V"));
        env->DeleteLocalRef(clsOs);
        env->DeleteLocalRef(objOs);
    }
    if (env->ExceptionCheck()) { env->ExceptionClear(); return ""; }

    jmethodID midGetIn = env->GetMethodID(clsConn, "getInputStream",
                                           "()Ljava/io/InputStream;");
    jobject objIs = env->CallObjectMethod(objConn, midGetIn);
    if (!objIs || env->ExceptionCheck()) {
        env->ExceptionClear();
        jmethodID midGetErr = env->GetMethodID(clsConn, "getErrorStream",
                                                "()Ljava/io/InputStream;");
        objIs = env->CallObjectMethod(objConn, midGetErr);
        if (!objIs || env->ExceptionCheck()) {
            env->ExceptionClear();
            env->DeleteLocalRef(clsConn); env->DeleteLocalRef(objConn);
            return "";
        }
    }

    jclass clsISR = env->FindClass("java/io/InputStreamReader");
    jobject objISR = env->NewObject(clsISR,
        env->GetMethodID(clsISR, "<init>", "(Ljava/io/InputStream;)V"), objIs);
    env->DeleteLocalRef(clsISR); env->DeleteLocalRef(objIs);

    jclass clsBR = env->FindClass("java/io/BufferedReader");
    jobject objBR = env->NewObject(clsBR,
        env->GetMethodID(clsBR, "<init>", "(Ljava/io/Reader;)V"), objISR);
    env->DeleteLocalRef(objISR);

    jmethodID midRL = env->GetMethodID(clsBR, "readLine", "()Ljava/lang/String;");
    while (true) {
        jstring line = (jstring)env->CallObjectMethod(objBR, midRL);
        if (!line || env->ExceptionCheck()) { env->ExceptionClear(); break; }
        result += jstr2str(env, line);
        env->DeleteLocalRef(line);
    }
    env->CallVoidMethod(objBR, env->GetMethodID(clsBR, "close", "()V"));
    env->DeleteLocalRef(clsBR); env->DeleteLocalRef(objBR);

    env->CallVoidMethod(objConn, env->GetMethodID(clsConn, "disconnect", "()V"));
    env->DeleteLocalRef(clsConn); env->DeleteLocalRef(objConn);

    if (env->ExceptionCheck()) env->ExceptionClear();
    return result;
}

// ── JSON helpers ──────────────────────────────────────────────────────────────

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
    return (end == std::string::npos) ? "" : json.substr(start, end - start);
}

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

struct ValidateArgs { char key[64]; };

static void* validateThread(void* arg) {
    ValidateArgs* va = (ValidateArgs*)arg;
    char key[64];
    strncpy(key, va->key, sizeof(key) - 1);
    key[sizeof(key) - 1] = '\0';
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

    char jsonBody[256];
    snprintf(jsonBody, sizeof(jsonBody),
             "{\"key\":\"%s\",\"android_id\":\"%s\",\"device_model\":\"%s\"}",
             key, androidId.c_str(), deviceModel.c_str());

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

    bool valid = (resp.find("\"valid\":true")  != std::string::npos ||
                  resp.find("\"valid\": true") != std::string::npos);

    if (valid) {
        long long remaining  = jsonLong(resp, "remaining_seconds");
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

// ── Leitura de key de arquivo externo ─────────────────────────────────────────

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

static bool readKeyFromFile(char* outKey, int maxLen) {
    // 1. External files dir via JNI (mais confiável no Android 10+)
    const std::string& extDir = cachedExternalFilesDir();
    if (!extDir.empty()) {
        char path[512];
        snprintf(path, sizeof(path), "%s/jawm.key", extDir.c_str());
        if (tryReadKeyFile(path, outKey, maxLen)) return true;
    }
    // 2. Fallbacks
    char dl[256], sdcard[256], emu[256];
    snprintf(dl,     sizeof(dl),     "/sdcard/Download/jawm.key");
    snprintf(sdcard, sizeof(sdcard), "/sdcard/jawm.key");
    snprintf(emu,    sizeof(emu),    "/storage/emulated/0/jawm.key");
    if (tryReadKeyFile(dl,     outKey, maxLen)) return true;
    if (tryReadKeyFile(sdcard, outKey, maxLen)) return true;
    if (tryReadKeyFile(emu,    outKey, maxLen)) return true;
    return false;
}

// ── API pública ───────────────────────────────────────────────────────────────

void submitKey(const char* key) {
    if (g_keyState.load() == KeyState::VALIDATING) return;
    g_keyState.store(KeyState::VALIDATING);
    g_keyErrorMsg[0] = '\0';
    strncpy(g_pendingKey, key, sizeof(g_pendingKey) - 1);
    g_pendingKey[sizeof(g_pendingKey) - 1] = '\0';

    ValidateArgs* va = new ValidateArgs();
    strncpy(va->key, key, sizeof(va->key) - 1);
    va->key[sizeof(va->key) - 1] = '\0';
    pthread_t t;
    pthread_create(&t, nullptr, validateThread, va);
    pthread_detach(t);
}

// Verifica arquivo de key a cada 2s (chamado do DrawKeyUI)
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

void startKeyValidation() {
    // 1. Key salva em cache local
    char savedKey[64] = {};
    long long expiresUnix = 0;
    if (loadKeyLocal(savedKey, sizeof(savedKey), &expiresUnix)) {
        long long now = (long long)time(nullptr);
        if (expiresUnix > now + 60) {
            g_keyExpiresUnix = expiresUnix;
            g_keyState.store(KeyState::VALID);
            // Revalidação silenciosa em background
            ValidateArgs* va = new ValidateArgs();
            strncpy(va->key, savedKey, sizeof(va->key) - 1);
            pthread_t t;
            pthread_create(&t, nullptr, validateThread, va);
            pthread_detach(t);
            return;
        }
    }
    // 2. Arquivo externo (se existir ao abrir o jogo)
    char fileKey[64] = {};
    if (readKeyFromFile(fileKey, sizeof(fileKey))) {
        submitKey(fileKey);
        return;
    }
    // 3. Aguardar UI
    g_keyState.store(KeyState::INPUT);
}

// ── ImGui UI ──────────────────────────────────────────────────────────────────

void DrawKeyUI() {
    ImGuiIO& io = ImGui::GetIO();
    float W = io.DisplaySize.x;
    float H = io.DisplaySize.y;

    // Verificar arquivo periodicamente
    checkKeyFile();

    // Fundo escuro
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    bg->AddRectFilled(ImVec2(0,0), ImVec2(W,H), IM_COL32(4,4,8,230));

    // Painel central
    float panW = W * 0.88f;
    if (panW > 460.0f) panW = 460.0f;
    float panH = 400.0f;
    float panX = (W - panW) * 0.5f;
    float panY = (H - panH) * 0.5f;

    static float phase = 0.0f;
    phase += io.DeltaTime * 2.0f;
    float glow = 0.5f + 0.5f * sinf(phase);
    ImU32 borderCol = IM_COL32((int)(80+50*glow),(int)(30+20*glow),(int)(200+55*glow),255);

    bg->AddRectFilled(ImVec2(panX,panY), ImVec2(panX+panW,panY+panH),
                      IM_COL32(10,11,18,255), 14.0f);
    bg->AddRect(ImVec2(panX,panY), ImVec2(panX+panW,panY+panH), borderCol, 14.0f, 0, 1.8f);
    bg->AddRectFilled(ImVec2(panX+20,panY+1), ImVec2(panX+panW*0.55f,panY+3.0f), borderCol);

    ImGui::SetNextWindowPos(ImVec2(panX, panY), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(panW, panH), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(26.0f, 22.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 10.0f));
    ImGui::Begin("##keyui", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoCollapse);

    // Logo
    ImGui::SetCursorPosY(16.0f);
    ImGui::TextColored(ImVec4(0.51f,0.31f,1.0f,1.0f), "JAW");
    ImGui::SameLine(0,4);
    ImGui::TextColored(ImVec4(0.82f,0.83f,0.86f,1.0f), "MODS");
    ImGui::SameLine(0,8);
    ImGui::TextColored(ImVec4(0.39f,0.41f,0.47f,1.0f), "Ativacao de Licenca");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    KeyState st = g_keyState.load();

    if (st == KeyState::VALID) {
        long long rem = g_keyExpiresUnix - (long long)time(nullptr);
        int days = (int)(rem / 86400);
        ImGui::TextColored(ImVec4(0.0f,0.90f,0.50f,1.0f), "  KEY ATIVADA!");
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.7f,0.7f,0.7f,1.0f), "  %d dias restantes", days);

    } else if (st == KeyState::VALIDATING) {
        static float dots = 0.0f;
        dots += io.DeltaTime * 3.0f;
        int d = (int)dots % 4;
        char dotStr[8] = {};
        for (int i = 0; i < d; i++) dotStr[i] = '.';
        ImGui::TextColored(ImVec4(0.9f,0.75f,0.1f,1.0f), "  Verificando%s", dotStr);
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.55f,0.57f,0.64f,1.0f), "  Conectando ao servidor...");

    } else {
        // ── Botão COLAR (método principal) ──────────────────────────
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.10f,0.36f,0.10f,0.90f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.15f,0.55f,0.15f,1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.20f,0.70f,0.20f,1.00f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 7.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,  ImVec2(0.0f,11.0f));
        if (ImGui::Button("  COLAR KEY DO CLIPBOARD  ", ImVec2(panW - 52.0f, 0.0f))) {
            std::string clip = getClipboardText();
            std::string key;
            for (char c : clip) {
                if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
                if (c >= 'a' && c <= 'z') c -= 32;
                key += c;
            }
            if (key.size() >= 10) submitKey(key.c_str());
        }
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(3);

        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.39f,0.41f,0.47f,1.0f),
                           "  Copie sua key e toque no botao acima.");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // ── Alternativa: arquivo ─────────────────────────────────────
        ImGui::TextColored(ImVec4(0.55f,0.57f,0.64f,1.0f),
                           "  Ou salve um arquivo  jawm.key  em:");
        {
            const std::string& extD = cachedExternalFilesDir();
            if (!extD.empty()) {
                char keyPath[512];
                snprintf(keyPath, sizeof(keyPath), "  %s/jawm.key", extD.c_str());
                ImGui::TextColored(ImVec4(0.0f,0.65f,0.85f,1.0f), "%s", keyPath);
            } else {
                ImGui::TextColored(ImVec4(0.0f,0.65f,0.85f,1.0f),
                                   "  /sdcard/Download/jawm.key");
            }
        }
        ImGui::TextColored(ImVec4(0.39f,0.41f,0.47f,1.0f),
                           "  com o conteudo:  JAWM-XXXX-XXXX-XXXX");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Status de erro
        if ((st == KeyState::INVALID || st == KeyState::NO_NETWORK) && g_keyErrorMsg[0]) {
            ImGui::TextColored(ImVec4(1.0f,0.3f,0.3f,1.0f),
                               "  Erro: %s", g_keyErrorMsg);
        }

        // Indicador de polling
        static float scanAnim = 0.0f;
        scanAnim += io.DeltaTime * 1.5f;
        int dot = (int)scanAnim % 4;
        char dotStr[8] = {};
        for (int i = 0; i < dot; i++) dotStr[i] = '.';
        ImGui::TextColored(ImVec4(0.28f,0.30f,0.36f,1.0f),
                           "  Aguardando%s", dotStr);
    }

    ImGui::End();
    ImGui::PopStyleVar(2);
}
