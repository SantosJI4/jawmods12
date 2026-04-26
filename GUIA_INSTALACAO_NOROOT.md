# JAWMODS — Guia de Instalação No-Root (Sistema Unificado)

## Visão Geral

O sistema unificado embutе **tudo** (hook, ESP, Aimbot, validação de key e menu)
dentro de uma única `libopus.so` injetada no APK do Free Fire.
**Nenhum APK extra é necessário.** O menu aparece dentro do próprio jogo.

```
APK do FF (modificado)
  └── lib/arm64-v8a/
        ├── libil2cpp.so     ← original (não modificado)
        ├── libmain.so       ← original (não modificado)
        └── libopus.so ← nosso mod (NOVO)
  └── smali/               ← bytecode patcheado para chamar loadLibrary("opus")
```

---

## Ferramentas Necessárias (PC)

| Ferramenta | Download |
|---|---|
| Java JDK 11+ | https://adoptium.net |
| apktool 2.9+ | https://apktool.org |
| zipalign | Incluso no Android SDK (build-tools) |
| apksigner | Incluso no Android SDK (build-tools) |

---

## Passo 1 — Compilar libopus.so

No Android Studio ou linha de comando:

```bash
# Windows PowerShell (dentro da pasta jawmods/)
./gradlew assembleDebug

# Ou diretamente com ndk-build:
ndk-build NDK_PROJECT_PATH=. APP_BUILD_SCRIPT=src/main/jni/Android.mk \
          APP_ABI=arm64-v8a NDK_APPLICATION_MK=Application.mk
```

A lib compilada estará em:
```
build/intermediates/cxx/.../arm64-v8a/libopus.so
```

---

## Passo 2 — Descompilar APK do Free Fire

```bash
apktool d freefireth.apk -o ff_out/ --no-res
```

> `--no-res` é mais rápido e suficiente para editar smali sem recompilar recursos.

---

## Passo 3 — Copiar a Lib

```bash
cp libopus.so ff_out/lib/arm64-v8a/libopus.so
```

---

## Passo 4 — Patchear o DEX (carregar nossa lib)

Abra o arquivo smali da classe principal do Free Fire.
Geralmente é `UnityPlayerActivity` ou `Application`.

```bash
# Encontrar a classe correta:
grep -r "loadLibrary\|onCreate" ff_out/smali/ --include="*.smali" -l | head -10
```

Geralmente o arquivo é:
```
ff_out/smali/com/dts/freefireth/UnityPlayerActivity.smali
```

### Editar o arquivo .smali

Encontre o método `onCreate(Landroid/os/Bundle;)V` e adicione
**antes do primeiro `return-void`**:

```smali
# Adicionar estas 2 linhas:
const-string v0, "opus"
invoke-static {v0}, Ljava/lang/System;->loadLibrary(Ljava/lang/String;)V
```

**Exemplo completo do método patcheado:**

```smali
.method protected onCreate(Landroid/os/Bundle;)V
    .locals 1

    invoke-super {p0, p1}, Lcom/unity3d/player/UnityPlayerActivity;->onCreate(Landroid/os/Bundle;)V

    # ↓ ADICIONAR AQUI
    const-string v0, "opus"
    invoke-static {v0}, Ljava/lang/System;->loadLibrary(Ljava/lang/String;)V
    # ↑ FIM DO PATCH

    return-void
.end method
```

> **Dica:** Se `UnityPlayerActivity` não existir, tente `MainActivity` ou
> qualquer classe que herde de `Activity` com método `onCreate`.

---

## Passo 5 — Recompilar APK

```bash
apktool b ff_out/ -o ff_modded_unsigned.apk
```

---

## Passo 6 — Alinhar e Assinar

```bash
# Alinhar (obrigatório para instalar no Android 7+)
zipalign -v 4 ff_modded_unsigned.apk ff_modded.apk

# Gerar chave de assinatura (fazer só uma vez)
keytool -genkey -v -keystore jawmods.jks -alias jawkey \
        -keyalg RSA -keysize 2048 -validity 10000 \
        -dname "CN=JawMods, OU=Dev, O=JM, L=BR, ST=BR, C=BR"

# Assinar o APK
apksigner sign --ks jawmods.jks --ks-key-alias jawkey \
               --out ff_final.apk ff_modded.apk
```

