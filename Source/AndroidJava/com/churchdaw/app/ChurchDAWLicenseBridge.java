package com.churchdaw.app;

import android.app.Activity;
import android.content.Context;
import android.content.SharedPreferences;
import android.os.Build;
import android.os.SystemClock;
import android.provider.Settings;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;
import android.util.Base64;
import android.graphics.Rect;

import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.security.KeyFactory;
import java.security.KeyPair;
import java.security.KeyPairGenerator;
import java.security.KeyStore;
import java.security.MessageDigest;
import java.security.PrivateKey;
import java.security.PublicKey;
import java.security.SecureRandom;
import java.security.Signature;
import java.security.spec.ECGenParameterSpec;
import java.security.spec.X509EncodedKeySpec;

import javax.crypto.Cipher;
import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;
import javax.crypto.spec.GCMParameterSpec;
import javax.net.ssl.HttpsURLConnection;

/**
 * Ponte pequena entre o cliente JUCE e o cofre nativo do Android.
 *
 * A chave EC nunca e exportada. Somente a chave publica sai do aparelho; cada
 * refresh e demonstracao precisa de uma assinatura feita pelo Android Keystore.
 * Refresh/lease sao cifrados com AES-GCM antes de chegar ao SharedPreferences.
 */
public final class ChurchDAWLicenseBridge {
    private static final String PREFS = "churchdaw-license-v2";
    private static final String EC_ALIAS = "churchdaw-device-identity-v2";
    private static final String AES_ALIAS = "churchdaw-session-v2";
    private static final String INSTALLATION_KEY = "installation_id";
    private static final String SESSION_KEY = "session_ciphertext";
    private static final String TRUSTED_WALL_KEY = "trusted_wall";
    private static final String TRUSTED_ELAPSED_KEY = "trusted_elapsed";
    private static final String TRUSTED_BOOT_KEY = "trusted_boot";
    private static final String KEY_ID = "churchdaw-2026-01";
    private static final SecureRandom RANDOM = new SecureRandom();
    private static volatile Context applicationContext;

    private static final String LICENSE_PUBLIC_KEY_DER_BASE64 =
        "MIIBojANBgkqhkiG9w0BAQEFAAOCAY8AMIIBigKCAYEAxHPtoU+l4R4cHBRZ/vkP"
        + "OW8eXB/Rpe1osTAsKuIpA6WwmC1jQfOkRigAASf5oy3NvZr05DRc+9QOtrgfLIA1"
        + "zy2yFMykRFe6Sq5Hw3btvgqKnkEJEPgeMdFyijuJzPpT8VjvstiAF67yv7HII05c"
        + "bsfsYfqb3SdKaEv00HLZDk/yuZgcFnqFu+2aQQFF7hl91nlZxe+LKc5ncMQAuKb3"
        + "ltsZRmpABH5m+JbfFMHa/aDI6Qp/ssclA57wc5pv8Bj+DINErPiLHKL+e6+JvWY5"
        + "xw7sAF55lA2JUEFRdaX81m9Q16dMPYhrEAS7WQvb3F8/UAUJt/9D/C94sKI1/OGv"
        + "PKw4u2tcKjfHaUyDK2A9VhSss9mmg5pcDHtbFVDD3kwyXGUbnpOSF2qs+vmds/FG"
        + "XuxeZv3hEYzIf8uSsOdhzXW1BtvIUIH+4sTxq4ckDOKVLPBmdrwT+A9aNOEUp2sC"
        + "dOZEBLcq4z8DicmC5kPM7K/OYS5dWFhJVvLyRK1Va6PbAgMBAAE=";

    private ChurchDAWLicenseBridge() {}

