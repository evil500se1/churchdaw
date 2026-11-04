package com.churchdaw.app;

import static android.view.View.SYSTEM_UI_FLAG_FULLSCREEN;
import static android.view.View.SYSTEM_UI_FLAG_HIDE_NAVIGATION;
import static android.view.View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY;
import static android.view.View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN;
import static android.view.View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION;
import static android.view.View.SYSTEM_UI_FLAG_LAYOUT_STABLE;

import android.app.Activity;
import android.content.pm.ActivityInfo;
import android.content.Intent;
import android.os.Build;
import android.os.Bundle;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;

/**
 * Activity final do ChurchDAW. Em projetos JUCE sem push notifications, o
 * Projucer usa android.app.Activity diretamente. Herdar de JuceActivity nesse
 * modo chama callbacks JNI de push que nao existem e derruba o app no resume.
 *
 * Esta Activity preserva o layout edge-to-edge do JUCE e acrescenta somente o
 * ciclo de vida necessario para a atualizacao imediata da Play Store.
 */
public final class ChurchDAWActivity extends Activity {
    @SuppressWarnings("deprecation")
    private void initEdgeToEdge() {
        if (Build.VERSION.SDK_INT < 35) {
            final View decorView = getWindow().getDecorView();
            final int flags = Build.VERSION.SDK_INT < 30
                    ? SYSTEM_UI_FLAG_LAYOUT_STABLE
                            | SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                            | SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                    : 0;

            decorView.setSystemUiVisibility(decorView.getSystemUiVisibility() | flags);
        }

        if (Build.VERSION.SDK_INT >= 30)
            getWindow().setDecorFitsSystemWindows(false);

        if (Build.VERSION.SDK_INT >= 29) {
            if (Build.VERSION.SDK_INT < 35)
                getWindow().setStatusBarContrastEnforced(false);

            getWindow().setNavigationBarContrastEnforced(false);
        }
    }

    @SuppressWarnings("deprecation")
    private void applyImmersiveSticky() {
        if (Build.VERSION.SDK_INT >= 30) {
            // Window.getInsetsController() estoura NPE no onCreate do Android 16 (DecorView ainda
            // nao instalada); pela View devolve null ate anexar, e onResume/foco repetem a chamada.
            final WindowInsetsController controller = getWindow().getDecorView().getWindowInsetsController();
            if (controller != null) {
                controller.hide(WindowInsets.Type.statusBars() | WindowInsets.Type.navigationBars());
                controller.setSystemBarsBehavior(
                        WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            final View decorView = getWindow().getDecorView();
            decorView.setSystemUiVisibility(
                    SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                            | SYSTEM_UI_FLAG_LAYOUT_STABLE
                            | SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                            | SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                            | SYSTEM_UI_FLAG_HIDE_NAVIGATION
                            | SYSTEM_UI_FLAG_FULLSCREEN);
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
        initEdgeToEdge();
        super.onCreate(savedInstanceState);
        getWindow().setSoftInputMode(
            WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE);
        // A tela do celular nao pode apagar sozinha durante o culto: o musico
        // toca de olho no app e nao pode ficar tocando a tela pra mantê-la
        // acesa. FLAG_KEEP_SCREEN_ON mantem o display ligado enquanto a
        // activity estiver visivel.
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        applyImmersiveSticky();
        ChurchDAWAudioDeviceBridge.initialize(getApplicationContext());
        ChurchDAWLicenseBridge.initialize(this);
        // Protege o processo do congelador do sistema quando o musico troca de app.
        ChurchDAWAudioService.start(this);
        // BLE MIDI: a permissao CONNECT/SCAN (API 31+) agora e pedida sob
        // demanda no painel Audio/MIDI Bluetooth, evitando modal do sistema em
        // cima do cold start (que disparava onPause e reiniciava o gate).
    }

    @Override
    protected void onResume() {
        super.onResume();
        applyImmersiveSticky();
        ChurchDAWAudioDeviceBridge.onResume();
        ChurchDAWUpdateBridge.resumeImmediateUpdate(this);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus)
            applyImmersiveSticky();
    }

    @Override
    protected void onDestroy() {
        // So encerra o servico quando o usuario realmente fecha o app, nao numa recriacao.
        if (isFinishing())
            ChurchDAWAudioService.stop(this);
        super.onDestroy();
    }

    @Override
    @SuppressWarnings("deprecation")
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        ChurchDAWUpdateBridge.handleActivityResult(this, requestCode, resultCode);
    }
}
