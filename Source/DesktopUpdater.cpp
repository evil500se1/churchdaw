#include "DesktopUpdater.h"
#include "DesktopUpdatePublicKey.h"

#include <juce_cryptography/juce_cryptography.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <bcrypt.h>
 #include <wincrypt.h>
 #include <softpub.h>
 #include <wintrust.h>
#elif JUCE_MAC
 #include <Security/Security.h>
#endif

namespace ChurchDAWDesktopUpdate
{

//==============================================================================
// 1. ASSINATURA RS256 DA POLICY
//
// Mesmo esquema do Android, elo por elo:
//   SignAndroidUpdatePolicy.ps1  ->  openssl dgst -sha256 -sign  (payload.json)
//   ChurchDAWSignedPolicyVerifier.java -> Signature("SHA256withRSA").update(payload)
// Ou seja: a mensagem assinada sao os BYTES JSON do payload, nao o Base64 deles.
// Errar isso faria toda assinatura legitima ser recusada.
//
// Este codigo repete a mecanica de DesktopLeaseSignature_*.cpp de proposito:
// aquele arquivo embute a chave de LICENCA e so existe quando o licenciamento
// esta ligado. A atualizacao obrigatoria nao pode depender de uma opcao de
// build nem usar a chave errada.
//==============================================================================

#if JUCE_WINDOWS

bool verifyUpdateRs256Signature (const juce::MemoryBlock& message,
                                 const juce::MemoryBlock& signature,
                                 juce::String& error)
{
    error.clear();

    juce::MemoryOutputStream decodedKey;
    if (! juce::Base64::convertFromBase64 (decodedKey, updatePolicyPublicKeyDerBase64))
    {
        error = "Chave publica de atualizacao invalida";
        return false;
    }

    CERT_PUBLIC_KEY_INFO* publicKeyInfo = nullptr;
    DWORD publicKeyInfoBytes = 0;
    if (CryptDecodeObjectEx (X509_ASN_ENCODING,
                             X509_PUBLIC_KEY_INFO,
                             static_cast<const BYTE*> (decodedKey.getData()),
                             static_cast<DWORD> (decodedKey.getDataSize()),
                             CRYPT_DECODE_ALLOC_FLAG,
                             nullptr,
                             &publicKeyInfo,
                             &publicKeyInfoBytes) == FALSE)
    {
        error = "Nao foi possivel decodificar a chave de atualizacao";
        return false;
    }

    BCRYPT_KEY_HANDLE key = nullptr;
    const auto imported = CryptImportPublicKeyInfoEx2 (
        X509_ASN_ENCODING, publicKeyInfo, 0, nullptr, &key);
    LocalFree (publicKeyInfo);

    if (imported == FALSE || key == nullptr)
    {
        error = "Nao foi possivel importar a chave de atualizacao";
        return false;
    }

    const auto digest = juce::SHA256 (message).getRawData();
    BCRYPT_PKCS1_PADDING_INFO padding { BCRYPT_SHA256_ALGORITHM };
    const auto status = BCryptVerifySignature (
        key,
        &padding,
        reinterpret_cast<PUCHAR> (const_cast<void*> (digest.getData())),
        static_cast<ULONG> (digest.getSize()),
        reinterpret_cast<PUCHAR> (const_cast<void*> (signature.getData())),
        static_cast<ULONG> (signature.getSize()),
        BCRYPT_PAD_PKCS1);
    BCryptDestroyKey (key);

    if (status < 0)
    {
        error = "Assinatura da politica de atualizacao invalida";
        return false;
    }

    return true;
}

#elif JUCE_MAC

bool verifyUpdateRs256Signature (const juce::MemoryBlock& message,
                                 const juce::MemoryBlock& signature,
                                 juce::String& error)
{
    error.clear();

    juce::MemoryOutputStream decodedKey;
    if (! juce::Base64::convertFromBase64 (decodedKey, updatePolicyPublicKeyDerBase64))
    {
        error = "Chave publica de atualizacao invalida";
        return false;
    }

    // API C do Security.framework -- nada de Objective-C, para este arquivo
    // seguir sendo um .cpp unico para as duas plataformas.
    auto release = [] (CFTypeRef r) { if (r != nullptr) CFRelease (r); };

    CFDataRef keyData = CFDataCreate (nullptr,
                                      static_cast<const UInt8*> (decodedKey.getData()),
                                      static_cast<CFIndex> (decodedKey.getDataSize()));
    if (keyData == nullptr)
    {
        error = "Falha ao preparar a chave de atualizacao";
        return false;
    }

    const int keyBits = 3072;
    CFNumberRef bits = CFNumberCreate (nullptr, kCFNumberIntType, &keyBits);
    const void* attrKeys[]   { kSecAttrKeyType,    kSecAttrKeyClass,       kSecAttrKeySizeInBits };
    const void* attrValues[] { kSecAttrKeyTypeRSA, kSecAttrKeyClassPublic, bits };
    CFDictionaryRef attributes = CFDictionaryCreate (nullptr, attrKeys, attrValues, 3,
                                                     &kCFTypeDictionaryKeyCallBacks,
                                                     &kCFTypeDictionaryValueCallBacks);

    CFErrorRef creationError = nullptr;
    SecKeyRef key = SecKeyCreateWithData (keyData, attributes, &creationError);
    release (attributes);
    release (bits);
    release (keyData);

    if (key == nullptr)
    {
        release (creationError);
        error = "Nao foi possivel importar a chave de atualizacao";
        return false;
    }

    CFDataRef messageData = CFDataCreate (nullptr,
                                          static_cast<const UInt8*> (message.getData()),
                                          static_cast<CFIndex> (message.getSize()));
    CFDataRef signatureData = CFDataCreate (nullptr,
                                            static_cast<const UInt8*> (signature.getData()),
                                            static_cast<CFIndex> (signature.getSize()));

    CFErrorRef verificationError = nullptr;
    const bool valid = (messageData != nullptr && signatureData != nullptr)
        && SecKeyVerifySignature (key,
                                  kSecKeyAlgorithmRSASignatureMessagePKCS1v15SHA256,
                                  messageData,
                                  signatureData,
                                  &verificationError);

    release (verificationError);
    release (signatureData);
    release (messageData);
    release (key);

    if (! valid)
    {
        error = "Assinatura da politica de atualizacao invalida";
        return false;
    }

    return true;
}

#else

bool verifyUpdateRs256Signature (const juce::MemoryBlock&, const juce::MemoryBlock&,
                                 juce::String& error)
{
    // Android usa ChurchDAWSignedPolicyVerifier.java; aqui nao ha caminho valido.
    error = "Verificacao de atualizacao indisponivel nesta plataforma";
    return false;
}

#endif

//==============================================================================
// 2. LEITURA ESTRITA DO ENVELOPE
//
// Tudo que vem da rede e tratado como hostil ate a assinatura provar o
// contrario -- e mesmo depois, os campos passam por faixa e formato. Uma policy
// assinada mas com numero absurdo ainda pode virar tijolo, entao a validacao
// nao termina na criptografia.
//==============================================================================
namespace
{
    #if JUCE_WINDOWS
     constexpr const char* platformField = "windows";
    #elif JUCE_MAC
     constexpr const char* platformField = "mac";
    #else
     constexpr const char* platformField = "unsupported";
    #endif

