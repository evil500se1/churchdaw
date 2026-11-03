#pragma once

#if JUCE_ANDROID

#include <juce_gui_extra/juce_gui_extra.h>
#include <array>

#include "ChurchDAWServices.h"

namespace ChurchDAWAndroidLicensing
{
enum class State
{
    signedOut,
    resuming,
    signingIn,
    preparingTrial,
    trialReady,
    active,
    activationConflict,
    passwordResetSent,
    serverUnavailable,
    revoked,
    trialExpired
};

struct Snapshot
{
    State state { State::signedOut };
    juce::String email;
    juce::String displayName;
    juce::String message;
    juce::String activationLabel;
    juce::int64 leaseExpiresAt = 0;
    juce::int64 offlineUntil = 0;
    juce::int64 trialExpiresAt = 0;
    bool canPrepareApp = false;
    bool canOpenApp = false;
    bool isTrial = false;
    bool busy = false;
};

class Client final : private juce::Thread
{
public:
    Client() : juce::Thread ("ChurchDAW Android licensing") {}

    ~Client() override
    {
        callbackAlive->store (false);
        signalThreadShouldExit();
        stopThread (12000);
        const juce::ScopedLock scoped (lock);
        pendingPassword.clear();
    }

    std::function<void (const Snapshot&)> onSnapshot;

    void resumeSavedSession()
    {
        begin (Command::resume, State::resuming,
               "Validando sua sessao protegida...");
    }

    void login (juce::String email, juce::String password)
    {
        if (isThreadRunning())
            return;

        email = email.trim().toLowerCase();
        if (! email.containsChar ('@') || password.length() < 8)
        {
            Snapshot next;
            next.email = email;
            next.message = "Confira o e-mail e a senha.";
            publish (std::move (next));
            return;
        }

        {
            const juce::ScopedLock scoped (lock);
            pendingEmail = std::move (email);
            pendingPassword = std::move (password);
        }
        begin (Command::login, State::signingIn,
               "Protegendo e validando seu acesso...");
    }

    void reserveTrial()
    {
        begin (Command::reserveTrial, State::preparingTrial,
               "Preparando sua demonstracao de 20 minutos...");
    }

    void startReservedTrial()
    {
        begin (Command::startTrial, State::preparingTrial,
               "Iniciando agora os 20 minutos...");
    }

    void requestPasswordReset (juce::String email)
    {
        if (isThreadRunning())
            return;
        email = email.trim().toLowerCase();
        if (! email.containsChar ('@'))
        {
            auto next = currentSnapshot();
            next.message = "Digite seu e-mail para recuperar o acesso.";
            publish (std::move (next));
            return;
        }
        {
            const juce::ScopedLock scoped (lock);
            pendingEmail = std::move (email);
        }
        begin (Command::passwordReset, State::resuming,
               "Solicitando recuperacao segura...");
    }

    void signOut()
    {
        signalThreadShouldExit();
        stopThread (12000);
        javaVoid ("clearSession", "()V");
        {
            const juce::ScopedLock scoped (lock);
            saved = {};
            libraryBearer.clear();
        }
        Snapshot next;
        next.message = "Sessao encerrada neste aparelho.";
        publish (std::move (next));
    }

    Snapshot currentSnapshot() const
    {
        const juce::ScopedLock scoped (lock);
        return snapshot;
    }

    juce::String libraryAuthorizationToken() const
    {
        const juce::ScopedLock scoped (lock);
        return libraryBearer;
    }

    juce::int64 trustedNowSeconds() const
    {
        auto* env = juce::getEnv();
        auto clazz = bridgeClass (env);
        if (env == nullptr || clazz == nullptr)
            return -1;
        const auto method = env->GetStaticMethodID (
            clazz, "trustedNowSeconds", "()J");
        const auto value = method != nullptr
            ? (juce::int64) env->CallStaticLongMethod (clazz, method)
            : -1;
        if (env->ExceptionCheck())
            env->ExceptionClear();
        env->DeleteLocalRef (clazz);
        return value;
    }

    static juce::URL accountPortalUrl()
    {
        return juce::URL ("https://churchdaw.com/minha-conta");
    }

private:
    enum class Command
    {
        none,
        resume,
        login,
        reserveTrial,
        startTrial,
        passwordReset
    };