    /**
     * JUCE's component OpenGL renderer is kept on real devices for the mixer,
     * but the translated arm64 image used by x86 Android emulators can lose its
     * GL texture entry points while the audio graph is starting.  That failure
     * happens below Java (SIGSEGV), so it cannot be caught in C++.  Emulator
     * instances use JUCE's software component renderer instead; production
     * phones and tablets remain GPU accelerated.
     */
    public static boolean isProbablyEmulator() {
        final String fingerprint =
                android.os.Build.FINGERPRINT == null
                        ? "" : android.os.Build.FINGERPRINT.toLowerCase(java.util.Locale.ROOT);
        final String model =
                android.os.Build.MODEL == null
                        ? "" : android.os.Build.MODEL.toLowerCase(java.util.Locale.ROOT);
        final String product =
                android.os.Build.PRODUCT == null
                        ? "" : android.os.Build.PRODUCT.toLowerCase(java.util.Locale.ROOT);
        final String hardware =
                android.os.Build.HARDWARE == null
                        ? "" : android.os.Build.HARDWARE.toLowerCase(java.util.Locale.ROOT);

        return fingerprint.startsWith("generic")
                || fingerprint.contains("emulator")
                || model.contains("emulator")
                || model.contains("android sdk built for")
                || model.contains("sdk_gphone")
                || product.contains("sdk_gphone")
                || product.contains("emulator")
                || hardware.contains("goldfish")
                || hardware.contains("ranchu");
    }

    public static float getFontScale(Activity activity) {
        if (activity == null)
            return 1.0f;
        try {
            return Math.max(
                1.0f,
                Math.min(
                    2.0f,
                    activity.getResources()
                        .getConfiguration().fontScale));
        } catch (RuntimeException ignored) {
            return 1.0f;
        }
    }

    /**
     * Returns the real IME state instead of inferring it from the remaining
     * height. Multi-window and short landscape windows can be low without the
     * keyboard being open, so those cases must not lose secondary content.
     */
    public static boolean isImeVisible(Activity activity) {
        if (activity == null)
            return false;
        try {
            android.view.View decor = activity.getWindow().getDecorView();
            if (decor == null)
                return false;

            android.view.WindowInsets insets = decor.getRootWindowInsets();
            if (insets != null && Build.VERSION.SDK_INT >= 30)
                return insets.isVisible(android.view.WindowInsets.Type.ime());

            Rect visible = new Rect();
            decor.getWindowVisibleDisplayFrame(visible);
            int rootHeight = decor.getRootView() != null
                ? decor.getRootView().getHeight() : decor.getHeight();
            int hiddenHeight = Math.max(0, rootHeight - visible.bottom);
            float density = activity.getResources()
                .getDisplayMetrics().density;
            return hiddenHeight > Math.round(96.0f * Math.max(1.0f, density));
        } catch (RuntimeException ignored) {
            return false;
        }
    }

    /**
     * The visible content height collapses when the IME opens. Comparing that
     * reduced rectangle with its width makes a portrait phone look like a
     * landscape window. Configuration orientation is stable across IME
     * visibility and still follows real multi-window configuration changes.
     */
    public static boolean isPortraitOrientation(Activity activity) {
        if (activity == null)
            return true;
        try {
            return activity.getResources().getConfiguration().orientation
                != android.content.res.Configuration.ORIENTATION_LANDSCAPE;
        } catch (RuntimeException ignored) {
            return true;
        }
    }

