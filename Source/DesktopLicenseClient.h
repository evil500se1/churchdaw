#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

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
    activeOnline,
    activeOffline,
    activationConflict,
    passwordResetSent,
    serverUnavailable,
    licensePending,
    sessionExpired,
    sessionReplaced,
    sessionRevoked,
    licenseSuspended,
    licenseRevoked,
    licenseExpired,
    licenseArchived,
    licenseDeleted,
    passwordChangeRequired,
    clientUpdateRequired
};

enum class ServerAction
{
    retryTransient,
    refreshAccessToken,
    terminateSession
};

struct ServerDecision
{
    ServerAction action { ServerAction::terminateSession };
    State state { State::sessionRevoked };
    juce::String code;
    juce::String message;
    bool clearSession = true;
    bool preserveDeviceIdentity = true;
};

enum class DeviceIdentityPlan
{
    createSchema4,
    migratePreservingInstallationId,
    preserveSchema4,
    rejectStoredIdentity
};

enum class CredentialReadDecision
{
    createNew,
    useStored,
    abortWithoutOverwrite
};

enum class LoginIdentifierKind
{
    invalid,
    email,
    licenseKey
};

struct LoginIdentifier
{
    LoginIdentifierKind kind { LoginIdentifierKind::invalid };
    juce::String value;

    bool valid() const noexcept
    {
        return kind != LoginIdentifierKind::invalid && value.isNotEmpty();
    }
};

enum class RuntimeValidity
{
    online,
    offline,
    expired
};

// Epoch local de operacoes de rede. O ticket e capturado antes do request e a
// mesma guarda e usada pelo cliente e pelos testes concorrentes para impedir
// que respostas de uma operacao invalidada alterem estado, disco ou UI.
class OperationEpochGate final
{
public:
    using Epoch = juce::uint64;

    Epoch capture() const noexcept;
    Epoch invalidate() noexcept;
    Epoch invalidateIfCurrent (Epoch expected) noexcept;
    bool isCurrent (Epoch expected) const noexcept;
    bool runIfCurrent (Epoch expected,
                       const std::function<void()>& mutation) const noexcept;

private:
    std::atomic<Epoch> current { 1 };
    mutable juce::CriticalSection commitLock;
};

enum class SessionTombstoneState
{
    absent,
    valid,
    presentButInvalid
};

// Seam pequeno para exercitar deterministicamente a mesma invalidacao local
// usada pelo Client sem acessar o cofre real durante os testes.
struct SessionInvalidationOperations
{
    std::function<bool (juce::String&)> persistAndValidateEmergencyBlock;
    std::function<SessionTombstoneState (juce::String&)> inspectEmergencyBlock;
    std::function<bool (juce::String&)> persistAndValidateTombstone;
    std::function<SessionTombstoneState (juce::String&)> inspectTombstone;
    std::function<bool (juce::String&)> removeCredential;
    std::function<bool (juce::String&)> credentialIsAbsent;
};

struct LocalSessionInvalidationResult
{
    bool guaranteed = false;
    bool emergencyBlockActive = false;
    bool tombstoneActive = false;
    bool credentialRemoved = false;
    juce::String error;
};

LocalSessionInvalidationResult invalidateLocalSession (
    const SessionInvalidationOperations& operations);
bool localSessionResumeIsBlocked (
    const SessionInvalidationOperations& operations,
    juce::String& diagnostic);

struct SessionCredentialReplacementOperations
{
    std::function<bool (juce::String&)> saveCredential;
    std::function<bool (juce::String&)> clearEmergencyBlock;
    std::function<bool (juce::String&)> clearTombstone;
    SessionInvalidationOperations invalidation;
};

struct SessionCredentialReplacementResult
{
    bool committed = false;
    bool failClosed = false;
    juce::String error;
};

SessionCredentialReplacementResult replaceLocalSessionCredential (
    const SessionCredentialReplacementOperations& operations);