    struct DeviceIdentity
    {
        juce::String installationId;
        juce::String publicKey;
        juce::String fingerprint;
        juce::String label;

        bool valid() const
        {
            return installationId.length() >= 16
                && publicKey.length() >= 80
                && fingerprint.length() >= 8
                && label.isNotEmpty();
        }
    };

    struct Session
    {
        juce::String mode;
        juce::String email;
        juce::String refreshToken;
        juce::String leaseToken;
        juce::String setupToken;
        juce::int64 serverTime = 0;
    };

    struct HttpResult
    {
        int status = 0;
        juce::var body;
        juce::String transportError;
    };

    static juce::var object()
    {
        return juce::var (new juce::DynamicObject());
    }

    static juce::String stringProperty (
        const juce::var& value, const juce::Identifier& key)
    {
        if (const auto* item = value.getDynamicObject())
            return item->getProperty (key).toString();
        return {};
    }

    static juce::int64 intProperty (
        const juce::var& value, const juce::Identifier& key)
    {
        if (const auto* item = value.getDynamicObject())
            return static_cast<juce::int64> (item->getProperty (key));
        return 0;
    }

    static void set (
        juce::var& target, const juce::Identifier& key,
        const juce::var& value)
    {
        if (auto* item = target.getDynamicObject())
            item->setProperty (key, value);
    }

    static juce::String fromJavaString (JNIEnv* env, jstring value)
    {
        if (env == nullptr || value == nullptr)
            return {};
        const auto* utf8 = env->GetStringUTFChars (value, nullptr);
        if (utf8 == nullptr)
        {
            env->ExceptionClear();
            return {};
        }
        const auto result = juce::String::fromUTF8 (utf8);
        env->ReleaseStringUTFChars (value, utf8);
        return result;
    }

    static jclass bridgeClass (JNIEnv* env)
    {
        const auto activity = juce::getCurrentActivity();
        if (env == nullptr || activity == nullptr)
            return nullptr;
        return ChurchDAWServices::findAndroidAppClass (
            env, activity.get(),
            "com.churchdaw.app.ChurchDAWLicenseBridge");
    }

    static juce::String javaString (
        const char* methodName, const char* signature,
        const juce::String* argument = nullptr, int integer = 0)
    {
        auto* env = juce::getEnv();
        auto clazz = bridgeClass (env);
        if (env == nullptr || clazz == nullptr)
            return {};
        const auto method = env->GetStaticMethodID (
            clazz, methodName, signature);
        if (method == nullptr)
        {
            env->ExceptionClear();
            env->DeleteLocalRef (clazz);
            return {};
        }

        jstring result = nullptr;
        jstring javaArgument = nullptr;
        if (argument != nullptr)
        {
            javaArgument = env->NewStringUTF (argument->toRawUTF8());
            result = static_cast<jstring> (
                env->CallStaticObjectMethod (clazz, method, javaArgument));
        }
        else if (juce::String (signature).contains ("(I)"))
        {
            result = static_cast<jstring> (
                env->CallStaticObjectMethod (clazz, method, (jint) integer));
        }
        else
        {
            result = static_cast<jstring> (
                env->CallStaticObjectMethod (clazz, method));
        }

        juce::String text;
        if (! env->ExceptionCheck())
            text = fromJavaString (env, result);
        else
            env->ExceptionClear();
        if (result != nullptr)
            env->DeleteLocalRef (result);
        if (javaArgument != nullptr)
            env->DeleteLocalRef (javaArgument);
        env->DeleteLocalRef (clazz);
        return text;
    }

    static bool javaBool (
        const char* methodName, const char* signature,
        const juce::String& argument)
    {
        auto* env = juce::getEnv();
        auto clazz = bridgeClass (env);
        if (env == nullptr || clazz == nullptr)
            return false;
        const auto method = env->GetStaticMethodID (
            clazz, methodName, signature);
        auto javaArgument = method != nullptr
            ? env->NewStringUTF (argument.toRawUTF8()) : nullptr;
        const bool ok = method != nullptr && javaArgument != nullptr
            && env->CallStaticBooleanMethod (
                clazz, method, javaArgument) == JNI_TRUE
            && ! env->ExceptionCheck();
        if (env->ExceptionCheck())
            env->ExceptionClear();
        if (javaArgument != nullptr)
            env->DeleteLocalRef (javaArgument);
        env->DeleteLocalRef (clazz);
        return ok;
    }