---

## Passo 7 — Instalar no Dispositivo

```bash
# Via ADB
adb install -r ff_final.apk

# Ou transferir o ff_final.apk para o celular e instalar manualmente
# (precisa ativar "Fontes desconhecidas" nas configurações)
```

---

## Passo 8 — Uso (dentro do jogo)

1. Abrir o Free Fire normalmente
2. Na primeira abertura: tela de **KEY** aparece automaticamente
   - Digite sua key no formato `JAWM-XXXX-XXXX-XXXX`
   - Toque em **ATIVAR**
   - Key válida → menu some → mod ativo
3. Entrar em uma partida
4. Pressionar **Volume BAIXO (-)** para abrir/fechar o menu
5. No menu:
   - Ativar/desativar **ESP** (caixas, snaplines, head dot)
   - Configurar **Aimbot** (FOV, smooth, paredes, knockado)
   - Ativar **Magic Bullet** (snap instantâneo na cabeça ao atirar)
   - Ajustar **Prediction** (previsão de movimento em ms)

---

## Distribuição para Clientes

Você entrega apenas **`ff_final.apk`**. O cliente:
1. Desinstala o FF original
2. Instala o `ff_final.apk`
3. Abre o jogo e insere a key

**Sem root. Sem Magisk. Sem APK adicional.**

---

## Segurança da Distribuição

| Risco | Mitigação Implementada |
|---|---|
| Engenharia reversa | Strings ofuscadas via XOR (OBFUSCATE macro) |
| Debugger | Verificação via TracerPid + ptrace |
| Frida | Scan de /proc/self/maps |
| Redistribuição sem controle | Sistema de key com HWID binding (SHA256 android_id:model) |
| Logs no logcat | Zero logs em produção (STEALTH_DEBUG desabilitado) |
| Detecção por nome de lib | libopus.so parece lib OpenGL legítima |

---

## Troubleshooting

**Menu não aparece:**
- Verifique se `libopus.so` está em `lib/arm64-v8a/` (não `armeabi-v7a`)
- Confirme que o patch smali foi aplicado corretamente (olhar logcat: `adb logcat | grep -E "gl2|INJ"`)

**Jogo crasha ao abrir:**
- O patch smali pode estar no método errado — tente `Application.attachBaseContext()` em vez de `Activity.onCreate()`
- Verifique se a lib foi compilada para `arm64-v8a` (a maioria dos dispositivos modernos)

**Key não valida:**
- Verifique conexão de internet no dispositivo
- Confirme que o servidor `jawmods.squareweb.app` está online
- Key já vinculada a outro HWID: solicite reset pelo painel admin

**ESP não aparece na partida:**
- Offsets podem ter mudado numa atualização do Free Fire
- Atualizar os defines `OFF_*` em GameHook.cpp e recompilar

---

## Arquitetura Técnica

```
Sistema Unificado libopus.so
├── inject.cpp          → JNI_OnLoad + anti-debug + startup
│   ├── Salva JavaVM* (para HTTP key validation)
│   ├── Inicia hack_thread (GameHook)
│   └── Inicia EGL hook thread (overlay)
├── GameHook.cpp        → VMT hook no il2cpp
│   ├── Coleta posições de jogadores
│   ├── Escreve em SharedESPData (memória interna)
│   └── Aimbot (SetAimRotation + Magic Bullet + Prediction)
├── egl_hook.cpp        → Hook de eglSwapBuffers
│   ├── Inicializa ImGui com contexto EGL do jogo
│   ├── Thread de touch (/dev/input/eventX)
│   └── Renderiza DrawKeyUI ou DrawESP+DrawMenu por frame
├── key_validator.cpp   → Validação de licença
│   ├── HTTP POST via JNI (sem libcurl — usa JVM do jogo)
│   ├── HWID binding (android_id + Build.MODEL)
│   └── Cache local em /data/data/com.dts.freefireth/.gl_key
└── main.cpp            → DrawESP + DrawMenu (ImGui)
    ├── ESP: corner boxes, head dot, snaplines, knocked state
    └── Menu: aimbot config, fov circle, magic bullet, prediction
```

---

*JAWMODS Unified System — Confidencial — Não redistribuir*
