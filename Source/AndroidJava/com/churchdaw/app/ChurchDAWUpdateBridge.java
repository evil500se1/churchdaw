package com.churchdaw.app;

import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageInfo;
import android.net.Uri;
import android.os.Build;

/**
 * Ponte do gate nativo para a distribuicao oficial do ChurchDAW.
 *
 * A policy recebida pelo C++ ja foi verificada com RSA. Builds instaladas pelo
 * site abrem somente o APK assinado no churchdaw.com. Se o instalador original
 * foi a Google Play, a atualizacao permanece na ficha oficial da loja.
 */
public final class ChurchDAWUpdateBridge {
    public static final int ELIGIBILITY_QUERY_FAILED = 0;
    public static final int ELIGIBILITY_CONFIRMED = 1;
    public static final int ELIGIBILITY_NOT_REQUIRED = 2;

    private static final String APP_PACKAGE = "com.churchdaw.app";
    private static final String PLAY_STORE_PACKAGE = "com.android.vending";
    private static final String PLAY_STORE_URL =
        "https://play.google.com/store/apps/details?id=" + APP_PACKAGE;
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
     * O candidato somente chega aqui apos policy RSA valida. O processo de
     * release verifica a disponibilidade antes de elevar minimum_version_code.
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

    /** Abre somente o APK oficial ou a ficha exata do pacote na Google Play. */
    public static boolean openWebsiteUpdate(Activity activity, String updateUrl) {
        if (activity == null
                || !APP_PACKAGE.equals(activity.getPackageName())
                || (!isOfficialApkUrl(updateUrl)
                    && !isOfficialPlayStoreUrl(updateUrl))) {
            return false;
        }

        if (isOfficialPlayStoreUrl(updateUrl) || wasInstalledFromPlay(activity)) {
            return openPlayStore(activity);
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

    /** Mantido para a Activity; a instalacao ocorre fora deste processo. */
    public static void resumeImmediateUpdate(Activity activity) {
        // A instalacao e controlada pelo Android ou pela Google Play.
    }

    /** Mantido para a Activity; este fluxo nao usa startActivityForResult. */
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

    private static boolean openPlayStore(Activity activity) {
        try {
            final Intent intent = new Intent(Intent.ACTION_VIEW, Uri.parse(PLAY_STORE_URL));
            intent.addCategory(Intent.CATEGORY_BROWSABLE);
            intent.setPackage(PLAY_STORE_PACKAGE);
            activity.startActivity(intent);
            return true;
        } catch (RuntimeException ignored) {
            try {
                final Intent browserIntent = new Intent(
                    Intent.ACTION_VIEW, Uri.parse(PLAY_STORE_URL));
                browserIntent.addCategory(Intent.CATEGORY_BROWSABLE);
                activity.startActivity(browserIntent);
                return true;
            } catch (RuntimeException ignoredAgain) {
                return false;
            }
        }
    }

    @SuppressWarnings("deprecation")
    private static boolean wasInstalledFromPlay(Activity activity) {
        try {
            return PLAY_STORE_PACKAGE.equals(
                activity.getPackageManager().getInstallerPackageName(APP_PACKAGE));
        } catch (RuntimeException ignored) {
            return false;
        }
    }

    private static boolean isOfficialApkUrl(String value) {
        if (value == null || value.length() < 45 || value.length() > 320
                || value.indexOf('\r') >= 0 || value.indexOf('\n') >= 0) {
            return false;
        }

        final Uri uri;
        try {
            uri = Uri.parse(value);
        } catch (RuntimeException ignored) {
            return false;
        }

        final String path = uri.getEncodedPath();
        return "https".equals(uri.getScheme())
            && "churchdaw.com".equals(uri.getHost())
            && uri.getUserInfo() == null
            && uri.getPort() == -1
            && uri.getQuery() == null
            && uri.getFragment() == null
            && path != null
            && path.matches(
                "^/app/android/ChurchDAW-[A-Za-z0-9._+\\-]{1,64}-signed\\.apk$");
    }

    private static boolean isOfficialPlayStoreUrl(String value) {
        return PLAY_STORE_URL.equals(value);
    }
}