    public static long getWindowBoundsDp(Activity activity) {
        if (activity == null)
            return 0L;
        try {
            android.content.res.Configuration configuration =
                activity.getResources().getConfiguration();
            int configWidth = configuration.screenWidthDp;
            int configHeight = configuration.screenHeightDp;
            android.view.View content =
                activity.findViewById(android.R.id.content);
            float density = activity.getResources()
                .getDisplayMetrics().density;
            if (density <= 0.0f)
                density = 1.0f;
            int contentWidth = content != null
                ? Math.round(content.getWidth() / density) : 0;
            int contentHeight = content != null
                ? Math.round(content.getHeight() / density) : 0;
            int[] location = new int[] { 0, 0 };
            android.view.View decor =
                activity.getWindow().getDecorView();
            if (decor != null)
                decor.getLocationOnScreen(location);
            int x = Math.max(
                0, Math.round(location[0] / density));
            int y = Math.max(
                0, Math.round(location[1] / density));

            // A janela JUCE cobre a tela INTEIRA (edge-to-edge). Barras do
            // sistema e recorte NAO sao descontados aqui: o MainComponent ja
            // recua os controles pelos safeAreaInsets do JUCE. Descontar nos
            // dois lugares aplicava o mesmo inset DUAS vezes — a janela
            // nascia deslocada (43 dp no Redmi em paisagem) e o layout recuava
            // de novo, somando 258 px de faixa preta morta onde nada era
            // desenhado nem tocavel. O deslocamento x/y do decor permanece:
            // em multi-window a janela realmente nao comeca em (0,0).
            int width = Math.max(1, configWidth);
            int height = Math.max(1, configHeight);
            // screenWidthDp/screenHeightDp podem EXCLUIR a regiao do recorte
            // (medido no TANK 3: 37 dp de faixa preta na borda da camera).
            // As WindowMetrics dao a janela real em pixels, recorte incluso.
            if (android.os.Build.VERSION.SDK_INT >= 30) {
                android.graphics.Rect metricsBounds = activity
                    .getWindowManager().getCurrentWindowMetrics().getBounds();
                int metricsWidth = Math.round(metricsBounds.width() / density);
                int metricsHeight = Math.round(metricsBounds.height() / density);
                if (metricsWidth > 0 && metricsHeight > 0) {
                    width = metricsWidth;
                    height = metricsHeight;
                }
            }
            final int fullHeight = height;
            int imeBottom = 0;
            if (decor != null && decor.getRootWindowInsets() != null
                    && android.os.Build.VERSION.SDK_INT >= 30) {
                android.graphics.Insets imeInsets =
                    decor.getRootWindowInsets().getInsets(
                        android.view.WindowInsets.Type.ime());
                imeBottom = imeInsets.bottom;
            }

            // ADJUST_RESIZE is visible in the content height. During a
            // rotation JUCE's old surface may briefly retain the previous
            // width; only trust the content height once widths agree.
            if (contentWidth > 0 && contentHeight > 0
                    && Math.abs(contentWidth - width) <= 2)
                height = Math.min(height, contentHeight);
            // min() sobre a altura CHEIA: se o ADJUST_RESIZE ja reduziu pela
            // contentHeight, subtrair o IME de novo descontaria duas vezes.
            if (imeBottom > 0)
                height = Math.min(
                    height,
                    Math.max(
                        1, fullHeight - Math.round(imeBottom / density)));

            return ((((long) x) & 0xffffL) << 48)
                | ((((long) y) & 0xffffL) << 32)
                | ((((long) width) & 0xffffL) << 16)
                | (((long) height) & 0xffffL);
        } catch (RuntimeException ignored) {
            return 0L;
        }
    }

    public static void initialize(Activity activity) {
        if (activity != null)
            applicationContext = activity.getApplicationContext();
    }

