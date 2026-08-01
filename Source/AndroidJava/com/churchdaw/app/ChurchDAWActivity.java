package com.churchdaw.app;

import static android.view.View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN;
import static android.view.View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION;
import static android.view.View.SYSTEM_UI_FLAG_LAYOUT_STABLE;

import android.app.Activity;
import android.content.pm.ActivityInfo;
import android.content.Intent;
import android.os.Build;
import android.os.Bundle;
import android.view.View;
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

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
        initEdgeToEdge();
        super.onCreate(savedInstanceState);
        getWindow().setSoftInputMode(
            WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE);
        ChurchDAWAudioDeviceBridge.initialize(getApplicationContext());
        ChurchDAWLicenseBridge.initialize(this);
    }

    @Override
    protected void onResume() {
        super.onResume();
        ChurchDAWAudioDeviceBridge.onResume();
        ChurchDAWUpdateBridge.resumeImmediateUpdate(this);
    }

    @Override
    @SuppressWarnings("deprecation")
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        ChurchDAWUpdateBridge.handleActivityResult(this, requestCode, resultCode);
    }
}
