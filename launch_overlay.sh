#!/system/bin/sh
# ============================================================
# launch.sh - Script para injetar o hook + iniciar overlay
# ============================================================
#
# Uso:
#   adb push launch.sh /data/local/tmp/
#   adb push libHook.so /data/local/tmp/
#   adb shell su -c "sh /data/local/tmp/launch.sh"
#
# Fluxo:
#   1. Configura SELinux permissive
#   2. Configura wrap property para LD_PRELOAD
#   3. Reinicia jogo (Zygote aplica LD_PRELOAD)
#   4. Inicia overlay APK
#
# Arquitetura:
#   [Jogo] <-- libHook.so (VMT hook -> SharedMemory)
#   [Overlay APK] <-- SharedMemory -> ImGui draw
# ============================================================

PACKAGE="com.android.support"
GAME_PACKAGE="com.fungames.sniper3d"
HOOK_LIB="/data/local/tmp/libHook.so"

echo "========================================="
echo " HYBRID MOD - VMT Hook + External Overlay"
echo "========================================="
echo "[*] Jogo:    $GAME_PACKAGE"
echo "[*] Overlay: $PACKAGE"
echo ""

# -- Verifica root --
if [ "$(id -u)" != "0" ]; then
    echo "[!] Este script precisa ser executado como root"
    echo "[!] Use: su -c 'sh $0'"
    exit 1
fi

# -- SELinux permissive --
SELINUX_MODE=$(getenforce 2>/dev/null)
if [ "$SELINUX_MODE" = "Enforcing" ]; then
    echo "[*] SELinux: Enforcing -> Permissive"
    setenforce 0
fi

# -- Verifica libHook.so --
if [ ! -f "$HOOK_LIB" ]; then
    echo "[!] libHook.so nao encontrada em $HOOK_LIB"
    echo "[!] Faca: adb push libs/arm64-v8a/libHook.so /data/local/tmp/"
    exit 1
fi
chmod 777 "$HOOK_LIB"
chcon u:object_r:system_lib_file:s0 "$HOOK_LIB" 2>/dev/null

# -- Limpar e pre-criar shared memory com permissoes abertas --
# O jogo roda como UID do app, nao como root
# IMPORTANTE: criar com 4096 bytes (nao vazio), senao mmap causa SIGBUS
rm -f /data/local/tmp/.esp_shm 2>/dev/null
rm -f /sdcard/.esp_shm 2>/dev/null
dd if=/dev/zero of=/data/local/tmp/.esp_shm bs=4096 count=1 2>/dev/null
chmod 666 /data/local/tmp/.esp_shm
chcon u:object_r:app_data_file:s0 /data/local/tmp/.esp_shm 2>/dev/null
dd if=/dev/zero of=/sdcard/.esp_shm bs=4096 count=1 2>/dev/null
chmod 666 /sdcard/.esp_shm

# -- Garantir permissao overlay --
appops set "$PACKAGE" SYSTEM_ALERT_WINDOW allow 2>/dev/null

# -- Verificar APK instalado --
pm path "$PACKAGE" > /dev/null 2>&1
if [ $? -ne 0 ]; then
    echo "[!] APK overlay nao encontrado: $PACKAGE"
    echo "[!] Instale o APK primeiro"
    exit 1
fi

# -- Iniciar overlay ANTES --
echo "[*] Iniciando overlay..."
am start -n "$PACKAGE/.MainActivity" 2>/dev/null

# ============================================
# METODO 1: setprop wrap.PACKAGE LD_PRELOAD=
# O Zygote le essa property ao criar o proceso
# ============================================
echo ""
echo "[*] Metodo 1: setprop wrap direto..."

setprop wrap."$GAME_PACKAGE" "" 2>/dev/null
sleep 1
setprop wrap."$GAME_PACKAGE" "LD_PRELOAD=$HOOK_LIB"
echo "[+] wrap.$GAME_PACKAGE = LD_PRELOAD=$HOOK_LIB"

am force-stop "$GAME_PACKAGE" 2>/dev/null
sleep 1

echo "[*] Iniciando jogo..."
monkey -p "$GAME_PACKAGE" -c android.intent.category.LAUNCHER 1 2>/dev/null