// Dono move-only para material sensivel. O armazenamento permanece alocado e
// zerado depois de clear(), permitindo que o teste prove a sobrescrita real.
class SecureText final
{
public:
    SecureText() = default;
    explicit SecureText (const juce::String& value);
    explicit SecureText (const char* value);
    ~SecureText();

    SecureText (SecureText&& other) noexcept;
    SecureText& operator= (SecureText&& other) noexcept;
    SecureText (const SecureText&) = delete;
    SecureText& operator= (const SecureText&) = delete;

    SecureText& operator= (const juce::String& value);
    void assign (const juce::String& value);
    void assign (const SecureText& value);
    juce::String reveal() const;
    void clear() noexcept;
    bool isEmpty() const noexcept;
    bool isNotEmpty() const noexcept;
    int length() const noexcept;

   #if defined(CHURCHDAW_LICENSE_ENABLE_TEST_HOOKS) \
       && CHURCHDAW_LICENSE_ENABLE_TEST_HOOKS
    bool storageIsZeroedForTest() const noexcept;
   #endif

private:
    std::vector<std::uint8_t> storage;
    size_t logicalBytes = 0;
    int characterCount = 0;
};

#if defined(CHURCHDAW_LICENSE_ENABLE_TEST_HOOKS) \
    && CHURCHDAW_LICENSE_ENABLE_TEST_HOOKS
bool secureSensitiveStorageWipeSelfTest();
bool boundedSensitiveReadWipeSelfTest();
#endif

struct SessionSecrets
{
    SessionSecrets() = default;
    SessionSecrets (const juce::String& access,
                    const juce::String& refresh,
                    const juce::String& lease,
                    const juce::String& session);

    SecureText accessToken;
    SecureText refreshToken;
    SecureText leaseToken;
    SecureText sessionId;

    void clear();
    bool empty() const;
};

struct SignedLeaseUpdate
{
    bool accepted = false;
    juce::int64 leaseExpiresAt = 0;
    juce::int64 offlineUntil = 0;
};

// Funcoes puras usadas pelo cliente e pelos testes de regressao. Elas mantem
// codigos de erro, intervalos e migracao de identidade fora da camada de UI.
ServerDecision classifySessionFailure (int httpStatus,
                                       juce::String code,
                                       bool replacementFlag = false);
DeviceIdentityPlan planDeviceIdentity (int schema,
                                       bool legacyIdentityValid,
                                       bool clientDeviceIdValid) noexcept;
CredentialReadDecision decideCredentialRead (
    bool hasStoredValue, bool hasReadError) noexcept;
LoginIdentifier normaliseLoginIdentifier (juce::String identifier);
bool loginCredentialsAreValid (const LoginIdentifier& identifier,
                               int passwordLength) noexcept;
juce::var makeLoginIdentifierPayload (const LoginIdentifier& identifier);
juce::String createRandomClientDeviceIdentifier();
int normaliseHeartbeatIntervalSeconds (int requested) noexcept;
int transientRetryDelaySeconds (int consecutiveFailures) noexcept;
bool shouldRefreshAccessToken (juce::int64 trustedNow,
                               juce::int64 accessExpiresAt) noexcept;
RuntimeValidity evaluateRuntimeValidity (juce::int64 trustedNow,
                                         juce::int64 leaseExpiresAt,
                                         juce::int64 offlineUntil,
                                         bool lastContactWasOnline) noexcept;
juce::int64 advanceTrustedTimeHighWater (juce::int64 storedHighWater,
                                         juce::int64 candidate) noexcept;
juce::int64 normaliseSavedTrustedTimeHighWater (
    juce::int64 serverTime,
    juce::int64 legacyTrustedTime,
    juce::int64 storedHighWater) noexcept;
juce::int64 reserveTrustedTimeHighWater (
    juce::int64 storedHighWater,
    juce::int64 estimatedTrustedNow,
    juce::int64 reservationSeconds = 30) noexcept;
bool trustedTimeRollbackDetected (juce::int64 observedWallClock,
                                  juce::int64 trustedHighWater,
                                  juce::int64 toleranceSeconds = 300) noexcept;
