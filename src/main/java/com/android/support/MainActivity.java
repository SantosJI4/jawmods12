package com.android.support;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.view.View;
import android.widget.Button;
import android.widget.ProgressBar;
import android.widget.TextView;
import android.widget.Toast;

import java.io.BufferedReader;
import java.io.File;
import java.io.InputStreamReader;

public class MainActivity extends Activity {
    private static final int OVERLAY_PERMISSION_CODE = 1234;

    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    // MUDE AQUI: Package name do jogo
    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    private static final String GAME_PACKAGE = "com.dts.freefireth";

    private TextView tvStatus;
    private ProgressBar progressInject;
    private Button btnStart;
    private Button btnStop;
    private Handler handler;
    private volatile boolean injecting = false;
    private volatile boolean hookPolling = false;
    private Thread hookPollThread = null;

    @Override
    public void onPointerCaptureChanged(boolean hasCapture) {
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        tvStatus = (TextView) findViewById(R.id.tvStatus);
        progressInject = (ProgressBar) findViewById(R.id.progressInject);
        btnStart = (Button) findViewById(R.id.btnStart);
        btnStop = (Button) findViewById(R.id.btnStop);
        handler = new Handler(Looper.getMainLooper());

        btnStart.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                onStartClicked();
            }
        });

        btnStop.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                onStopClicked();
            }
        });

        // Checar permissão de overlay
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M && !Settings.canDrawOverlays(this)) {
            setStatus("Requesting overlay permission...");
            Intent intent = new Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION,
                    Uri.parse("package:" + getPackageName()));
            startActivityForResult(intent, OVERLAY_PERMISSION_CODE);
        } else {
            setStatus("Ready to inject");
        }

        // Se o módulo já está instalado, ir direto para START OVERLAY
        new Thread(new Runnable() {
            @Override
            public void run() {
                final boolean moduleInstalled = isModuleInstalled();
                if (moduleInstalled) {
                    runOnUi(new Runnable() {
                        @Override
                        public void run() {
                            setStatus("Módulo Zygisk instalado.\nInicie o Free Fire e clique START OVERLAY.");
                            btnStart.setText("START OVERLAY");
                            btnStart.setOnClickListener(new View.OnClickListener() {
                                @Override
                                public void onClick(View v) {
                                    startOverlayOnly();
                                }
                            });
                        }
                    });
                }
            }
        }).start();
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == OVERLAY_PERMISSION_CODE) {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M && Settings.canDrawOverlays(this)) {
                setStatus("Ready to inject");
            } else {
                setStatus("Overlay permission denied");
            }
        }
    }

    private void showLoading(final boolean show) {
        runOnUi(new Runnable() {
            @Override
            public void run() {
                progressInject.setVisibility(show ? View.VISIBLE : View.GONE);
            }
        });
    }

    private void onStartClicked() {
        if (injecting) return;

        // Verificar permissÃ£o de overlay
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M && !Settings.canDrawOverlays(this)) {
            Toast.makeText(this, "Overlay permission required", Toast.LENGTH_SHORT).show();
            return;
        }

        injecting = true;
        btnStart.setEnabled(false);
        btnStart.setVisibility(View.GONE);
        showLoading(true);
        setStatus("Initializing...");

        // Tudo roda em background thread (su commands bloqueiam)
        new Thread(new Runnable() {
            @Override
            public void run() {
                doInjectAndStart();
            }
        }).start();
    }

    private void onStopClicked() {
        // Parar poll thread
        hookPolling = false;
        if (hookPollThread != null) {
            hookPollThread.interrupt();
            hookPollThread = null;
        }
        new Thread(new Runnable() {
            @Override
            public void run() {
                runOnUi(new Runnable() {
                    @Override
                    public void run() {
                        stopService(new Intent(MainActivity.this, OverlayService.class));
                        setStatus("Overlay stopped.\nHook stays active until game restarts.");
                        btnStop.setVisibility(View.GONE);
                        btnStart.setVisibility(View.VISIBLE);
                        btnStart.setEnabled(true);
                    }
                });
            }
        }).start();
    }

    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    // LOGICA â€” ZYGISK MODULE INSTALL (v18)
    // Instala libzygisk.so como mÃ³dulo Magisk.
    // O hook roda antes do anti-cheat, sem ptrace, sem arquivo no game dir.
    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    private void doInjectAndStart() {
        try {
            // 1. Verificar root
            updateStatus("Checking root...");
            final String rootCheck = rootExec("id");
            if (rootCheck == null || !rootCheck.contains("uid=0")) {
                updateStatus("No root access.\nRoot (Magisk) required.");
                resetButton();
                return;
            }

            // 2. Verificar se Magisk estÃ¡ presente
            updateStatus("Checking Magisk...");
            final String magiskVer = rootExec("magisk -v 2>/dev/null || echo 'not found'");
            if (magiskVer == null || magiskVer.contains("not found")) {
                updateStatus("Magisk not found.\nInstall Magisk first.");
                resetButton();
                return;
            }

            // 3. Extrair libzygisk.so do APK
            updateStatus("Installing Zygisk module...");
            final String nativeDir = getApplicationInfo().nativeLibraryDir;
            final String zygiskSrc = nativeDir + "/libzygisk.so";

            if (!new File(zygiskSrc).exists()) {
                updateStatus("libzygisk.so not found.\nRebuild the project (clean build).");
                resetButton();
                return;
            }

            // 4. Criar estrutura do módulo Magisk
            final String moduleDir = "/data/adb/modules/jawmods";
            rootExec("mkdir -p " + moduleDir + "/zygisk");

            // module.prop — usar printf para garantir newlines reais
            // echo 'text\n' nao gera newlines, printf sim
            rootExec("printf 'id=jawmods\\nname=JawMods ESP\\nversion=v18\\nversionCode=18\\nauthor=JawMods\\ndescription=Free Fire ESP Zygisk module\\n'"
                   + " > " + moduleDir + "/module.prop");

            // Copiar .so para zygisk/arm64-v8a.so
            rootExec("cp " + zygiskSrc + " " + moduleDir + "/zygisk/arm64-v8a.so"
                   + " ; chmod 644 " + moduleDir + "/zygisk/arm64-v8a.so");

            // Remover flags de disable/remove se existirem
            rootExec("rm -f " + moduleDir + "/disable " + moduleDir + "/remove");

            // Limpar arquivos antigos do ptrace em /data/local/tmp/
            rootExec("rm -f /data/local/tmp/libgl2.so"
                   + " /data/local/tmp/libHook.so"
                   + " /data/local/tmp/libinjector.so"
                   + " /data/local/tmp/jshook_clip_result"
                   + " /data/local/tmp/.gl_log");

            // 5. Verificar instalaÃ§Ã£o
            String check = rootExec("ls -la " + moduleDir + "/zygisk/arm64-v8a.so 2>/dev/null");
            if (check == null || !check.contains("arm64-v8a.so")) {
                updateStatus("Failed to install module.\nCheck root access.\nDir: " + moduleDir);
                resetButton();
                return;
            }

            updateStatus("MÃ³dulo Zygisk instalado!\nPath: " + moduleDir + "/zygisk/arm64-v8a.so\n\n"
                       + "AÃ‡ÃƒO NECESSÃRIA:\n"
                       + "1. Reinicie o dispositivo\n"
                       + "2. Abra o Free Fire\n"
                       + "3. Volte aqui e aperte START novamente\n"
                       + "   (sÃ³ para iniciar o overlay)");

            // Marcar que precisa de reboot
            final boolean needsReboot = true;

            // 6. Pre-criar SHM para o hook escrever
            rootExec("rm -f /data/local/tmp/.gl_cache /data/local/tmp/.esp_shm"
                   + " ; dd if=/dev/zero of=/data/local/tmp/.gl_cache bs=4096 count=1 2>/dev/null"
                   + " ; chmod 666 /data/local/tmp/.gl_cache");

            showLoading(false);
            runOnUi(new Runnable() {
                @Override
                public void run() {
                    btnStart.setVisibility(View.VISIBLE);
                    btnStart.setEnabled(true);
                    btnStart.setText("START OVERLAY");
                    injecting = false;
                }
            });

            // Substituir o listener do botÃ£o para sÃ³ iniciar overlay no prÃ³ximo clique
            runOnUi(new Runnable() {
                @Override
                public void run() {
                    btnStart.setOnClickListener(new View.OnClickListener() {
                        @Override
                        public void onClick(View v) {
                            startOverlayOnly();
                        }
                    });
                }
            });
            return;

        } catch (final Exception e) {
            final String err = "Fatal error: " + e.getClass().getSimpleName() + ": " + e.getMessage();
            runOnUi(new Runnable() {
                @Override
                public void run() {
                    Toast.makeText(MainActivity.this, err, Toast.LENGTH_LONG).show();
                }
            });
            updateStatus(err);
            resetButton();
        }
    }

    // Chamado apÃ³s mÃ³dulo instalado + reboot: sÃ³ inicia o overlay e monitora SHM
    private void startOverlayOnly() {
        if (injecting) return;
        injecting = true;
        btnStart.setEnabled(false);
        showLoading(true);
        updateStatus("Iniciando overlay...");

        new Thread(new Runnable() {
            @Override
            public void run() {
                try {
                    // Iniciar overlay
                    runOnUi(new Runnable() {
                        @Override
                        public void run() {
                            startService(new Intent(MainActivity.this, OverlayService.class));
                        }
                    });

                    // Lançar Free Fire via monkey (root — mais confiável que Intent API)
                    // monkey -p <pkg> -c android.intent.category.LAUNCHER 1
                    // envia exatamente 1 evento de launch para o app, independente
                    // de estado da Activity ou do PackageManager.
                    updateStatus("Lançando Free Fire...");
                    String launchResult = rootExec(
                        "monkey -p " + GAME_PACKAGE + " -c android.intent.category.LAUNCHER 1 2>&1");
                    // Fallback: Intent API caso monkey falhe (sem monkey no device)
                    if (launchResult == null || launchResult.isEmpty() ||
                            launchResult.contains("Error") || launchResult.contains("No activities")) {
                        runOnUi(new Runnable() {
                            @Override
                            public void run() {
                                try {
                                    Intent ffIntent = getPackageManager()
                                        .getLaunchIntentForPackage(GAME_PACKAGE);
                                    if (ffIntent != null) {
                                        ffIntent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK
                                                | Intent.FLAG_ACTIVITY_RESET_TASK_IF_NEEDED);
                                        startActivity(ffIntent);
                                    }
                                } catch (Exception e) {
                                    updateStatus("Não foi possível lançar " + GAME_PACKAGE
                                        + ": " + e.getMessage());
                                }
                            }
                        });
                    }

                    // Aguardar SHM magic (hook Zygisk ativo = 0xDEADF00D)
                    updateStatus("Iniciando Free Fire e aguardando hook...");
                    boolean connected = false;
                    for (int i = 0; i < 60; i++) {
                        Thread.sleep(1000);
                        String shmMagic = rootExec("od -A n -t x4 -N 4 /data/local/tmp/.gl_cache 2>/dev/null");
                        if (shmMagic != null && shmMagic.trim().contains("deadf00d")) {
                            connected = true;
                            break;
                        }
                        updateStatus("Aguardando SHM... (" + (i+1) + "/60s)\n"
                            + "Abra o Free Fire e aguarde o carregamento completo.");
                    }

                    showLoading(false);
                    if (connected) {
                        updateStatus("HOOK ZYGISK ATIVO!\nESP funcionando.");
                        runOnUi(new Runnable() {
                            @Override
                            public void run() {
                                btnStop.setVisibility(View.VISIBLE);
                            }
                        });
                    } else {
                        updateStatus("Hook nÃ£o detectado apÃ³s 60s.\n"
                            + "Verifique:\n"
                            + "1. MÃ³dulo Zygisk ativo no Magisk\n"
                            + "2. Reiniciou apÃ³s instalar?\n"
                            + "3. SHM: od -A n -t x4 -N 4 /data/local/tmp/.gl_cache");
                    }
                    resetButton();
                } catch (Exception e) {
                    updateStatus("Erro: " + e.getMessage());
                    resetButton();
                }
            }
        }).start();
    }

    private void updateStatus(final String text) {
        runOnUi(new Runnable() {
            @Override
            public void run() {
                setStatus(text);
            }
        });
    }

    private void resetButton() {
        showLoading(false);
        runOnUi(new Runnable() {
            @Override
            public void run() {
                btnStart.setVisibility(View.VISIBLE);
                btnStart.setEnabled(true);
                injecting = false;
            }
        });
    }

    // Verifica se o módulo Zygisk já está instalado (sem precisar de root)
    private boolean isModuleInstalled() {
        // Tenta via root (mais confiável)
        String result = rootExec("test -f /data/adb/modules/jawmods/zygisk/arm64-v8a.so"
                + " && test ! -f /data/adb/modules/jawmods/disable"
                + " && test ! -f /data/adb/modules/jawmods/remove"
                + " && echo YES || echo NO");
        return result != null && result.contains("YES");
    }

    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    // Helpers
    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

    private String rootExec(String cmd) {
        Process su = null;
        try {
            // su -c "cmd" Ã© mais robusto que piping via stdin em dispositivos Android
            // Evita confusao do su ao spawnar muitos processos rapidamente
            su = Runtime.getRuntime().exec(new String[]{"su", "-c", cmd});

            // Consumir stderr em thread separada para evitar deadlock no pipe
            final Process proc = su;
            Thread stderrDrainer = new Thread(new Runnable() {
                @Override
                public void run() {
                    try {
                        byte[] buf = new byte[4096];
                        while (proc.getErrorStream().read(buf) != -1) { /* descarta */ }
                    } catch (Exception ignored) {}
                }
            });
            stderrDrainer.setDaemon(true);
            stderrDrainer.start();

            BufferedReader reader = new BufferedReader(
                    new InputStreamReader(su.getInputStream()));
            StringBuilder sb = new StringBuilder();
            String line;
            while ((line = reader.readLine()) != null) {
                sb.append(line).append("\n");
            }
            reader.close();
            su.waitFor();
            return sb.toString().trim();
        } catch (Exception e) {
            return null;
        } finally {
            if (su != null) {
                su.destroy();
            }
        }
    }

    private String getLauncherActivity(String pkg) {
        String result = rootExec("cmd package resolve-activity --brief " + pkg
                + " | tail -1");
        if (result != null && result.contains("/")) {
            String activity = result.trim();
            if (activity.contains(pkg)) {
                return activity.substring(activity.indexOf("/"));
            }
        }
        result = rootExec("dumpsys package " + pkg
                + " | grep -A1 'android.intent.action.MAIN' | grep -o '"
                + pkg + "/[^ ]*'");
        if (result != null && result.contains("/")) {
            return result.trim().substring(result.indexOf("/"));
        }
        return "/.MainActivity";
    }

    private void setStatus(String text) {
        tvStatus.setText(text);
    }

    private void runOnUi(Runnable r) {
        handler.post(r);
    }
}