echo "[*] Aguardando hook ativar..."
HOOK_OK=0
for i in $(seq 1 30); do
    # Checar se hook fez ftruncate (arquivo cresce de 0 para 4096)
    SZ=$(wc -c < /data/local/tmp/.esp_shm 2>/dev/null)
    SZ2=$(wc -c < /sdcard/.esp_shm 2>/dev/null)
    if [ "${SZ:-0}" -ge 4096 ] 2>/dev/null || [ "${SZ2:-0}" -ge 4096 ] 2>/dev/null; then
        HOOK_OK=1
        break
    fi
    GAME_PID=$(pidof "$GAME_PACKAGE" 2>/dev/null)
    if [ -n "$GAME_PID" ]; then
        HOOK_MAPS=$(grep -c libHook.so /proc/$GAME_PID/maps 2>/dev/null)
        IL2CPP_MAPS=$(grep -c libil2cpp.so /proc/$GAME_PID/maps 2>/dev/null)
        echo "  ${i}s: PID=$GAME_PID hook=$HOOK_MAPS il2cpp=$IL2CPP_MAPS"
    else
        echo "  ${i}s: jogo nao iniciou..."
    fi
    sleep 1
done

setprop wrap."$GAME_PACKAGE" ""

# ============================================
# METODO 2 (fallback): wrapper shell script
# ============================================
if [ "$HOOK_OK" = "0" ]; then
    echo ""
    echo "[!] Metodo 1 nao funcionou"
    echo "[*] Tentando metodo 2: wrapper script..."

    cat > /data/local/tmp/wrap_hook.sh << 'WRAPEOF'
#!/system/bin/sh
export LD_PRELOAD=/data/local/tmp/libHook.so
exec "$@"
WRAPEOF
    chmod 755 /data/local/tmp/wrap_hook.sh
    chcon u:object_r:system_file:s0 /data/local/tmp/wrap_hook.sh 2>/dev/null

    setprop wrap."$GAME_PACKAGE" "/data/local/tmp/wrap_hook.sh"
    am force-stop "$GAME_PACKAGE" 2>/dev/null
    sleep 1
    monkey -p "$GAME_PACKAGE" -c android.intent.category.LAUNCHER 1 2>/dev/null

    echo "[*] Aguardando hook (fallback)..."
    for i in $(seq 1 20); do
        SZ=$(wc -c < /data/local/tmp/.esp_shm 2>/dev/null)
        SZ2=$(wc -c < /sdcard/.esp_shm 2>/dev/null)
        if [ "${SZ:-0}" -ge 4096 ] 2>/dev/null || [ "${SZ2:-0}" -ge 4096 ] 2>/dev/null; then
            HOOK_OK=1
            break
        fi
        GP=$(pidof "$GAME_PACKAGE" 2>/dev/null)
        HM=$(grep -c libHook.so /proc/$GP/maps 2>/dev/null)
        echo "  ${i}s: PID=$GP hook=$HM"
        sleep 1
    done

    setprop wrap."$GAME_PACKAGE" ""
fi

# -- Status final --
echo ""
echo "========================================="
echo " STATUS"
echo "========================================="
GAME_PID=$(pidof "$GAME_PACKAGE" 2>/dev/null)
echo "[+] Jogo:    PID ${GAME_PID:-N/A}"
echo "[+] Overlay: Iniciado"
echo "[+] Hook:    libHook.so (VMT)"
echo "[+] IPC:     /data/local/tmp/.esp_shm"

if [ "$HOOK_OK" = "1" ]; then
    echo "[+] HOOK ATIVO!"
else
    echo "[!] Hook NAO carregou."
    echo ""
    echo "Diagnostico:"
    if [ -n "$GAME_PID" ]; then
        echo "  PID: $GAME_PID"
        echo "  Hook maps: $(grep -c libHook.so /proc/$GAME_PID/maps 2>/dev/null)"
        echo "  il2cpp maps: $(grep -c libil2cpp.so /proc/$GAME_PID/maps 2>/dev/null)"
    fi
    echo "  wrap prop: $(getprop wrap.$GAME_PACKAGE)"
    echo "  Logcat GameHook:"
    logcat -d -s GameHook:* 2>/dev/null | tail -5
    echo ""
    echo "Se hook_maps=0, use um injector ptrace:"
    echo "  /data/local/tmp/injector \$PID $HOOK_LIB"
fi

echo ""
echo "[*] Para parar:"
echo "    am force-stop $PACKAGE"
echo "    rm /data/local/tmp/.esp_shm"