SignedLeaseUpdate validateSignedLeaseUpdate (
    const juce::String& candidateLeaseToken,
    const juce::String& expectedInstallationId,
    juce::int64 trustedNow,
    juce::int64 currentLeaseExpiresAt,
    juce::int64 currentOfflineUntil);
bool shouldAttemptRemoteLogout (bool hasAccessToken,
                                bool hasRefreshToken = false,
                                bool hasDeviceProof = false) noexcept;
bool isUsableJsonHttpSuccess (int httpStatus,
                              bool hasObjectBody,
                              bool hasTransportError) noexcept;
bool parseStrictJsonObject (const juce::String& text,
                            juce::var& parsedResult);
bool isValidAuthSuccessPayload (
    const juce::var& body,
    const juce::String& expectedInstallationId,
    const juce::String& expectedSessionId = {});
bool isValidRuntimeSuccessPayload (const juce::var& body,
                                   const juce::String& expectedSessionId);
bool logoutQueueIdentityMatches (const juce::String& firstSessionId,
                                 juce::uint64 firstEpoch,
                                 const juce::String& secondSessionId,
                                 juce::uint64 secondEpoch) noexcept;
int maximumPendingLogoutQueueSize() noexcept;
bool webRequestDeadlineCancellationSelfTest (int iterations = 64);
int remainingHttpDeadlineMs (int elapsedMs,
                             int totalDeadlineMs = 15000) noexcept;
bool isActiveState (State state) noexcept;
bool isTerminalState (State state) noexcept;

struct Snapshot
{
    State state { State::signedOut };
    juce::String email;
    juce::String displayName;
    juce::String message;
    juce::String reasonCode;
    juce::String activationLabel;
    juce::int64 leaseExpiresAt = 0;
    juce::int64 offlineUntil = 0;
    bool canOpenApp = false;
    bool busy = false;
    bool online = false;
    bool requiresAppClosure = false;
};

bool gateEpochAllowsOpen (juce::uint64 scheduledEpoch,
                          juce::uint64 currentEpoch,
                          bool gatePassed,
                          bool terminalTransitionPending,
                          const Snapshot& snapshot) noexcept;
// Cliente assincrono. Senha existe somente durante a chamada de login e nunca
// e gravada. Depois do gate, esta thread permanece viva para serializar refresh,
// heartbeat e status; logout remoto best-effort usa um worker separado.
class Client final : private juce::Thread
{
public:
    Client();
    ~Client() override;

    std::function<void (const Snapshot&)> onSnapshot;

    void resumeSavedSession();
    void login (juce::String identifier, juce::String password,
                bool rememberSession);
    void requestPasswordReset (juce::String email);
    void signOut();
    void applicationSuspended();
    void applicationResumed();

    Snapshot currentSnapshot() const;

   #if defined(CHURCHDAW_LICENSE_ENABLE_TEST_HOOKS) \
       && CHURCHDAW_LICENSE_ENABLE_TEST_HOOKS
    bool dispatchSnapshotForTest (Snapshot next);
    using CompensatingLogoutObserverForTest =
        std::function<void (const juce::String&, bool, bool)>;
    void setCompensatingLogoutObserverForTest (
        CompensatingLogoutObserverForTest observer);
    juce::uint64 captureOperationEpochForTest() const noexcept;
    juce::uint64 invalidateOperationEpochForSignOutTest();
    bool processSupersededAuthResponseForTest (
        const juce::var& body,
        const juce::String& installationId,
        const juce::String& clientDeviceId,
        const juce::String& deviceSecretBase64,
        const juce::String& expectedSessionId,
        juce::uint64 requestEpoch);
   #endif

    static juce::URL purchaseUrl();
    static juce::URL accountPortalUrl();
    static juce::URL downloadUrl();

private:
    enum class Command
    {
        none,
        resume,
        login,
        passwordReset
    };

    enum class RequestOutcome
    {
        success,
        transientFailure,
        accessTokenExpired,
        terminalFailure,
        superseded
    };

    using OperationEpoch = OperationEpochGate::Epoch;