    bool readBoundedInt (const juce::var& value, juce::int64 maximum, juce::int64& out)
    {
        if (! value.isInt() && ! value.isInt64() && ! value.isDouble())
            return false;

        const auto asDouble = static_cast<double> (value);
        if (asDouble < 0.0 || asDouble > static_cast<double> (maximum))
            return false;

        const auto asInt = static_cast<juce::int64> (value);
        if (static_cast<double> (asInt) != asDouble)   // recusa fracionario
            return false;

        out = asInt;
        return true;
    }

    bool isSha256Hex (const juce::String& text)
    {
        return text.length() == 64 && text.containsOnly ("0123456789abcdef");
    }

    bool isSafeVersionName (const juce::String& text)
    {
        return text.isNotEmpty() && text.length() <= 48
            && text.containsOnly ("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                  "abcdefghijklmnopqrstuvwxyz._+-");
    }

    // A mensagem aparece em janela nossa. Sem quebras de linha nem controle:
    // uma policy assinada por engano nao deve conseguir desenhar uma tela falsa.
    bool isSafeMessage (const juce::String& text)
    {
        if (text.length() > 640)
            return false;

        for (auto c : text)
            if (c < 32 && c != 0)
                return false;

        return ! text.containsChar (0);
    }

    bool isTrustedHttpsUrl (const juce::String& url)
    {
        if (url.length() > 512)
            return false;

       #if CHURCHDAW_UPDATE_ALLOW_HTTP_LOCALHOST
        // Build de teste: so 127.0.0.1, so nesta compilacao.
        if (url.startsWith ("http://127.0.0.1:") || url.startsWith ("http://127.0.0.1/"))
            return true;
       #endif

        if (! url.startsWith ("https://"))
            return false;

        const auto host = url.fromFirstOccurrenceOf ("https://", false, false)
                             .upToFirstOccurrenceOf ("/", false, false)
                             .upToFirstOccurrenceOf (":", false, false)
                             .toLowerCase();

        return host == juce::String (requiredHostSuffix)
            || host.endsWith (juce::String (".") + requiredHostSuffix);
    }

    juce::File updateStateFile()
    {
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("ChurchDAW")
                   .getChildFile ("update-state.json");
    }

    struct UpdateState
    {
        int highestPolicySerial = 0;
        juce::String lastInstalledSha256;
    };

    UpdateState loadUpdateState()
    {
        UpdateState state;
        const auto file = updateStateFile();

        if (! file.existsAsFile())
            return state;

        juce::var parsed;
        if (juce::JSON::parse (file.loadFileAsString(), parsed).failed())
            return state;

        if (auto* object = parsed.getDynamicObject())
        {
            juce::int64 serial = 0;
            if (readBoundedInt (object->getProperty ("highest_policy_serial"),
                                1000000000, serial))
                state.highestPolicySerial = static_cast<int> (serial);

            const auto sha = object->getProperty ("last_installed_sha256")
                                 .toString().trim().toLowerCase();
            if (isSha256Hex (sha))
                state.lastInstalledSha256 = sha;
        }

        return state;
    }

    void saveUpdateState (const UpdateState& state)
    {
        auto* object = new juce::DynamicObject();
        object->setProperty ("highest_policy_serial", state.highestPolicySerial);

        if (state.lastInstalledSha256.isNotEmpty())
            object->setProperty ("last_installed_sha256", state.lastInstalledSha256);

        const auto file = updateStateFile();
        file.getParentDirectory().createDirectory();
        file.replaceWithText (juce::JSON::toString (juce::var (object)));
    }

    // Maior policy_serial ja aceito. Sem isto, uma policy ANTIGA e legitimamente
    // assinada poderia ser reapresentada para segurar o app numa versao velha --
    // a assinatura continua valida, o que muda e so a atualidade.
    int highestAcceptedSerial()
    {
        return loadUpdateState().highestPolicySerial;
    }

    void rememberAcceptedSerial (int serial)
    {
        auto state = loadUpdateState();

        if (serial <= state.highestPolicySerial)
            return;

        state.highestPolicySerial = serial;
        saveUpdateState (state);
    }
}

bool verifySignedPolicyEnvelope (const juce::String& payloadBase64,
                                 const juce::String& signatureBase64,
                                 const juce::String& keyId)
{
    // Mesmas checagens do lado Java, na mesma ordem: chave certa, tamanhos
    // certos, e so entao a conta cara da assinatura.
    if (keyId != juce::String (policyKeyId)
         || payloadBase64.isEmpty()
         || payloadBase64.length() > maximumPayloadBytes * 2
         || signatureBase64.length() != 512)
        return false;

    juce::MemoryOutputStream payload, signature;

    if (! juce::Base64::convertFromBase64 (payload, payloadBase64)
         || payload.getDataSize() == 0
         || payload.getDataSize() > (size_t) maximumPayloadBytes
         || ! juce::Base64::convertFromBase64 (signature, signatureBase64)
         || signature.getDataSize() != (size_t) signatureBytes)
        return false;

    juce::String error;
    return verifyUpdateRs256Signature (
        juce::MemoryBlock (payload.getData(), payload.getDataSize()),
        juce::MemoryBlock (signature.getData(), signature.getDataSize()),
        error);
}

bool parseSignedPolicy (const juce::String& envelopeJson,
                        Policy& policy,
                        juce::String& error)
{
    policy = {};

    if (envelopeJson.isEmpty()
         || envelopeJson.getNumBytesAsUTF8() > (size_t) maximumPolicyBytes)
    {
        error = "Politica vazia ou grande demais";
        return false;
    }

    juce::var parsed;
    if (juce::JSON::parse (envelopeJson, parsed).failed())
    {
        error = "JSON da politica invalido";
        return false;
    }

    auto* envelope = parsed.getDynamicObject();
    if (envelope == nullptr || envelope->getProperties().size() != 5)
    {
        error = "Envelope assinado invalido";
        return false;
    }

    juce::int64 envelopeSchema = 0;
    const auto algorithm       = envelope->getProperty ("algorithm").toString().trim();
    const auto keyId           = envelope->getProperty ("key_id").toString().trim();
    const auto payloadBase64   = envelope->getProperty ("payload").toString().trim();
    const auto signatureBase64 = envelope->getProperty ("signature").toString().trim();

    const juce::String base64Alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=";

    if (! readBoundedInt (envelope->getProperty ("schema_version"), 1000000000, envelopeSchema)
         || envelopeSchema != 1
         || algorithm != juce::String (policyAlgorithm)
         || keyId != juce::String (policyKeyId)
         || payloadBase64.isEmpty()
         || payloadBase64.length() > maximumPayloadBytes * 2
         || payloadBase64.length() % 4 != 0
         || signatureBase64.length() != 512          // 384 bytes em Base64
         || ! payloadBase64.containsOnly (base64Alphabet.toRawUTF8())
         || ! signatureBase64.containsOnly (base64Alphabet.toRawUTF8()))
    {
        error = "Algoritmo, chave ou Base64 da politica rejeitados";
        return false;
    }

    juce::MemoryOutputStream decodedPayload, decodedSignature;
    if (! juce::Base64::convertFromBase64 (decodedPayload, payloadBase64)
         || decodedPayload.getDataSize() == 0
         || decodedPayload.getDataSize() > (size_t) maximumPayloadBytes
         || ! juce::Base64::convertFromBase64 (decodedSignature, signatureBase64)
         || decodedSignature.getDataSize() != (size_t) signatureBytes)
    {
        error = "Base64 da politica invalido";
        return false;
    }

    const juce::MemoryBlock message  (decodedPayload.getData(), decodedPayload.getDataSize());
    const juce::MemoryBlock signature (decodedSignature.getData(), decodedSignature.getDataSize());

    if (! verifyUpdateRs256Signature (message, signature, error))
        return false;

    // Daqui para baixo o conteudo e autentico -- mas ainda precisa ser sensato.
    juce::var parsedPayload;
    const auto payloadJson = juce::String::fromUTF8 (
        static_cast<const char*> (decodedPayload.getData()),
        static_cast<int> (decodedPayload.getDataSize()));

    if (juce::JSON::parse (payloadJson, parsedPayload).failed())
    {
        error = "Payload assinado nao e JSON valido";
        return false;
    }

    auto* object = parsedPayload.getDynamicObject();
    if (object == nullptr || object->getProperties().size() != 17)
    {
        error = "Payload com conjunto de campos inesperado";
        return false;
    }

    juce::int64 schema = 0, serial = 0, revokes = 0, latest = 0, minimum = 0;
    juce::int64 issued = 0, expires = 0, installerSize = 0;

    const auto product     = object->getProperty ("product_id").toString().trim();
    const auto mode        = object->getProperty ("mode").toString().trim();
    const auto versionName = object->getProperty ("latest_version_name").toString().trim();
    const auto message8    = object->getProperty ("update_message").toString();

    const juce::String urlField  = juce::String (platformField) + "_installer_url";
    const juce::String hashField = juce::String (platformField) + "_installer_sha256";
    const juce::String sizeField = juce::String (platformField) + "_installer_bytes";

    const auto installerUrl  = object->getProperty (urlField).toString().trim();
    const auto installerHash = object->getProperty (hashField).toString().trim().toLowerCase();

    if (! readBoundedInt (object->getProperty ("schema_version"), 1000000000, schema)
         || ! readBoundedInt (object->getProperty ("policy_serial"), 1000000000, serial)
         || ! readBoundedInt (object->getProperty ("revokes_policy_serial"), 1000000000, revokes)
         || ! readBoundedInt (object->getProperty ("latest_version_code"), 1000000000, latest)
         || ! readBoundedInt (object->getProperty ("minimum_version_code"), 1000000000, minimum)
         || ! readBoundedInt (object->getProperty ("issued_at"), 4102444800LL, issued)
         || ! readBoundedInt (object->getProperty ("expires_at"), 4102444800LL, expires)
         || ! readBoundedInt (object->getProperty (sizeField), maximumInstallerBytes, installerSize))
    {
        error = "Campos numericos da politica rejeitados";
        return false;
    }

    if (schema != 1
         || product != juce::String (productId)
         || serial < 1
         || latest < 1
         || minimum < 1
         || latest < minimum
         || expires <= issued
         || ! isSafeVersionName (versionName)
         || ! isSafeMessage (message8)
         || (mode != "normal" && mode != "rollback")
         || (mode == "normal"   && revokes != 0)
         || (mode == "rollback" && revokes < 1))
    {
        error = "Valores da politica incoerentes";
        return false;
    }

    // Reapresentacao de policy velha: recusar. Um rollback legitimo e assinado
    // com serial MAIOR e declara qual serial esta revogando, entao ele passa.
    const int knownSerial = highestAcceptedSerial();
    if (static_cast<int> (serial) < knownSerial)
    {
        error = "Politica mais antiga que a ultima aceita (serial "
                 + juce::String (serial) + " < " + juce::String (knownSerial) + ")";
        return false;
    }

    policy.schemaVersion       = static_cast<int> (schema);
    policy.policySerial        = static_cast<int> (serial);
    policy.revokesPolicySerial = static_cast<int> (revokes);
    policy.latestVersionCode   = static_cast<int> (latest);
    policy.minimumVersionCode  = static_cast<int> (minimum);
    policy.issuedAt            = issued;
    policy.expiresAt           = expires;
    policy.mode                = mode;
    policy.latestVersionName   = versionName;
    policy.updateMessage       = message8;

    // Um instalador so entra se a URL for nossa e o hash tiver forma de SHA-256.
    if (isTrustedHttpsUrl (installerUrl) && isSha256Hex (installerHash) && installerSize > 0)
    {
        policy.installerUrl    = installerUrl;
        policy.installerSha256 = installerHash;
        policy.installerBytes  = installerSize;
    }

    policy.valid = true;
    rememberAcceptedSerial (policy.policySerial);
    return true;
}

//==============================================================================
// 3. BUSCA NA REDE E DECISAO
//==============================================================================
Decision fetchPolicy (Policy& policy, juce::String& error)
{
    error.clear();
    policy = {};

    juce::StringPairArray responseHeaders;
    int statusCode = 0;

    // A PRIMEIRA conexao HTTPS do processo costuma falhar no Windows.
    //
    // Medido em 31/07/2026: no mesmo arranque, a busca da politica de
    // update falhava, a da politica de distribuicao falhava, e a do
    // catalogo — a terceira — funcionava. Servidor respondendo em ~110 ms
    // o tempo todo (seis medicoes seguidas com HTTP 200). O que falha e o
    // WinINet: a sessao e aberta com INTERNET_OPEN_TYPE_PRECONFIG e a
    // deteccao de proxy do sistema so fica pronta depois da primeira
    // tentativa. Resultado pratico: o aviso de atualizacao NUNCA aparecia,
    // e o cliente ficava numa versao velha sem saber que havia outra.
    //
    // Tres tentativas com uma pausa curta cobrem o aquecimento sem atrasar
    // o arranque de quem esta offline (o timeout de conexao continua o
    // mesmo; offline falha rapido nas tres).
    const juce::URL url (policyUrl);
    std::unique_ptr<juce::InputStream> stream;

    for (int tentativa = 0; tentativa < 3 && stream == nullptr; ++tentativa)
    {
        if (tentativa > 0)
            juce::Thread::sleep (350);

        responseHeaders.clear();
        statusCode = 0;

        stream = url.createInputStream (
            juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                .withConnectionTimeoutMs (policyTimeoutMs)
                .withResponseHeaders (&responseHeaders)
                .withStatusCode (&statusCode)
                .withNumRedirectsToFollow (2));
    }

    if (stream == nullptr)
    {
        error = "Sem resposta do servidor de atualizacao";
        return Decision::unavailable;
    }

    if (statusCode != 0 && statusCode != 200)
    {
        error = "Servidor de atualizacao respondeu " + juce::String (statusCode);
        return Decision::unavailable;
    }

    juce::MemoryOutputStream body;
    body.writeFromInputStream (*stream, maximumPolicyBytes + 1);

    if (body.getDataSize() == 0 || body.getDataSize() > (size_t) maximumPolicyBytes)
    {
        error = "Resposta vazia ou grande demais";
        return Decision::unavailable;
    }

    if (! parseSignedPolicy (body.toString(), policy, error))
        return Decision::unavailable;

    // Relogio: preferir a hora do SERVIDOR quando ela vier no cabecalho. O
    // relogio local pode estar errado -- e num PC de igreja frequentemente esta.
    juce::int64 now = juce::Time::getCurrentTime().toMilliseconds() / 1000;
    const auto serverDate = responseHeaders.getValue ("Date", {});
    if (serverDate.isNotEmpty())
    {
        const auto parsed = juce::Time::fromISO8601 (serverDate).toMilliseconds() / 1000;
        if (parsed > 1700000000)   // sanidade: depois de nov/2023
            now = parsed;
    }

    const auto decision = decideFor (policy, now, thisVersionCode, error);

    // TRAVA DE LOOP.
    //
    // Se a policy exigir uma versao que o instalador publicado nao entrega --
    // um minimum_version_code digitado alto demais, ou um binario antigo subido
    // por engano no lugar do novo -- o app instalaria, reabriria ainda abaixo do
    // minimo, instalaria de novo, para sempre. O musico veria o ChurchDAW
    // reiniciando sozinho sem parar, e nada indicaria o motivo.
    //
    // Entao: se este SHA-256 exato ja foi executado e ainda estamos abaixo do
    // minimo, ele nao resolve. Vira aviso. Um instalador NOVO (outro hash) e
    // tentado normalmente.
    if (decision == Decision::mandatory
         && policy.installerSha256 == loadUpdateState().lastInstalledSha256)
    {
        error = "Instalador " + policy.installerSha256.substring (0, 12)
                 + " ja foi executado e a versao continua abaixo do minimo";
        juce::Logger::writeToLog ("[update] " + error);
        return Decision::optional;
    }

    return decision;
}

void rememberInstallerWasRun (const juce::String& installerSha256)
{
    if (! isSha256Hex (installerSha256.toLowerCase()))
        return;

    auto state = loadUpdateState();
    state.lastInstalledSha256 = installerSha256.toLowerCase();
    saveUpdateState (state);
}

Decision decideFor (const Policy& policy,
                    juce::int64 nowEpochSeconds,
                    int installedVersionCode,
                    juce::String& reason)
{
    reason.clear();

    if (! policy.valid)
    {
        reason = "Politica nao validada";
        return Decision::unavailable;
    }

    if (nowEpochSeconds > policy.expiresAt)
    {
        // Falha para o lado seguro: policy vencida NUNCA bloqueia. Se pararmos
        // de publicar, o app continua funcionando -- ele nao vira refem de um
        // servidor que talvez nem exista mais daqui a alguns anos.
        reason = "Politica vencida em "
                  + juce::Time (policy.expiresAt * 1000).toString (true, false);
        return Decision::unavailable;
    }

    // Build sem numero de versao (compilacao avulsa) nao pode ser bloqueada:
    // nao ha como provar que ela e velha.
    if (installedVersionCode <= 0)
    {
        reason = "Build sem CHURCHDAW_VERSION_CODE";
        return Decision::unavailable;
    }

    if (installedVersionCode >= policy.latestVersionCode)
        return Decision::upToDate;

    if (installedVersionCode < policy.minimumVersionCode)
    {
        // TRAVA ANTI-TIJOLO. A versao esta abaixo do minimo, mas sem instalador
        // publicado para ESTA plataforma nao existe caminho de saida: obrigar
        // aqui deixaria o musico com um app que so sabe dizer "atualize" e
        // nenhuma forma de atualizar. Entao vira aviso.
        if (! policy.hasInstaller())
        {
            reason = "Abaixo do minimo, mas sem instalador publicado";
            return Decision::optional;
        }

        return Decision::mandatory;
    }

    return Decision::optional;
}

//==============================================================================
// 4. DOWNLOAD COM PROVA DE INTEGRIDADE
//==============================================================================
bool downloadAndVerifyInstaller (const Policy& policy,
                                 juce::File& destination,
                                 juce::String& error,
                                 std::function<void (double)> onProgress)
{
    error.clear();

    if (! policy.valid || ! policy.hasInstaller())
    {
        error = "Politica sem instalador para esta plataforma";
        return false;
    }

    if (! isTrustedHttpsUrl (policy.installerUrl))
    {
        error = "URL do instalador recusada";
        return false;
    }

    const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                            .getChildFile ("ChurchDAWUpdate");
    folder.createDirectory();

    destination = folder.getChildFile (
        juce::URL (policy.installerUrl).getFileName());

    if (destination.getFileName().isEmpty())
        destination = folder.getChildFile ("ChurchDAW-update.bin");

    destination.deleteFile();

    auto stream = juce::URL (policy.installerUrl).createInputStream (
        juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
            .withConnectionTimeoutMs (downloadTimeoutMs)
            .withNumRedirectsToFollow (2));

    if (stream == nullptr)
    {
        error = "Nao foi possivel baixar o instalador";
        return false;
    }

    {
        juce::FileOutputStream output (destination);
        if (! output.openedOk())
        {
            error = "Sem permissao para gravar o instalador temporario";
            return false;
        }

        juce::HeapBlock<char> buffer (65536);
        juce::int64 written = 0;

        while (! stream->isExhausted())
        {
            const int read = stream->read (buffer, 65536);
            if (read <= 0)
                break;

            written += read;
            if (written > policy.installerBytes)
            {
                error = "Instalador maior do que o anunciado";
                destination.deleteFile();
                return false;
            }

            if (! output.write (buffer, (size_t) read))
            {
                error = "Falha ao gravar o instalador (disco cheio?)";
                destination.deleteFile();
                return false;
            }

            if (onProgress != nullptr)
                onProgress ((double) written / (double) policy.installerBytes);
        }
    }

    if (destination.getSize() != policy.installerBytes)
    {
        error = "Instalador incompleto: " + juce::String (destination.getSize())
                 + " de " + juce::String (policy.installerBytes) + " bytes";
        destination.deleteFile();
        return false;
    }

    juce::FileInputStream hashInput (destination);
    if (! hashInput.openedOk())
    {
        error = "Nao foi possivel reler o instalador para conferencia";
        destination.deleteFile();
        return false;
    }

    const auto actual = juce::SHA256 (hashInput).toHexString().toLowerCase();
    if (actual != policy.installerSha256)
    {
        // Ou o download corrompeu, ou alguem trocou o arquivo. Nos dois casos a
        // resposta e a mesma: nao executar.
        error = "SHA-256 do instalador nao confere";
        destination.deleteFile();
        return false;
    }

    return true;
}

//==============================================================================
// 5. SEGUNDA PROVA: O SISTEMA CONFIA NESTE ARQUIVO?
//==============================================================================
bool operatingSystemTrustsInstaller (const juce::File& installer, juce::String& error)
{
    error.clear();

    if (! installer.existsAsFile())
    {
        error = "Instalador nao encontrado";
        return false;
    }

   #if JUCE_WINDOWS
    const auto path = installer.getFullPathName();
    std::wstring widePath (path.toWideCharPointer());

    WINTRUST_FILE_INFO fileInfo {};
    fileInfo.cbStruct      = sizeof (fileInfo);
    fileInfo.pcwszFilePath = widePath.c_str();

    WINTRUST_DATA trustData {};
    trustData.cbStruct            = sizeof (trustData);
    trustData.dwUIChoice          = WTD_UI_NONE;
    trustData.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
    trustData.dwUnionChoice       = WTD_CHOICE_FILE;
    trustData.dwStateAction       = WTD_STATEACTION_VERIFY;
    trustData.pFile               = &fileInfo;

    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG result = WinVerifyTrust (
        static_cast<HWND> (INVALID_HANDLE_VALUE), &action, &trustData);

    trustData.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust (static_cast<HWND> (INVALID_HANDLE_VALUE), &action, &trustData);

    if (result != ERROR_SUCCESS)
    {
       #if ! CHURCHDAW_UPDATE_REQUIRE_OS_SIGNATURE
        // Apenas a AUSENCIA de assinatura e tolerada. TRUST_E_NOSIGNATURE
        // significa "este arquivo nao foi assinado"; qualquer outro codigo
        // significa que existe assinatura e ela NAO confere -- adulteracao,
        // certificado revogado, cadeia quebrada -- e ai a instalacao para.
        // A prova que sustenta o arquivo neste caminho e o SHA-256 vindo da
        // policy RSA, ja conferido antes desta funcao ser chamada.
        if (result == (LONG) TRUST_E_NOSIGNATURE
             || result == (LONG) TRUST_E_PROVIDER_UNKNOWN
             || result == (LONG) TRUST_E_SUBJECT_FORM_UNKNOWN)
        {
            juce::Logger::writeToLog (
                "[update] instalador sem assinatura do sistema (0x"
                 + juce::String::toHexString ((int) result)
                 + "); aceito pelo SHA-256 da policy assinada");
            return true;
        }
       #endif

        error = "Windows nao confia na assinatura do instalador (0x"
                 + juce::String::toHexString ((int) result) + ")";
        return false;
    }

    return true;

   #elif JUCE_MAC
    // O Gatekeeper e quem decide de verdade no macOS -- perguntamos a ele em vez
    // de reimplementar a regra. "--type install" e a avaliacao usada para
    // instaladores e imagens de disco.
    // "--verbose" NAO e enfeite: sem ele o spctl imprime apenas
    // "<caminho>: rejected", e a linha "source=..." — a unica que diz SE a
    // recusa foi por falta de assinatura ou por assinatura quebrada — nunca
    // aparece. O ramo de tolerancia abaixo procura exatamente esse texto,
    // entao sem verbose ele jamais casava e todo DMG ad-hoc virava
    // "Gatekeeper recusou o instalador". Medido no Mac em 31/07/2026:
    // com --verbose=4 a saida traz "source=no usable signature".
    juce::ChildProcess assess;
    juce::StringArray command { "/usr/sbin/spctl", "--assess", "--type", "install",
                                "--verbose=4", installer.getFullPathName() };

    if (! assess.start (command, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
    {
        error = "Nao foi possivel consultar o Gatekeeper";
        return false;
    }

    const auto output = assess.readAllProcessOutput();
    if (! assess.waitForProcessToFinish (20000))
    {
        assess.kill();
        error = "Gatekeeper demorou demais para responder";
        return false;
    }

    if (assess.getExitCode() != 0)
    {
       #if ! CHURCHDAW_UPDATE_REQUIRE_OS_SIGNATURE
        // Mesma regra do Windows: "nao esta assinado" passa, "assinatura
        // quebrada" nao. O spctl diz qual dos dois na propria saida.
        //
        // Registro honesto: no macOS isto NAO torna a atualizacao silenciosa.
        // O Gatekeeper barra o DMG no nivel do sistema quando o usuario o abre,
        // independentemente do que decidirmos aqui. O efeito util deste ramo e
        // o app conseguir baixar, conferir e OFERECER a atualizacao em vez de
        // travar o musico numa tela que exige algo que ele nao consegue fazer.
        const auto lower = output.toLowerCase();
        if (lower.contains ("not signed")
             || lower.contains ("no usable signature")
             || lower.contains ("unsigned"))
        {
            juce::Logger::writeToLog (
                "[update] DMG sem assinatura do sistema; aceito pelo SHA-256 "
                "da policy assinada -- o Gatekeeper ainda pedira confirmacao");
            return true;
        }
       #endif

        error = "Gatekeeper recusou o instalador: " + output.trim();
        return false;
    }

    return true;

   #else
    error = "Verificacao de assinatura indisponivel nesta plataforma";
    return false;
   #endif
}

//==============================================================================
// 6. ENTREGA AO SISTEMA
//==============================================================================
bool launchInstaller (const juce::File& installer, juce::String& error)
{
    error.clear();

    if (! installer.existsAsFile())
    {
        error = "Instalador nao encontrado";
        return false;
    }

   #if JUCE_MAC
    // Um .dmg precisa ser montado; quem faz isso e o Finder.
    // (nome "opener", nao "open": uma variavel chamada open sombreia a funcao
    // POSIX de mesmo nome e e uma armadilha esperando alguem editar isto)
    juce::ChildProcess opener;
    if (! opener.start (juce::StringArray { "/usr/bin/open", installer.getFullPathName() }))
    {
        error = "Nao foi possivel abrir o instalador";
        return false;
    }
    return true;
   #else
    // /SILENT   -- o usuario ja confirmou na nossa janela; nao ha o que
    //              perguntar de novo, e "automatica" era o pedido.
    // /RELAUNCH -- o instalador reabre o ChurchDAW ao terminar, entao de quem
    //              esta na igreja o app apenas reinicia atualizado.
    //
    // O instalador grava no perfil do usuario: nao ha UAC no caminho.
    if (! installer.startAsProcess ("/SILENT /RELAUNCH"))
    {
        error = "Nao foi possivel iniciar o instalador";
        return false;
    }
    return true;
   #endif
}

//==============================================================================
// 7. QUANDO PODE IMPOR
//==============================================================================
bool mayEnforceNow (bool audioDeviceOpen, int activeVoices, bool startupPhase) noexcept
{
    // Uma nota soando ja e motivo suficiente para nao interromper: pode ser o
    // ensaio, pode ser o culto. So o arranque e seguro.
    if (activeVoices > 0)
        return false;

    if (! startupPhase)
        return false;

    juce::ignoreUnused (audioDeviceOpen);
    return true;
}

//==============================================================================
// 8. ORQUESTRACAO DO ARRANQUE
//
// Este JUCE compila com JUCE_MODAL_LOOPS_PERMITTED=0 -- nao existe
// runModalLoop() nem AlertWindow::showMessageBox() para parar e esperar o
// usuario. Todo o fluxo e uma maquina de estados em callbacks do
// MessageManager:
//
//   buscar policy -> perguntar -> baixar -> conferir assinatura -> instalar
//
// Cada etapa pode terminar em finish(false), que simplesmente deixa o
// ChurchDAW abrir. So o caminho completo, com todas as provas conferidas,
// chega a finish(true).
//==============================================================================
namespace
{
    // O JUCE numera os botoes do fim para o comeco: com dois botoes o PRIMEIRO
    // devolve 1 e o segundo (o de escape) devolve 0. Ver
    // LookAndFeel_V2::createAlertWindow.
    constexpr int firstButtonResult = 1;

    class UpdateGate;
    std::unique_ptr<UpdateGate> activeGate;

    class UpdateGate
    {
    public:
        explicit UpdateGate (std::function<void (bool)> callback)
            : onFinished (std::move (callback))
        {
        }

        void start()
        {
            fetchJob = std::make_unique<FetchJob> (*this);
            fetchJob->launchThread();
        }

    private:
        //======================================================================
        class FetchJob final : public juce::ThreadWithProgressWindow
        {
        public:
            explicit FetchJob (UpdateGate& owner)
                : juce::ThreadWithProgressWindow (
                      "ChurchDAW", false, false, 20000, {}, nullptr),
                  gate (owner)
            {
                setStatusMessage (juce::String::fromUTF8 ("Verificando atualiza\xc3\xa7\xc3\xb5""es..."));
            }

            void run() override
            {
                gate.decision = fetchPolicy (gate.policy, gate.error);
            }

            void threadComplete (bool) override   { gate.policyArrived(); }

        private:
            UpdateGate& gate;
        };

        //======================================================================
        class DownloadJob final : public juce::ThreadWithProgressWindow
        {
        public:
            DownloadJob (UpdateGate& owner, bool allowCancel)
                : juce::ThreadWithProgressWindow (
                      "ChurchDAW", true, allowCancel, 20000,
                      allowCancel ? juce::String ("Cancelar") : juce::String(),
                      nullptr),
                  gate (owner)
            {
                setStatusMessage (baseMessage());
            }

            void run() override
            {
                gate.downloadOk = downloadAndVerifyInstaller (
                    gate.policy, gate.installer, gate.error,
                    [this] (double fraction)
                    {
                        setProgress (fraction);
                        setStatusMessage (baseMessage() + " "
                            + juce::String (juce::roundToInt (fraction * 100.0)) + "%");
                    });

                if (gate.downloadOk && ! threadShouldExit())
                {
                    setProgress (-1.0);
                    setStatusMessage (juce::String::fromUTF8 (
                        "Conferindo a assinatura do instalador..."));

                    // Segunda prova. Um arquivo com o SHA-256 certo mas sem
                    // assinatura do sistema nao roda: os dois elos, ou nenhum.
                    gate.downloadOk = operatingSystemTrustsInstaller (
                        gate.installer, gate.error);

                    if (! gate.downloadOk)
                        gate.installer.deleteFile();
                }
            }

            void threadComplete (bool userCancelled) override
            {
                gate.downloadFinished (userCancelled);
            }

        private:
            juce::String baseMessage() const
            {
                return juce::String::fromUTF8 ("Baixando a atualiza\xc3\xa7\xc3\xa3o ")
                        + gate.policy.latestVersionName + "...";
            }

            UpdateGate& gate;
        };

        //======================================================================
        void policyArrived()
        {
            fetchJob = nullptr;

            if (decision == Decision::unavailable)
            {
                // Silencio proposital. Um PC de igreja sem internet nao pode
                // receber aviso nenhum por causa disso.
                juce::Logger::writeToLog ("[update] indisponivel: " + error);
                finish (false);
                return;
            }

            if (decision == Decision::upToDate)
            {
                finish (false);
                return;
            }

            mandatory = (decision == Decision::mandatory);

            if (! policy.hasInstaller())
            {
                // Nao deveria acontecer no caminho obrigatorio (fetchPolicy so
                // devolve mandatory com instalador), mas o caminho opcional
                // chega aqui quando ainda nao ha binario publicado.
                juce::Logger::writeToLog (
                    "[update] versao " + policy.latestVersionName
                     + " anunciada sem instalador para esta plataforma");
                finish (false);
                return;
            }

            askUser();
        }

        //======================================================================
        void askUser()
        {
            juce::String text;
            text << juce::String::fromUTF8 ("Vers\xc3\xa3o instalada: ")
                 << versionText (thisVersionCode) << "\n"
                 << juce::String::fromUTF8 ("Nova vers\xc3\xa3o: ")
                 << policy.latestVersionName << "\n\n";

            if (policy.updateMessage.isNotEmpty())
                text << policy.updateMessage << "\n\n";

            auto options = juce::MessageBoxOptions()
                .withIconType (juce::MessageBoxIconType::InfoIcon);

            if (mandatory)
            {
                text << juce::String::fromUTF8 (
                    "Esta atualiza\xc3\xa7\xc3\xa3o \xc3\xa9 obrigat\xc3\xb3ria para continuar usando o ChurchDAW.");

                options = options
                    .withTitle (juce::String::fromUTF8 ("Atualiza\xc3\xa7\xc3\xa3o obrigat\xc3\xb3ria"))
                    .withMessage (text)
                    .withButton (juce::String::fromUTF8 ("Atualizar agora"));
            }
            else
            {
                text << juce::String::fromUTF8 ("Deseja atualizar agora?");

                options = options
                    .withTitle (juce::String::fromUTF8 ("Atualiza\xc3\xa7\xc3\xa3o dispon\xc3\xadvel"))
                    .withMessage (text)
                    .withButton (juce::String::fromUTF8 ("Atualizar agora"))
                    .withButton ("Depois");
            }

            const bool isMandatory = mandatory;
            juce::AlertWindow::showAsync (options,
                [this, isMandatory] (int result)
                {
                    // Com um botao so, qualquer resultado significa "vai".
                    if (! isMandatory && result != firstButtonResult)
                    {
                        finish (false);
                        return;
                    }

                    beginDownload();
                });
        }

        //======================================================================
        void beginDownload()
        {
            // Cancelar so existe no caminho opcional. No obrigatorio a saida e
            // pela falha honesta (ver downloadFinished), nunca por desistencia.
            downloadJob = std::make_unique<DownloadJob> (*this, ! mandatory);
            downloadJob->launchThread();
        }

        //======================================================================
        void downloadFinished (bool userCancelled)
        {
            downloadJob = nullptr;

            if (userCancelled)
            {
                installer.deleteFile();
                finish (false);
                return;
            }

            if (! downloadOk)
            {
                // TRAVA ANTI-TIJOLO. Falhou o download ou a assinatura: avisa e
                // ABRE. Deixar o musico sem instrumento por causa de uma queda
                // de rede seria estrago maior do que rodar a versao antiga por
                // mais um dia.
                juce::Logger::writeToLog ("[update] falha: " + error);
                showAndFinish (
                    juce::String::fromUTF8 ("N\xc3\xa3o foi poss\xc3\xadvel atualizar"),
                    error + juce::String::fromUTF8 (
                        "\n\nO ChurchDAW vai abrir na vers\xc3\xa3o atual e tentar\xc3\xa1 de novo depois."),
                    false);
                return;
            }

            // Antes de executar, nao depois: se o instalador reiniciar a
            // maquina ou matar este processo, o registro precisa ja existir --
            // senao a trava de loop nunca chega a ser gravada.
            rememberInstallerWasRun (policy.installerSha256);

            juce::String launchError;
            if (! launchInstaller (installer, launchError))
            {
                showAndFinish (
                    juce::String::fromUTF8 ("N\xc3\xa3o foi poss\xc3\xadvel iniciar o instalador"),
                    launchError + juce::String::fromUTF8 (
                        "\n\nO ChurchDAW vai abrir na vers\xc3\xa3o atual."),
                    false);
                return;
            }

            // Instalador rodando: o app precisa sair para ele poder substituir
            // os arquivos que estao em uso.
            finish (true);
        }

        //======================================================================
        void showAndFinish (const juce::String& title,
                            const juce::String& message,
                            bool shouldQuit)
        {
            auto options = juce::MessageBoxOptions()
                .withIconType (juce::MessageBoxIconType::WarningIcon)
                .withTitle (title)
                .withMessage (message)
                .withButton ("OK");

            juce::AlertWindow::showAsync (options,
                [this, shouldQuit] (int) { finish (shouldQuit); });
        }

        void finish (bool shouldQuit)
        {
            auto callback = std::move (onFinished);

            // Este metodo e chamado de dentro de um callback de um job que ESTE
            // objeto possui. Soltar no proximo ciclo do MessageManager evita
            // destruir o job durante a propria pilha dele.
            juce::MessageManager::callAsync ([] { activeGate.reset(); });

            if (callback != nullptr)
                callback (shouldQuit);
        }

        static juce::String versionText (int code)
        {
            return juce::String (code / 10000) + "."
                 + juce::String ((code / 100) % 100) + "."
                 + juce::String (code % 100);
        }

        //======================================================================
        std::function<void (bool)> onFinished;
        std::unique_ptr<FetchJob> fetchJob;
        std::unique_ptr<DownloadJob> downloadJob;

        Policy policy;
        juce::String error;
        Decision decision = Decision::unavailable;
        juce::File installer;
        bool downloadOk = false;
        bool mandatory = false;
    };
}

void runStartupUpdateGate (std::function<void (bool)> onFinished)
{
   #if ! (JUCE_WINDOWS || JUCE_MAC)
    if (onFinished != nullptr)
        onFinished (false);
   #else
    if (activeGate != nullptr)     // ja em andamento: nao empilhar dois portoes
    {
        if (onFinished != nullptr)
            onFinished (false);

        return;
    }

    activeGate = std::make_unique<UpdateGate> (std::move (onFinished));
    activeGate->start();
   #endif
}

}   // namespace ChurchDAWDesktopUpdate