    /**
     * Small HTTPS bridge for the authentication API.
     *
     * JUCE's bundled Android HTTP class suppresses the Java exception that
     * caused a connection to fail, returning only a null stream to C++. Keep
     * this path deliberately pinned to the ChurchDAW API, reject redirects,
     * cap the response, and return only a safe exception class for diagnostics.
     */
    public static String postJson(
        String address,
        String requestBody,
        String bearer
    ) {
        HttpsURLConnection connection = null;
        final JSONObject result = new JSONObject();
        try {
            final URL url = new URL(address == null ? "" : address);
            if (!"https".equalsIgnoreCase(url.getProtocol())
                    || !"churchdaw.com".equalsIgnoreCase(url.getHost())
                    || url.getPort() != -1
                    || !url.getPath().startsWith("/api/v1/")) {
                return result
                    .put("status", 0)
                    .put("body", "")
                    .put("transport_error", "EndpointRejected")
                    .toString();
            }

            final byte[] body = (
                requestBody == null ? "" : requestBody
            ).getBytes(StandardCharsets.UTF_8);
            connection = (HttpsURLConnection) url.openConnection();
            connection.setInstanceFollowRedirects(false);
            connection.setConnectTimeout(12000);
            connection.setReadTimeout(12000);
            connection.setRequestMethod("POST");
            connection.setDoOutput(true);
            connection.setUseCaches(false);
            connection.setRequestProperty(
                "Content-Type",
                "application/json; charset=utf-8"
            );
            connection.setRequestProperty("Accept", "application/json");
            connection.setRequestProperty("Cache-Control", "no-store");
            connection.setRequestProperty(
                "User-Agent",
                "ChurchDAW-Android"
            );
            if (bearer != null && !bearer.isEmpty())
                connection.setRequestProperty(
                    "Authorization",
                    "Bearer " + bearer
                );
            connection.setFixedLengthStreamingMode(body.length);

            try (OutputStream output = connection.getOutputStream()) {
                output.write(body);
                output.flush();
            }

            final int status = connection.getResponseCode();
            final InputStream stream = status >= 400
                ? connection.getErrorStream()
                : connection.getInputStream();
            final ByteArrayOutputStream response = new ByteArrayOutputStream();
            if (stream != null) {
                try (InputStream input = stream) {
                    final byte[] buffer = new byte[4096];
                    int total = 0;
                    while (true) {
                        final int read = input.read(buffer);
                        if (read < 0)
                            break;
                        total += read;
                        if (total > 128 * 1024)
                            throw new IllegalStateException(
                                "ResponseTooLarge"
                            );
                        response.write(buffer, 0, read);
                    }
                }
            }
            return result
                .put("status", status)
                .put(
                    "body",
                    new String(
                        response.toByteArray(),
                        StandardCharsets.UTF_8
                    )
                )
                .put("transport_error", "")
                .toString();
        } catch (Exception error) {
            try {
                return result
                    .put("status", 0)
                    .put("body", "")
                    .put(
                        "transport_error",
                        error.getClass().getSimpleName()
                    )
                    .toString();
            } catch (Exception ignored) {
                return "";
            }
        } finally {
            if (connection != null)
                connection.disconnect();
        }
    }