    struct DeviceIdentity
    {
        DeviceIdentity() = default;
        DeviceIdentity (const DeviceIdentity& other)
            : schema (other.schema),
              legacySchema (other.legacySchema),
              installationId (other.installationId),
              clientDeviceId (other.clientDeviceId),
              label (other.label)
        {
            secretBase64.assign (other.secretBase64);
        }

        DeviceIdentity& operator= (const DeviceIdentity& other)
        {
            if (this != &other)
            {
                schema = other.schema;
                legacySchema = other.legacySchema;
                installationId = other.installationId;
                clientDeviceId = other.clientDeviceId;
                secretBase64.assign (other.secretBase64);
                label = other.label;
            }
            return *this;
        }

        DeviceIdentity (DeviceIdentity&&) noexcept = default;
        DeviceIdentity& operator= (DeviceIdentity&&) noexcept = default;

        int schema = 0;
        int legacySchema = 0;
        juce::String installationId;
        juce::String clientDeviceId;
        SecureText secretBase64;
        juce::String label;

        bool valid() const;
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
        SecureText refreshToken;
        SecureText leaseToken;
        juce::String sessionId;
        juce::int64 serverTime = 0;
        juce::int64 trustedTimeHighWater = 0;
        juce::int64 generation = 0;
        int heartbeatIntervalSeconds = 45;
    };

    struct PendingLogout
    {
        SecureText accessToken;
        SecureText refreshToken;
        SecureText sessionId;
        juce::String installationId;
        juce::String clientDeviceId;
        SecureText deviceSecretBase64;
        OperationEpoch epoch = 0;
        int attempts = 0;
        bool compensating = false;

        bool canContactServer() const noexcept
        {
            return shouldAttemptRemoteLogout (
                accessToken.isNotEmpty(),
                refreshToken.isNotEmpty(),
                deviceSecretBase64.isNotEmpty());
        }

        void clear()
        {
            accessToken.clear();
            refreshToken.clear();
            sessionId.clear();
            installationId.clear();
            clientDeviceId.clear();
            deviceSecretBase64.clear();
            epoch = 0;
            attempts = 0;
            compensating = false;
        }
    };

    class LogoutWorker;

    struct RuntimeSession
    {
        SessionSecrets secrets;
        DeviceIdentity device;
        juce::String email;
        juce::String displayName;
        juce::String activationLabel;
        juce::int64 accessExpiresAt = 0;
        juce::int64 leaseExpiresAt = 0;
        juce::int64 offlineUntil = 0;
        juce::int64 serverTime = 0;
        juce::int64 trustedTimeHighWater = 0;
        juce::int64 persistedTrustedTimeHighWater = 0;
        juce::int64 generation = 0;
        double serverTimeAnchorMs = 0.0;
        int heartbeatIntervalSeconds = 45;
        bool remember = false;
        bool online = false;
        bool active = false;

        void clear();
    };

    struct CallbackDispatchState
    {
        juce::CriticalSection dispatchLock;
        std::atomic<OperationEpoch> currentEpoch { 1 };
        bool alive = true;
    };

    void run() override;
    bool runResume (OperationEpoch commandEpoch);
    bool runLogin (OperationEpoch commandEpoch);
    void runPasswordReset (OperationEpoch commandEpoch);
    void runSessionLoop (OperationEpoch sessionEpoch);

    DeviceIdentity ensureDeviceIdentity (OperationEpoch expectedEpoch,
                                         juce::String& error);
    bool saveDeviceIdentity (const DeviceIdentity& identity,
                             OperationEpoch expectedEpoch,
                             juce::String& error) const;
    SavedSession loadSavedSession (OperationEpoch expectedEpoch,
                                   juce::String& error) const;
    bool saveSession (const SavedSession& session, juce::String& error) const;
    bool saveSessionForEpoch (const SavedSession& session,
                              OperationEpoch expectedEpoch,
                              juce::String& error) const;
    bool persistRuntimeSession (OperationEpoch expectedEpoch,
                                juce::String& error) const;
    bool removeSavedSession (juce::String& error) const;
    LocalSessionInvalidationResult invalidatePersistentSession (
        OperationEpoch expectedEpoch) const;

