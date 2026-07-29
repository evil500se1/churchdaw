#pragma once

#include <juce_cryptography/juce_cryptography.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include "DesktopCredentialStore.h"

namespace ChurchDAWDesktopLicensing
{
enum class State
{
    signedOut,
    resuming,
    signingIn,
    active,
    activationConflict,
    passwordResetSent,
    serverUnavailable,
    revoked
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
    bool canOpenApp = false;
    bool busy = false;
};

// Cliente assíncrono. Senha existe somente durante a chamada de login e nunca
// é gravada. A sessão persistente contém um refresh token rotativo protegido
// pelo cofre nativo e vinculado ao segredo desta instalação.
class Client final : private juce::Thread
{
public:
    Client();
    ~Client() override;

    std::function<void (const Snapshot&)> onSnapshot;

    void resumeSavedSession();
    void login (juce::String email, juce::String password, bool rememberSession);
    void requestPasswordReset (juce::String email);
    void signOut();

    Snapshot currentSnapshot() const;

    static juce::URL purchaseUrl();
    static juce::URL accountPortalUrl();

private:
    enum class Command
    {
        none,
        resume,
        login,
        passwordReset
    };

    struct DeviceIdentity
    {
        juce::String installationId;
        juce::String secretBase64;
        juce::String label;

        bool valid() const
        {
            constexpr auto base64UrlCharacters =
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
            return installationId.length() >= 16
                && installationId.length() <= 128
                && installationId.containsOnly (base64UrlCharacters)
                && secretBase64.length() >= 40
                && secretBase64.length() <= 128
                && secretBase64.containsOnly (base64UrlCharacters);
        }
    };

    struct HttpResult
    {
        int status = 0;
        juce::var body;
        juce::String transportError;
    };

    struct SavedSession
    {
        juce::String email;
        juce::String refreshToken;
        juce::String leaseToken;
        juce::int64 trustedTime = 0;
    };

    void run() override;
    void runResume();
    void runLogin();
    void runPasswordReset();

    DeviceIdentity ensureDeviceIdentity (juce::String& error);
    SavedSession loadSavedSession (juce::String& error) const;
    bool saveRefreshToken (const juce::String& refreshToken,
                           const juce::String& email,
                           const juce::String& leaseToken,
                           juce::int64 trustedTime,
                           juce::String& error);
    void removeSavedRefreshToken();

    HttpResult postJson (const juce::String& route,
                         const juce::var& payload) const;
    void acceptAuthResponse (const HttpResult& result,
                             bool persistRefreshToken,
                             const juce::String& expectedInstallationId);
    bool acceptOfflineLease (const SavedSession& saved,
                             const juce::String& expectedInstallationId);
    void publish (Snapshot next);
    void beginCommand (Command next);

    static juce::String randomToken (size_t bytes);
    static juce::String hmacSha256Base64 (const juce::String& keyBase64,
                                          const juce::String& message);
    static juce::String genericErrorForStatus (int status);

    mutable juce::CriticalSection lock;
    Snapshot snapshot;
    Command command { Command::none };
    juce::String pendingEmail;
    juce::String pendingPassword;
    bool pendingRemember = true;
    std::shared_ptr<std::atomic<bool>> callbackAlive {
        std::make_shared<std::atomic<bool>> (true)
    };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Client)
};
}