    /**
     * HTTPS GET used by the signed update policy and library catalog.
     *
     * Keep this separate from JUCE's Android URL stream: on some devices that
     * stream returns null while hiding the Java TLS exception.  Only the two
     * immutable ChurchDAW distribution documents are accepted here.
     */
    public static String getDistributionJson(
        String address,
        String bearer,
        int maximumBytes,
        int timeoutMs
    ) {
        HttpsURLConnection connection = null;
        final JSONObject result = new JSONObject();
        try {
            final URL url = new URL(address == null ? "" : address);
            final String host = url.getHost();
            final String path = url.getPath();
            final boolean acceptedHost =
                "churchdaw.com".equalsIgnoreCase(host)
                || "www.churchdaw.com".equalsIgnoreCase(host);
            final boolean acceptedPath =
                "/app/android/update-policy-v2.json".equals(path)
                || "/app/android/manifest.json".equals(path);
            if (!"https".equalsIgnoreCase(url.getProtocol())
                    || !acceptedHost
                    || url.getPort() != -1
                    || !acceptedPath) {
                return result
                    .put("status", 0)
                    .put("body", "")
                    .put("server_time", 0)
                    .put("transport_error", "EndpointRejected")
                    .toString();
            }

            final int limit = Math.max(
                1024,
                Math.min(2 * 1024 * 1024, maximumBytes)
            );
            final int timeout = Math.max(
                250,
                Math.min(15000, timeoutMs)
            );
            connection = (HttpsURLConnection) url.openConnection();
            connection.setInstanceFollowRedirects(false);
            connection.setConnectTimeout(timeout);
            connection.setReadTimeout(timeout);
            connection.setRequestMethod("GET");
            connection.setDoInput(true);
            connection.setUseCaches(false);
            connection.setRequestProperty("Accept", "application/json");
            connection.setRequestProperty("Cache-Control", "no-cache");
            connection.setRequestProperty(
                "User-Agent",
                "ChurchDAW-Android"
            );
            if (bearer != null && !bearer.isEmpty())
                connection.setRequestProperty(
                    "Authorization",
                    "Bearer " + bearer
                );

            final int status = connection.getResponseCode();
            final InputStream stream = status >= 400
                ? connection.getErrorStream()
                : connection.getInputStream();
            final ByteArrayOutputStream response = new ByteArrayOutputStream();
            if (stream != null) {
                try (InputStream input = stream) {
                    final byte[] buffer = new byte[8192];
                    int total = 0;
                    while (true) {
                        final int read = input.read(buffer);
                        if (read < 0)
                            break;
                        total += read;
                        if (total > limit)
                            throw new IllegalStateException(
                                "ResponseTooLarge"
                            );
                        response.write(buffer, 0, read);
                    }
                }
            }

            final String envelope = result
                .put("status", status)
                .put(
                    "body",
                    new String(
                        response.toByteArray(),
                        StandardCharsets.UTF_8
                    )
                )
                .put(
                    "server_time",
                    Math.max(0L, connection.getDate() / 1000L)
                )
                .put("transport_error", "")
                .toString();
            android.util.Log.i(
                "ChurchDAW",
                "distribution GET " + path
                    + " status=" + status
                    + " bytes=" + response.size()
                    + " date=" + Math.max(
                        0L, connection.getDate() / 1000L)
            );
            return envelope;
        } catch (Exception error) {
            android.util.Log.e(
                "ChurchDAW",
                "distribution GET failed: "
                    + error.getClass().getSimpleName()
            );
            try {
                return result
                    .put("status", 0)
                    .put("body", "")
                    .put("server_time", 0)
                    .put(
                        "transport_error",
                        error.getClass().getSimpleName()
                    )
                    .toString();
            } catch (Exception ignored) {
                return "";
            }
        } finally {
            if (connection != null)
                connection.disconnect();
        }
    }

    /**
     * Identificador estavel do aparelho, derivado do ANDROID_ID.
     *
     * O ANDROID_ID sobrevive a desinstalar e reinstalar o app; so muda em
     * reset de fabrica (e, desde o Android 8, ele ja e diferente por app,
     * entao nem serve para rastrear entre aplicativos). O hash com sal
     * garante que o valor bruto do sistema nunca saia do aparelho.
     *
     * String vazia se o SHA-256 nao estiver disponivel — o chamador volta
     * ao sorteio aleatorio, que era o comportamento antigo.
     */
    private static String deriveInstallationId(String androidId) {
        try {
            final MessageDigest digest = MessageDigest.getInstance("SHA-256");
            final byte[] hash = digest.digest(
                ("churchdaw-device-v3|" + androidId).getBytes(StandardCharsets.UTF_8));
            final StringBuilder hex = new StringBuilder(32);
            for (int i = 0; i < 16; ++i)
                hex.append(String.format("%02x", hash[i]));
            return hex.toString();
        } catch (Exception ignored) {
            return "";
        }
    }