    HttpResult requestJson (const juce::String& route,
                            const juce::var& payload,
                            const juce::String& bearerToken = {},
                            const juce::String& method = "POST",
                            const std::atomic<bool>* externalCancellation = nullptr) const;
    bool acceptAuthResponse (const HttpResult& result,
                             bool persistRefreshToken,
                             const DeviceIdentity& expectedDevice,
                             const juce::String& expectedSessionId,
                             bool runtimeRefresh,
                             OperationEpoch expectedEpoch);
    bool acceptOfflineLease (const SavedSession& saved,
                             const DeviceIdentity& expectedDevice,
                             OperationEpoch expectedEpoch);
    RequestOutcome performRefresh (OperationEpoch sessionEpoch,
                                   OperationEpoch& requestEpoch);
    RequestOutcome performSessionValidation (bool statusRequest,
                                              OperationEpoch sessionEpoch,
                                              OperationEpoch& requestEpoch);
    RequestOutcome processRuntimeHttpResult (const HttpResult& result,
                                             OperationEpoch requestEpoch);
    bool updateRuntimeFromOnlineResponse (const juce::var& body,
                                          OperationEpoch requestEpoch);
    bool publishActiveSnapshot (bool online,
                                juce::String message,
                                OperationEpoch expectedEpoch);
    bool publishTerminal (const ServerDecision& decision,
                          bool closeRunningApplication,
                          OperationEpoch expectedEpoch = 0);
    LocalSessionInvalidationResult clearRuntimeSession (
        bool invalidatePersistentSession,
        OperationEpoch expectedEpoch);
    bool performBestEffortLogout (
        const PendingLogout& logout,
        const std::atomic<bool>* externalCancellation = nullptr);
    bool enqueuePendingLogout (PendingLogout logout);
    bool queueCompensatingLogoutFromAuthResponse (
        const HttpResult& result,
        const DeviceIdentity& device,
        const juce::String& expectedSessionId,
        OperationEpoch requestEpoch);
    bool checkpointTrustedTimeHighWater (bool forceWrite,
                                         OperationEpoch expectedEpoch = 0);
    juce::int64 estimatedTrustedNowSecondsLocked() const;
    juce::int64 trustedNowSeconds() const;
    bool runtimeSessionIsActive() const;
    OperationEpoch invalidateOperationEpoch();
    OperationEpoch invalidateOperationEpochIfCurrent (
        OperationEpoch expectedEpoch);

    bool publish (Snapshot next, OperationEpoch expectedEpoch = 0);
    void beginCommand (Command next, OperationEpoch commandEpoch);

    static juce::String randomToken (size_t bytes);
    static juce::String hmacSha256Base64 (const juce::String& keyBase64,
                                         const juce::String& message);
    static juce::String genericErrorForStatus (int status);

    mutable juce::CriticalSection lock;
    Snapshot snapshot;
    RuntimeSession runtime;
    Command command { Command::none };
    OperationEpoch pendingCommandEpoch = 0;
    juce::String pendingEmail;
    SecureText pendingPassword;
    bool pendingRemember = true;
    std::atomic<bool> forceStatusRequested { false };
    std::atomic<bool> checkpointRequested { false };
    std::atomic<bool> shutdownRequested { false };
    OperationEpochGate operationEpoch;
    mutable juce::CriticalSection persistenceLock;
    std::unique_ptr<LogoutWorker> logoutWorker;
    std::shared_ptr<CallbackDispatchState> callbackDispatch {
        std::make_shared<CallbackDispatchState>()
    };
   #if defined(CHURCHDAW_LICENSE_ENABLE_TEST_HOOKS) \
       && CHURCHDAW_LICENSE_ENABLE_TEST_HOOKS
    CompensatingLogoutObserverForTest compensatingLogoutObserverForTest;
   #endif

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Client)
};
}
