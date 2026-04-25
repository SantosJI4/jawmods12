package com.android.support;

import android.app.Service;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.PixelFormat;
import android.os.Build;
import android.os.IBinder;
import android.util.DisplayMetrics;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.graphics.drawable.GradientDrawable;

/**
 * Overlay Service - Desenha ImGui em processo separado do jogo
 * 
 * FLAG_SECURE = Exclusão de Captura (não aparece em screenshots/gravações)
 * 
 * Arquitetura:
 *   - SurfaceView fullscreen para rendering OpenGL (ImGui)
 *   - Toggle button flutuante para ativar/desativar interação com menu
 *   - Quando menu ATIVO: overlay recebe touch (interage com ImGui)
 *   - Quando menu INATIVO: overlay é transparente ao touch (ESP desenha, touch passa pro jogo)
 */
public class OverlayService extends Service {

    static {
        System.loadLibrary("MEOW");
    }

    // Native methods
    private static native void nativeOnSurfaceCreated(android.view.Surface surface, int width, int height);
    private static native void nativeOnSurfaceDestroyed();
    private static native void nativeOnTouch(int action, float x, float y);
    private static native void nativeSetScreenSize(int width, int height);

    private WindowManager windowManager;
    private SurfaceView overlaySurface;
    private View toggleButton;
    private WindowManager.LayoutParams overlayParams;
    private WindowManager.LayoutParams btnParams; // moved to field for inner class access

    private boolean menuActive = false;

    @Override
    public void onCreate() {
        super.onCreate();
        windowManager = (WindowManager) getSystemService(WINDOW_SERVICE);

        DisplayMetrics dm = getResources().getDisplayMetrics();
        int screenW = dm.widthPixels;
        int screenH = dm.heightPixels;

        createOverlaySurface(screenW, screenH);
        createToggleButton();
    }