    public static String identityJson() {
        try {
            final Context context = requireContext();
            final SharedPreferences preferences =
                context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
            String androidId = Settings.Secure.getString(
                context.getContentResolver(), Settings.Secure.ANDROID_ID);
            if (androidId == null || androidId.length() < 8)
                return "";

            // IDENTIDADE PRESA AO APARELHO.
            //
            // O id era sorteado e guardado nas preferencias. Desinstalar o
            // app apagava as preferencias, o proximo arranque sorteava
            // outro, e o servidor via "outro aparelho" — o cliente
            // reinstalava no MESMO celular e batia em "licenca ja ativa em
            // outra maquina".
            //
            // Agora ele e derivado do ANDROID_ID, que sobrevive a
            // desinstalacao (so muda em reset de fabrica). Um id ja
            // gravado e MANTIDO: trocar o de quem esta ativado o faria
            // aparecer como aparelho novo e derrubaria a licenca.
            //
            // O ANDROID_ID nao vai cru para o servidor: segue um hash com
            // sal do produto, como no desktop.
            String installationId = preferences.getString(INSTALLATION_KEY, "");
            if (installationId == null || !installationId.matches("[A-Za-z0-9_-]{16,128}")) {
                installationId = deriveInstallationId(androidId);

                if (installationId.isEmpty()) {
                    final byte[] random = new byte[24];
                    RANDOM.nextBytes(random);
                    installationId = Base64.encodeToString(
                        random, Base64.URL_SAFE | Base64.NO_WRAP | Base64.NO_PADDING);
                }

                if (!preferences.edit().putString(INSTALLATION_KEY, installationId).commit())
                    return "";
            }

            final KeyPair pair = ensureIdentityKey();
            final String publicKey = Base64.encodeToString(
                pair.getPublic().getEncoded(),
                Base64.URL_SAFE | Base64.NO_WRAP | Base64.NO_PADDING);

            final String model = (Build.MANUFACTURER + " " + Build.MODEL).trim();
            return new JSONObject()
                .put("installation_id", installationId)
                .put("device_public_key", publicKey)
                .put("device_fingerprint", "android-id:" + androidId)
                .put("device_label", (model.length() > 0 ? model : "Android") + " / Android")
                .put("platform", "android")
                .toString();
        } catch (Exception ignored) {
            return "";
        }
    }

    public static String signProof(String message) {
        if (message == null || message.length() < 16 || message.length() > 32768)
            return "";
        try {
            final KeyStore store = keyStore();
            final PrivateKey key = (PrivateKey) store.getKey(EC_ALIAS, null);
            if (key == null)
                return "";
            final Signature signer = Signature.getInstance("SHA256withECDSA");
            signer.initSign(key);
            signer.update(message.getBytes(StandardCharsets.UTF_8));
            return Base64.encodeToString(signer.sign(), Base64.NO_WRAP);
        } catch (Exception ignored) {
            return "";
        }
    }

    public static String randomToken(int bytes) {
        if (bytes < 16 || bytes > 64)
            return "";
        final byte[] value = new byte[bytes];
        RANDOM.nextBytes(value);
        return Base64.encodeToString(
            value, Base64.URL_SAFE | Base64.NO_WRAP | Base64.NO_PADDING);
    }

    public static boolean saveSession(String plainJson) {
        if (plainJson == null || plainJson.length() > 65536)
            return false;
        try {
            final Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
            cipher.init(Cipher.ENCRYPT_MODE, ensureSessionKey());
            final byte[] encrypted =
                cipher.doFinal(plainJson.getBytes(StandardCharsets.UTF_8));
            final String packed = Base64.encodeToString(
                    cipher.getIV(), Base64.NO_WRAP)
                + "."
                + Base64.encodeToString(encrypted, Base64.NO_WRAP);
            return requireContext().getSharedPreferences(PREFS, Context.MODE_PRIVATE)
                .edit().putString(SESSION_KEY, packed).commit();
        } catch (Exception ignored) {
            return false;
        }
    }

