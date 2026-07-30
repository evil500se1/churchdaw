package com.churchdaw.app;

import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageInfo;
import android.net.Uri;
import android.os.Build;

/**
 * Ponte do gate nativo para a distribuicao direta do ChurchDAW.
 *
 * A policy recebida pelo C++ ja foi verificada com RSA antes de esta classe
 * receber a URL. Esta ponte abre somente um APK HTTPS no dominio oficial. A
 * confirmacao final de instalacao pertence ao Android e nunca e simulada.
 */
public final class ChurchDAWUpdateBridge {
    public static final int ELIGIBILITY_QUERY_FAILED = 0;
    public static final int ELIGIBILITY_CONFIRMED = 1;
    public static final int ELIGIBILITY_NOT_REQUIRED = 2;

    private static final String APP_PACKAGE = "com.churchdaw.app";
    private static final String UPDATE_PREFS = "churchdaw_update_bridge";
    private static final String REQUIRED_VERSION_KEY = "confirmed_required_version";

    private ChurchDAWUpdateBridge() {}

    public static int getInstalledVersionCode(Activity activity) {
        if (activity == null || !APP_PACKAGE.equals(activity.getPackageName())) {
            return 0;
        }

        try {
            final PackageInfo info = activity.getPackageManager()
                .getPackageInfo(activity.getPackageName(), 0);

            if (Build.VERSION.SDK_INT >= 28) {
                final long code = info.getLongVersionCode();
                return code > 0 && code <= Integer.MAX_VALUE ? (int) code : 0;
            }

            //noinspection deprecation
            return info.versionCode;
        } catch (Exception ignored) {
            return 0;
        }
    }

    /**
     * O candidato somente chega aqui apos policy RSA valida. O servidor deve
     * publicar o APK antes de elevar minimum_version_code; a tela nativa fica
     * bloqueada enquanto o Android instala a versao nova.
     */
    public static int queryImmediateUpdateEligibilityBlocking(
            Activity activity, int requiredVersionCode) {
        if (activity == null || !APP_PACKAGE.equals(activity.getPackageName())) {
            return ELIGIBILITY_QUERY_FAILED;
        }

        final int installedVersion = getInstalledVersionCode(activity);
        if (installedVersion <= 0) {
            return ELIGIBILITY_QUERY_FAILED;
        }

        return installedVersion >= clampVersionCode(requiredVersionCode)
            ? ELIGIBILITY_NOT_REQUIRED
            : ELIGIBILITY_CONFIRMED;
    }

    /** Abre o APK oficial para download/instalacao confirmada pelo Android. */
    public static boolean openWebsiteUpdate(Activity activity, String updateUrl) {
        if (activity == null
                || !APP_PACKAGE.equals(activity.getPackageName())
                || !isOfficialApkUrl(updateUrl)) {
            return false;
        }

        try {
            final Intent intent = new Intent(Intent.ACTION_VIEW, Uri.parse(updateUrl));
            intent.addCategory(Intent.CATEGORY_BROWSABLE);
            activity.startActivity(intent);
            return true;
        } catch (RuntimeException ignored) {
            return false;
        }
    }

    /** Mantido para a Activity; nao existe fluxo pendente de loja para retomar. */
    public static void resumeImmediateUpdate(Activity activity) {
        // A instalacao ocorre no navegador/instalador do sistema.
    }

    /** Mantido para a Activity; o instalador do sistema nao retorna por este fluxo. */
    public static void handleActivityResult(Activity activity, int requestCode, int resultCode) {
        // Sem acao.
    }

    public static int setConfirmedRequiredVersion(
            Activity activity, int requiredVersionCode) {
        if (activity == null || !APP_PACKAGE.equals(activity.getPackageName())) {
            return 0;
        }

        try {
            final SharedPreferences prefs = activity.getSharedPreferences(
                UPDATE_PREFS, Activity.MODE_PRIVATE);
            final int requested = requiredVersionCode <= 1
                ? 1 : clampVersionCode(requiredVersionCode);
            final int installed = getInstalledVersionCode(activity);

            if (requested <= 1 || installed >= requested) {
                return prefs.edit().remove(REQUIRED_VERSION_KEY).commit() ? 1 : 0;
            }

            return prefs.edit().putInt(REQUIRED_VERSION_KEY, requested).commit()
                ? requested : 0;
        } catch (RuntimeException ignored) {
            return 0;
        }
    }

    private static int clampVersionCode(int versionCode) {
        return Math.max(1, Math.min(1000000000, versionCode));
    }

    private static boolean isOfficialApkUrl(String value) {
        if (value == null || value.length() < 45 || value.length() > 320) {
            return false;
        }

        final Uri uri;
        try {
            uri = Uri.parse(value);
        } catch (RuntimeException ignored) {
            return false;
        }

        final String path = uri.getPath();
        return "https".equals(uri.getScheme())
            && "churchdaw.com".equals(uri.getHost())
            && uri.getPort() == -1
            && uri.getQuery() == null
            && uri.getFragment() == null
            && path != null
            && path.startsWith("/app/android/ChurchDAW-")
            && path.endsWith("-signed.apk");
    }
}