    static void javaVoid (const char* methodName, const char* signature)
    {
        auto* env = juce::getEnv();
        auto clazz = bridgeClass (env);
        if (env == nullptr || clazz == nullptr)
            return;
        const auto method = env->GetStaticMethodID (
            clazz, methodName, signature);
        if (method != nullptr)
            env->CallStaticVoidMethod (clazz, method);
        if (env->ExceptionCheck())
            env->ExceptionClear();
        env->DeleteLocalRef (clazz);
    }

    static void rememberServerTime (juce::int64 epoch)
    {
        auto* env = juce::getEnv();
        auto clazz = bridgeClass (env);
        if (env == nullptr || clazz == nullptr)
            return;
        const auto method = env->GetStaticMethodID (
            clazz, "rememberServerTime", "(J)V");
        if (method != nullptr)
            env->CallStaticVoidMethod (clazz, method, (jlong) epoch);
        if (env->ExceptionCheck())
            env->ExceptionClear();
        env->DeleteLocalRef (clazz);
    }

    static juce::String verifyLease (
        const juce::String& token, const juce::String& installation)
    {
        auto* env = juce::getEnv();
        auto clazz = bridgeClass (env);
        if (env == nullptr || clazz == nullptr)
            return {};
        const auto method = env->GetStaticMethodID (
            clazz, "verifyLease",
            "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
        auto tokenValue = method != nullptr
            ? env->NewStringUTF (token.toRawUTF8()) : nullptr;
        auto installationValue = method != nullptr
            ? env->NewStringUTF (installation.toRawUTF8()) : nullptr;
        jstring result = nullptr;
        if (method != nullptr && tokenValue != nullptr
            && installationValue != nullptr)
            result = static_cast<jstring> (
                env->CallStaticObjectMethod (
                    clazz, method, tokenValue, installationValue));
        juce::String claims;
        if (! env->ExceptionCheck())
            claims = fromJavaString (env, result);
        else
            env->ExceptionClear();
        if (result != nullptr) env->DeleteLocalRef (result);
        if (tokenValue != nullptr) env->DeleteLocalRef (tokenValue);
        if (installationValue != nullptr)
            env->DeleteLocalRef (installationValue);
        env->DeleteLocalRef (clazz);
        return claims;
    }

    static juce::String javaPostJson (
        const juce::String& address,
        const juce::String& body,
        const juce::String& bearer)
    {
        auto* env = juce::getEnv();
        auto clazz = bridgeClass (env);
        if (env == nullptr || clazz == nullptr)
            return {};
        const auto method = env->GetStaticMethodID (
            clazz, "postJson",
            "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)"
            "Ljava/lang/String;");
        auto addressValue = method != nullptr
            ? env->NewStringUTF (address.toRawUTF8()) : nullptr;
        auto bodyValue = method != nullptr
            ? env->NewStringUTF (body.toRawUTF8()) : nullptr;
        auto bearerValue = method != nullptr
            ? env->NewStringUTF (bearer.toRawUTF8()) : nullptr;
        jstring result = nullptr;
        if (method != nullptr
            && addressValue != nullptr
            && bodyValue != nullptr
            && bearerValue != nullptr)
            result = static_cast<jstring> (
                env->CallStaticObjectMethod (
                    clazz, method,
                    addressValue, bodyValue, bearerValue));
        juce::String response;
        if (! env->ExceptionCheck())
            response = fromJavaString (env, result);
        else
            env->ExceptionClear();
        if (result != nullptr)
            env->DeleteLocalRef (result);
        if (addressValue != nullptr)
            env->DeleteLocalRef (addressValue);
        if (bodyValue != nullptr)
            env->DeleteLocalRef (bodyValue);
        if (bearerValue != nullptr)
            env->DeleteLocalRef (bearerValue);
        env->DeleteLocalRef (clazz);
        return response;
    }

    DeviceIdentity deviceIdentity() const
    {
        const auto raw = javaString (
            "identityJson", "()Ljava/lang/String;");
        juce::var value;
        if (raw.isEmpty() || juce::JSON::parse (raw, value).failed())
            return {};
        return {
            stringProperty (value, "installation_id"),
            stringProperty (value, "device_public_key"),
            stringProperty (value, "device_fingerprint"),
            stringProperty (value, "device_label")
        };
    }

    Session loadSession() const
    {
        const auto raw = javaString (
            "loadSession", "()Ljava/lang/String;");
        juce::var value;
        if (raw.isEmpty() || juce::JSON::parse (raw, value).failed())
            return {};
        return {
            stringProperty (value, "mode"),
            stringProperty (value, "email"),
            stringProperty (value, "refresh_token"),
            stringProperty (value, "lease_token"),
            stringProperty (value, "setup_token"),
            intProperty (value, "server_time")
        };
    }

    bool saveSession (const Session& value)
    {
        auto json = object();
        set (json, "schema", 2);
        set (json, "mode", value.mode);
        set (json, "email", value.email);
        set (json, "refresh_token", value.refreshToken);
        set (json, "lease_token", value.leaseToken);
        set (json, "setup_token", value.setupToken);
        set (json, "server_time", value.serverTime);
        const auto raw = juce::JSON::toString (json, false);
        return javaBool (
            "saveSession", "(Ljava/lang/String;)Z", raw);
    }

    void begin (Command next, State state, const juce::String& message)
    {
        if (isThreadRunning())
            return;
        {
            const juce::ScopedLock scoped (lock);
            command = next;
        }
        Snapshot snapshotToPublish = currentSnapshot();
        snapshotToPublish.state = state;
        snapshotToPublish.busy = true;
        snapshotToPublish.message = message;
        publish (std::move (snapshotToPublish));
        startThread (juce::Thread::Priority::normal);
    }

    void run() override
    {
        Command selected;
        {
            const juce::ScopedLock scoped (lock);
            selected = command;
        }
        switch (selected)
        {
            case Command::resume:        runResume();        break;
            case Command::login:         runLogin();         break;
            case Command::reserveTrial:  runReserveTrial();  break;
            case Command::startTrial:    runStartTrial();    break;
            case Command::passwordReset: runPasswordReset(); break;
            case Command::none:                                  break;
        }
        const juce::ScopedLock scoped (lock);
        command = Command::none;
    }

    void runResume()
    {
        const auto device = deviceIdentity();
        const auto session = loadSession();
        {
            const juce::ScopedLock scoped (lock);
            saved = session;
        }
        if (! device.valid())
        {
            fail (State::signedOut,
                  "Nao foi possivel proteger este aparelho.");
            return;
        }

        if (session.mode == "trial_setup" && session.setupToken.isNotEmpty())
        {
            activateTrialSetupSnapshot (session);
            return;
        }
        if (session.mode == "trial"
            && session.leaseToken.isNotEmpty())
        {
            if (acceptLease (session, device.installationId, true))
                return;
            fail (State::trialExpired,
                  "A demonstracao de 20 minutos terminou.");
            return;
        }
        if (session.refreshToken.isEmpty())
        {
            fail (State::signedOut,
                  "Entre com sua compra ou teste por 20 minutos.");
            return;
        }

        const auto nonce = javaString (
            "randomToken", "(I)Ljava/lang/String;", nullptr, 24);
        const auto now = juce::Time::getCurrentTime().toMilliseconds() / 1000;
        const auto message = device.installationId + "|"
            + juce::String (now) + "|" + nonce + "|"
            + session.refreshToken;
        const auto proof = javaString (
            "signProof", "(Ljava/lang/String;)Ljava/lang/String;",
            &message);
        if (nonce.isEmpty() || proof.isEmpty())
        {
            fail (State::signedOut,
                  "Nao foi possivel assinar a validacao do aparelho.");
            return;
        }

        auto payload = object();
        set (payload, "refresh_token", session.refreshToken);
        set (payload, "installation_id", device.installationId);
        set (payload, "timestamp", now);
        set (payload, "nonce", nonce);
        set (payload, "proof", proof);
        const auto response = postJson ("/auth/refresh", payload);
        if ((response.status == 0 || response.status >= 500)
            && acceptLease (session, device.installationId, true))
            return;
        acceptPaidResponse (response, device.installationId);
    }

    void runLogin()
    {
        juce::String email, password;
        {
            const juce::ScopedLock scoped (lock);
            email = pendingEmail;
            password = pendingPassword;
            pendingPassword.clear();
        }
        const auto device = deviceIdentity();
        if (! device.valid())
        {
            password.clear();
            fail (State::signedOut,
                  "Nao foi possivel proteger este aparelho.");
            return;
        }
        auto payload = object();
        set (payload, "email", email);
        set (payload, "password", password);
        set (payload, "installation_id", device.installationId);
        set (payload, "device_secret", "");
        set (payload, "device_public_key", device.publicKey);
        set (payload, "device_fingerprint", device.fingerprint);
        set (payload, "device_label", device.label);
        set (payload, "platform", "android");
        if (auto* app = juce::JUCEApplicationBase::getInstance())
            set (payload, "app_version", app->getApplicationVersion());
        password.clear();
        acceptPaidResponse (
            postJson ("/auth/login", payload),
            device.installationId);
    }

    void runReserveTrial()
    {
        const auto device = deviceIdentity();
        if (! device.valid())
        {
            fail (State::signedOut,
                  "Nao foi possivel proteger este aparelho.");
            return;
        }
        auto payload = object();
        set (payload, "installation_id", device.installationId);
        set (payload, "device_public_key", device.publicKey);
        set (payload, "device_fingerprint", device.fingerprint);
        set (payload, "device_label", device.label);
        const auto response = postJson ("/trial/reserve", payload);
        if (response.status < 200 || response.status >= 300)
        {
            auto code = stringProperty (response.body, "error");
            if (code.isEmpty())
                code = stringProperty (response.body, "code");
            juce::Logger::writeToLog (
                "ChurchDAW trial reserve rejected: http="
                + juce::String (response.status)
                + " code=" + (code.isNotEmpty() ? code : "UNKNOWN")
                + (response.transportError.isNotEmpty()
                    ? " transport=" + response.transportError
                    : juce::String()));
            fail (code == "TRIAL_EXPIRED"
                    || code == "TRIAL_ALREADY_USED"
                      ? State::trialExpired : State::serverUnavailable,
                  code == "TRIAL_EXPIRED"
                    || code == "TRIAL_ALREADY_USED"
                      ? "A demonstracao deste aparelho ja foi utilizada."
                      : "Nao foi possivel preparar o teste agora.");
            return;
        }

        const auto mode = stringProperty (response.body, "mode");
        if (mode == "trial")
        {
            acceptTrialResponse (response, device.installationId);
            return;
        }
        const auto setupToken = stringProperty (
            response.body, "setup_token");
        const auto serverTime = intProperty (response.body, "server_time");
        if (setupToken.isEmpty() || serverTime <= 0)
        {
            fail (State::serverUnavailable,
                  "Resposta de demonstracao invalida.");
            return;
        }
        rememberServerTime (serverTime);
        Session session {
            "trial_setup", {}, {}, {}, setupToken, serverTime
        };
        if (! saveSession (session))
        {
            fail (State::signedOut,
                  "Nao foi possivel proteger o teste neste aparelho.");
            return;
        }
        {
            const juce::ScopedLock scoped (lock);
            saved = session;
            libraryBearer = setupToken;
        }
        activateTrialSetupSnapshot (session);
    }

    void runStartTrial()
    {
        Session session;
        {
            const juce::ScopedLock scoped (lock);
            session = saved;
        }
        if (session.setupToken.isEmpty())
            session = loadSession();
        const auto device = deviceIdentity();
        if (! device.valid() || session.setupToken.isEmpty())
        {
            fail (State::signedOut,
                  "Prepare novamente sua demonstracao.");
            return;
        }
        const auto nonce = javaString (
            "randomToken", "(I)Ljava/lang/String;", nullptr, 24);
        const auto now = juce::Time::getCurrentTime().toMilliseconds() / 1000;
        const auto message = device.installationId + "|"
            + juce::String (now) + "|" + nonce + "|"
            + session.setupToken;
        const auto proof = javaString (
            "signProof", "(Ljava/lang/String;)Ljava/lang/String;",
            &message);
        auto payload = object();
        set (payload, "installation_id", device.installationId);
        set (payload, "timestamp", now);
        set (payload, "nonce", nonce);
        set (payload, "proof", proof);
        acceptTrialResponse (
            postJson ("/trial/start", payload, session.setupToken),
            device.installationId);
    }

    void runPasswordReset()
    {
        juce::String email;
        {
            const juce::ScopedLock scoped (lock);
            email = pendingEmail;
        }
        auto payload = object();
        set (payload, "email", email);
        const auto response = postJson (
            "/auth/password/forgot", payload);
        Snapshot next = currentSnapshot();
        next.state = State::passwordResetSent;
        next.email = email;
        next.busy = false;
        next.message = response.status == 0
            ? "Nao foi possivel conectar agora."
            : "Se o e-mail estiver cadastrado, enviaremos as instrucoes.";
        publish (std::move (next));
    }

    HttpResult postJson (
        const juce::String& route, const juce::var& payload,
        const juce::String& bearer = {}) const
    {
        HttpResult result;
       #ifndef CHURCHDAW_LICENSE_API_BASE
        const auto base = juce::String (
            "https://churchdaw.com/api/v1");
       #else
        const auto base = juce::String (
            CHURCHDAW_LICENSE_API_BASE);
       #endif
        if (! base.startsWithIgnoreCase ("https://"))
        {
            result.transportError = "Endpoint inseguro";
            return result;
        }
        const auto body = juce::JSON::toString (payload, false);
        const auto address =
            base.trimCharactersAtEnd ("/") + route;
        const auto raw = javaPostJson (address, body, bearer);
        juce::var envelope;
        if (raw.isEmpty()
            || juce::JSON::parse (raw, envelope).failed())
        {
            result.transportError = "Ponte HTTPS indisponivel";
            juce::Logger::writeToLog (
                "ChurchDAW license request failed: route=" + route
                + " http=" + juce::String (result.status)
                + " transport=" + result.transportError);
            return result;
        }
        result.status = (int) intProperty (envelope, "status");
        result.transportError =
            stringProperty (envelope, "transport_error");
        const auto responseBody =
            stringProperty (envelope, "body");
        if (result.transportError.isEmpty()
            && (responseBody.isEmpty()
                || juce::JSON::parse (
                    responseBody, result.body).failed()))
            result.transportError = "Resposta invalida";
        juce::Logger::writeToLog (
            "ChurchDAW license request complete: route=" + route
            + " http=" + juce::String (result.status)
            + (result.transportError.isNotEmpty()
                ? " transport=" + result.transportError
                : juce::String()));
        return result;
    }

    bool acceptLease (
        const Session& session,
        const juce::String& installationId,
        bool offline)
    {
        const auto claimsRaw = verifyLease (
            session.leaseToken, installationId);
        juce::var claims;
        if (claimsRaw.isEmpty()
            || juce::JSON::parse (claimsRaw, claims).failed())
            return false;

        Snapshot next;
        next.state = State::active;
        next.email = stringProperty (claims, "email");
        next.displayName = stringProperty (claims, "display_name");
        next.activationLabel =
            stringProperty (claims, "activation_label");
        next.leaseExpiresAt =
            intProperty (claims, "lease_expires_at");
        next.offlineUntil =
            intProperty (claims, "offline_until");
        next.trialExpiresAt =
            intProperty (claims, "trial_expires_at");
        next.isTrial = stringProperty (claims, "mode") == "trial";
        next.canPrepareApp = true;
        next.canOpenApp = true;
        next.message = next.isTrial
            ? "Demonstracao ativa por 20 minutos."
            : (offline
                ? "Modo offline protegido."
                : "Licenca validada neste aparelho.");
        {
            const juce::ScopedLock scoped (lock);
            saved = session;
            libraryBearer = session.leaseToken;
        }
        publish (std::move (next));
        return true;
    }

    void acceptPaidResponse (
        const HttpResult& response,
        const juce::String& installationId)
    {
        if (response.status < 200 || response.status >= 300)
        {
            const auto code = stringProperty (response.body, "code");
            if (response.status == 401 || response.status == 403
                || code == "SESSION_REUSED"
                || code == "LICENSE_REVOKED")
                javaVoid ("clearSession", "()V");
            fail (code == "ACTIVATION_LIMIT"
                    ? State::activationConflict
                    : (code == "LICENSE_REVOKED"
                        ? State::revoked
                        : (response.status == 0
                            ? State::serverUnavailable
                            : State::signedOut)),
                  code == "ACTIVATION_LIMIT"
                    ? "Esta compra ja esta ativa em outro aparelho."
                    : (code == "LICENSE_REVOKED"
                        ? "Esta licenca foi suspensa."
                        : "Nao foi possivel validar o acesso."));
            return;
        }
        const auto refreshToken = stringProperty (
            response.body, "refresh_token");
        const auto leaseToken = stringProperty (
            response.body, "lease_token");
        const auto serverTime = intProperty (
            response.body, "server_time");
        if (refreshToken.isEmpty() || leaseToken.isEmpty()
            || serverTime <= 0)
        {
            fail (State::serverUnavailable,
                  "Resposta de licenca invalida.");
            return;
        }
        rememberServerTime (serverTime);
        Session session {
            "licensed",
            stringProperty (response.body, "email"),
            refreshToken,
            leaseToken,
            {},
            serverTime
        };
        if (! saveSession (session)
            || ! acceptLease (session, installationId, false))
            fail (State::signedOut,
                  "A licenca chegou, mas nao passou na validacao local.");
    }

    void acceptTrialResponse (
        const HttpResult& response,
        const juce::String& installationId)
    {
        if (response.status < 200 || response.status >= 300)
        {
            const auto code = stringProperty (response.body, "code");
            fail (code == "TRIAL_EXPIRED"
                    || code == "TRIAL_ALREADY_USED"
                      ? State::trialExpired : State::serverUnavailable,
                  code == "TRIAL_EXPIRED"
                    || code == "TRIAL_ALREADY_USED"
                      ? "A demonstracao de 20 minutos terminou."
                      : "Nao foi possivel iniciar o teste.");
            return;
        }
        const auto leaseToken = stringProperty (
            response.body, "lease_token");
        const auto serverTime = intProperty (
            response.body, "server_time");
        if (leaseToken.isEmpty() || serverTime <= 0)
        {
            fail (State::serverUnavailable,
                  "Resposta de demonstracao invalida.");
            return;
        }
        rememberServerTime (serverTime);
        Session session {
            "trial", {}, {}, leaseToken, {}, serverTime
        };
        if (! saveSession (session)
            || ! acceptLease (session, installationId, false))
            fail (State::signedOut,
                  "A demonstracao nao passou na validacao local.");
    }

    void activateTrialSetupSnapshot (const Session& session)
    {
        {
            const juce::ScopedLock scoped (lock);
            saved = session;
            libraryBearer = session.setupToken;
        }
        Snapshot next;
        next.state = State::trialReady;
        next.canPrepareApp = true;
        next.canOpenApp = false;
        next.message =
            "Prepare a biblioteca. O tempo so comeca ao abrir o piano.";
        publish (std::move (next));
    }

    void fail (State state, const juce::String& message)
    {
        auto next = currentSnapshot();
        next.state = state;
        next.busy = false;
        next.canPrepareApp = false;
        next.canOpenApp = false;
        next.message = message;
        publish (std::move (next));
    }

    void publish (Snapshot next)
    {
        {
            const juce::ScopedLock scoped (lock);
            snapshot = next;
        }
        auto alive = callbackAlive;
        juce::MessageManager::callAsync (
            [this, alive = std::move (alive),
             next = std::move (next)]
            {
                if (alive->load() && onSnapshot != nullptr)
                    onSnapshot (next);
            });
    }

    mutable juce::CriticalSection lock;
    Snapshot snapshot;
    Session saved;
    Command command { Command::none };
    juce::String pendingEmail;
    juce::String pendingPassword;
    juce::String libraryBearer;
    std::shared_ptr<std::atomic<bool>> callbackAlive {
        std::make_shared<std::atomic<bool>> (true)
    };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Client)
};
}

#endif
