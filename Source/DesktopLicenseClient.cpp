#include "DesktopLicenseClient.h"
#include "DesktopLeaseVerifier.h"
#include "DesktopSecureRandom.h"

#include <array>
#include <cstring>

namespace ChurchDAWDesktopLicensing
{
namespace
{
#ifndef CHURCHDAW_LICENSE_API_BASE
 #define CHURCHDAW_LICENSE_API_BASE "https://churchdaw.com/api/v1"
#endif

constexpr auto deviceCredentialAccount = "desktop-device-v1";
constexpr auto sessionCredentialAccount = "desktop-session-v1";
constexpr int maximumResponseBytes = 128 * 1024;

juce::String apiBase()
{
    return juce::String (CHURCHDAW_LICENSE_API_BASE).trimCharactersAtEnd ("/");
}

juce::String normaliseEmail (juce::String value)
{
    return value.trim().toLowerCase();
}

juce::String deviceLabel()
{
    auto name = juce::SystemStats::getComputerName().trim();
    if (name.isEmpty())
        name = "Computador";

    if (name.length() > 48)
        name = name.substring (0, 48);

    return name + " / " + juce::SystemStats::getOperatingSystemName();
}

juce::var makeObject()
{
    return juce::var (new juce::DynamicObject());
}

juce::DynamicObject* objectOf (juce::var& value)
{
    return value.getDynamicObject();
}

juce::DynamicObject* objectOf (const juce::var& value)
{
    return value.getDynamicObject();
}

juce::String propertyString (const juce::var& value, const juce::Identifier& key)
{
    if (const auto* object = objectOf (value))
        return object->getProperty (key).toString();

    return {};
}

juce::int64 propertyInt64 (const juce::var& value,
                           const juce::Identifier& key)
{
    if (const auto* object = objectOf (value))
        return static_cast<juce::int64> (object->getProperty (key));

    return 0;
}

void setProperty (juce::var& value,
                  const juce::Identifier& key,
                  const juce::var& property)
{
    if (auto* object = objectOf (value))
        object->setProperty (key, property);
}

juce::String readBoundedText (juce::InputStream& stream)
{
    juce::MemoryOutputStream output;
    std::array<char, 4096> buffer {};

    while (! stream.isExhausted() && output.getDataSize() <= maximumResponseBytes)
    {
        const auto remaining = maximumResponseBytes
                             - static_cast<int> (output.getDataSize());
        const auto amount = stream.read (buffer.data(),
                                         juce::jmin ((int) buffer.size(),
                                                     remaining));
        if (amount <= 0)
            break;

        output.write (buffer.data(), static_cast<size_t> (amount));
    }

    if (output.getDataSize() >= maximumResponseBytes)
        return {};

    return output.toString();
}
}

Client::Client()
    : juce::Thread ("ChurchDAW desktop licensing")
{
}

Client::~Client()
{
    callbackAlive->store (false);
    stopThread (-1);

    const juce::ScopedLock scoped (lock);
    pendingPassword.clear();
}

void Client::resumeSavedSession()
{
    if (isThreadRunning())
        return;

    Snapshot next;
    next.state = State::resuming;
    next.busy = true;
    next.message = "Validando sua sessao protegida...";
    publish (std::move (next));
    beginCommand (Command::resume);
}

void Client::login (juce::String email,
                    juce::String password,
                    bool rememberSession)
{
    if (isThreadRunning())
        return;

    email = normaliseEmail (std::move (email));

    if (! email.containsChar ('@') || password.length() < 8)
    {
        Snapshot next;
        next.state = State::signedOut;
        next.email = email;
        next.message = "Confira o e-mail e a senha.";
        publish (std::move (next));
        return;
    }

    Snapshot next;
    {
        const juce::ScopedLock scoped (lock);
        pendingEmail = std::move (email);
        pendingPassword = std::move (password);
        pendingRemember = rememberSession;
        next.state = State::signingIn;
        next.email = pendingEmail;
        next.busy = true;
        next.message = "Protegendo e validando seu acesso...";
    }

    publish (std::move (next));
    beginCommand (Command::login);
}

void Client::requestPasswordReset (juce::String email)
{
    if (isThreadRunning())
        return;

    email = normaliseEmail (std::move (email));

    if (! email.containsChar ('@'))
    {
        Snapshot next = currentSnapshot();
        next.state = State::signedOut;
        next.busy = false;
        next.message = "Digite seu e-mail para recuperar o acesso.";
        publish (std::move (next));
        return;
    }

    Snapshot next = currentSnapshot();
    {
        const juce::ScopedLock scoped (lock);
        pendingEmail = std::move (email);
        pendingPassword.clear();
        next.email = pendingEmail;
    }

    next.state = State::resuming;
    next.busy = true;
    next.message = "Solicitando recuperacao segura...";
    publish (std::move (next));
    beginCommand (Command::passwordReset);
}

void Client::signOut()
{
    stopThread (-1);
    removeSavedRefreshToken();

    Snapshot next;
    next.state = State::signedOut;
    next.message = "Sessao encerrada neste computador.";
    publish (std::move (next));
}

Snapshot Client::currentSnapshot() const
{
    const juce::ScopedLock scoped (lock);
    return snapshot;
}

juce::URL Client::purchaseUrl()
{
    return juce::URL ("https://churchdaw.com/comprar");
}

juce::URL Client::accountPortalUrl()
{
    return juce::URL ("https://churchdaw.com/minha-conta");
}

void Client::beginCommand (Command next)
{
    jassert (! isThreadRunning());
    {
        const juce::ScopedLock scoped (lock);
        command = next;
    }
    startThread (juce::Thread::Priority::normal);
}

void Client::run()
{
    Command commandToRun;
    {
        const juce::ScopedLock scoped (lock);
        commandToRun = command;
    }

    switch (commandToRun)
    {
        case Command::resume:        runResume();        break;
        case Command::login:         runLogin();         break;
        case Command::passwordReset: runPasswordReset(); break;
        case Command::none:                                  break;
    }

    {
        const juce::ScopedLock scoped (lock);
        command = Command::none;
    }
}

void Client::runResume()
{
    juce::String error;
    const auto saved = loadSavedSession (error);

    if (saved.refreshToken.isEmpty())
    {
        Snapshot next;
        next.state = State::signedOut;
        next.email = saved.email;
        next.message = error.isNotEmpty()
                         ? "A sessao salva nao pode ser aberta. Entre novamente."
                         : "Entre para ativar este computador.";
        publish (std::move (next));
        return;
    }

    const auto device = ensureDeviceIdentity (error);
    if (! device.valid())
    {
        Snapshot next;
        next.state = State::signedOut;
        next.email = saved.email;
        next.message = "Nao foi possivel proteger a identidade deste computador.";
        publish (std::move (next));
        return;
    }

    const auto now = juce::Time::getCurrentTime().toMilliseconds() / 1000;
    const auto nonce = randomToken (24);
    if (nonce.isEmpty())
    {
        Snapshot next;
        next.state = State::serverUnavailable;
        next.email = saved.email;
        next.message = "Nao foi possivel criar uma solicitacao segura.";
        publish (std::move (next));
        return;
    }
    const auto proofMessage = device.installationId + "|"
                            + juce::String (now) + "|"
                            + nonce + "|" + saved.refreshToken;

    auto payload = makeObject();
    setProperty (payload, "refresh_token", saved.refreshToken);
    setProperty (payload, "installation_id", device.installationId);
    setProperty (payload, "timestamp", now);
    setProperty (payload, "nonce", nonce);
    setProperty (payload, "proof",
                 hmacSha256Base64 (device.secretBase64, proofMessage));

    if (threadShouldExit())
        return;

    const auto result = postJson ("/auth/refresh", payload);
    if ((result.status == 0 || result.status >= 500)
        && acceptOfflineLease (saved, device.installationId))
    {
        juce::Logger::writeToLog (
            "LICENCA DESKTOP: servidor indisponivel; lease offline aceita"
            " (HTTP " + juce::String (result.status) + ")");
        return;
    }

    acceptAuthResponse (result, true, device.installationId);
}

void Client::runLogin()
{
    juce::String email;
    juce::String password;
    bool remember = true;

    {
        const juce::ScopedLock scoped (lock);
        email = pendingEmail;
        password = pendingPassword;
        remember = pendingRemember;
        pendingPassword.clear();
    }

    juce::String error;
    const auto device = ensureDeviceIdentity (error);
    if (! device.valid())
    {
        Snapshot next;
        next.state = State::signedOut;
        next.email = email;
        next.message = "Nao foi possivel proteger a identidade deste computador.";
        publish (std::move (next));
        return;
    }

    auto payload = makeObject();
    setProperty (payload, "email", email);
    setProperty (payload, "password", password);
    setProperty (payload, "installation_id", device.installationId);
    setProperty (payload, "device_secret", device.secretBase64);
    setProperty (payload, "device_label", device.label);
    setProperty (payload, "platform",
                #if JUCE_MAC
                 "macos"
                #else
                 "windows"
                #endif
    );
    auto* application = juce::JUCEApplicationBase::getInstance();
    setProperty (payload, "app_version",
                 application != nullptr
                    ? application->getApplicationVersion()
                    : juce::String());

    password.clear();

    if (threadShouldExit())
        return;

    acceptAuthResponse (postJson ("/auth/login", payload),
                        remember,
                        device.installationId);
}

void Client::runPasswordReset()
{
    juce::String email;
    {
        const juce::ScopedLock scoped (lock);
        email = pendingEmail;
    }

    auto payload = makeObject();
    setProperty (payload, "email", email);
    const auto result = postJson ("/auth/password/forgot", payload);

    Snapshot next;
    next.state = State::passwordResetSent;
    next.email = email;
    next.busy = false;
    // Resposta deliberadamente identica para conta existente ou inexistente.
    next.message = "Se este e-mail estiver cadastrado, enviaremos as instrucoes.";

    if (result.status == 0 && result.transportError.isNotEmpty())
        next.message = "Nao foi possivel conectar agora. Tente novamente.";

    publish (std::move (next));
}

Client::DeviceIdentity Client::ensureDeviceIdentity (juce::String& error)
{
    const auto saved = CredentialStore::loadSecret (
        deviceCredentialAccount, error);

    if (saved.isNotEmpty())
    {
        juce::var parsed;
        if (juce::JSON::parse (saved, parsed).wasOk())
        {
            DeviceIdentity existing {
                propertyString (parsed, "installation_id"),
                propertyString (parsed, "device_secret"),
                propertyString (parsed, "device_label")
            };

            if (propertyInt64 (parsed, "schema") == 2 && existing.valid())
                return existing;
        }
    }

    DeviceIdentity created {
        randomToken (16),
        randomToken (32),
        deviceLabel()
    };

    auto value = makeObject();
    setProperty (value, "schema", 2);
    setProperty (value, "installation_id", created.installationId);
    setProperty (value, "device_secret", created.secretBase64);
    setProperty (value, "device_label", created.label);

    if (! CredentialStore::saveSecret (
            deviceCredentialAccount, juce::JSON::toString (value, false), error))
        return {};

    return created;
}

Client::SavedSession Client::loadSavedSession (juce::String& error) const
{
    const auto saved = CredentialStore::loadSecret (
        sessionCredentialAccount, error);

    if (saved.isEmpty())
        return {};

    juce::var parsed;
    if (juce::JSON::parse (saved, parsed).failed())
    {
        error = "Sessao protegida invalida";
        return {};
    }

    return {
        propertyString (parsed, "email"),
        propertyString (parsed, "refresh_token"),
        propertyString (parsed, "lease_token"),
        propertyInt64 (parsed, "trusted_time")
    };
}

bool Client::saveRefreshToken (const juce::String& refreshToken,
                               const juce::String& email,
                               const juce::String& leaseToken,
                               juce::int64 trustedTime,
                               juce::String& error)
{
    auto value = makeObject();
    setProperty (value, "schema", 2);
    setProperty (value, "email", email);
    setProperty (value, "refresh_token", refreshToken);
    setProperty (value, "lease_token", leaseToken);
    setProperty (value, "trusted_time", trustedTime);
    return CredentialStore::saveSecret (
        sessionCredentialAccount, juce::JSON::toString (value, false), error);
}

void Client::removeSavedRefreshToken()
{
    juce::String ignored;
    CredentialStore::removeSecret (sessionCredentialAccount, ignored);
}

Client::HttpResult Client::postJson (const juce::String& route,
                                    const juce::var& payload) const
{
    HttpResult result;
    const auto base = apiBase();

    if (! base.startsWithIgnoreCase ("https://")
       #if CHURCHDAW_LICENSE_ALLOW_HTTP_LOCALHOST
        && ! base.startsWithIgnoreCase ("http://127.0.0.1")
        && ! base.startsWithIgnoreCase ("http://localhost")
       #endif
    )
    {
        result.transportError = "Endpoint de licenca inseguro";
        return result;
    }

    const auto json = juce::JSON::toString (payload, false);
    const auto url = juce::URL (base + route).withPOSTData (json);
    juce::StringPairArray responseHeaders;
    const auto options =
        juce::URL::InputStreamOptions (
            juce::URL::ParameterHandling::inAddress)
            .withHttpRequestCmd ("POST")
            .withConnectionTimeoutMs (10000)
            .withNumRedirectsToFollow (0)
            .withStatusCode (&result.status)
            .withResponseHeaders (&responseHeaders)
            .withExtraHeaders (
                "Content-Type: application/json\r\n"
                "Accept: application/json\r\n"
                "Cache-Control: no-store\r\n"
                "User-Agent: ChurchDAW-Desktop\r\n");

    auto stream = url.createInputStream (options);
    if (stream == nullptr)
    {
        result.transportError = "Falha de conexao";
        return result;
    }

    const auto raw = readBoundedText (*stream);
    if (raw.isEmpty())
    {
        result.transportError = "Resposta vazia ou grande demais";
        return result;
    }

    if (juce::JSON::parse (raw, result.body).failed()
        || result.body.getDynamicObject() == nullptr)
        result.transportError = "Resposta invalida";

    return result;
}

void Client::acceptAuthResponse (const HttpResult& result,
                                 bool persistRefreshToken,
                                 const juce::String& expectedInstallationId)
{
    Snapshot next;
    next.busy = false;

    if (result.status < 200 || result.status >= 300
        || result.body.getDynamicObject() == nullptr)
    {
        const auto code = propertyString (result.body, "code");
        juce::Logger::writeToLog (
            "LICENCA DESKTOP: autenticacao recusada (HTTP "
            + juce::String (result.status) + ", codigo "
            + (code.isNotEmpty() ? code : juce::String ("sem-codigo"))
            + ")");
        next.state = code == "ACTIVATION_LIMIT"
                       ? State::activationConflict
                       : (code == "LICENSE_REVOKED"
                            ? State::revoked
                            : (result.status == 0
                                 ? State::serverUnavailable
                                 : State::signedOut));
        next.message = code == "ACTIVATION_LIMIT"
                         ? "Esta licenca ja esta ativa em outro computador."
                         : (code == "LICENSE_REVOKED"
                              ? "Esta licenca foi suspensa. Fale com o suporte."
                              : genericErrorForStatus (result.status));
        next.email = currentSnapshot().email;
        if (result.status == 401 || result.status == 403
            || code == "LICENSE_REVOKED"
            || code == "SESSION_REUSED")
            removeSavedRefreshToken();
        publish (std::move (next));
        return;
    }

    const auto refreshToken = propertyString (result.body, "refresh_token");
    const auto accessToken = propertyString (result.body, "access_token");
    const auto leaseToken = propertyString (result.body, "lease_token");
    const auto now = juce::Time::getCurrentTime().toMilliseconds() / 1000;
    VerifiedLease verified;
    juce::String verificationError;

    if (accessToken.isEmpty() || refreshToken.isEmpty()
        || leaseToken.isEmpty()
        || ! verifyLeaseToken (leaseToken,
                               expectedInstallationId,
                               now,
                               false,
                               verified,
                               verificationError))
    {
        juce::Logger::writeToLog (
            "LICENCA DESKTOP: resposta 2xx rejeitada (access="
            + juce::String (accessToken.isNotEmpty() ? 1 : 0)
            + ", refresh="
            + juce::String (refreshToken.isNotEmpty() ? 1 : 0)
            + ", lease="
            + juce::String (leaseToken.isNotEmpty() ? 1 : 0)
            + ", verificacao="
            + (verificationError.isNotEmpty()
                   ? verificationError
                   : juce::String ("campos ausentes"))
            + ")");
        next.state = State::signedOut;
        next.email = currentSnapshot().email;
        next.message = "A assinatura da licenca recebida e invalida.";
        publish (std::move (next));
        return;
    }

    if (persistRefreshToken)
    {
        juce::String error;
        if (! saveRefreshToken (refreshToken,
                                verified.email,
                                leaseToken,
                                now,
                                error))
        {
            next.state = State::signedOut;
            next.email = verified.email;
            next.message =
                "Acesso validado, mas a sessao nao pôde ser protegida neste computador.";
            publish (std::move (next));
            return;
        }
    }
    else
    {
        removeSavedRefreshToken();
    }

    next.state = State::active;
    next.email = verified.email;
    next.displayName = verified.displayName;
    next.activationLabel = verified.activationLabel;
    next.leaseExpiresAt = verified.leaseExpiresAt;
    next.offlineUntil = verified.offlineUntil;
    next.canOpenApp = true;
    next.message = "Licenca validada para este computador.";
    publish (std::move (next));
}

bool Client::acceptOfflineLease (
    const SavedSession& saved,
    const juce::String& expectedInstallationId)
{
    if (saved.leaseToken.isEmpty() || saved.refreshToken.isEmpty())
        return false;

    const auto now = juce::Time::getCurrentTime().toMilliseconds() / 1000;
    if (saved.trustedTime > 0 && now + 300 < saved.trustedTime)
    {
        Snapshot next;
        next.state = State::serverUnavailable;
        next.email = saved.email;
        next.message =
            "O relogio do computador voltou no tempo. Conecte-se para validar.";
        publish (std::move (next));
        return true;
    }

    VerifiedLease verified;
    juce::String verificationError;
    if (! verifyLeaseToken (saved.leaseToken,
                            expectedInstallationId,
                            now,
                            true,
                            verified,
                            verificationError))
        return false;

    juce::String saveError;
    if (! saveRefreshToken (saved.refreshToken,
                            verified.email,
                            saved.leaseToken,
                            juce::jmax (saved.trustedTime, now),
                            saveError))
        return false;

    Snapshot next;
    next.state = State::active;
    next.email = verified.email;
    next.displayName = verified.displayName;
    next.activationLabel = verified.activationLabel;
    next.leaseExpiresAt = verified.leaseExpiresAt;
    next.offlineUntil = verified.offlineUntil;
    next.canOpenApp = true;
    next.message =
        "Modo offline protegido. Conecte-se antes do prazo da licenca.";
    publish (std::move (next));
    return true;
}

void Client::publish (Snapshot next)
{
    {
        const juce::ScopedLock scoped (lock);
        snapshot = next;
    }

    auto alive = callbackAlive;
    juce::MessageManager::callAsync (
        [this, alive = std::move (alive), next = std::move (next)]
        {
            if (alive->load() && onSnapshot != nullptr)
                onSnapshot (next);
        });
}

juce::String Client::randomToken (size_t bytes)
{
    juce::MemoryBlock value (bytes, true);
    if (! fillSecureRandom (value.getData(), value.getSize()))
        return {};

    return juce::Base64::toBase64 (value.getData(), value.getSize())
        .replaceCharacter ('+', '-')
        .replaceCharacter ('/', '_')
        .trimCharactersAtEnd ("=");
}

juce::String Client::hmacSha256Base64 (const juce::String& keyBase64,
                                       const juce::String& message)
{
    // The server stores the original Base64URL token and deliberately uses
    // those exact UTF-8 bytes as the HMAC key. Decoding it here produces a
    // different key and makes every otherwise valid refresh fail with
    // INVALID_DEVICE_PROOF.
    juce::MemoryBlock key (
        keyBase64.toRawUTF8(),
        static_cast<size_t> (keyBase64.getNumBytesAsUTF8()));

    constexpr size_t blockSize = 64;
    juce::MemoryBlock normalised (blockSize, true);

    if (key.getSize() > blockSize)
    {
        const auto digest = juce::SHA256 (key).getRawData();
        std::memcpy (normalised.getData(), digest.getData(),
                     juce::jmin (digest.getSize(), blockSize));
    }
    else
    {
        std::memcpy (normalised.getData(), key.getData(), key.getSize());
    }

    std::array<juce::uint8, blockSize> innerPad {};
    std::array<juce::uint8, blockSize> outerPad {};
    const auto* normalisedBytes =
        static_cast<const juce::uint8*> (normalised.getData());

    for (size_t i = 0; i < blockSize; ++i)
    {
        innerPad[i] = static_cast<juce::uint8> (normalisedBytes[i] ^ 0x36);
        outerPad[i] = static_cast<juce::uint8> (normalisedBytes[i] ^ 0x5c);
    }

    juce::MemoryBlock inner;
    inner.append (innerPad.data(), innerPad.size());
    inner.append (message.toRawUTF8(),
                  static_cast<size_t> (message.getNumBytesAsUTF8()));
    const auto innerDigest = juce::SHA256 (inner).getRawData();

    juce::MemoryBlock outer;
    outer.append (outerPad.data(), outerPad.size());
    outer.append (innerDigest.getData(), innerDigest.getSize());
    auto result = juce::SHA256 (outer).getRawData();

    juce::zeromem (normalised.getData(), normalised.getSize());
    juce::zeromem (key.getData(), key.getSize());
    return juce::Base64::toBase64 (result.getData(), result.getSize());
}

juce::String Client::genericErrorForStatus (int status)
{
    if (status == 401 || status == 403)
        return "E-mail, senha ou licenca invalidos.";

    if (status == 429)
        return "Muitas tentativas. Aguarde alguns minutos.";

    if (status >= 500 || status == 0)
        return "O servidor esta indisponivel. Tente novamente.";

    return "Nao foi possivel concluir a validacao.";
}
}
