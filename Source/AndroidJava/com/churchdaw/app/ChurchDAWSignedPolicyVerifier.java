package com.churchdaw.app;

import android.util.Base64;

import java.nio.charset.StandardCharsets;
import java.security.KeyFactory;
import java.security.PublicKey;
import java.security.Signature;
import java.security.spec.X509EncodedKeySpec;

/**
 * Verifica a autoridade criptografica da politica que pode bloquear o app.
 *
 * Somente a chave PUBLICA fica no APK. A chave privada vive fora do projeto e
 * da hospedagem, portanto editar um JSON no servidor nao basta para inventar
 * uma atualizacao obrigatoria.
 */
public final class ChurchDAWSignedPolicyVerifier {
    public static final String KEY_ID_2026_11 = "churchdaw-update-2026-11";
    public static final String KEY_ID = "churchdaw-update-2026-08";
    public static final String KEY_ID_ANTERIOR = "churchdaw-update-2026-01";
    private static final int MAX_PAYLOAD_BYTES = 24 * 1024;
    private static final int RSA_3072_SIGNATURE_BYTES = 384;
    private static final String PUBLIC_KEY_DER_BASE64 =
        "MIIBojANBgkqhkiG9w0BAQEFAAOCAY8AMIIBigKCAYEAsOVhe3vxcydvPbdkGQMg"
        + "53MFe5k7r+HFeMd4RRqWXdZIgAeF9RfMt3vFkX+AFgOHh6HsNR5CTA4UZXqtOBut"
        + "I/oZibVjWC1C1F3EHAgSqjLwOhT58vs+ucTkdaS2zcdkB4fPOt80OM+yo4vINOl9"
        + "ajh9ABDtcPk5p4LWMUiP9+eO+KsarJ7kB2XcerGYQzb1GtrmNAVLxq8wPsks/rhZ"
        + "GjF38ctuhxbtPVwVzlLeyWl//kzW8uzFp/gSuO8yb3emaRk3k7OVZrx7YH8cDvSx"
        + "F8E4tV+scd9l09l1Qi/a5hq9n9bxod8Yak4tdQzQuk1eM2F9HeNE+4C7OuVMXzT"
        + "v1TPNezQydX5sowLwGX8LOUcM/BumKM9NMGEd8dOB5BHqJlA9KWThRANkjcudhYS"
        + "rgJbRWvM9ZOgMknOl18+Q0F+/XWSjfMWzBzDdggmZY69MfgQXVGkW5X0RrS1o+G"
        + "A3T/949ToRXGhcRzKmXgmiQwPl8b6K2oA4tO2l9w9X/Y7xAgMBAAE=";

    // Chave publicada com a rotacao 2026-08. A chave anterior permanece
    // acima para que politicas antigas assinadas continuem verificaveis.
    private static final String PUBLIC_KEY_DER_BASE64_2026_08 =
        "MIIBojANBgkqhkiG9w0BAQEFAAOCAY8AMIIBigKCAYEAv5ZtiPbh+xhdk9POqWX7VQcCKzOcFXfxFLYUnFkClzc3ErmIVbkY4hBsbfHUwKhEiUdbCRTzNiOn7Y6hGX6iuN5FtPPqhcecXPbxWaSyP2x6Ukk2ThKov8Y9i1eo7y7zltVOlHVcDp8kqsQNobWUS93gONE33xha0uriF6Mhge5EHXGbEfBLSDLYojm46dqwDrQpE7VPfV1hPETTWBlU3oLqs1mAhkD5tAXEVPOfmkVR35ZxPyycdvUp62x2TwNY/sflHcDls0Jtg/SrH91sUJtUkY6wnWzK7RXlLCJyQ5aC1oPBICZSVb0n07xDj1Use3Rr7hQAi91cDL2GKQN1X3MJ7Go9NgENvPB19kSZDDQt62WRMF1fQNDVWtxDc0kMy6qVWwTv/jCc9X+YMyQ/M9ebkkYSjdgpioVms1jfBN/vMHn+qLalo1zL2EHjOS+JncHZhEbvxYsOAmRdvKZpxWntvdPCqvvlJg+r6+JuluYf3p29+vp7jKCbW2TeXjD3AgMBAAE=";