    public static String loadSession() {
        try {
            final String packed = requireContext()
                .getSharedPreferences(PREFS, Context.MODE_PRIVATE)
                .getString(SESSION_KEY, "");
            if (packed == null || packed.length() > 131072)
                return "";
            final String[] parts = packed.split("\\.", -1);
            if (parts.length != 2)
                return "";
            final byte[] iv = Base64.decode(parts[0], Base64.NO_WRAP);
            final byte[] encrypted = Base64.decode(parts[1], Base64.NO_WRAP);
            if (iv.length != 12 || encrypted.length < 16 || encrypted.length > 65536)
                return "";
            final Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
            cipher.init(
                Cipher.DECRYPT_MODE,
                ensureSessionKey(),
                new GCMParameterSpec(128, iv));
            return new String(cipher.doFinal(encrypted), StandardCharsets.UTF_8);
        } catch (Exception ignored) {
            clearSession();
            return "";
        }
    }

    public static void clearSession() {
        try {
            requireContext().getSharedPreferences(PREFS, Context.MODE_PRIVATE)
                .edit().remove(SESSION_KEY).commit();
        } catch (Exception ignored) {
        }
    }

    public static void rememberServerTime(long epochSeconds) {
        if (epochSeconds < 1700000000L)
            return;
        try {
            requireContext().getSharedPreferences(PREFS, Context.MODE_PRIVATE)
                .edit()
                .putLong(TRUSTED_WALL_KEY, epochSeconds)
                .putLong(TRUSTED_ELAPSED_KEY, SystemClock.elapsedRealtime() / 1000L)
                .putInt(TRUSTED_BOOT_KEY, bootCount())
                .commit();
        } catch (Exception ignored) {
        }
    }

    public static long trustedNowSeconds() {
        try {
            return trustedNow();
        } catch (Exception ignored) {
            return -1L;
        }
    }

    /**
     * Returns the verified JWT payload, or an empty string. The caller still
     * checks the desired mode. This method rejects clock rollback and binds the
     * lease to this exact installation.
     */
    public static String verifyLease(String compactToken, String installationId) {
        if (compactToken == null
                || compactToken.length() < 128
                || compactToken.length() > 16384
                || installationId == null)
            return "";
        try {
            final String[] parts = compactToken.split("\\.", -1);
            if (parts.length != 3)
                return "";
            final JSONObject header = new JSONObject(new String(
                decodeUrl(parts[0]), StandardCharsets.UTF_8));
            if (!"RS256".equals(header.optString("alg"))
                    || !"churchdaw-lease+jwt".equals(header.optString("typ"))
                    || !KEY_ID.equals(header.optString("kid")))
                return "";

            final byte[] publicBytes =
                Base64.decode(LICENSE_PUBLIC_KEY_DER_BASE64, Base64.NO_WRAP);
            final PublicKey publicKey = KeyFactory.getInstance("RSA")
                .generatePublic(new X509EncodedKeySpec(publicBytes));
            final Signature verifier = Signature.getInstance("SHA256withRSA");
            verifier.initVerify(publicKey);
            verifier.update(
                (parts[0] + "." + parts[1]).getBytes(StandardCharsets.US_ASCII));
            if (!verifier.verify(decodeUrl(parts[2])))
                return "";

            final JSONObject claims = new JSONObject(new String(
                decodeUrl(parts[1]), StandardCharsets.UTF_8));
            if (!"https://churchdaw.com".equals(claims.optString("iss"))
                    || !"churchdaw-android".equals(claims.optString("aud"))
                    || !installationId.equals(claims.optString("installation_id")))
                return "";
            final String mode = claims.optString("mode");
            final String product = claims.optString("product_code");
            if (!("licensed".equals(mode)
                    && ("churchdaw-perpetual".equals(product)
                        || "churchdaw-desktop-perpetual".equals(product)))
                    && !("trial".equals(mode) && "churchdaw-demo".equals(product)))
                return "";

            final long now = trustedNow();
            final long issuedAt = claims.optLong("iat", 0L);
            final long notBefore = claims.optLong("nbf", 0L);
            final long offlineUntil = claims.optLong("offline_until", 0L);
            if (now <= 0L
                    || issuedAt <= 0L
                    || issuedAt > now + 60L
                    || notBefore > now + 60L
                    || offlineUntil < now)
                return "";
            if ("trial".equals(mode)
                    && claims.optLong("trial_expires_at", 0L) < now)
                return "";
            return claims.toString();
        } catch (Exception ignored) {
            return "";
        }
    }

