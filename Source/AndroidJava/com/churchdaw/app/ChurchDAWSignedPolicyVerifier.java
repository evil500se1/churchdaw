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
    public static final String KEY_ID = "churchdaw-update-2026-01";
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

    private ChurchDAWSignedPolicyVerifier() {}

    public static boolean verify(
            String payloadBase64, String signatureBase64, String keyId) {
        if (!KEY_ID.equals(keyId)
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

            final byte[] publicKeyBytes =
                Base64.decode(PUBLIC_KEY_DER_BASE64, Base64.NO_WRAP);
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