    // Chave publicada com a rotacao 2026-11 (update-policy-v3.json).
    private static final String PUBLIC_KEY_DER_BASE64_2026_11 =
        "MIIBojANBgkqhkiG9w0BAQEFAAOCAY8AMIIBigKCAYEAtPuym7becfl2i5m2CB3AoDc4ydpGzYmmjDlEOUcyBHQK4QWXTeI8l9G9EHVBfSxDIDNBHGVMicVVyxof2w6aflo+cK9jFL5s4EZz60xfVSfYlOjeLjxWXmbxPAWl1HVEotJtP4DGFcwek5MBT2LV/mg82+9pa9oyzF6MiRsGdfg74dz9dxAPvpsnXAMIAyPIcgf5zAaIh+UkNwwhaPYD6ntOhmCfgE7t1ZBLv4FZvP/10SvqsyKlDEKbd8rr9/eAcxupWlZzepyI7MxRDF5/o30OXcPiMHfs1PVx7bd2w9Y53Nvx6CfkcJ503zQHyBOy+G4wQCrUNOkFLMa4sktIJnOuTt3fz5WgJi/3fTi7t6xwhs6G/kV0bWOIClW3zdwdq+mzyFl7bQxrAAn94c2WsBUSdSnT/PqyYAf0JUxuAk/0tDLqGp/phKPyTQZ/hG1O11hLBLUfunavMSkPosob+FviNpBjTrAbX0DMwakTGGN3clcf9cxn9D/h7oVgevKVAgMBAAE=";

    private ChurchDAWSignedPolicyVerifier() {}

    public static boolean verify(
            String payloadBase64, String signatureBase64, String keyId) {
        final boolean is202611Key = KEY_ID_2026_11.equals(keyId);
        final boolean isCurrentKey = KEY_ID.equals(keyId);
        final boolean isPreviousKey = KEY_ID_ANTERIOR.equals(keyId);
        if ((!is202611Key && !isCurrentKey && !isPreviousKey)
                || payloadBase64 == null
                || signatureBase64 == null
                || payloadBase64.length() > MAX_PAYLOAD_BYTES * 2
                || signatureBase64.length() > RSA_3072_SIGNATURE_BYTES * 2) {
            return false;
        }

        try {
            final byte[] payload = Base64.decode(payloadBase64, Base64.NO_WRAP);
            final byte[] signatureBytes =
                Base64.decode(signatureBase64, Base64.NO_WRAP);

            if (payload.length <= 0
                    || payload.length > MAX_PAYLOAD_BYTES
                    || signatureBytes.length != RSA_3072_SIGNATURE_BYTES) {
                return false;
            }

            // O payload e JSON UTF-8 canonico; rejeite uma sequencia que nao
            // possa fazer round-trip para impedir interpretacoes divergentes.
            final String utf8 = new String(payload, StandardCharsets.UTF_8);
            if (!java.util.Arrays.equals(
                    payload, utf8.getBytes(StandardCharsets.UTF_8))) {
                return false;
            }

            final String publicKeyBase64 = is202611Key
                ? PUBLIC_KEY_DER_BASE64_2026_11
                : (isCurrentKey
                    ? PUBLIC_KEY_DER_BASE64_2026_08
                    : PUBLIC_KEY_DER_BASE64);
            final byte[] publicKeyBytes =
                Base64.decode(publicKeyBase64, Base64.NO_WRAP);
            final PublicKey publicKey = KeyFactory.getInstance("RSA")
                .generatePublic(new X509EncodedKeySpec(publicKeyBytes));
            final Signature verifier = Signature.getInstance("SHA256withRSA");
            verifier.initVerify(publicKey);
            verifier.update(payload);
            return verifier.verify(signatureBytes);
        } catch (Exception ignored) {
            return false;
        }
    }
}