    private static Context requireContext() {
        final Context context = applicationContext;
        if (context == null)
            throw new IllegalStateException("ChurchDAW license bridge not initialized");
        return context;
    }

    private static KeyStore keyStore() throws Exception {
        final KeyStore store = KeyStore.getInstance("AndroidKeyStore");
        store.load(null);
        return store;
    }

    private static KeyPair ensureIdentityKey() throws Exception {
        final KeyStore store = keyStore();
        if (store.containsAlias(EC_ALIAS)) {
            final PublicKey publicKey = store.getCertificate(EC_ALIAS).getPublicKey();
            final PrivateKey privateKey = (PrivateKey) store.getKey(EC_ALIAS, null);
            return new KeyPair(publicKey, privateKey);
        }
        final KeyPairGenerator generator = KeyPairGenerator.getInstance(
            KeyProperties.KEY_ALGORITHM_EC, "AndroidKeyStore");
        generator.initialize(new KeyGenParameterSpec.Builder(
                EC_ALIAS,
                KeyProperties.PURPOSE_SIGN | KeyProperties.PURPOSE_VERIFY)
            .setAlgorithmParameterSpec(new ECGenParameterSpec("secp256r1"))
            .setDigests(KeyProperties.DIGEST_SHA256)
            .setUserAuthenticationRequired(false)
            .build());
        return generator.generateKeyPair();
    }

    private static SecretKey ensureSessionKey() throws Exception {
        final KeyStore store = keyStore();
        if (store.containsAlias(AES_ALIAS))
            return (SecretKey) store.getKey(AES_ALIAS, null);
        final KeyGenerator generator = KeyGenerator.getInstance(
            KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore");
        generator.init(new KeyGenParameterSpec.Builder(
                AES_ALIAS,
                KeyProperties.PURPOSE_ENCRYPT | KeyProperties.PURPOSE_DECRYPT)
            .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
            .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
            .setRandomizedEncryptionRequired(true)
            .build());
        return generator.generateKey();
    }

    private static byte[] decodeUrl(String value) {
        return Base64.decode(
            value, Base64.URL_SAFE | Base64.NO_WRAP | Base64.NO_PADDING);
    }

    private static int bootCount() {
        try {
            return Settings.Global.getInt(
                requireContext().getContentResolver(), Settings.Global.BOOT_COUNT);
        } catch (Exception ignored) {
            return -1;
        }
    }

    private static long trustedNow() {
        final long wall = System.currentTimeMillis() / 1000L;
        final SharedPreferences preferences =
            requireContext().getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        final long trustedWall = preferences.getLong(TRUSTED_WALL_KEY, 0L);
        final long trustedElapsed = preferences.getLong(TRUSTED_ELAPSED_KEY, 0L);
        if (trustedWall <= 0L)
            return wall;

        final int savedBoot = preferences.getInt(TRUSTED_BOOT_KEY, -2);
        final int currentBoot = bootCount();
        if (savedBoot >= 0 && savedBoot == currentBoot) {
            final long elapsed = SystemClock.elapsedRealtime() / 1000L;
            if (elapsed < trustedElapsed)
                return -1L;
            final long monotonic = trustedWall + (elapsed - trustedElapsed);
            if (wall + 300L < monotonic)
                return monotonic;
            return Math.max(wall, monotonic);
        }

        // Apos reboot o relogio monotonic zera. Permita o lease pago/demo apenas
        // se o relogio civil nao voltou para antes do ultimo horario do servidor.
        return wall + 300L < trustedWall ? -1L : Math.max(wall, trustedWall);
    }
}