    private int getOverlayType() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            return WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY;
        }
        return WindowManager.LayoutParams.TYPE_PHONE;
    }

    /**
     * Cria a surface OpenGL fullscreen para rendering do ImGui
     */
    private void createOverlaySurface(int screenW, int screenH) {
        overlayParams = new WindowManager.LayoutParams(
                WindowManager.LayoutParams.MATCH_PARENT,
                WindowManager.LayoutParams.MATCH_PARENT,
                getOverlayType(),
                // Inicia como NOT_TOUCHABLE (ESP desenha, touch passa pro jogo)
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                        | WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE
                        | WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL
                        | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN
                        | WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS
                        | WindowManager.LayoutParams.FLAG_HARDWARE_ACCELERATED,
                PixelFormat.TRANSLUCENT
        );
        overlayParams.gravity = Gravity.TOP | Gravity.START;
        overlayParams.x = 0;
        overlayParams.y = 0;

        overlaySurface = new SurfaceView(this);
        overlaySurface.setZOrderOnTop(true);
        overlaySurface.getHolder().setFormat(PixelFormat.TRANSLUCENT);

        overlaySurface.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override
            public void surfaceCreated(SurfaceHolder holder) {
                // Usar dimensões reais da surface (surfaceChanged é chamado logo após
                // surfaceCreated com os valores corretos), mas passamos um tamanho inicial
                // usando getRealMetrics() que inclui barras do sistema (nav bar, status bar)
                // para garantir alinhamento correto em qualquer device/resolução.
                DisplayMetrics dm = new DisplayMetrics();
                windowManager.getDefaultDisplay().getRealMetrics(dm);
                nativeOnSurfaceCreated(holder.getSurface(), dm.widthPixels, dm.heightPixels);
            }

            @Override
            public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
                nativeSetScreenSize(width, height);
            }

            @Override
            public void surfaceDestroyed(SurfaceHolder holder) {
                nativeOnSurfaceDestroyed();
            }
        });

        // Touch handler para quando o menu está ativo
        overlaySurface.setOnTouchListener(new View.OnTouchListener() {
            @Override
            public boolean onTouch(View v, MotionEvent event) {
                if (menuActive) {
                    nativeOnTouch(event.getActionMasked(), event.getX(), event.getY());
                    return true;
                }
                return false;
            }
        });

        windowManager.addView(overlaySurface, overlayParams);
    }

    /**
     * Cria botão flutuante para toggle do menu
     * Sempre visível e touchable, independente do estado do menu
     */
    private void createToggleButton() {
        // Botão circular
        GradientDrawable circle = new GradientDrawable();
        circle.setShape(GradientDrawable.OVAL);
        circle.setColor(Color.argb(180, 30, 30, 30));
        circle.setStroke(3, Color.argb(200, 0, 200, 0));

        ImageView btn = new ImageView(this);
        btn.setBackground(circle);

        int btnSize = 80; // dp
        float density = getResources().getDisplayMetrics().density;
        int btnPx = (int) (btnSize * density);

        btnParams = new WindowManager.LayoutParams(
                btnPx, btnPx,
                getOverlayType(),
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                        | WindowManager.LayoutParams.FLAG_SECURE, // Também excluído de captura
                PixelFormat.TRANSLUCENT
        );
        btnParams.gravity = Gravity.TOP | Gravity.START;
        btnParams.x = 20;
        btnParams.y = 200;

        // Toggle menu on/off
        btn.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                menuActive = !menuActive;
                updateOverlayTouchability();

                // Feedback visual
                GradientDrawable bg = (GradientDrawable) v.getBackground();
                if (menuActive) {
                    bg.setStroke(3, Color.argb(200, 200, 0, 0)); // Vermelho = menu ativo
                } else {
                    bg.setStroke(3, Color.argb(200, 0, 200, 0)); // Verde = ESP only
                }
            }
        });

        // Drag do botão
        btn.setOnTouchListener(new View.OnTouchListener() {
            private int lastX, lastY;
            private int touchStartX, touchStartY;
            private boolean isDragging = false;

            @Override
            public boolean onTouch(View v, MotionEvent event) {
                switch (event.getActionMasked()) {
                    case MotionEvent.ACTION_DOWN:
                        lastX = (int) event.getRawX();
                        lastY = (int) event.getRawY();
                        touchStartX = lastX;
                        touchStartY = lastY;
                        isDragging = false;
                        return true;

                    case MotionEvent.ACTION_MOVE:
                        int dx = (int) event.getRawX() - lastX;
                        int dy = (int) event.getRawY() - lastY;

                        if (!isDragging && (Math.abs(event.getRawX() - touchStartX) > 10
                                || Math.abs(event.getRawY() - touchStartY) > 10)) {
                            isDragging = true;
                        }

                        if (isDragging) {
                            btnParams.x += dx;
                            btnParams.y += dy;
                            windowManager.updateViewLayout(v, btnParams);
                        }

                        lastX = (int) event.getRawX();
                        lastY = (int) event.getRawY();
                        return true;

                    case MotionEvent.ACTION_UP:
                        if (!isDragging) {
                            v.performClick();
                        }
                        return true;
                }
                return false;
            }
        });

        toggleButton = btn;
        windowManager.addView(toggleButton, btnParams);
    }

    /**
     * Alterna overlay entre touchable (menu) e not_touchable (ESP only)
     */
    private void updateOverlayTouchability() {
        if (menuActive) {
            // Menu ativo: overlay recebe touch
            overlayParams.flags = WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                    | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN
                    | WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS
                    | WindowManager.LayoutParams.FLAG_HARDWARE_ACCELERATED
                    | WindowManager.LayoutParams.FLAG_SECURE;
            // NOT_TOUCHABLE removido = overlay recebe touch
        } else {
            // Apenas ESP: touch passa pro jogo
            overlayParams.flags = WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                    | WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE
                    | WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL
                    | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN
                    | WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS
                    | WindowManager.LayoutParams.FLAG_HARDWARE_ACCELERATED
                    | WindowManager.LayoutParams.FLAG_SECURE;
        }
        windowManager.updateViewLayout(overlaySurface, overlayParams);
    }

    @Override
    public void onDestroy() {
        super.onDestroy();
        nativeOnSurfaceDestroyed();

        if (overlaySurface != null) {
            windowManager.removeView(overlaySurface);
            overlaySurface = null;
        }
        if (toggleButton != null) {
            windowManager.removeView(toggleButton);
            toggleButton = null;
        }
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
