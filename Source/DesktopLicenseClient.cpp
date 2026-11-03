#include "DesktopLicenseClient.h"
#include "DesktopLeaseVerifier.h"
#include "DesktopSecureRandom.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <iterator>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX 1
 #endif
 #include <windows.h>
#endif

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
constexpr int maximumRequestDurationMs = 15000;
constexpr int maximumQueuedLogoutCount = 16;
constexpr int defaultHeartbeatSeconds = 45;
constexpr juce::int64 trustedTimeCheckpointSeconds = 15;
constexpr juce::int64 trustedTimeReservationSeconds = 30;

void secureWipeBytes (void* memory, size_t bytes) noexcept
{
    auto* cursor = static_cast<volatile std::uint8_t*> (memory);
    while (bytes-- > 0)
        *cursor++ = 0;
    std::atomic_signal_fence (std::memory_order_seq_cst);
}

// Usar somente em Strings temporarias que pertencem ao request. A escrita
// alcanca tambem copias ref-counted (payload/URL) antes que todas sejam soltas.
void secureClearOwnedJuceString (juce::String& value) noexcept
{
    if (value.isNotEmpty())
    {
        const auto bytes = static_cast<size_t> (value.getNumBytesAsUTF8());
        secureWipeBytes (
            const_cast<char*> (value.toRawUTF8()), bytes);
    }
    value.clear();
}

juce::String makeIndependentStringCopy (const juce::String& value)
{
    return value.isEmpty()
             ? juce::String()
             : juce::String::fromUTF8 (
                   value.toRawUTF8(), value.getNumBytesAsUTF8());
}

class ScopedOwnedJuceStringWiper final
{
public:
    explicit ScopedOwnedJuceStringWiper (juce::String& valueToWipe)
        : value (valueToWipe)
    {
    }

    ~ScopedOwnedJuceStringWiper()
    {
        secureClearOwnedJuceString (value);
    }

private:
    juce::String& value;
};

class ScopedMemoryBlockWiper final
{
public:
    explicit ScopedMemoryBlockWiper (juce::MemoryBlock& valueToWipe)
        : value (valueToWipe)
    {
    }

    ~ScopedMemoryBlockWiper()
    {
        if (! value.isEmpty())
            secureWipeBytes (value.getData(), value.getSize());
    }

private:
    juce::MemoryBlock& value;
};

class ScopedRawMemoryWiper final
{
public:
    ScopedRawMemoryWiper (void* memoryToWipe, size_t bytesToWipe)
        : memory (memoryToWipe), bytes (bytesToWipe)
    {
    }

    ~ScopedRawMemoryWiper()
    {
        if (memory != nullptr && bytes > 0)
            secureWipeBytes (memory, bytes);
    }

private:
    void* const memory;
    const size_t bytes;
};

class ScopedWebRequestDeadline final
{
public:
    ScopedWebRequestDeadline (juce::WebInputStream* streamToCancel,
                              int timeoutMs,
                              const std::atomic<bool>* ownerCancellation,
                              const std::atomic<bool>* externalCancellation)
        : stream (streamToCancel),
          ownerCancellationFlag (ownerCancellation),
          externalCancellationFlag (externalCancellation),
          worker ([this, timeoutMs]
          {
              std::unique_lock<std::mutex> scoped (mutex);
              const auto expiresAt = std::chrono::steady_clock::now()
                                   + std::chrono::milliseconds (
                                         juce::jmax (1, timeoutMs));

              while (! finished)
              {
                  const bool cancellationRequested =
                      (ownerCancellationFlag != nullptr
                       && ownerCancellationFlag->load())
                      || (externalCancellationFlag != nullptr
                          && externalCancellationFlag->load());
                  const auto now = std::chrono::steady_clock::now();
                  if (cancellationRequested || now >= expiresAt)
                  {
                      cancelled.store (cancellationRequested);
                      expired.store (! cancellationRequested);
                      scoped.unlock();
                      if (stream != nullptr)
                          stream->cancel();
                      return;
                  }

                  condition.wait_until (
                      scoped,
                      juce::jmin (expiresAt,
                                  now + std::chrono::milliseconds (50)),
                      [this] { return finished; });
              }
          })
    {
    }

    ~ScopedWebRequestDeadline()
    {
        {
            std::lock_guard<std::mutex> scoped (mutex);
            finished = true;
        }
        condition.notify_one();
        if (worker.joinable())
            worker.join();
    }

    bool hasExpired() const noexcept
    {
        return expired.load();
    }

    bool wasCancelled() const noexcept
    {
        return cancelled.load();
    }

private:
    juce::WebInputStream* const stream;
    const std::atomic<bool>* const ownerCancellationFlag;
    const std::atomic<bool>* const externalCancellationFlag;
    std::mutex mutex;
    std::condition_variable condition;
    std::atomic<bool> expired { false };
    std::atomic<bool> cancelled { false };
    bool finished = false;
    // Deve ser o ultimo membro: a lambda pode executar imediatamente e acessar
    // todos os campos anteriores assim que o construtor de std::thread retorna.
    std::thread worker;
};

juce::String apiBase()
{
    return juce::String (CHURCHDAW_LICENSE_API_BASE).trimCharactersAtEnd ("/");
}

juce::String normaliseEmail (juce::String value)
{
    return value.trim().toLowerCase();
}

bool isAsciiAlphaNumeric (juce::juce_wchar character) noexcept
{
    return (character >= 'A' && character <= 'Z')
        || (character >= 'a' && character <= 'z')
        || (character >= '0' && character <= '9');
}

bool isPlausibleEmailAddress (const juce::String& value) noexcept
{
    if (value.isEmpty() || value.length() > 254
        || value.containsAnyOf ("\r\n\t "))
        return false;

    const auto separator = value.indexOfChar ('@');
    if (separator <= 0 || separator != value.lastIndexOfChar ('@')
        || separator >= value.length() - 1 || separator > 64)
        return false;

    const auto local = value.substring (0, separator);
    const auto domain = value.substring (separator + 1);
    if (local.startsWithChar ('.') || local.endsWithChar ('.')
        || local.contains ("..")
        || domain.startsWithChar ('.') || domain.endsWithChar ('.')
        || domain.startsWithChar ('-') || domain.endsWithChar ('-')
        || domain.contains (".."))
        return false;

    constexpr auto allowedLocalPunctuation = ".!#$%&'*+/=?^_`{|}~-";
    for (int index = 0; index < local.length(); ++index)
    {
        const auto character = local[index];
        if (! isAsciiAlphaNumeric (character)
            && ! juce::String (allowedLocalPunctuation).containsChar (
                character))
            return false;
    }

    int labelLength = 0;
    for (int index = 0; index < domain.length(); ++index)
    {
        const auto character = domain[index];
        if (character == '.')
        {
            if (labelLength == 0 || domain[index - 1] == '-')
                return false;
            labelLength = 0;
            continue;
        }

        if (! isAsciiAlphaNumeric (character) && character != '-')
            return false;
        if (labelLength == 0 && character == '-')
            return false;
        ++labelLength;
    }
    return labelLength > 0 && domain.getLastCharacter() != '-';
}

juce::String normaliseLicenseKey (juce::String value)
{
    value = value.trim();
    juce::String result;
    bool replacingWhitespace = false;
    for (int index = 0; index < value.length(); ++index)
    {
        const auto character = value[index];
        if (juce::CharacterFunctions::isWhitespace (character))
        {
            if (! replacingWhitespace)
                result += '-';
            replacingWhitespace = true;
            continue;
        }

        replacingWhitespace = false;
        result += juce::CharacterFunctions::toUpperCase (character);
    }
    return result;
}

bool isValidLicenseKey (const juce::String& value) noexcept
{
    if (value.length() < 6 || value.length() > 64
        || ! isAsciiAlphaNumeric (value[0]))
        return false;

    for (int index = 1; index < value.length(); ++index)
    {
        const auto character = value[index];
        if (! isAsciiAlphaNumeric (character)
            && character != '_' && character != '-')
            return false;
    }
    return true;
}

juce::int64 unixTimeNow()
{
    return juce::Time::getCurrentTime().toMilliseconds() / 1000;
}

double monotonicTimeMs()
{
    return juce::Time::getMillisecondCounterHiRes();
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

juce::String currentArchitecture()
{
   #if JUCE_ARM && JUCE_64BIT
    return "arm64";
   #elif JUCE_ARM
    return "arm";
   #elif JUCE_INTEL && JUCE_64BIT
    return "x86_64";
   #elif JUCE_INTEL
    return "x86";
   #else
    return "unknown";
   #endif
}

juce::String currentPlatform()
{
   #if JUCE_MAC
    return "macos";
   #else
    return "windows";
   #endif
}

juce::String secureRandomToken (size_t bytes)
{
    juce::MemoryBlock value (bytes, true);
    ScopedMemoryBlockWiper valueWiper (value);
    if (! fillSecureRandom (value.getData(), value.getSize()))
        return {};

    return juce::Base64::toBase64 (value.getData(), value.getSize())
        .replaceCharacter ('+', '-')
        .replaceCharacter ('/', '_')
        .trimCharactersAtEnd ("=");
}

bool validOpaqueIdentifier (const juce::String& value,
                            int minimumLength = 16,
                            int maximumLength = 128)
{
    constexpr auto characters =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    return value.length() >= minimumLength
        && value.length() <= maximumLength
        && value.containsOnly (characters);
}

juce::var makeObject()
{
    return juce::var (new juce::DynamicObject());
}

juce::DynamicObject* objectOf (juce::var& value)
{
    return value.getDynamicObject();
}

const juce::DynamicObject* objectOf (const juce::var& value)
{
    return value.getDynamicObject();
}

juce::String propertyString (const juce::var& value,
                             const juce::Identifier& key)
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

bool propertyBool (const juce::var& value,
                   const juce::Identifier& key)
{
    if (const auto* object = objectOf (value))
        return static_cast<bool> (object->getProperty (key));

    return false;
}

void setProperty (juce::var& value,
                  const juce::Identifier& key,
                  const juce::var& property)
{
    if (auto* object = objectOf (value))
        object->setProperty (key, property);
}

void secureClearJsonProperty (juce::var& value,
                              const juce::Identifier& key) noexcept
{
    auto* object = objectOf (value);
    if (object == nullptr || ! object->hasProperty (key))
        return;

    auto property = object->getProperty (key);
    if (property.isString())
    {
        auto sensitive = property.toString();
        secureClearOwnedJuceString (sensitive);
    }
    object->setProperty (key, juce::var());
}

void secureClearSensitiveJson (juce::var& value) noexcept
{
    static const std::array<juce::Identifier, 9> sensitiveKeys {
        juce::Identifier ("password"),
        juce::Identifier ("access_token"),
        juce::Identifier ("refresh_token"),
        juce::Identifier ("lease_token"),
        juce::Identifier ("session_id"),
        juce::Identifier ("device_secret"),
        juce::Identifier ("proof"),
        juce::Identifier ("nonce"),
        juce::Identifier ("authorization")
    };
    for (const auto& key : sensitiveKeys)
        secureClearJsonProperty (value, key);
}

class ScopedSensitiveJsonWiper final
{
public:
    explicit ScopedSensitiveJsonWiper (juce::var& valueToWipe)
        : value (valueToWipe)
    {
    }

    ~ScopedSensitiveJsonWiper()
    {
        secureClearSensitiveJson (value);
    }

private:
    juce::var& value;
};

juce::String responseCode (const juce::var& body)
{
    auto code = propertyString (body, "code").trim().toUpperCase();
    if (code.isEmpty())
        code = propertyString (body, "error_code").trim().toUpperCase();
    return code;
}

struct SensitiveReadWipeReport
{
    size_t outputUsedBytes = 0;
    size_t outputAllocatedBytes = 0;
    size_t chunkAllocatedBytes = 0;
    bool outputStorageAddressStable = false;
    bool outputStorageZeroed = false;
    bool chunkStorageZeroed = false;
};

bool allBytesAreZero (const void* memory, size_t bytes) noexcept
{
    if (bytes == 0)
        return true;
    if (memory == nullptr)
        return false;

    auto* cursor = static_cast<const volatile std::uint8_t*> (memory);
    while (bytes-- > 0)
        if (*cursor++ != 0)
            return false;
    return true;
}

// MemoryOutputStream conserva capacidade extra alem de getDataSize(). O bloco
// externo nasce com toda a capacidade maxima: assim nenhum realloc pode liberar
// uma copia intermediaria dos tokens sem wipe. O destrutor sobrescreve o bloco
// inteiro antes de o stream reduzi-lo/libera-lo e tambem roda nos retornos
// antecipados e durante o unwind de uma excecao de InputStream.
class SensitiveReadBuffers final
{
public:
    explicit SensitiveReadBuffers (
        SensitiveReadWipeReport* reportToPopulate = nullptr)
        : outputStorage (
              static_cast<size_t> (maximumResponseBytes) + 1u, true),
          initialOutputAddress (outputStorage.getData()),
          initialOutputAllocatedBytes (outputStorage.getSize()),
          output (outputStorage, false),
          report (reportToPopulate)
    {
    }

    ~SensitiveReadBuffers() noexcept
    {
        const auto usedBytes = output.getDataSize();
        const auto allocatedBytes = outputStorage.getSize();
        const bool addressStable = outputStorage.getData()
                                == initialOutputAddress
                            && allocatedBytes
                                == initialOutputAllocatedBytes;
        if (allocatedBytes > 0)
            secureWipeBytes (outputStorage.getData(), allocatedBytes);
        secureWipeBytes (chunk.data(), chunk.size());

        if (report != nullptr)
        {
            report->outputUsedBytes = usedBytes;
            report->outputAllocatedBytes = allocatedBytes;
            report->chunkAllocatedBytes = chunk.size();
            report->outputStorageAddressStable = addressStable;
            report->outputStorageZeroed = allBytesAreZero (
                outputStorage.getData(), allocatedBytes);
            report->chunkStorageZeroed = allBytesAreZero (
                chunk.data(), chunk.size());
        }
    }

    size_t dataSize() const noexcept
    {
        return output.getDataSize();
    }

    int readNext (juce::InputStream& stream, int maximumBytes)
    {
        return stream.read (
            chunk.data(),
            juce::jmin (static_cast<int> (chunk.size()), maximumBytes));
    }

    void appendLastRead (int amount)
    {
        if (amount > 0)
            output.write (chunk.data(), static_cast<size_t> (amount));
    }

    juce::String materialise() const
    {
        return output.toString();
    }

private:
    juce::MemoryBlock outputStorage;
    const void* const initialOutputAddress;
    const size_t initialOutputAllocatedBytes;
    juce::MemoryOutputStream output;
    std::array<char, 4096> chunk {};
    SensitiveReadWipeReport* const report;
};

juce::String readBoundedText (
    juce::InputStream& stream,
    bool& tooLarge,
    SensitiveReadWipeReport* wipeReport = nullptr)
{
    SensitiveReadBuffers buffers (wipeReport);
    tooLarge = false;

    while (! stream.isExhausted())
    {
        const auto remaining = maximumResponseBytes
                             - static_cast<int> (buffers.dataSize());
        if (remaining <= 0)
        {
            tooLarge = true;
            return {};
        }

        const auto amount = buffers.readNext (stream, remaining);
        if (amount <= 0)
            break;

        buffers.appendLastRead (amount);
    }

    return buffers.materialise();
}

bool isCredentialFailureCode (const juce::String& code)
{
    return code == "INVALID_CREDENTIALS"
        || code == "INVALID_LOGIN"
        || code == "AUTHENTICATION_FAILED"
        || code == "EMAIL_NOT_VERIFIED"
        || code == "USER_DISABLED";
}

juce::File licensingDataFolder()
{
    return juce::File::getSpecialLocation (
               juce::File::userApplicationDataDirectory)
        .getChildFile ("ChurchDAW")
        .getChildFile ("Licensing");
}

juce::File sessionTombstoneFile()
{
    return licensingDataFolder().getChildFile (
        "desktop-session-revoked-v1.tombstone");
}

#if ! JUCE_WINDOWS
juce::File emergencySessionBlockFile()
{
    return licensingDataFolder().getChildFile (
        "desktop-session-block-emergency-v1.marker");
}
#endif

juce::String emergencySessionBlockContents()
{
    const juce::String payload (
        "CHURCHDAW_EMERGENCY_SESSION_BLOCK\n"
        "schema=1\n"
        "state=blocked\n");
    const auto checksum = juce::SHA256 (
        payload.toRawUTF8(),
        static_cast<size_t> (payload.getNumBytesAsUTF8())).toHexString();
    return payload + "sha256=" + checksum + "\n";
}

#if JUCE_WINDOWS
constexpr auto emergencySessionBlockRegistryKey =
    L"Software\\ChurchDAW\\Licensing";
constexpr auto emergencySessionBlockRegistryValue =
    L"DesktopSessionBlockedV1";
#endif

juce::String sessionTombstoneContents()
{
    const juce::String payload (
        "CHURCHDAW_SESSION_TOMBSTONE\n"
        "schema=1\n"
        "state=revoked\n");
    const auto checksum = juce::SHA256 (
        payload.toRawUTF8(),
        static_cast<size_t> (payload.getNumBytesAsUTF8())).toHexString();
    return payload + "sha256=" + checksum + "\n";
}

SessionTombstoneState inspectSessionTombstone (juce::String& error)
{
    error.clear();
    const auto file = sessionTombstoneFile();
    if (! file.exists())
        return SessionTombstoneState::absent;

    if (! file.existsAsFile() || file.getSize() <= 0 || file.getSize() > 4096)
    {
        error = "Tombstone local presente, mas ilegivel";
        return SessionTombstoneState::presentButInvalid;
    }

    if (file.loadFileAsString() != sessionTombstoneContents())
    {
        error = "Tombstone local presente, mas invalido";
        return SessionTombstoneState::presentButInvalid;
    }

    return SessionTombstoneState::valid;
}

bool persistAndValidateSessionTombstone (juce::String& error)
{
    error.clear();
    const auto folder = licensingDataFolder();
    if (folder.createDirectory().failed())
    {
        error = "Nao foi possivel criar o bloqueio local da sessao";
        return false;
    }

    const auto target = sessionTombstoneFile();
    const auto temporary = folder.getNonexistentChildFile (
        "desktop-session-revoked-v1", ".tmp", false);
    const auto contents = sessionTombstoneContents();

    {
        juce::FileOutputStream output (temporary);
        if (output.getStatus().failed()
            || ! output.write (contents.toRawUTF8(),
                               static_cast<size_t> (
                                   contents.getNumBytesAsUTF8())))
        {
            error = "Nao foi possivel gravar o bloqueio local da sessao";
            temporary.deleteFile();
            return false;
        }

        output.flush();
        if (output.getStatus().failed())
        {
            error = "Nao foi possivel confirmar o bloqueio local no disco";
            temporary.deleteFile();
            return false;
        }
    }

    if (! temporary.existsAsFile()
        || temporary.loadFileAsString() != contents
        || ! temporary.replaceFileIn (target))
    {
        error = "O bloqueio local da sessao nao passou na validacao";
        temporary.deleteFile();
        return false;
    }

    juce::String validationError;
    const auto state = inspectSessionTombstone (validationError);
    if (state != SessionTombstoneState::valid)
    {
        error = validationError.isNotEmpty()
                  ? validationError
                  : juce::String (
                        "O bloqueio local da sessao nao foi confirmado");
        return false;
    }

    return true;
}

bool clearSessionTombstone (juce::String& error)
{
    error.clear();
    const auto file = sessionTombstoneFile();
    if (! file.exists())
        return true;

    if (! file.deleteFile() || file.exists())
    {
        error = "Nao foi possivel liberar o novo login local";
        return false;
    }

    return true;
}

SessionTombstoneState inspectEmergencySessionBlock (juce::String& error)
{
    error.clear();
   #if JUCE_WINDOWS
    HKEY key = nullptr;
    const auto openStatus = RegOpenKeyExW (
        HKEY_CURRENT_USER,
        emergencySessionBlockRegistryKey,
        0,
        KEY_QUERY_VALUE,
        &key);
    if (openStatus == ERROR_FILE_NOT_FOUND)
        return SessionTombstoneState::absent;
    if (openStatus != ERROR_SUCCESS || key == nullptr)
    {
        error = "Bloqueio emergencial da sessao nao pode ser verificado";
        return SessionTombstoneState::presentButInvalid;
    }

    DWORD type = 0;
    DWORD bytes = 0;
    auto queryStatus = RegQueryValueExW (
        key,
        emergencySessionBlockRegistryValue,
        nullptr,
        &type,
        nullptr,
        &bytes);
    if (queryStatus == ERROR_FILE_NOT_FOUND)
    {
        RegCloseKey (key);
        return SessionTombstoneState::absent;
    }
    if (queryStatus != ERROR_SUCCESS || type != REG_SZ
        || bytes < sizeof (wchar_t) || bytes > 8192
        || bytes % sizeof (wchar_t) != 0)
    {
        RegCloseKey (key);
        error = "Bloqueio emergencial da sessao presente, mas ilegivel";
        return SessionTombstoneState::presentButInvalid;
    }

    std::vector<wchar_t> value (
        static_cast<size_t> (bytes / sizeof (wchar_t)) + 1,
        L'\0');
    queryStatus = RegQueryValueExW (
        key,
        emergencySessionBlockRegistryValue,
        nullptr,
        &type,
        reinterpret_cast<LPBYTE> (value.data()),
        &bytes);
    RegCloseKey (key);
    if (queryStatus != ERROR_SUCCESS || type != REG_SZ
        || juce::String (value.data()) != emergencySessionBlockContents())
    {
        error = "Bloqueio emergencial da sessao presente, mas invalido";
        return SessionTombstoneState::presentButInvalid;
    }
   #else
    const auto file = emergencySessionBlockFile();
    if (! file.exists())
        return SessionTombstoneState::absent;
    if (! file.existsAsFile() || file.getSize() <= 0
        || file.getSize() > 4096
        || file.loadFileAsString() != emergencySessionBlockContents())
    {
        error = "Bloqueio emergencial da sessao presente, mas invalido";
        return SessionTombstoneState::presentButInvalid;
    }
   #endif
    return SessionTombstoneState::valid;
}

bool persistAndValidateEmergencySessionBlock (juce::String& error)
{
    error.clear();
   #if JUCE_WINDOWS
    HKEY key = nullptr;
    DWORD disposition = 0;
    auto status = RegCreateKeyExW (
        HKEY_CURRENT_USER,
        emergencySessionBlockRegistryKey,
        0,
        nullptr,
        REG_OPTION_NON_VOLATILE,
        KEY_SET_VALUE | KEY_QUERY_VALUE,
        nullptr,
        &key,
        &disposition);
    if (status == ERROR_SUCCESS && key != nullptr)
    {
        const std::wstring contents (
            emergencySessionBlockContents().toWideCharPointer());
        status = RegSetValueExW (
            key,
            emergencySessionBlockRegistryValue,
            0,
            REG_SZ,
            reinterpret_cast<const BYTE*> (contents.c_str()),
            static_cast<DWORD> (
                (contents.size() + 1) * sizeof (wchar_t)));
        if (status == ERROR_SUCCESS)
            status = RegFlushKey (key);
        RegCloseKey (key);
    }
    if (status != ERROR_SUCCESS)
    {
        error = "Nao foi possivel gravar o bloqueio emergencial da sessao";
        return false;
    }
   #else
    const auto folder = licensingDataFolder();
    if (folder.createDirectory().failed())
    {
        error = "Nao foi possivel criar o bloqueio emergencial da sessao";
        return false;
    }
    const auto temporary = folder.getNonexistentChildFile (
        "desktop-session-block-emergency-v1", ".tmp", false);
    if (! temporary.replaceWithText (emergencySessionBlockContents())
        || ! temporary.replaceFileIn (emergencySessionBlockFile()))
    {
        temporary.deleteFile();
        error = "Nao foi possivel gravar o bloqueio emergencial da sessao";
        return false;
    }
   #endif

    juce::String validationError;
    if (inspectEmergencySessionBlock (validationError)
        != SessionTombstoneState::valid)
    {
        error = validationError.isNotEmpty()
                  ? validationError
                  : juce::String (
                        "O bloqueio emergencial da sessao nao foi confirmado");
        return false;
    }
    return true;
}

bool clearEmergencySessionBlock (juce::String& error)
{
    error.clear();
   #if JUCE_WINDOWS
    HKEY key = nullptr;
    auto status = RegOpenKeyExW (
        HKEY_CURRENT_USER,
        emergencySessionBlockRegistryKey,
        0,
        KEY_SET_VALUE | KEY_QUERY_VALUE,
        &key);
    if (status == ERROR_FILE_NOT_FOUND)
        return true;
    if (status != ERROR_SUCCESS || key == nullptr)
    {
        error = "Nao foi possivel liberar o bloqueio emergencial do novo login";
        return false;
    }

    status = RegDeleteValueW (key, emergencySessionBlockRegistryValue);
    if (status == ERROR_SUCCESS)
        status = RegFlushKey (key);
    RegCloseKey (key);
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND)
    {
        error = "Nao foi possivel liberar o bloqueio emergencial do novo login";
        return false;
    }

    juce::String inspectionError;
    if (inspectEmergencySessionBlock (inspectionError)
        != SessionTombstoneState::absent
        || inspectionError.isNotEmpty())
    {
        error = inspectionError.isNotEmpty()
                  ? inspectionError
                  : juce::String (
                        "O bloqueio emergencial do novo login permaneceu ativo");
        return false;
    }
   #else
    const auto file = emergencySessionBlockFile();
    if (file.exists() && (! file.deleteFile() || file.exists()))
    {
        error = "Nao foi possivel liberar o bloqueio emergencial do novo login";
        return false;
    }
   #endif
    return true;
}

SessionInvalidationOperations productionInvalidationOperations()
{
    SessionInvalidationOperations operations;
    operations.persistAndValidateEmergencyBlock =
        [] (juce::String& error)
        {
            return persistAndValidateEmergencySessionBlock (error);
        };
    operations.inspectEmergencyBlock =
        [] (juce::String& error)
        {
            return inspectEmergencySessionBlock (error);
        };
    operations.persistAndValidateTombstone =
        [] (juce::String& error)
        {
            return persistAndValidateSessionTombstone (error);
        };
    operations.inspectTombstone =
        [] (juce::String& error)
        {
            return inspectSessionTombstone (error);
        };
    operations.removeCredential =
        [] (juce::String& error)
        {
            return CredentialStore::removeSecret (
                sessionCredentialAccount, error);
        };
    operations.credentialIsAbsent =
        [] (juce::String& error)
        {
            auto stored = CredentialStore::loadSecret (
                sessionCredentialAccount, error);
            const bool absent = stored.isEmpty() && error.isEmpty();
            stored.clear();
            return absent;
        };
    return operations;
}
}

SecureText::SecureText (const juce::String& value)
{
    assign (value);
}

SecureText::SecureText (const char* value)
{
    assign (juce::String (value != nullptr ? value : ""));
}

SecureText::~SecureText()
{
    clear();
}

SecureText::SecureText (SecureText&& other) noexcept
    : storage (std::move (other.storage)),
      logicalBytes (other.logicalBytes),
      characterCount (other.characterCount)
{
    other.logicalBytes = 0;
    other.characterCount = 0;
    if (! other.storage.empty())
        secureWipeBytes (other.storage.data(), other.storage.size());
}

SecureText& SecureText::operator= (SecureText&& other) noexcept
{
    if (this == &other)
        return *this;

    clear();
    storage = std::move (other.storage);
    logicalBytes = other.logicalBytes;
    characterCount = other.characterCount;
    other.logicalBytes = 0;
    other.characterCount = 0;
    if (! other.storage.empty())
        secureWipeBytes (other.storage.data(), other.storage.size());
    return *this;
}

SecureText& SecureText::operator= (const juce::String& value)
{
    assign (value);
    return *this;
}

void SecureText::assign (const juce::String& value)
{
    clear();
    logicalBytes = static_cast<size_t> (value.getNumBytesAsUTF8());
    characterCount = value.length();
    storage.resize (logicalBytes);
    if (logicalBytes > 0)
        std::memcpy (storage.data(), value.toRawUTF8(), logicalBytes);
}

void SecureText::assign (const SecureText& value)
{
    if (this == &value)
        return;

    clear();
    logicalBytes = value.logicalBytes;
    characterCount = value.characterCount;
    storage.resize (logicalBytes);
    if (logicalBytes > 0)
        std::memcpy (storage.data(), value.storage.data(), logicalBytes);
}

juce::String SecureText::reveal() const
{
    return logicalBytes > 0
        ? juce::String::fromUTF8 (
              reinterpret_cast<const char*> (storage.data()),
              static_cast<int> (logicalBytes))
        : juce::String();
}

void SecureText::clear() noexcept
{
    if (! storage.empty())
        secureWipeBytes (storage.data(), storage.size());
    logicalBytes = 0;
    characterCount = 0;
}

bool SecureText::isEmpty() const noexcept
{
    return logicalBytes == 0;
}

bool SecureText::isNotEmpty() const noexcept
{
    return ! isEmpty();
}

int SecureText::length() const noexcept
{
    return characterCount;
}

#if defined(CHURCHDAW_LICENSE_ENABLE_TEST_HOOKS) \
    && CHURCHDAW_LICENSE_ENABLE_TEST_HOOKS
bool SecureText::storageIsZeroedForTest() const noexcept
{
    return std::all_of (
        storage.begin(), storage.end(),
        [] (std::uint8_t byte) { return byte == 0; });
}

bool secureSensitiveStorageWipeSelfTest()
{
    SecureText queuedSecret (
        "refresh-token-da-fila-ABCDEFGHIJKLMNOPQRSTUVWXYZ");
    queuedSecret.clear();
    if (! queuedSecret.storageIsZeroedForTest())
        return false;

    juce::String requestTemporary (
        "password-ou-token-temporario-ABCDEFGHIJKLMNOPQRSTUVWXYZ");
    const auto bytes = static_cast<size_t> (
        requestTemporary.getNumBytesAsUTF8());
    auto* const raw = const_cast<char*> (
        requestTemporary.toRawUTF8());
    secureWipeBytes (raw, bytes);
    bool zeroed = true;
    for (size_t index = 0; index < bytes; ++index)
        zeroed = zeroed && raw[index] == 0;
    requestTemporary.clear();
    return zeroed;
}

bool boundedSensitiveReadWipeSelfTest()
{
    const auto normalText = juce::String ("access-refresh-lease-")
                          + juce::String::repeatedString ("S", 8192);
    juce::MemoryInputStream normalInput (
        normalText.toRawUTF8(),
        static_cast<size_t> (normalText.getNumBytesAsUTF8()),
        false);
    SensitiveReadWipeReport normalReport;
    bool normalTooLarge = false;
    auto normalCopy = readBoundedText (
        normalInput, normalTooLarge, &normalReport);
    ScopedOwnedJuceStringWiper normalCopyWiper (normalCopy);
    const bool normalPathPassed = ! normalTooLarge
        && normalCopy == normalText
        && normalReport.outputUsedBytes
             == static_cast<size_t> (normalText.getNumBytesAsUTF8())
        && normalReport.outputAllocatedBytes
             == static_cast<size_t> (maximumResponseBytes) + 1u
        && normalReport.chunkAllocatedBytes == 4096
        && normalReport.outputStorageAddressStable
        && normalReport.outputStorageZeroed
        && normalReport.chunkStorageZeroed;

    juce::MemoryBlock oversized (
        static_cast<size_t> (maximumResponseBytes + 1), true);
    ScopedMemoryBlockWiper oversizedWiper (oversized);
    std::memset (oversized.getData(), 0x5a, oversized.getSize());
    juce::MemoryInputStream oversizedInput (
        oversized.getData(), oversized.getSize(), false);
    SensitiveReadWipeReport oversizedReport;
    bool oversizedTooLarge = false;
    auto rejectedCopy = readBoundedText (
        oversizedInput, oversizedTooLarge, &oversizedReport);
    ScopedOwnedJuceStringWiper rejectedCopyWiper (rejectedCopy);
    const bool earlyReturnPassed = oversizedTooLarge
        && rejectedCopy.isEmpty()
        && oversizedReport.outputUsedBytes
             == static_cast<size_t> (maximumResponseBytes)
        && oversizedReport.outputAllocatedBytes
             == static_cast<size_t> (maximumResponseBytes) + 1u
        && oversizedReport.chunkAllocatedBytes == 4096
        && oversizedReport.outputStorageAddressStable
        && oversizedReport.outputStorageZeroed
        && oversizedReport.chunkStorageZeroed;

    class ThrowAfterFirstRead final : public juce::InputStream
    {
    public:
        juce::int64 getTotalLength() override { return 128; }
        bool isExhausted() override { return false; }
        juce::int64 getPosition() override { return position; }
        bool setPosition (juce::int64 next) override
        {
            if (next < 0 || next > 64)
                return false;
            position = next;
            return true;
        }

        int read (void* destination, int maximumBytes) override
        {
            if (! firstReadCompleted)
            {
                const auto amount = juce::jmin (64, maximumBytes);
                std::memset (destination, 0x6b,
                             static_cast<size_t> (amount));
                position += amount;
                firstReadCompleted = true;
                return amount;
            }

            throw 1;
        }

    private:
        juce::int64 position = 0;
        bool firstReadCompleted = false;
    } throwingInput;

    SensitiveReadWipeReport exceptionReport;
    bool exceptionTooLarge = false;
    bool exceptionObserved = false;
    try
    {
        auto unexpected = readBoundedText (
            throwingInput, exceptionTooLarge, &exceptionReport);
        ScopedOwnedJuceStringWiper unexpectedWiper (unexpected);
    }
    catch (...)
    {
        exceptionObserved = true;
    }

    const bool exceptionPathPassed = exceptionObserved
        && ! exceptionTooLarge
        && exceptionReport.outputUsedBytes == 64
        && exceptionReport.outputAllocatedBytes
             == static_cast<size_t> (maximumResponseBytes) + 1u
        && exceptionReport.chunkAllocatedBytes == 4096
        && exceptionReport.outputStorageAddressStable
        && exceptionReport.outputStorageZeroed
        && exceptionReport.chunkStorageZeroed;

    return normalPathPassed && earlyReturnPassed && exceptionPathPassed;
}
#endif

bool webRequestDeadlineCancellationSelfTest (int iterations)
{
    for (int index = 0; index < juce::jlimit (1, 256, iterations); ++index)
    {
        std::atomic<bool> cancel { false };
        ScopedWebRequestDeadline deadline (
            nullptr, 2000, &cancel, nullptr);
        cancel.store (true);
        const auto stopAt = juce::Time::getMillisecondCounterHiRes() + 500.0;
        while (! deadline.wasCancelled()
               && juce::Time::getMillisecondCounterHiRes() < stopAt)
            juce::Thread::sleep (1);
        if (! deadline.wasCancelled() || deadline.hasExpired())
            return false;
    }
    return true;
}

OperationEpochGate::Epoch OperationEpochGate::capture() const noexcept
{
    return current.load();
}

OperationEpochGate::Epoch OperationEpochGate::invalidate() noexcept
{
    const juce::ScopedLock scoped (commitLock);
    return current.fetch_add (1) + 1;
}

OperationEpochGate::Epoch OperationEpochGate::invalidateIfCurrent (
    Epoch expected) noexcept
{
    const juce::ScopedLock scoped (commitLock);
    if (current.load() != expected)
        return 0;

    return current.fetch_add (1) + 1;
}

bool OperationEpochGate::isCurrent (Epoch expected) const noexcept
{
    return expected != 0 && current.load() == expected;
}

bool OperationEpochGate::runIfCurrent (
    Epoch expected,
    const std::function<void()>& mutation) const noexcept
{
    const juce::ScopedLock scoped (commitLock);
    if (! isCurrent (expected) || mutation == nullptr)
        return false;

    mutation();
    return isCurrent (expected);
}

LocalSessionInvalidationResult invalidateLocalSession (
    const SessionInvalidationOperations& operations)
{
    LocalSessionInvalidationResult result;
    juce::String emergencyWriteError;
    juce::String emergencyInspectionError;
    juce::String tombstoneError;
    juce::String inspectionError;
    juce::String removalError;
    juce::String absenceError;

    const bool emergencyBlockWritten =
        operations.persistAndValidateEmergencyBlock != nullptr
        && operations.persistAndValidateEmergencyBlock (
            emergencyWriteError);
    const auto emergencyBlockState =
        operations.inspectEmergencyBlock != nullptr
        ? operations.inspectEmergencyBlock (emergencyInspectionError)
        : SessionTombstoneState::absent;
    result.emergencyBlockActive =
        emergencyBlockState != SessionTombstoneState::absent;

    const bool tombstoneWritten =
        operations.persistAndValidateTombstone != nullptr
        && operations.persistAndValidateTombstone (tombstoneError);
    const auto tombstoneState = operations.inspectTombstone != nullptr
        ? operations.inspectTombstone (inspectionError)
        : SessionTombstoneState::absent;
    result.tombstoneActive =
        tombstoneState != SessionTombstoneState::absent;

    const bool removalReported = operations.removeCredential != nullptr
        && operations.removeCredential (removalError);
    const bool absenceConfirmed = operations.credentialIsAbsent != nullptr
        && operations.credentialIsAbsent (absenceError);
    // O resultado do delete pode ser pessimista (por exemplo, a API removeu
    // o item e falhou ao confirmar o flush). A leitura posterior do cofre e a
    // evidencia autoritativa: ausencia confirmada tambem fecha o rollback.
    result.credentialRemoved = absenceConfirmed;
    result.guaranteed = result.emergencyBlockActive
                     || result.tombstoneActive
                     || result.credentialRemoved;

    juce::StringArray diagnostics;
    if (! emergencyBlockWritten && emergencyWriteError.isNotEmpty())
        diagnostics.add (emergencyWriteError);
    if (emergencyInspectionError.isNotEmpty())
        diagnostics.add (emergencyInspectionError);
    if (! tombstoneWritten && tombstoneError.isNotEmpty())
        diagnostics.add (tombstoneError);
    if (inspectionError.isNotEmpty())
        diagnostics.add (inspectionError);
    if (! removalReported && removalError.isNotEmpty())
        diagnostics.add (removalError);
    if (! absenceConfirmed && absenceError.isNotEmpty())
        diagnostics.add (absenceError);
    if (! result.guaranteed && diagnostics.isEmpty())
        diagnostics.add (
            "Nao foi possivel confirmar a invalidacao local da sessao");
    result.error = diagnostics.joinIntoString ("; ");
    return result;
}

SessionCredentialReplacementResult replaceLocalSessionCredential (
    const SessionCredentialReplacementOperations& operations)
{
    SessionCredentialReplacementResult result;
    juce::String saveError;
    juce::String emergencyClearError;
    juce::String tombstoneClearError;

    const bool saved = operations.saveCredential != nullptr
        && operations.saveCredential (saveError);
    const bool emergencyCleared = saved
        && operations.clearEmergencyBlock != nullptr
        && operations.clearEmergencyBlock (emergencyClearError);
    const bool tombstoneCleared = saved
        && operations.clearTombstone != nullptr
        && operations.clearTombstone (tombstoneClearError);

    if (saved && emergencyCleared && tombstoneCleared)
    {
        result.committed = true;
        result.failClosed = true;
        return result;
    }

    // Uma limpeza pode ter removido o marcador antes de falhar no flush ou na
    // verificacao. Reexecutar a invalidacao primeiro regrava os dois canais e
    // so depois tenta remover/confirmar a credencial parcialmente salva.
    const auto rollback = invalidateLocalSession (operations.invalidation);
    result.failClosed = rollback.guaranteed;

    juce::StringArray diagnostics;
    if (saveError.isNotEmpty())
        diagnostics.add (saveError);
    if (emergencyClearError.isNotEmpty())
        diagnostics.add (emergencyClearError);
    if (tombstoneClearError.isNotEmpty())
        diagnostics.add (tombstoneClearError);
    if (rollback.error.isNotEmpty())
        diagnostics.add (rollback.error);
    if (! result.failClosed && diagnostics.isEmpty())
        diagnostics.add (
            "Falha ao substituir a credencial sem bloqueio local confirmado");
    result.error = diagnostics.joinIntoString ("; ");
    return result;
}

bool localSessionResumeIsBlocked (
    const SessionInvalidationOperations& operations,
    juce::String& diagnostic)
{
    diagnostic.clear();
    if (operations.inspectEmergencyBlock == nullptr
        || operations.inspectTombstone == nullptr)
    {
        diagnostic = "Estado local da sessao nao pode ser verificado";
        return true;
    }

    juce::String emergencyDiagnostic;
    const auto emergencyState = operations.inspectEmergencyBlock (
        emergencyDiagnostic);
    juce::String tombstoneDiagnostic;
    const auto tombstoneState = operations.inspectTombstone (
        tombstoneDiagnostic);
    if (emergencyState == SessionTombstoneState::absent
        && tombstoneState == SessionTombstoneState::absent
        && emergencyDiagnostic.isEmpty()
        && tombstoneDiagnostic.isEmpty())
        return false;

    juce::StringArray diagnostics;
    if (emergencyDiagnostic.isNotEmpty())
        diagnostics.add (emergencyDiagnostic);
    if (tombstoneDiagnostic.isNotEmpty())
        diagnostics.add (tombstoneDiagnostic);
    diagnostic = diagnostics.joinIntoString ("; ");
    if (diagnostic.isEmpty())
        diagnostic = emergencyState == SessionTombstoneState::valid
                          || tombstoneState == SessionTombstoneState::valid
                       ? juce::String (
                             "Sessao encerrada localmente")
                       : juce::String (
                             "Bloqueio local de sessao presente");
    return true;
}

SessionSecrets::SessionSecrets (const juce::String& access,
                                const juce::String& refresh,
                                const juce::String& lease,
                                const juce::String& session)
    : accessToken (access),
      refreshToken (refresh),
      leaseToken (lease),
      sessionId (session)
{
}

void SessionSecrets::clear()
{
    accessToken.clear();
    refreshToken.clear();
    leaseToken.clear();
    sessionId.clear();
}

bool SessionSecrets::empty() const
{
    return accessToken.isEmpty() && refreshToken.isEmpty()
        && leaseToken.isEmpty() && sessionId.isEmpty();
}

ServerDecision classifySessionFailure (int httpStatus,
                                       juce::String code,
                                       bool replacementFlag)
{
    code = code.trim().toUpperCase();

    if (replacementFlag || code == "SESSION_REPLACED"
        || code == "SESSION_REPLACED_BY_NEW_DEVICE")
        return { ServerAction::terminateSession,
                 State::sessionReplaced,
                 "SESSION_REPLACED",
                 juce::String::fromUTF8 (
                     u8"Esta licença foi ativada em outro aparelho. Sua sessão foi encerrada automaticamente."),
                 true,
                 true };

    if (code == "ACCESS_TOKEN_EXPIRED" || code == "TOKEN_EXPIRED"
        || code == "AUTH_TOKEN_EXPIRED")
        return { ServerAction::refreshAccessToken,
                 State::activeOnline,
                 code,
                 {},
                 false,
                 true };

    if (httpStatus == 0 || httpStatus == 404 || httpStatus == 405
        || httpStatus == 408 || httpStatus == 425 || httpStatus == 429
        || httpStatus == 501 || httpStatus >= 500)
        return { ServerAction::retryTransient,
                 State::serverUnavailable,
                 code.isNotEmpty() ? code : juce::String ("NETWORK_UNAVAILABLE"),
                 "A conexao com o servidor foi interrompida. O ChurchDAW continuara no modo offline permitido.",
                 false,
                 true };

    if (code == "ACTIVATION_LIMIT")
        return { ServerAction::terminateSession,
                 State::activationConflict,
                 code,
                 "Esta licenca ja esta ativa em outro computador.",
                 false,
                 true };

    if (code == "LICENSE_PENDING")
        return { ServerAction::terminateSession,
                 State::licensePending,
                 code,
                 "Esta licenca ainda nao esta ativa. Confira a data de inicio ou aguarde a liberacao.",
                 true,
                 true };

    if (code == "LICENSE_SUSPENDED" || code == "LICENSE_PAUSED")
        return { ServerAction::terminateSession,
                 State::licenseSuspended,
                 "LICENSE_SUSPENDED",
                 "Esta licenca esta suspensa. Fale com o suporte ChurchDAW.",
                 true,
                 true };

    if (code == "LICENSE_REVOKED")
        return { ServerAction::terminateSession,
                 State::licenseRevoked,
                 code,
                 "Esta licenca foi revogada. Fale com o suporte ChurchDAW.",
                 true,
                 true };

    if (code == "LICENSE_EXPIRED")
        return { ServerAction::terminateSession,
                 State::licenseExpired,
                 code,
                 "O prazo desta licenca terminou. Renove o acesso para continuar.",
                 true,
                 true };

    if (code == "LICENSE_ARCHIVED")
        return { ServerAction::terminateSession,
                 State::licenseArchived,
                 code,
                 "Esta licenca esta arquivada. Fale com o suporte ChurchDAW.",
                 true,
                 true };

    if (code == "LICENSE_DELETED")
        return { ServerAction::terminateSession,
                 State::licenseDeleted,
                 code,
                 "Esta licenca nao esta mais disponivel. Fale com o suporte ChurchDAW.",
                 true,
                 true };

    if (code == "PASSWORD_CHANGE_REQUIRED")
        return { ServerAction::terminateSession,
                 State::passwordChangeRequired,
                 code,
                 "Sua senha precisa ser alterada antes de continuar. Use o botao abaixo para abrir sua conta.",
                 true,
                 true };

    if (code == "CLIENT_UPDATE_REQUIRED")
        return { ServerAction::terminateSession,
                 State::clientUpdateRequired,
                 code,
                 "Esta versao do ChurchDAW precisa ser atualizada antes de continuar.",
                 false,
                 true };

    if (code == "SESSION_EXPIRED" || code == "REFRESH_TOKEN_EXPIRED")
        return { ServerAction::terminateSession,
                 State::sessionExpired,
                 code,
                 "Sua sessao expirou. Entre novamente para continuar.",
                 true,
                 true };

    if (code == "SESSION_REVOKED" || code == "SESSION_REUSED"
        || code == "SESSION_INVALID" || code == "REFRESH_REVOKED"
        || code == "TOKEN_REVOKED" || httpStatus == 401
        || httpStatus == 403)
        return { ServerAction::terminateSession,
                 State::sessionRevoked,
                 code.isNotEmpty() ? code : juce::String ("SESSION_INVALID"),
                 "Sua sessao nao e mais valida. Entre novamente para continuar.",
                 true,
                 true };

    return { ServerAction::terminateSession,
             State::sessionRevoked,
             code.isNotEmpty() ? code : juce::String ("SESSION_VALIDATION_FAILED"),
             "Nao foi possivel validar esta sessao. Entre novamente.",
             true,
             true };
}

DeviceIdentityPlan planDeviceIdentity (int schema,
                                       bool legacyIdentityValid,
                                       bool clientDeviceIdValid) noexcept
{
    if (schema == 4 && legacyIdentityValid && clientDeviceIdValid)
        return DeviceIdentityPlan::preserveSchema4;

    if ((schema == 2 || schema == 3) && legacyIdentityValid)
        return DeviceIdentityPlan::migratePreservingInstallationId;

    return DeviceIdentityPlan::rejectStoredIdentity;
}

CredentialReadDecision decideCredentialRead (bool hasStoredValue,
                                               bool hasReadError) noexcept
{
    if (hasReadError)
        return CredentialReadDecision::abortWithoutOverwrite;

    return hasStoredValue ? CredentialReadDecision::useStored
                          : CredentialReadDecision::createNew;
}

LoginIdentifier normaliseLoginIdentifier (juce::String identifier)
{
    identifier = identifier.trim();
    if (identifier.containsChar ('@'))
    {
        identifier = normaliseEmail (std::move (identifier));
        return { isPlausibleEmailAddress (identifier)
                     ? LoginIdentifierKind::email
                     : LoginIdentifierKind::invalid,
                 std::move (identifier) };
    }

    identifier = normaliseLicenseKey (std::move (identifier));
    return { isValidLicenseKey (identifier)
                 ? LoginIdentifierKind::licenseKey
                 : LoginIdentifierKind::invalid,
             std::move (identifier) };
}

bool loginCredentialsAreValid (const LoginIdentifier& identifier,
                               int passwordLength) noexcept
{
    return identifier.valid()
        && passwordLength >= 8
        && passwordLength <= 256;
}

juce::var makeLoginIdentifierPayload (const LoginIdentifier& identifier)
{
    auto payload = makeObject();
    if (! identifier.valid())
        return payload;

    setProperty (payload, "identifier", identifier.value);
    if (identifier.kind == LoginIdentifierKind::email)
        setProperty (payload, "email", identifier.value);
    else if (identifier.kind == LoginIdentifierKind::licenseKey)
        setProperty (payload, "license_key", identifier.value);
    return payload;
}

juce::String createRandomClientDeviceIdentifier()
{
    return secureRandomToken (24);
}

int normaliseHeartbeatIntervalSeconds (int requested) noexcept
{
    return requested <= 0 ? defaultHeartbeatSeconds
                          : juce::jlimit (30, 60, requested);
}

int transientRetryDelaySeconds (int consecutiveFailures) noexcept
{
    constexpr std::array<int, 6> delays { 5, 10, 20, 30, 45, 60 };
    const auto index = juce::jlimit (0, (int) delays.size() - 1,
                                    consecutiveFailures - 1);
    return delays[static_cast<size_t> (index)];
}

bool shouldRefreshAccessToken (juce::int64 trustedNow,
                               juce::int64 accessExpiresAt) noexcept
{
    return accessExpiresAt <= 0 || trustedNow + 60 >= accessExpiresAt;
}

RuntimeValidity evaluateRuntimeValidity (juce::int64 trustedNow,
                                         juce::int64 leaseExpiresAt,
                                         juce::int64 offlineUntil,
                                         bool lastContactWasOnline) noexcept
{
    if (offlineUntil <= 0 || trustedNow > offlineUntil)
        return RuntimeValidity::expired;

    if (lastContactWasOnline
        && (leaseExpiresAt <= 0 || trustedNow <= leaseExpiresAt))
        return RuntimeValidity::online;

    return RuntimeValidity::offline;
}

juce::int64 advanceTrustedTimeHighWater (juce::int64 storedHighWater,
                                         juce::int64 candidate) noexcept
{
    return juce::jmax (storedHighWater, candidate);
}

juce::int64 normaliseSavedTrustedTimeHighWater (
    juce::int64 serverTime,
    juce::int64 legacyTrustedTime,
    juce::int64 storedHighWater) noexcept
{
    return advanceTrustedTimeHighWater (
        storedHighWater,
        advanceTrustedTimeHighWater (serverTime, legacyTrustedTime));
}

juce::int64 reserveTrustedTimeHighWater (
    juce::int64 storedHighWater,
    juce::int64 estimatedTrustedNow,
    juce::int64 reservationSeconds) noexcept
{
    return advanceTrustedTimeHighWater (
        storedHighWater,
        estimatedTrustedNow
            + juce::jmax<juce::int64> (0, reservationSeconds));
}

bool trustedTimeRollbackDetected (juce::int64 observedWallClock,
                                  juce::int64 trustedHighWater,
                                  juce::int64 toleranceSeconds) noexcept
{
    return trustedHighWater > 0
        && observedWallClock + juce::jmax<juce::int64> (0, toleranceSeconds)
             < trustedHighWater;
}

SignedLeaseUpdate validateSignedLeaseUpdate (
    const juce::String& candidateLeaseToken,
    const juce::String& expectedInstallationId,
    juce::int64 trustedNow,
    juce::int64 currentLeaseExpiresAt,
    juce::int64 currentOfflineUntil)
{
    SignedLeaseUpdate update {
        false, currentLeaseExpiresAt, currentOfflineUntil
    };

    if (candidateLeaseToken.isEmpty()
        || expectedInstallationId.isEmpty()
        || trustedNow <= 0)
        return update;

    VerifiedLease verified;
    juce::String verificationError;
    if (! verifyLeaseToken (candidateLeaseToken,
                            expectedInstallationId,
                            trustedNow,
                            false,
                            verified,
                            verificationError))
        return update;

    update.accepted = true;
    update.leaseExpiresAt = verified.leaseExpiresAt;
    update.offlineUntil = verified.offlineUntil;
    return update;
}

bool shouldAttemptRemoteLogout (bool hasAccessToken,
                                bool hasRefreshToken,
                                bool hasDeviceProof) noexcept
{
    return hasAccessToken || (hasRefreshToken && hasDeviceProof);
}

bool isUsableJsonHttpSuccess (int httpStatus,
                              bool hasObjectBody,
                              bool hasTransportError) noexcept
{
    return httpStatus >= 200 && httpStatus < 300
        && hasObjectBody && ! hasTransportError;
}

bool parseStrictJsonObject (const juce::String& text,
                            juce::var& parsedResult)
{
    parsedResult = juce::var();
    const auto length = text.length();
    int index = 0;
    while (index < length
           && juce::CharacterFunctions::isWhitespace (text[index]))
        ++index;

    if (index >= length || text[index] != '{')
        return false;

    juce::Array<juce::juce_wchar> delimiters;
    bool inString = false;
    bool escaped = false;
    bool complete = false;

    for (; index < length; ++index)
    {
        const auto character = text[index];
        if (inString)
        {
            if (escaped)
            {
                if (character == 'u')
                {
                    if (index + 4 >= length)
                        return false;
                    for (int digit = 1; digit <= 4; ++digit)
                        if (juce::CharacterFunctions::getHexDigitValue (
                                text[index + digit]) < 0)
                            return false;
                    index += 4;
                }
                else if (! juce::String ("\"\\/bfnrt")
                               .containsChar (character))
                {
                    return false;
                }

                escaped = false;
                continue;
            }

            if (character == '\\')
            {
                escaped = true;
                continue;
            }
            if (character == '"')
            {
                inString = false;
                continue;
            }
            if (character < 0x20)
                return false;
            continue;
        }

        if (character == '"')
        {
            inString = true;
            continue;
        }
        if (character == '\'')
            return false;
        if (character == '{' || character == '[')
        {
            delimiters.add (character);
            continue;
        }
        if (character == '}' || character == ']')
        {
            if (delimiters.isEmpty())
                return false;
            const auto opening = delimiters.getLast();
            if ((character == '}' && opening != '{')
                || (character == ']' && opening != '['))
                return false;
            delimiters.removeLast();
            if (delimiters.isEmpty())
            {
                complete = true;
                ++index;
                break;
            }
        }
    }

    if (! complete || inString || escaped)
        return false;
    for (; index < length; ++index)
        if (! juce::CharacterFunctions::isWhitespace (text[index]))
            return false;

    juce::var candidate;
    if (juce::JSON::parse (text, candidate).failed()
        || candidate.getDynamicObject() == nullptr)
        return false;

    parsedResult = std::move (candidate);
    return true;
}

namespace
{
bool isCanonicalUuid (const juce::String& value) noexcept
{
    if (value.length() != 36 || value[8] != '-' || value[13] != '-'
        || value[18] != '-' || value[23] != '-')
        return false;

    bool hasNonZeroDigit = false;
    for (int index = 0; index < value.length(); ++index)
    {
        if (index == 8 || index == 13 || index == 18 || index == 23)
            continue;
        const auto digit = juce::CharacterFunctions::getHexDigitValue (
            value[index]);
        if (digit < 0)
            return false;
        hasNonZeroDigit = hasNonZeroDigit || digit != 0;
    }
    return hasNonZeroDigit;
}

bool strictIntegerProperty (const juce::DynamicObject& object,
                            const juce::Identifier& key,
                            juce::int64 minimum,
                            juce::int64 maximum,
                            juce::int64& result) noexcept
{
    if (! object.hasProperty (key))
        return false;
    const auto value = object.getProperty (key);
    if (! value.isInt() && ! value.isInt64())
        return false;
    result = static_cast<juce::int64> (value);
    return result >= minimum && result <= maximum;
}

bool strictSecretStringProperty (const juce::DynamicObject& object,
                                 const juce::Identifier& key,
                                 int minimumLength,
                                 int maximumLength,
                                 juce::String& result)
{
    if (! object.hasProperty (key))
        return false;
    const auto value = object.getProperty (key);
    if (! value.isString())
        return false;
    result = value.toString();
    if (result.length() < minimumLength || result.length() > maximumLength)
        return false;
    for (int index = 0; index < result.length(); ++index)
        if (juce::CharacterFunctions::isWhitespace (result[index])
            || result[index] < 0x20)
            return false;
    return true;
}

bool strictSuccessCode (const juce::DynamicObject& object)
{
    const bool hasCode = object.hasProperty ("code");
    const bool hasErrorCode = object.hasProperty ("error_code");
    if (! hasCode && ! hasErrorCode)
        return false;

    const auto isAllowed = [] (const juce::var& value)
    {
        if (! value.isString())
            return false;
        const auto code = value.toString();
        return code == "OK" || code == "SESSION_ACTIVE";
    };
    return (! hasCode || isAllowed (object.getProperty ("code")))
        && (! hasErrorCode
            || isAllowed (object.getProperty ("error_code")));
}

bool isCompactBase64UrlToken (const juce::String& value)
{
    const auto parts = juce::StringArray::fromTokens (value, ".", "");
    if (parts.size() != 3)
        return false;

    constexpr auto base64UrlCharacters =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    for (const auto& part : parts)
        if (part.isEmpty() || ! part.containsOnly (base64UrlCharacters))
            return false;
    return true;
}

bool decodeBase64UrlSection (juce::String encoded,
                             juce::MemoryBlock& decoded)
{
    constexpr auto base64UrlCharacters =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    if (encoded.isEmpty() || ! encoded.containsOnly (base64UrlCharacters))
        return false;

    encoded = encoded.replaceCharacter ('-', '+')
                     .replaceCharacter ('_', '/');
    while (encoded.length() % 4 != 0)
        encoded += '=';

    juce::MemoryOutputStream output;
    if (! juce::Base64::convertFromBase64 (output, encoded))
        return false;
    decoded.replaceWith (output.getData(), output.getDataSize());
    return ! decoded.isEmpty() && decoded.getSize() <= 32 * 1024;
}

bool extractSignedLeaseSessionId (const juce::String& leaseToken,
                                  juce::String& sessionId)
{
    sessionId.clear();
    const auto parts = juce::StringArray::fromTokens (
        leaseToken, ".", "");
    if (parts.size() != 3)
        return false;

    juce::MemoryBlock payloadData;
    ScopedMemoryBlockWiper payloadDataWiper (payloadData);
    if (! decodeBase64UrlSection (parts[1], payloadData))
        return false;

    auto payloadText = juce::String::fromUTF8 (
        static_cast<const char*> (payloadData.getData()),
        static_cast<int> (payloadData.getSize()));
    ScopedOwnedJuceStringWiper payloadTextWiper (payloadText);
    juce::var payload;
    if (! parseStrictJsonObject (payloadText, payload))
        return false;

    const auto* object = payload.getDynamicObject();
    const bool hasSession = object->hasProperty ("session_id");
    const bool hasActivation = object->hasProperty ("activation_id");
    const auto session = object->getProperty ("session_id");
    const auto activation = object->getProperty ("activation_id");
    if ((hasSession && ! session.isString())
        || (hasActivation && ! activation.isString())
        || (! hasSession && ! hasActivation))
        return false;

    const auto signedSession = hasSession
                                 ? session.toString()
                                 : activation.toString();
    if (! isCanonicalUuid (signedSession)
        || (hasSession && hasActivation
            && activation.toString() != signedSession))
        return false;

    sessionId = signedSession;
    return true;
}

struct ParsedAuthSuccess
{
    SecureText accessToken;
    SecureText refreshToken;
    SecureText leaseToken;
    juce::String sessionId;
    juce::int64 serverTime = 0;
    juce::int64 expiresIn = 0;
    juce::int64 generation = 0;
    int heartbeatSeconds = 0;
    bool replacedPreviousSession = false;
    VerifiedLease verifiedLease;
};

bool parseStrictAuthSuccessPayload (
    const juce::var& body,
    const juce::String& expectedInstallationId,
    const juce::String& expectedSessionId,
    ParsedAuthSuccess& parsed,
    juce::String& error)
{
    error.clear();
    const auto* object = body.getDynamicObject();
    if (object == nullptr || expectedInstallationId.isEmpty())
    {
        error = "objeto ou dispositivo ausente";
        return false;
    }

    const auto status = object->getProperty ("status");
    const auto tokenType = object->getProperty ("token_type");
    // O backend pre-015 (schema 014) nao emitia os marcadores adicionados ao
    // contrato atual. A compatibilidade e deliberadamente reconhecida apenas
    // quando todos esses campos estao ausentes; qualquer resposta parcial ou
    // hibrida continua seguindo o contrato moderno e falha fechada.
    const bool legacySchema014 =
        ! object->hasProperty ("status")
        && ! object->hasProperty ("code")
        && ! object->hasProperty ("error_code")
        && ! object->hasProperty ("session_id")
        && ! object->hasProperty ("activation_id")
        && ! object->hasProperty ("heartbeat_interval_seconds")
        && ! object->hasProperty ("heartbeat_interval");
    const bool modernContractValid =
        status.isString() && status.toString() == "active"
        && strictSuccessCode (*object);
    if ((! legacySchema014 && ! modernContractValid)
        || ! tokenType.isString() || tokenType.toString() != "Bearer")
    {
        error = "status, codigo ou token_type invalido";
        return false;
    }

    juce::String accessToken;
    juce::String refreshToken;
    juce::String leaseToken;
    if (! strictSecretStringProperty (
            *object, "access_token", 32, 16384, accessToken)
        || ! strictSecretStringProperty (
            *object, "refresh_token", 32, 2048, refreshToken)
        || ! strictSecretStringProperty (
            *object, "lease_token", 16, 16384, leaseToken))
    {
        error = "tokens ausentes, tipados incorretamente ou fora do limite";
        return false;
    }
    if (! isCompactBase64UrlToken (accessToken)
        || ! validOpaqueIdentifier (refreshToken, 32, 2048))
    {
        error = "formato de token invalido";
        return false;
    }

    const auto sessionValue = object->getProperty ("session_id");
    if (sessionValue.isString())
        parsed.sessionId = sessionValue.toString();
    else if (! legacySchema014)
    {
        error = "session_id ausente";
        return false;
    }
    if (parsed.sessionId.isNotEmpty()
        && ! isCanonicalUuid (parsed.sessionId))
    {
        error = "session_id invalido ou incoerente";
        return false;
    }
    if (expectedSessionId.isNotEmpty()
        && parsed.sessionId.isNotEmpty()
        && parsed.sessionId != expectedSessionId)
    {
        error = "session_id invalido ou incoerente";
        return false;
    }

    if (object->hasProperty ("activation_id"))
    {
        const auto activation = object->getProperty ("activation_id");
        if (! activation.isString()
            || ! isCanonicalUuid (activation.toString())
            || activation.toString() != parsed.sessionId)
        {
            error = "activation_id incoerente";
            return false;
        }
    }

    juce::int64 heartbeat = defaultHeartbeatSeconds;
    if (! strictIntegerProperty (*object, "server_time", 1,
                                 std::numeric_limits<juce::int64>::max(),
                                 parsed.serverTime)
        || ! strictIntegerProperty (*object, "expires_in", 1, 86400,
                                    parsed.expiresIn)
        || (! legacySchema014
            && ! strictIntegerProperty (*object,
                                        "heartbeat_interval_seconds",
                                        30, 60, heartbeat)))
    {
        error = "tempo ou heartbeat invalido";
        return false;
    }
    parsed.heartbeatSeconds = static_cast<int> (heartbeat);

    if (object->hasProperty ("heartbeat_interval"))
    {
        juce::int64 alias = 0;
        if (! strictIntegerProperty (*object, "heartbeat_interval",
                                     30, 60, alias)
            || alias != heartbeat)
        {
            error = "aliases de heartbeat incoerentes";
            return false;
        }
    }

    if (object->hasProperty ("generation"))
    {
        if (! strictIntegerProperty (*object, "generation", 0,
                                     std::numeric_limits<int>::max(),
                                     parsed.generation))
        {
            error = "generation invalida";
            return false;
        }
    }

    if (object->hasProperty ("session_replaced"))
    {
        const auto replaced = object->getProperty ("session_replaced");
        if (! replaced.isBool())
        {
            error = "session_replaced deve ser booleano";
            return false;
        }
        parsed.replacedPreviousSession = static_cast<bool> (replaced);
    }

    parsed.accessToken.assign (accessToken);
    parsed.refreshToken.assign (refreshToken);
    parsed.leaseToken.assign (leaseToken);
    auto leaseForVerification = parsed.leaseToken.reveal();
    ScopedOwnedJuceStringWiper leaseWiper (leaseForVerification);
    juce::String verificationError;
    juce::String signedLeaseSessionId;
    if (! verifyLeaseToken (leaseForVerification,
                            expectedInstallationId,
                            parsed.serverTime,
                            false,
                            parsed.verifiedLease,
                            verificationError)
        || ! extractSignedLeaseSessionId (
               leaseForVerification, signedLeaseSessionId)
        || (parsed.sessionId.isNotEmpty()
            && signedLeaseSessionId != parsed.sessionId)
        || parsed.verifiedLease.leaseExpiresAt <= parsed.serverTime
        || parsed.verifiedLease.offlineUntil
             < parsed.verifiedLease.leaseExpiresAt)
    {
        error = verificationError.isNotEmpty()
                  ? verificationError
                  : juce::String ("lease assinado fora da validade");
        return false;
    }

    if (parsed.sessionId.isEmpty())
        parsed.sessionId = signedLeaseSessionId;
    if (expectedSessionId.isNotEmpty()
        && parsed.sessionId != expectedSessionId)
    {
        error = "session_id invalido ou incoerente";
        return false;
    }

    return true;
}

struct ParsedCompensatingLogout
{
    SecureText accessToken;
    SecureText refreshToken;
    SecureText sessionId;
};

bool parseCompensatingLogoutPayload (
    const juce::var& body,
    const juce::String& expectedSessionId,
    ParsedCompensatingLogout& parsed)
{
    const auto* object = body.getDynamicObject();
    if (object == nullptr)
        return false;

    juce::String accessToken;
    juce::String refreshToken;
    if (strictSecretStringProperty (
            *object, "access_token", 16, 16384, accessToken))
        parsed.accessToken.assign (accessToken);
    if (strictSecretStringProperty (
            *object, "refresh_token", 16, 2048, refreshToken))
        parsed.refreshToken.assign (refreshToken);

    juce::String responseSessionId;
    const auto responseSession = object->getProperty ("session_id");
    if (responseSession.isString()
        && isCanonicalUuid (responseSession.toString()))
        responseSessionId = responseSession.toString();
    else if (isCanonicalUuid (expectedSessionId))
        responseSessionId = expectedSessionId;

    if (responseSessionId.isNotEmpty())
        parsed.sessionId.assign (responseSessionId);

    // Para a compensacao, o bearer rotacionado sozinho ja identifica a
    // sessao no servidor. Sem bearer, refresh exige prova do dispositivo.
    return parsed.accessToken.isNotEmpty()
        || parsed.refreshToken.isNotEmpty();
}
}

bool isValidAuthSuccessPayload (
    const juce::var& body,
    const juce::String& expectedInstallationId,
    const juce::String& expectedSessionId)
{
    ParsedAuthSuccess parsed;
    juce::String error;
    return parseStrictAuthSuccessPayload (
        body, expectedInstallationId, expectedSessionId, parsed, error);
}

bool isValidRuntimeSuccessPayload (const juce::var& body,
                                   const juce::String& expectedSessionId)
{
    const auto* object = body.getDynamicObject();
    if (object == nullptr || expectedSessionId.isEmpty())
        return false;

    const auto status = object->getProperty ("status");
    const auto sessionId = object->getProperty ("session_id");
    const auto serverTime = object->getProperty ("server_time");
    const auto heartbeat = object->getProperty (
        "heartbeat_interval_seconds");
    if (! status.isString() || status.toString() != "active"
        || ! sessionId.isString()
        || ! isCanonicalUuid (sessionId.toString())
        || sessionId.toString() != expectedSessionId
        || (! serverTime.isInt() && ! serverTime.isInt64())
        || static_cast<juce::int64> (serverTime) <= 0
        || (! heartbeat.isInt() && ! heartbeat.isInt64()))
        return false;

    const auto heartbeatSeconds = static_cast<juce::int64> (heartbeat);
    if (heartbeatSeconds < 30 || heartbeatSeconds > 60)
        return false;

    const auto code = responseCode (body);
    return code.isEmpty() || code == "OK" || code == "SESSION_ACTIVE";
}

bool logoutQueueIdentityMatches (const juce::String& firstSessionId,
                                 juce::uint64 firstEpoch,
                                 const juce::String& secondSessionId,
                                 juce::uint64 secondEpoch) noexcept
{
    return firstEpoch != 0 && firstEpoch == secondEpoch
        && firstSessionId == secondSessionId;
}

int maximumPendingLogoutQueueSize() noexcept
{
    return maximumQueuedLogoutCount;
}

namespace
{
bool secureLogoutIdentityMatches (const SecureText& firstSessionId,
                                  juce::uint64 firstEpoch,
                                  const SecureText& secondSessionId,
                                  juce::uint64 secondEpoch)
{
    auto first = firstSessionId.reveal();
    auto second = secondSessionId.reveal();
    ScopedOwnedJuceStringWiper firstWiper (first);
    ScopedOwnedJuceStringWiper secondWiper (second);
    return logoutQueueIdentityMatches (
        first, firstEpoch, second, secondEpoch);
}
}

int remainingHttpDeadlineMs (int elapsedMs,
                             int totalDeadlineMs) noexcept
{
    return juce::jmax (0, totalDeadlineMs - juce::jmax (0, elapsedMs));
}

bool isActiveState (State state) noexcept
{
    return state == State::activeOnline || state == State::activeOffline;
}

bool isTerminalState (State state) noexcept
{
    switch (state)
    {
        case State::sessionExpired:
        case State::sessionReplaced:
        case State::sessionRevoked:
        case State::activationConflict:
        case State::licensePending:
        case State::licenseSuspended:
        case State::licenseRevoked:
        case State::licenseExpired:
        case State::licenseArchived:
        case State::licenseDeleted:
        case State::passwordChangeRequired:
        case State::clientUpdateRequired:
            return true;

        default:
            return false;
    }
}

bool gateEpochAllowsOpen (juce::uint64 scheduledEpoch,
                          juce::uint64 currentEpoch,
                          bool gatePassed,
                          bool terminalTransitionPending,
                          const Snapshot& snapshot) noexcept
{
    return scheduledEpoch == currentEpoch
        && gatePassed
        && ! terminalTransitionPending
        && snapshot.canOpenApp
        && isActiveState (snapshot.state)
        && ! snapshot.requiresAppClosure;
}

bool Client::DeviceIdentity::valid() const
{
    auto deviceSecret = secretBase64.reveal();
    ScopedOwnedJuceStringWiper secretWiper (deviceSecret);
    const bool baseValid = validOpaqueIdentifier (installationId)
                        && validOpaqueIdentifier (deviceSecret, 40);
    if (! baseValid)
        return false;

    return schema < 4 || validOpaqueIdentifier (clientDeviceId);
}

void Client::RuntimeSession::clear()
{
    secrets.clear();
    device = {};
    email.clear();
    displayName.clear();
    activationLabel.clear();
    accessExpiresAt = 0;
    leaseExpiresAt = 0;
    offlineUntil = 0;
    serverTime = 0;
    trustedTimeHighWater = 0;
    persistedTrustedTimeHighWater = 0;
    generation = 0;
    serverTimeAnchorMs = 0.0;
    heartbeatIntervalSeconds = defaultHeartbeatSeconds;
    remember = false;
    online = false;
    active = false;
}

class Client::LogoutWorker final : private juce::Thread
{
public:
    explicit LogoutWorker (Client& ownerToUse)
        : juce::Thread ("ChurchDAW desktop logout"),
          owner (ownerToUse)
    {
    }

    ~LogoutWorker() override
    {
        shutdown();
    }

    bool enqueue (PendingLogout next)
    {
        if (! next.canContactServer() || shuttingDown.load())
        {
            next.clear();
            return false;
        }

        {
            const juce::ScopedLock scoped (queueLock);
            if (shuttingDown.load())
            {
                next.clear();
                return false;
            }
            if (secureLogoutIdentityMatches (
                    inFlightSessionId, inFlightEpoch,
                    next.sessionId, next.epoch))
            {
                next.clear();
                return true;
            }
            for (const auto& queued : queue)
                if (secureLogoutIdentityMatches (
                        queued.sessionId, queued.epoch,
                        next.sessionId, next.epoch))
                {
                    next.clear();
                    return true;
                }

            if (! insertWithPriorityLocked (next))
            {
                next.clear();
                return false;
            }
        }

        if (! isThreadRunning()
            && ! startThread (juce::Thread::Priority::normal))
        {
            {
                const juce::ScopedLock scoped (queueLock);
                clearQueueLocked();
            }
            juce::Logger::writeToLog (
                "LICENCA DESKTOP: logout remoto em segundo plano nao pode ser iniciado");
            return false;
        }
        notify();
        return true;
    }

    void shutdown()
    {
        shuttingDown.store (true);
        signalThreadShouldExit();
        notify();
        waitForThreadToExit (-1);

        const juce::ScopedLock scoped (queueLock);
        clearQueueLocked();
    }

private:
    void run() override
    {
        while (! threadShouldExit())
        {
            PendingLogout next;
            {
                const juce::ScopedLock scoped (queueLock);
                if (! queue.empty())
                {
                    next = std::move (queue.front());
                    queue.pop_front();
                    inFlightSessionId.assign (next.sessionId);
                    inFlightEpoch = next.epoch;
                }
            }

            if (next.canContactServer())
            {
                if (! owner.performBestEffortLogout (next, &shuttingDown)
                    && ++next.attempts < 3
                    && ! threadShouldExit()
                    && ! shuttingDown.load())
                {
                    wait (transientRetryDelaySeconds (
                              next.attempts) * 1000);
                    if (! threadShouldExit() && ! shuttingDown.load())
                    {
                        const juce::ScopedLock scoped (queueLock);
                        inFlightSessionId.clear();
                        inFlightEpoch = 0;
                        insertWithPriorityLocked (next);
                    }
                }
                next.clear();
                {
                    const juce::ScopedLock scoped (queueLock);
                    inFlightSessionId.clear();
                    inFlightEpoch = 0;
                }
                continue;
            }

            {
                const juce::ScopedLock scoped (queueLock);
                inFlightSessionId.clear();
                inFlightEpoch = 0;
            }
            wait (1000);
        }
    }

    bool insertWithPriorityLocked (PendingLogout& next)
    {
        if (queue.size()
            >= static_cast<size_t> (maximumQueuedLogoutCount))
        {
            auto normal = std::find_if (
                queue.begin(), queue.end(),
                [] (const PendingLogout& pending)
                {
                    return ! pending.compensating;
                });

            // Um logout comum nunca pode desalojar uma resposta rotacionada
            // que ainda precisa de compensacao. A compensacao nova substitui
            // primeiro uma entrada comum e so entao a compensacao mais antiga.
            if (normal == queue.end())
            {
                if (! next.compensating)
                    return false;
                normal = std::prev (queue.end());
            }

            normal->clear();
            queue.erase (normal);
        }

        if (next.compensating)
            queue.push_front (std::move (next));
        else
            queue.push_back (std::move (next));
        return true;
    }

    void clearQueueLocked()
    {
        for (auto& pending : queue)
            pending.clear();
        queue.clear();
        inFlightSessionId.clear();
        inFlightEpoch = 0;
    }

    Client& owner;
    juce::CriticalSection queueLock;
    std::deque<PendingLogout> queue;
    SecureText inFlightSessionId;
    OperationEpoch inFlightEpoch = 0;
    std::atomic<bool> shuttingDown { false };
};

Client::Client()
    : juce::Thread ("ChurchDAW desktop licensing"),
      logoutWorker (std::make_unique<LogoutWorker> (*this))
{
}

Client::~Client()
{
    {
        const juce::ScopedLock dispatch (
            callbackDispatch->dispatchLock);
        callbackDispatch->alive = false;
    }
    shutdownRequested.store (true);
    signalThreadShouldExit();
    notify();
    waitForThreadToExit (-1);

    if (logoutWorker != nullptr)
        logoutWorker->shutdown();

    checkpointTrustedTimeHighWater (true, operationEpoch.capture());

    const juce::ScopedLock scoped (lock);
    pendingPassword.clear();
    runtime.clear();
}

#if defined(CHURCHDAW_LICENSE_ENABLE_TEST_HOOKS) \
    && CHURCHDAW_LICENSE_ENABLE_TEST_HOOKS
bool Client::dispatchSnapshotForTest (Snapshot next)
{
    return publish (std::move (next), operationEpoch.capture());
}

void Client::setCompensatingLogoutObserverForTest (
    CompensatingLogoutObserverForTest observer)
{
    const juce::ScopedLock scoped (lock);
    compensatingLogoutObserverForTest = std::move (observer);
}

juce::uint64 Client::captureOperationEpochForTest() const noexcept
{
    return operationEpoch.capture();
}

juce::uint64 Client::invalidateOperationEpochForSignOutTest()
{
    return invalidateOperationEpoch();
}

bool Client::processSupersededAuthResponseForTest (
    const juce::var& body,
    const juce::String& installationId,
    const juce::String& clientDeviceId,
    const juce::String& deviceSecretBase64,
    const juce::String& expectedSessionId,
    juce::uint64 requestEpoch)
{
    if (operationEpoch.isCurrent (requestEpoch))
        return false;

    HttpResult result;
    result.status = 200;
    result.body = body;
    DeviceIdentity device;
    device.schema = 4;
    device.legacySchema = 3;
    device.installationId = installationId;
    device.clientDeviceId = clientDeviceId;
    device.secretBase64 = deviceSecretBase64;
    return queueCompensatingLogoutFromAuthResponse (
        result, device, expectedSessionId, requestEpoch);
}
#endif

void Client::resumeSavedSession()
{
    if (isThreadRunning())
        return;

    const auto commandEpoch = invalidateOperationEpoch();
    if (! operationEpoch.runIfCurrent (
            commandEpoch,
            [&]
            {
                const juce::ScopedLock scoped (lock);
                pendingPassword.clear();
                forceStatusRequested.store (false);
            }))
        return;

    Snapshot next;
    next.state = State::resuming;
    next.busy = true;
    next.message = "Validando sua sessao protegida...";
    publish (std::move (next), commandEpoch);
    beginCommand (Command::resume, commandEpoch);
}

void Client::login (juce::String identifier,
                    juce::String password,
                    bool rememberSession)
{
    if (isThreadRunning())
        return;

    auto loginIdentifier = normaliseLoginIdentifier (std::move (identifier));
    const auto commandEpoch = invalidateOperationEpoch();

    if (! loginCredentialsAreValid (loginIdentifier, password.length()))
    {
        operationEpoch.runIfCurrent (
            commandEpoch,
            [&]
            {
                const juce::ScopedLock scoped (lock);
                pendingPassword.clear();
            });
        password.clear();
        Snapshot next;
        next.state = State::signedOut;
        next.email = loginIdentifier.value;
        next.message = "Confira o e-mail ou a chave de licenca e a senha.";
        publish (std::move (next), commandEpoch);
        return;
    }

    Snapshot next;
    if (! operationEpoch.runIfCurrent (
            commandEpoch,
            [&]
            {
                const juce::ScopedLock scoped (lock);
                pendingEmail = std::move (loginIdentifier.value);
                pendingPassword.assign (password);
                pendingRemember = rememberSession;
                forceStatusRequested.store (false);
                next.state = State::signingIn;
                next.email = pendingEmail;
                next.busy = true;
                next.message = "Protegendo e validando seu acesso...";
            }))
        return;

    publish (std::move (next), commandEpoch);
    beginCommand (Command::login, commandEpoch);
}

void Client::requestPasswordReset (juce::String email)
{
    if (isThreadRunning())
        return;

    auto loginIdentifier = normaliseLoginIdentifier (std::move (email));
    email = loginIdentifier.value;
    const auto commandEpoch = invalidateOperationEpoch();

    if (loginIdentifier.kind != LoginIdentifierKind::email)
    {
        operationEpoch.runIfCurrent (
            commandEpoch,
            [&]
            {
                const juce::ScopedLock scoped (lock);
                pendingPassword.clear();
            });
        Snapshot next = currentSnapshot();
        next.state = State::signedOut;
        next.busy = false;
        next.message = "Digite seu e-mail para recuperar o acesso.";
        publish (std::move (next), commandEpoch);
        return;
    }

    Snapshot next = currentSnapshot();
    if (! operationEpoch.runIfCurrent (
            commandEpoch,
            [&]
            {
                const juce::ScopedLock scoped (lock);
                pendingEmail = std::move (email);
                pendingPassword.clear();
                forceStatusRequested.store (false);
                next.email = pendingEmail;
            }))
        return;

    next.state = State::resuming;
    next.busy = true;
    next.message = "Solicitando recuperacao segura...";
    publish (std::move (next), commandEpoch);
    beginCommand (Command::passwordReset, commandEpoch);
}

void Client::signOut()
{
    const auto logoutEpoch = invalidateOperationEpoch();
    PendingLogout remoteLogout;
    juce::String email;
    const bool stateInvalidated = operationEpoch.runIfCurrent (
        logoutEpoch,
        [&]
        {
            const juce::ScopedLock scoped (lock);
            email = runtime.email.isNotEmpty() ? runtime.email : snapshot.email;
            remoteLogout.accessToken.assign (runtime.secrets.accessToken);
            remoteLogout.refreshToken.assign (runtime.secrets.refreshToken);
            remoteLogout.sessionId.assign (runtime.secrets.sessionId);
            remoteLogout.installationId = runtime.device.installationId;
            remoteLogout.clientDeviceId = runtime.device.clientDeviceId;
            remoteLogout.deviceSecretBase64.assign (
                runtime.device.secretBase64);
            remoteLogout.epoch = logoutEpoch;
            runtime.clear();
            pendingPassword.clear();
            forceStatusRequested.store (false);
            checkpointRequested.store (false);
        });
    if (! stateInvalidated)
        return;

    const auto invalidation = invalidatePersistentSession (logoutEpoch);
    if (! invalidation.credentialRemoved)
        juce::Logger::writeToLog (
            (invalidation.emergencyBlockActive
             || invalidation.tombstoneActive)
                ? "LICENCA DESKTOP: cofre nao removeu a sessao; marcador local bloqueou a retomada"
                : "LICENCA DESKTOP: invalidacao local da sessao nao pode ser confirmada");

    Snapshot next;
    next.state = State::signedOut;
    next.email = email;
    next.reasonCode = invalidation.guaranteed
                        ? "USER_LOGOUT_LOCAL_COMPLETE"
                        : "LOCAL_LOGOUT_INCOMPLETE";
    next.message = invalidation.guaranteed
                     ? juce::String (
                           "Sessao encerrada neste computador.")
                     : juce::String (
                           "A DAW foi bloqueada, mas nao foi possivel garantir a remocao da sessao salva. Tente sair novamente antes de fechar o programa.");
    next.canOpenApp = false;
    next.busy = false;
    next.online = false;
    publish (std::move (next), logoutEpoch);

    if (remoteLogout.canContactServer())
        enqueuePendingLogout (std::move (remoteLogout));
}

void Client::applicationSuspended()
{
    const auto suspendEpoch = operationEpoch.capture();
    bool sessionActive = false;
    if (! operationEpoch.runIfCurrent (
            suspendEpoch,
            [&]
            {
                const juce::ScopedLock scoped (lock);
                sessionActive = runtime.active;
                if (sessionActive)
                    checkpointRequested.store (true);
            })
        || ! sessionActive)
        return;

    notify();

    if (! isThreadRunning())
        checkpointTrustedTimeHighWater (true, suspendEpoch);
}

void Client::applicationResumed()
{
    // Alguns contadores monotonicamente crescentes podem pausar durante sleep.
    // Na retomada aceitamos apenas o avanco do relogio civil; um rollback nunca
    // reduz o piso ja protegido. O proximo ciclo persiste esse novo high-water.
    const auto resumeEpoch = operationEpoch.capture();
    bool sessionActive = false;
    if (! operationEpoch.runIfCurrent (
            resumeEpoch,
            [&]
            {
                const juce::ScopedLock scoped (lock);
                if (! runtime.active)
                    return;

                sessionActive = true;
                const auto resumedTrustedNow = advanceTrustedTimeHighWater (
                    runtime.trustedTimeHighWater,
                    advanceTrustedTimeHighWater (
                        estimatedTrustedNowSecondsLocked(), unixTimeNow()));
                runtime.serverTime = resumedTrustedNow;
                runtime.serverTimeAnchorMs = monotonicTimeMs();
                runtime.trustedTimeHighWater = resumedTrustedNow;
                checkpointRequested.store (true);
                forceStatusRequested.store (true);
            }))
        return;

    if (! sessionActive)
        return;

    notify();
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

juce::URL Client::downloadUrl()
{
    return juce::URL ("https://churchdaw.com/");
}

void Client::beginCommand (Command next, OperationEpoch commandEpoch)
{
    jassert (! isThreadRunning());
    if (! operationEpoch.runIfCurrent (
            commandEpoch,
            [&]
            {
                const juce::ScopedLock scoped (lock);
                command = next;
                pendingCommandEpoch = commandEpoch;
            }))
        return;

    if (! startThread (juce::Thread::Priority::normal))
    {
        Snapshot failed;
        if (! operationEpoch.runIfCurrent (
                commandEpoch,
                [&]
                {
                    const juce::ScopedLock scoped (lock);
                    command = Command::none;
                    pendingCommandEpoch = 0;
                    pendingPassword.clear();
                    failed = snapshot;
                }))
            return;
        failed.state = State::serverUnavailable;
        failed.busy = false;
        failed.message = "Nao foi possivel iniciar a validacao da licenca.";
        publish (std::move (failed), commandEpoch);
    }
}

void Client::run()
{
    Command commandToRun;
    OperationEpoch commandEpoch = 0;
    {
        const juce::ScopedLock scoped (lock);
        commandToRun = command;
        commandEpoch = pendingCommandEpoch;
    }

    bool keepSessionAlive = false;
    switch (commandToRun)
    {
        case Command::resume:
            keepSessionAlive = runResume (commandEpoch);
            break;
        case Command::login:
            keepSessionAlive = runLogin (commandEpoch);
            break;
        case Command::passwordReset:
            runPasswordReset (commandEpoch);
            break;
        case Command::none:                                        break;
    }

    {
        const juce::ScopedLock scoped (lock);
        command = Command::none;
        pendingCommandEpoch = 0;
    }

    if (keepSessionAlive && ! threadShouldExit())
        runSessionLoop (commandEpoch);
}

bool Client::runResume (OperationEpoch commandEpoch)
{
    if (! operationEpoch.isCurrent (commandEpoch))
        return false;

    juce::String error;
    const auto saved = loadSavedSession (commandEpoch, error);

    if (saved.refreshToken.isEmpty())
    {
        Snapshot next;
        next.state = State::signedOut;
        next.email = saved.email;
        next.message = error.isNotEmpty()
                         ? "A sessao salva nao pode ser aberta. Entre novamente."
                         : "Entre para ativar este computador.";
        publish (std::move (next), commandEpoch);
        return false;
    }

    const auto device = ensureDeviceIdentity (commandEpoch, error);
    if (! device.valid())
    {
        Snapshot next;
        next.state = State::signedOut;
        next.email = saved.email;
        next.message = "Nao foi possivel proteger a identidade deste computador.";
        publish (std::move (next), commandEpoch);
        return false;
    }

    if (! operationEpoch.runIfCurrent (
            commandEpoch,
            [&]
            {
                const juce::ScopedLock scoped (lock);
                runtime.clear();
                runtime.device = device;
                runtime.email = saved.email;
                runtime.secrets.refreshToken.assign (saved.refreshToken);
                runtime.secrets.leaseToken.assign (saved.leaseToken);
                runtime.secrets.sessionId = saved.sessionId;
                runtime.serverTime = saved.serverTime;
                runtime.trustedTimeHighWater = saved.trustedTimeHighWater;
                runtime.persistedTrustedTimeHighWater =
                    saved.trustedTimeHighWater;
                runtime.serverTimeAnchorMs = monotonicTimeMs();
                runtime.generation = saved.generation;
                runtime.heartbeatIntervalSeconds =
                    normaliseHeartbeatIntervalSeconds (
                        saved.heartbeatIntervalSeconds);
                runtime.remember = true;
            }))
        return false;

    OperationEpoch requestEpoch = 0;
    const auto outcome = performRefresh (commandEpoch, requestEpoch);
    if (outcome == RequestOutcome::success)
        return true;

    if (outcome == RequestOutcome::superseded)
        return false;

    if (outcome == RequestOutcome::transientFailure
        && acceptOfflineLease (saved, device, requestEpoch))
    {
        juce::Logger::writeToLog (
            "LICENCA DESKTOP: servidor indisponivel; lease offline aceita");
        return true;
    }

    if (outcome == RequestOutcome::transientFailure)
    {
        clearRuntimeSession (false, requestEpoch);
        Snapshot next;
        next.state = State::serverUnavailable;
        next.email = saved.email;
        next.message = "O servidor esta indisponivel e o prazo offline terminou. Conecte-se e entre novamente.";
        publish (std::move (next), requestEpoch);
    }

    return false;
}

bool Client::runLogin (OperationEpoch commandEpoch)
{
    if (! operationEpoch.isCurrent (commandEpoch))
        return false;

    juce::String identifier;
    SecureText password;
    bool remember = true;

    if (! operationEpoch.runIfCurrent (
            commandEpoch,
            [&]
            {
                const juce::ScopedLock scoped (lock);
                identifier = pendingEmail;
                password = std::move (pendingPassword);
                remember = pendingRemember;
                pendingPassword.clear();
            }))
        return false;

    const auto loginIdentifier = normaliseLoginIdentifier (identifier);
    if (! loginCredentialsAreValid (loginIdentifier, password.length()))
    {
        password.clear();
        Snapshot next;
        next.state = State::signedOut;
        next.email = loginIdentifier.value;
        next.message = "Confira o e-mail ou a chave de licenca e a senha.";
        publish (std::move (next), commandEpoch);
        return false;
    }

    juce::String error;
    const auto device = ensureDeviceIdentity (commandEpoch, error);
    if (! device.valid())
    {
        Snapshot next;
        next.state = State::signedOut;
        next.email = loginIdentifier.value;
        next.message = "Nao foi possivel proteger a identidade deste computador.";
        publish (std::move (next), commandEpoch);
        return false;
    }

    auto payload = makeLoginIdentifierPayload (loginIdentifier);
    ScopedSensitiveJsonWiper payloadWiper (payload);
    auto passwordForRequest = password.reveal();
    ScopedOwnedJuceStringWiper passwordWiper (passwordForRequest);
    auto deviceSecretForRequest = device.secretBase64.reveal();
    ScopedOwnedJuceStringWiper deviceSecretWiper (
        deviceSecretForRequest);
    setProperty (payload, "password", passwordForRequest);
    setProperty (payload, "installation_id", device.installationId);
    if (device.clientDeviceId.isNotEmpty())
        setProperty (payload, "client_device_id", device.clientDeviceId);
    setProperty (payload, "device_secret", deviceSecretForRequest);
    setProperty (payload, "device_label", device.label);
    setProperty (payload, "platform", currentPlatform());
    setProperty (payload, "os_version",
                 juce::SystemStats::getOperatingSystemName());
    setProperty (payload, "architecture", currentArchitecture());
    setProperty (payload, "timestamp", unixTimeNow());

    auto* application = juce::JUCEApplicationBase::getInstance();
    setProperty (payload, "app_version",
                 application != nullptr
                    ? application->getApplicationVersion()
                    : juce::String());

    password.clear();

    if (threadShouldExit())
        return false;

    const auto requestEpoch = commandEpoch;
    if (! operationEpoch.isCurrent (requestEpoch))
        return false;

    auto result = requestJson ("/auth/login", payload);
    ScopedSensitiveJsonWiper responseWiper (result.body);
    if (! operationEpoch.isCurrent (requestEpoch))
    {
        queueCompensatingLogoutFromAuthResponse (
            result, device, {}, requestEpoch);
        return false;
    }

    return acceptAuthResponse (result, remember, device, {}, false,
                               requestEpoch);
}

void Client::runPasswordReset (OperationEpoch commandEpoch)
{
    if (! operationEpoch.isCurrent (commandEpoch))
        return;

    juce::String email;
    {
        const juce::ScopedLock scoped (lock);
        email = pendingEmail;
    }

    auto payload = makeObject();
    setProperty (payload, "email", email);
    const auto requestEpoch = commandEpoch;
    if (! operationEpoch.isCurrent (requestEpoch))
        return;
    const auto result = requestJson ("/auth/password/forgot", payload);
    if (! operationEpoch.isCurrent (requestEpoch))
        return;

    Snapshot next;
    next.state = State::passwordResetSent;
    next.email = email;
    next.busy = false;
    next.message = "Se este e-mail estiver cadastrado, enviaremos as instrucoes.";

    if (result.status == 0 && result.transportError.isNotEmpty())
        next.message = "Nao foi possivel conectar agora. Tente novamente.";

    publish (std::move (next), requestEpoch);
}

void Client::runSessionLoop (OperationEpoch sessionEpoch)
{
    if (! operationEpoch.isCurrent (sessionEpoch))
        return;

    int initialHeartbeatSeconds = defaultHeartbeatSeconds;
    {
        const juce::ScopedLock scoped (lock);
        initialHeartbeatSeconds = runtime.heartbeatIntervalSeconds;
    }
    auto nextContactMs = monotonicTimeMs()
                       + normaliseHeartbeatIntervalSeconds (
                             initialHeartbeatSeconds) * 1000.0;
    int consecutiveFailures = 0;

    while (! threadShouldExit()
           && operationEpoch.isCurrent (sessionEpoch)
           && runtimeSessionIsActive())
    {
        bool forceCheckpoint = false;
        if (! operationEpoch.runIfCurrent (
                sessionEpoch,
                [&]
                {
                    forceCheckpoint = checkpointRequested.exchange (false);
                }))
            return;

        checkpointTrustedTimeHighWater (
            forceCheckpoint,
            sessionEpoch);

        const auto trustedNow = trustedNowSeconds();
        juce::int64 leaseExpiresAt = 0;
        juce::int64 offlineUntil = 0;
        juce::int64 accessExpiresAt = 0;
        bool online = false;
        bool hasAccessToken = false;
        int heartbeatSeconds = defaultHeartbeatSeconds;

        {
            const juce::ScopedLock scoped (lock);
            leaseExpiresAt = runtime.leaseExpiresAt;
            offlineUntil = runtime.offlineUntil;
            accessExpiresAt = runtime.accessExpiresAt;
            online = runtime.online;
            hasAccessToken = runtime.secrets.accessToken.isNotEmpty();
            heartbeatSeconds = runtime.heartbeatIntervalSeconds;
        }

        if (evaluateRuntimeValidity (trustedNow, leaseExpiresAt,
                                     offlineUntil, online)
            == RuntimeValidity::expired)
        {
            publishTerminal (
                { ServerAction::terminateSession,
                  State::sessionExpired,
                  "OFFLINE_WINDOW_EXPIRED",
                  "O prazo offline desta sessao terminou. Conecte-se e entre novamente.",
                  true,
                  true },
                true,
                sessionEpoch);
            return;
        }

        bool forcedStatus = false;
        if (! operationEpoch.runIfCurrent (
                sessionEpoch,
                [&]
                {
                    forcedStatus = forceStatusRequested.exchange (false);
                }))
            return;
        const bool refreshDue = ! hasAccessToken
                             || shouldRefreshAccessToken (trustedNow,
                                                          accessExpiresAt)
                             || (leaseExpiresAt > 0
                                 && trustedNow + 30 >= leaseExpiresAt);
        const auto nowMs = monotonicTimeMs();

        // Offline/transient refreshes obey nextContactMs. Sem esta condicao,
        // accessToken vazio faria a thread furar o backoff em um loop apertado.
        const bool contactDue = forcedStatus || nowMs >= nextContactMs
                             || (refreshDue && online);
        if (contactDue)
        {
            OperationEpoch requestEpoch = 0;
            auto outcome = refreshDue
                         ? performRefresh (sessionEpoch, requestEpoch)
                         : performSessionValidation (forcedStatus,
                                                     sessionEpoch,
                                                     requestEpoch);

            // Somente heartbeat/status ganha uma tentativa de refresh. Uma
            // falha do proprio refresh e terminal e nunca entra em rotacao.
            if (! refreshDue
                && outcome == RequestOutcome::accessTokenExpired)
                outcome = performRefresh (sessionEpoch, requestEpoch);

            if (outcome == RequestOutcome::superseded)
                continue;

            if (outcome == RequestOutcome::success)
            {
                consecutiveFailures = 0;
                {
                    const juce::ScopedLock scoped (lock);
                    heartbeatSeconds = runtime.heartbeatIntervalSeconds;
                }
                nextContactMs = monotonicTimeMs()
                              + normaliseHeartbeatIntervalSeconds (
                                    heartbeatSeconds) * 1000.0;
                continue;
            }

            if (outcome == RequestOutcome::terminalFailure)
                return;

            if (outcome == RequestOutcome::accessTokenExpired)
            {
                publishTerminal (
                    classifySessionFailure (401, "SESSION_INVALID"),
                    true,
                    requestEpoch);
                return;
            }

            ++consecutiveFailures;
            const auto offlineNow = trustedNowSeconds();
            if (! operationEpoch.runIfCurrent (
                    requestEpoch,
                    [&]
                    {
                        const juce::ScopedLock scoped (lock);
                        runtime.online = false;
                        leaseExpiresAt = runtime.leaseExpiresAt;
                        offlineUntil = runtime.offlineUntil;
                    }))
                continue;

            if (evaluateRuntimeValidity (offlineNow, leaseExpiresAt,
                                         offlineUntil, false)
                == RuntimeValidity::expired)
            {
                publishTerminal (
                    { ServerAction::terminateSession,
                      State::sessionExpired,
                      "OFFLINE_WINDOW_EXPIRED",
                      "Nao foi possivel validar online e o prazo offline terminou. Entre novamente.",
                      true,
                      true },
                    true,
                    requestEpoch);
                return;
            }

            publishActiveSnapshot (
                false,
                "Modo offline protegido. A sessao sera validada assim que a conexao voltar.",
                requestEpoch);
            nextContactMs = monotonicTimeMs()
                          + transientRetryDelaySeconds (
                                consecutiveFailures) * 1000.0;
        }

        const auto waitMs = juce::jlimit (
            50, 1000,
            static_cast<int> (std::ceil (nextContactMs
                                         - monotonicTimeMs())));
        wait (waitMs);
    }
}

Client::DeviceIdentity Client::ensureDeviceIdentity (
    OperationEpoch expectedEpoch,
    juce::String& error)
{
    if (! operationEpoch.isCurrent (expectedEpoch))
        return {};

    auto saved = CredentialStore::loadSecret (
        deviceCredentialAccount, error);
    ScopedOwnedJuceStringWiper savedWiper (saved);
    if (! operationEpoch.isCurrent (expectedEpoch))
        return {};

    const auto readDecision = decideCredentialRead (
        saved.isNotEmpty(), error.isNotEmpty());

    if (readDecision == CredentialReadDecision::abortWithoutOverwrite)
    {
        juce::Logger::writeToLog (
            "LICENCA DESKTOP: leitura da identidade protegida falhou; nenhum dado foi criado ou sobrescrito");
        return {};
    }

    if (readDecision == CredentialReadDecision::useStored)
    {
        juce::var parsed;
        ScopedSensitiveJsonWiper parsedWiper (parsed);
        if (juce::JSON::parse (saved, parsed).failed()
            || parsed.getDynamicObject() == nullptr)
        {
            error = "Identidade protegida invalida";
            return {};
        }

        DeviceIdentity existing;
        existing.schema = static_cast<int> (
            propertyInt64 (parsed, "schema"));
        existing.legacySchema = static_cast<int> (
            propertyInt64 (parsed, "legacy_schema"));
        existing.installationId =
            propertyString (parsed, "installation_id");
        existing.clientDeviceId =
            propertyString (parsed, "client_device_id");
        auto storedDeviceSecret =
            propertyString (parsed, "device_secret");
        ScopedOwnedJuceStringWiper storedSecretWiper (
            storedDeviceSecret);
        existing.secretBase64.assign (storedDeviceSecret);
        existing.label = propertyString (parsed, "device_label");

        auto existingSecret = existing.secretBase64.reveal();
        ScopedOwnedJuceStringWiper existingSecretWiper (
            existingSecret);
        const bool legacyValid =
            validOpaqueIdentifier (existing.installationId)
            && validOpaqueIdentifier (existingSecret, 40);
        const auto plan = planDeviceIdentity (
            existing.schema,
            legacyValid,
            validOpaqueIdentifier (existing.clientDeviceId));

        if (plan == DeviceIdentityPlan::preserveSchema4
            && existing.valid())
            return existing;

        if (plan == DeviceIdentityPlan::migratePreservingInstallationId)
        {
            DeviceIdentity migrated = existing;
            migrated.legacySchema = existing.schema;
            migrated.schema = 4;
            migrated.clientDeviceId = createRandomClientDeviceIdentifier();
            if (migrated.label.isEmpty())
                migrated.label = deviceLabel();

            juce::String migrationError;
            if (migrated.valid()
                && saveDeviceIdentity (migrated, expectedEpoch,
                                       migrationError))
            {
                juce::Logger::writeToLog (
                    "LICENCA DESKTOP: identidade legada migrada para schema 4 sem alterar installation_id");
                return migrated;
            }

            // Compatibilidade: uma falha ao atualizar o cofre nao muda o
            // installation_id que o servidor ja conhece nem bloqueia o dono.
            existing.clientDeviceId.clear();
            juce::Logger::writeToLog (
                "LICENCA DESKTOP: migracao de identidade adiada; credencial legada preservada");
            return existing;
        }

        error = "Identidade protegida incompativel";
        juce::Logger::writeToLog (
            "LICENCA DESKTOP: identidade existente recusada sem sobrescrever o cofre");
        return {};
    }

    const auto clientDeviceId = createRandomClientDeviceIdentifier();
    auto generatedDeviceSecret = randomToken (32);
    ScopedOwnedJuceStringWiper generatedSecretWiper (
        generatedDeviceSecret);
    DeviceIdentity created;
    created.schema = 4;
    created.installationId = clientDeviceId;
    created.clientDeviceId = clientDeviceId;
    created.secretBase64.assign (generatedDeviceSecret);
    created.label = deviceLabel();

    if (! created.valid()
        || ! saveDeviceIdentity (created, expectedEpoch, error))
        return {};

    juce::Logger::writeToLog (
        "LICENCA DESKTOP: nova identidade aleatoria schema 4 protegida no cofre do sistema");
    return created;
}

bool Client::saveDeviceIdentity (const DeviceIdentity& identity,
                                 OperationEpoch expectedEpoch,
                                 juce::String& error) const
{
    auto value = makeObject();
    ScopedSensitiveJsonWiper valueWiper (value);
    auto deviceSecret = identity.secretBase64.reveal();
    ScopedOwnedJuceStringWiper deviceSecretWiper (deviceSecret);
    setProperty (value, "schema", identity.schema);
    if (identity.legacySchema > 0)
        setProperty (value, "legacy_schema", identity.legacySchema);
    setProperty (value, "installation_id", identity.installationId);
    setProperty (value, "client_device_id", identity.clientDeviceId);
    setProperty (value, "device_secret", deviceSecret);
    setProperty (value, "device_label", identity.label);
    bool saved = false;
    const bool epochStayedCurrent = operationEpoch.runIfCurrent (
        expectedEpoch,
        [&]
        {
            auto serialized = juce::JSON::toString (value, false);
            ScopedOwnedJuceStringWiper serializedWiper (serialized);
            saved = CredentialStore::saveSecret (
                deviceCredentialAccount,
                serialized, error);
        });
    if (! epochStayedCurrent && error.isEmpty())
        error = "Operacao de identidade substituida";
    return epochStayedCurrent && saved;
}

Client::SavedSession Client::loadSavedSession (
    OperationEpoch expectedEpoch,
    juce::String& error) const
{
    if (! operationEpoch.isCurrent (expectedEpoch))
        return {};

    const juce::ScopedLock persistence (persistenceLock);
    const auto operations = productionInvalidationOperations();
    juce::String tombstoneDiagnostic;
    if (localSessionResumeIsBlocked (operations, tombstoneDiagnostic))
    {
        juce::String removalError;
        bool removed = false;
        const bool removalApplied = operationEpoch.runIfCurrent (
            expectedEpoch,
            [&]
            {
                removed = operations.removeCredential != nullptr
                    && operations.removeCredential (removalError);
            });
        if (! removalApplied)
            return {};
        if (! removed)
            juce::Logger::writeToLog (
                "LICENCA DESKTOP: tombstone bloqueou uma credencial antiga que o cofre nao removeu");

        error = "Esta sessao foi encerrada localmente. Entre novamente.";
        return {};
    }

    auto saved = CredentialStore::loadSecret (
        sessionCredentialAccount, error);
    ScopedOwnedJuceStringWiper savedWiper (saved);

    if (saved.isEmpty())
        return {};

    juce::var parsed;
    ScopedSensitiveJsonWiper parsedWiper (parsed);
    if (juce::JSON::parse (saved, parsed).failed())
    {
        error = "Sessao protegida invalida";
        return {};
    }

    const auto legacyTrustedTime = propertyInt64 (parsed, "trusted_time");
    auto serverTime = propertyInt64 (parsed, "server_time");
    if (serverTime <= 0)
        serverTime = legacyTrustedTime;
    const auto trustedTimeHighWater =
        normaliseSavedTrustedTimeHighWater (
            serverTime,
            legacyTrustedTime,
            propertyInt64 (parsed, "trusted_time_high_water"));

    SavedSession result;
    result.email = propertyString (parsed, "email");
    result.refreshToken = propertyString (parsed, "refresh_token");
    result.leaseToken = propertyString (parsed, "lease_token");
    result.sessionId = makeIndependentStringCopy (
        propertyString (parsed, "session_id"));
    result.serverTime = serverTime;
    result.trustedTimeHighWater = trustedTimeHighWater;
    result.generation = propertyInt64 (parsed, "generation");
    result.heartbeatIntervalSeconds = normaliseHeartbeatIntervalSeconds (
        static_cast<int> (propertyInt64 (
            parsed, "heartbeat_interval_seconds")));
    return result;
}

bool Client::saveSession (const SavedSession& session,
                          juce::String& error) const
{
    auto value = makeObject();
    ScopedSensitiveJsonWiper valueWiper (value);
    auto refreshToken = session.refreshToken.reveal();
    auto leaseToken = session.leaseToken.reveal();
    ScopedOwnedJuceStringWiper refreshWiper (refreshToken);
    ScopedOwnedJuceStringWiper leaseWiper (leaseToken);
    setProperty (value, "schema", 4);
    setProperty (value, "email", session.email);
    setProperty (value, "refresh_token", refreshToken);
    setProperty (value, "lease_token", leaseToken);
    setProperty (value, "session_id",
                 makeIndependentStringCopy (session.sessionId));
    setProperty (value, "server_time", session.serverTime);
    setProperty (value, "trusted_time_high_water",
                 session.trustedTimeHighWater);
    setProperty (value, "generation", session.generation);
    setProperty (value, "heartbeat_interval_seconds",
                 normaliseHeartbeatIntervalSeconds (
                     session.heartbeatIntervalSeconds));
    auto serialized = juce::JSON::toString (value, false);
    ScopedOwnedJuceStringWiper serializedWiper (serialized);
    return CredentialStore::saveSecret (
        sessionCredentialAccount, serialized, error);
}

bool Client::saveSessionForEpoch (const SavedSession& session,
                                  OperationEpoch expectedEpoch,
                                  juce::String& error) const
{
    const juce::ScopedLock persistence (persistenceLock);
    bool committed = false;
    const bool epochStayedCurrent = operationEpoch.runIfCurrent (
        expectedEpoch,
        [&]
        {
            SessionCredentialReplacementOperations operations;
            operations.saveCredential = [&] (juce::String& saveError)
            {
                return saveSession (session, saveError);
            };
            operations.clearEmergencyBlock = [] (juce::String& clearError)
            {
                return clearEmergencySessionBlock (clearError);
            };
            operations.clearTombstone = [] (juce::String& clearError)
            {
                return clearSessionTombstone (clearError);
            };
            operations.invalidation = productionInvalidationOperations();
            const auto replacement = replaceLocalSessionCredential (
                operations);
            committed = replacement.committed;
            if (! committed)
                error = replacement.error;
        });

    if (! epochStayedCurrent)
    {
        if (error.isEmpty())
            error = "Operacao de sessao substituida";
        return false;
    }
    return committed;
}

bool Client::persistRuntimeSession (OperationEpoch expectedEpoch,
                                    juce::String& error) const
{
    SavedSession saved;
    bool shouldPersist = false;
    if (! operationEpoch.runIfCurrent (
            expectedEpoch,
            [&]
            {
                const juce::ScopedLock scoped (lock);
                shouldPersist = runtime.remember;
                if (! shouldPersist)
                    return;

                saved.email = runtime.email;
                saved.refreshToken.assign (runtime.secrets.refreshToken);
                saved.leaseToken.assign (runtime.secrets.leaseToken);
                saved.sessionId = runtime.secrets.sessionId.reveal();
                saved.serverTime = runtime.serverTime;
                saved.trustedTimeHighWater = runtime.trustedTimeHighWater;
                saved.generation = runtime.generation;
                saved.heartbeatIntervalSeconds =
                    runtime.heartbeatIntervalSeconds;
            }))
        return false;

    return ! shouldPersist
        || saveSessionForEpoch (saved, expectedEpoch, error);
}

bool Client::removeSavedSession (juce::String& error) const
{
    return CredentialStore::removeSecret (sessionCredentialAccount, error);
}

LocalSessionInvalidationResult Client::invalidatePersistentSession (
    OperationEpoch expectedEpoch) const
{
    const juce::ScopedLock persistence (persistenceLock);
    LocalSessionInvalidationResult result;
    const bool applied = operationEpoch.runIfCurrent (
        expectedEpoch,
        [&]
        {
            result = invalidateLocalSession (
                productionInvalidationOperations());
        });
    if (! applied && ! result.guaranteed)
        result.error = "Invalidacao local substituida por outra operacao";
    return result;
}

Client::HttpResult Client::requestJson (const juce::String& route,
                                        const juce::var& payload,
                                        const juce::String& bearerToken,
                                        const juce::String& method,
                                        const std::atomic<bool>* externalCancellation) const
{
    HttpResult result;
    const auto base = apiBase();
    const auto cancellationRequested = [&]
    {
        return shutdownRequested.load()
            || (externalCancellation != nullptr
                && externalCancellation->load());
    };

    if (cancellationRequested())
    {
        result.transportError = "Requisicao cancelada";
        return result;
    }

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

    if (bearerToken.containsAnyOf ("\r\n"))
    {
        result.transportError = "Token de autorizacao invalido";
        return result;
    }

    const auto requestMethod = method.trim().toUpperCase();
    auto url = juce::URL (base + route);
    juce::String postData;
    ScopedOwnedJuceStringWiper postDataWiper (postData);
    if (requestMethod != "GET")
    {
        postData = juce::JSON::toString (payload, false);
        url = url.withPOSTData (postData);
    }

    auto headers = juce::String (
        "Content-Type: application/json\r\n"
        "Accept: application/json\r\n"
        "Cache-Control: no-store\r\n"
        "User-Agent: ChurchDAW-Desktop\r\n");
    ScopedOwnedJuceStringWiper headersWiper (headers);
    if (bearerToken.isNotEmpty())
        headers += "Authorization: Bearer " + bearerToken + "\r\n";

    juce::StringPairArray responseHeaders;
    const auto requestStartedMs = monotonicTimeMs();
    const auto options =
        juce::URL::InputStreamOptions (
            juce::URL::ParameterHandling::inAddress)
            .withHttpRequestCmd (requestMethod)
            .withConnectionTimeoutMs (10000)
            .withNumRedirectsToFollow (0)
            .withStatusCode (&result.status)
            .withResponseHeaders (&responseHeaders)
            .withExtraHeaders (headers);

    auto stream = url.createInputStream (options);
    if (stream == nullptr)
    {
        result.transportError = "Falha de conexao";
        return result;
    }

    if (cancellationRequested())
    {
        if (auto* webStream =
                dynamic_cast<juce::WebInputStream*> (stream.get()))
            webStream->cancel();
        result.transportError = "Requisicao cancelada";
        return result;
    }

    const auto elapsedMs = juce::roundToInt (
        monotonicTimeMs() - requestStartedMs);
    const auto remainingMs = remainingHttpDeadlineMs (
        elapsedMs, maximumRequestDurationMs);
    if (remainingMs <= 0)
    {
        if (auto* webStream =
                dynamic_cast<juce::WebInputStream*> (stream.get()))
            webStream->cancel();
        result.transportError = "Tempo limite da requisicao excedido";
        return result;
    }

    ScopedWebRequestDeadline deadline (
        dynamic_cast<juce::WebInputStream*> (stream.get()),
        remainingMs,
        &shutdownRequested,
        externalCancellation);
    bool tooLarge = false;
    auto raw = readBoundedText (*stream, tooLarge);
    ScopedOwnedJuceStringWiper rawWiper (raw);
    if (deadline.wasCancelled() || cancellationRequested())
    {
        result.transportError = "Requisicao cancelada";
        return result;
    }
    if (deadline.hasExpired())
    {
        result.transportError = "Tempo limite da requisicao excedido";
        return result;
    }

    if (raw.isEmpty())
    {
        result.transportError = tooLarge
                              ? "Resposta grande demais"
                              : "Resposta vazia";
        return result;
    }

    if (! parseStrictJsonObject (raw, result.body))
    {
        result.body = juce::var();
        result.transportError = "Resposta invalida";
    }

    return result;
}

bool Client::acceptAuthResponse (const HttpResult& result,
                                 bool persistRefreshToken,
                                 const DeviceIdentity& expectedDevice,
                                 const juce::String& expectedSessionId,
                                 bool runtimeRefresh,
                                 OperationEpoch expectedEpoch)
{
    if (! operationEpoch.isCurrent (expectedEpoch))
    {
        queueCompensatingLogoutFromAuthResponse (
            result, expectedDevice, expectedSessionId, expectedEpoch);
        return false;
    }

    if (result.status < 200 || result.status >= 300
        || result.body.getDynamicObject() == nullptr)
    {
        const auto code = responseCode (result.body);
        juce::Logger::writeToLog (
            "LICENCA DESKTOP: autenticacao recusada (HTTP "
            + juce::String (result.status) + ", codigo "
            + (code.isNotEmpty() ? code : juce::String ("sem-codigo"))
            + ")");

        if (runtimeRefresh)
            return false;

        Snapshot next;
        next.busy = false;
        next.email = currentSnapshot().email;

        if (isCredentialFailureCode (code)
            || ((result.status == 401 || result.status == 403)
                && code.isEmpty()))
        {
            next.state = State::signedOut;
            next.message = genericErrorForStatus (result.status);
        }
        else
        {
            const auto decision = classifySessionFailure (
                result.status, code,
                propertyBool (result.body, "session_replaced"));
            if (decision.action == ServerAction::retryTransient)
            {
                next.state = State::serverUnavailable;
                next.message = genericErrorForStatus (result.status);
            }
            else
            {
                next.state = decision.state;
                next.reasonCode = decision.code;
                next.message = decision.message;
                if (decision.clearSession)
                {
                    publishTerminal (decision, false, expectedEpoch);
                    return false;
                }
            }
        }

        publish (std::move (next), expectedEpoch);
        return false;
    }

    ParsedAuthSuccess parsed;
    juce::String validationError;
    if (! parseStrictAuthSuccessPayload (
            result.body,
            expectedDevice.installationId,
            expectedSessionId,
            parsed,
            validationError))
    {
        juce::Logger::writeToLog (
            "LICENCA DESKTOP: resposta auth 2xx rejeitada por contrato estrito ("
            + (validationError.isNotEmpty()
                   ? validationError
                   : juce::String ("campos invalidos"))
            + ")");

        queueCompensatingLogoutFromAuthResponse (
            result, expectedDevice, expectedSessionId, expectedEpoch);

        publishTerminal (
            { ServerAction::terminateSession,
              State::sessionRevoked,
              "INVALID_SIGNED_RESPONSE",
              "A assinatura da licenca recebida e invalida.",
              true,
              true },
            runtimeRefresh,
            expectedEpoch);
        return false;
    }

    juce::int64 previousTrustedHighWater = 0;
    if (! operationEpoch.runIfCurrent (
            expectedEpoch,
            [&]
            {
                const juce::ScopedLock scoped (lock);
                previousTrustedHighWater = runtime.trustedTimeHighWater;
            }))
    {
        queueCompensatingLogoutFromAuthResponse (
            result, expectedDevice, expectedSessionId, expectedEpoch);
        return false;
    }
    const auto trustedHighWater = advanceTrustedTimeHighWater (
        previousTrustedHighWater, parsed.serverTime);

    SavedSession saved;
    saved.email = parsed.verifiedLease.email;
    saved.refreshToken.assign (parsed.refreshToken);
    saved.leaseToken.assign (parsed.leaseToken);
    saved.sessionId = parsed.sessionId;
    saved.serverTime = parsed.serverTime;
    saved.trustedTimeHighWater = trustedHighWater;
    saved.generation = parsed.generation;
    saved.heartbeatIntervalSeconds = parsed.heartbeatSeconds;

    bool remember = persistRefreshToken;
    if (persistRefreshToken)
    {
        juce::String error;
        if (! saveSessionForEpoch (saved, expectedEpoch, error))
        {
            if (! operationEpoch.isCurrent (expectedEpoch))
            {
                queueCompensatingLogoutFromAuthResponse (
                    result, expectedDevice, expectedSessionId, expectedEpoch);
                return false;
            }

            if (! runtimeRefresh)
            {
                queueCompensatingLogoutFromAuthResponse (
                    result, expectedDevice, expectedSessionId, expectedEpoch);
                Snapshot next;
                next.state = State::signedOut;
                next.email = parsed.verifiedLease.email;
                next.message = "Acesso validado, mas a sessao nao pode ser protegida neste computador.";
                publish (std::move (next), expectedEpoch);
                return false;
            }

            const auto invalidation =
                invalidatePersistentSession (expectedEpoch);
            if (! invalidation.guaranteed)
            {
                queueCompensatingLogoutFromAuthResponse (
                    result, expectedDevice, expectedSessionId, expectedEpoch);
                publishTerminal (
                    { ServerAction::terminateSession,
                      State::sessionRevoked,
                      "LOCAL_SESSION_INVALIDATION_FAILED",
                      "A sessao foi bloqueada porque o armazenamento seguro nao confirmou a substituicao da credencial antiga.",
                      true,
                      true },
                    true,
                    expectedEpoch);
                return false;
            }

            remember = false;
            juce::Logger::writeToLog (
                "LICENCA DESKTOP: refresh aceito, mas persistencia falhou; retomada antiga bloqueada");
        }
    }
    else
    {
        const auto invalidation = invalidatePersistentSession (expectedEpoch);
        if (! invalidation.guaranteed)
        {
            queueCompensatingLogoutFromAuthResponse (
                result, expectedDevice, expectedSessionId, expectedEpoch);
            Snapshot next;
            next.state = State::signedOut;
            next.email = parsed.verifiedLease.email;
            next.reasonCode = "LOCAL_SESSION_INVALIDATION_FAILED";
            next.message = "O acesso foi validado, mas a sessao antiga nao pode ser bloqueada neste computador.";
            publish (std::move (next), expectedEpoch);
            return false;
        }
    }

    if (! operationEpoch.runIfCurrent (
            expectedEpoch,
            [&]
            {
                const juce::ScopedLock scoped (lock);
                runtime.secrets.accessToken.assign (parsed.accessToken);
                runtime.secrets.refreshToken.assign (parsed.refreshToken);
                runtime.secrets.leaseToken.assign (parsed.leaseToken);
                runtime.secrets.sessionId = parsed.sessionId;
                runtime.device = expectedDevice;
                runtime.email = parsed.verifiedLease.email;
                runtime.displayName = parsed.verifiedLease.displayName;
                runtime.activationLabel = parsed.verifiedLease.activationLabel;
                runtime.accessExpiresAt = parsed.serverTime + parsed.expiresIn;
                runtime.leaseExpiresAt =
                    parsed.verifiedLease.leaseExpiresAt;
                runtime.offlineUntil = parsed.verifiedLease.offlineUntil;
                runtime.serverTime = parsed.serverTime;
                runtime.trustedTimeHighWater = trustedHighWater;
                runtime.persistedTrustedTimeHighWater =
                    remember ? trustedHighWater : 0;
                runtime.serverTimeAnchorMs = monotonicTimeMs();
                runtime.generation = saved.generation;
                runtime.heartbeatIntervalSeconds = parsed.heartbeatSeconds;
                runtime.remember = remember;
                runtime.online = true;
                runtime.active = true;
            }))
    {
        queueCompensatingLogoutFromAuthResponse (
            result, expectedDevice, expectedSessionId, expectedEpoch);
        return false;
    }

    const bool published = publishActiveSnapshot (
        true,
        parsed.replacedPreviousSession
            ? juce::String (
                  "Licenca ativada neste aparelho. A sessao anterior foi encerrada.")
            : juce::String ("Licenca validada para este computador."),
        expectedEpoch);
    if (! published)
        queueCompensatingLogoutFromAuthResponse (
            result, expectedDevice, expectedSessionId, expectedEpoch);
    return published;
}

bool Client::acceptOfflineLease (const SavedSession& saved,
                                 const DeviceIdentity& expectedDevice,
                                 OperationEpoch expectedEpoch)
{
    if (! operationEpoch.isCurrent (expectedEpoch))
        return false;
    if (saved.leaseToken.isEmpty() || saved.refreshToken.isEmpty())
        return false;

    const auto localNow = unixTimeNow();
    if (trustedTimeRollbackDetected (localNow,
                                     saved.trustedTimeHighWater))
    {
        Snapshot next;
        next.state = State::serverUnavailable;
        next.email = saved.email;
        next.message = "O relogio do computador voltou no tempo. Conecte-se para validar.";
        publish (std::move (next), expectedEpoch);
        return true;
    }

    const auto validationTime = advanceTrustedTimeHighWater (
        advanceTrustedTimeHighWater (saved.serverTime, localNow),
        saved.trustedTimeHighWater);

    VerifiedLease verified;
    juce::String verificationError;
    auto savedLeaseToken = saved.leaseToken.reveal();
    ScopedOwnedJuceStringWiper savedLeaseWiper (savedLeaseToken);
    if (! verifyLeaseToken (savedLeaseToken,
                            expectedDevice.installationId,
                            validationTime,
                            true,
                            verified,
                            verificationError))
        return false;

    if (! operationEpoch.runIfCurrent (
            expectedEpoch,
            [&]
            {
                const juce::ScopedLock scoped (lock);
                runtime.secrets.accessToken.clear();
                runtime.secrets.refreshToken.assign (saved.refreshToken);
                runtime.secrets.leaseToken.assign (saved.leaseToken);
                runtime.secrets.sessionId = saved.sessionId;
                runtime.device = expectedDevice;
                runtime.email = verified.email;
                runtime.displayName = verified.displayName;
                runtime.activationLabel = verified.activationLabel;
                runtime.accessExpiresAt = 0;
                runtime.leaseExpiresAt = verified.leaseExpiresAt;
                runtime.offlineUntil = verified.offlineUntil;
                runtime.serverTime = validationTime;
                runtime.trustedTimeHighWater = advanceTrustedTimeHighWater (
                    saved.trustedTimeHighWater, validationTime);
                runtime.persistedTrustedTimeHighWater =
                    saved.trustedTimeHighWater;
                runtime.serverTimeAnchorMs = monotonicTimeMs();
                runtime.generation = saved.generation;
                runtime.heartbeatIntervalSeconds =
                    normaliseHeartbeatIntervalSeconds (
                        saved.heartbeatIntervalSeconds);
                runtime.remember = true;
                runtime.online = false;
                runtime.active = true;
            }))
        return false;

    // Reserva um pequeno trecho de tempo futuro no cofre. Assim, mesmo uma
    // interrupcao brusca e reinicios repetidos com o relogio congelado
    // continuam consumindo a janela offline em vez de congelar offlineUntil.
    if (! checkpointTrustedTimeHighWater (true, expectedEpoch))
    {
        clearRuntimeSession (false, expectedEpoch);
        Snapshot next;
        next.state = State::serverUnavailable;
        next.email = saved.email;
        next.message = "Nao foi possivel proteger o tempo offline neste computador. Conecte-se e entre novamente.";
        publish (std::move (next), expectedEpoch);
        return true;
    }

    if (evaluateRuntimeValidity (trustedNowSeconds(),
                                 verified.leaseExpiresAt,
                                 verified.offlineUntil,
                                 false)
        == RuntimeValidity::expired)
    {
        publishTerminal (
            { ServerAction::terminateSession,
              State::sessionExpired,
              "OFFLINE_WINDOW_EXPIRED",
              "O prazo offline desta sessao terminou. Conecte-se e entre novamente.",
              true,
              true },
            false,
            expectedEpoch);
        return true;
    }

    return publishActiveSnapshot (
        false,
        "Modo offline protegido. Conecte-se antes do prazo da licenca.",
        expectedEpoch);
}

Client::RequestOutcome Client::performRefresh (
    OperationEpoch sessionEpoch,
    OperationEpoch& requestEpoch)
{
    requestEpoch = sessionEpoch;
    if (! operationEpoch.isCurrent (requestEpoch))
        return RequestOutcome::superseded;

    DeviceIdentity device;
    SecureText refreshToken;
    juce::String expectedSessionId;
    bool remember = false;
    bool wasActive = false;
    if (! operationEpoch.runIfCurrent (
            requestEpoch,
            [&]
            {
                const juce::ScopedLock scoped (lock);
                device = runtime.device;
                refreshToken.assign (runtime.secrets.refreshToken);
                expectedSessionId = runtime.secrets.sessionId.reveal();
                remember = runtime.remember;
                wasActive = runtime.active;
            }))
        return RequestOutcome::superseded;

    if (! device.valid() || refreshToken.isEmpty())
    {
        publishTerminal (
            classifySessionFailure (401, "SESSION_INVALID"), wasActive,
            requestEpoch);
        return RequestOutcome::terminalFailure;
    }

    const auto now = unixTimeNow();
    auto nonce = randomToken (24);
    ScopedOwnedJuceStringWiper nonceWiper (nonce);
    if (nonce.isEmpty())
        return RequestOutcome::transientFailure;

    auto refreshForRequest = refreshToken.reveal();
    ScopedOwnedJuceStringWiper refreshWiper (refreshForRequest);
    ScopedOwnedJuceStringWiper expectedSessionWiper (
        expectedSessionId);
    auto proofMessage = device.installationId + "|"
                      + juce::String (now) + "|"
                      + nonce + "|" + refreshForRequest;
    ScopedOwnedJuceStringWiper proofMessageWiper (proofMessage);

    auto payload = makeObject();
    ScopedSensitiveJsonWiper payloadWiper (payload);
    setProperty (payload, "refresh_token", refreshForRequest);
    setProperty (payload, "installation_id", device.installationId);
    if (device.clientDeviceId.isNotEmpty())
        setProperty (payload, "client_device_id", device.clientDeviceId);
    setProperty (payload, "timestamp", now);
    setProperty (payload, "nonce", nonce);
    auto deviceSecretForProof = device.secretBase64.reveal();
    ScopedOwnedJuceStringWiper deviceSecretWiper (
        deviceSecretForProof);
    auto proof = hmacSha256Base64 (
        deviceSecretForProof, proofMessage);
    ScopedOwnedJuceStringWiper proofWiper (proof);
    setProperty (payload, "proof", proof);

    if (threadShouldExit())
        return RequestOutcome::terminalFailure;

    if (! operationEpoch.isCurrent (requestEpoch))
        return RequestOutcome::superseded;
    auto result = requestJson ("/auth/refresh", payload);
    ScopedSensitiveJsonWiper responseWiper (result.body);
    if (! operationEpoch.isCurrent (requestEpoch))
    {
        queueCompensatingLogoutFromAuthResponse (
            result, device, expectedSessionId, requestEpoch);
        return RequestOutcome::superseded;
    }
    if (result.status >= 200 && result.status < 300
        && result.body.getDynamicObject() != nullptr)
    {
        if (acceptAuthResponse (result, remember, device,
                                expectedSessionId, wasActive,
                                requestEpoch))
            return RequestOutcome::success;
        return operationEpoch.isCurrent (requestEpoch)
                 ? RequestOutcome::terminalFailure
                 : RequestOutcome::superseded;
    }

    return processRuntimeHttpResult (result, requestEpoch);
}

Client::RequestOutcome Client::performSessionValidation (
    bool statusRequest,
    OperationEpoch sessionEpoch,
    OperationEpoch& requestEpoch)
{
    requestEpoch = sessionEpoch;
    if (! operationEpoch.isCurrent (requestEpoch))
        return RequestOutcome::superseded;

    SecureText accessToken;
    juce::String sessionId;
    DeviceIdentity device;
    if (! operationEpoch.runIfCurrent (
            requestEpoch,
            [&]
            {
                const juce::ScopedLock scoped (lock);
                accessToken.assign (runtime.secrets.accessToken);
                sessionId = runtime.secrets.sessionId.reveal();
                device = runtime.device;
            }))
        return RequestOutcome::superseded;

    ScopedOwnedJuceStringWiper sessionWiper (sessionId);

    if (accessToken.isEmpty())
        return RequestOutcome::accessTokenExpired;

    auto payload = makeObject();
    ScopedSensitiveJsonWiper payloadWiper (payload);
    if (sessionId.isNotEmpty())
        setProperty (payload, "session_id", sessionId);
    setProperty (payload, "installation_id", device.installationId);
    if (device.clientDeviceId.isNotEmpty())
        setProperty (payload, "client_device_id", device.clientDeviceId);
    setProperty (payload, "timestamp", unixTimeNow());

    if (! operationEpoch.isCurrent (requestEpoch))
        return RequestOutcome::superseded;
    auto accessForRequest = accessToken.reveal();
    ScopedOwnedJuceStringWiper accessWiper (accessForRequest);
    auto result = requestJson (
        statusRequest ? "/license/session/status" : "/license/heartbeat",
        payload,
        accessForRequest);
    ScopedSensitiveJsonWiper responseWiper (result.body);
    if (! operationEpoch.isCurrent (requestEpoch))
        return RequestOutcome::superseded;
    return processRuntimeHttpResult (result, requestEpoch);
}

Client::RequestOutcome Client::processRuntimeHttpResult (
    const HttpResult& result,
    OperationEpoch requestEpoch)
{
    if (! operationEpoch.isCurrent (requestEpoch))
        return RequestOutcome::superseded;

    if (isUsableJsonHttpSuccess (
            result.status,
            result.body.getDynamicObject() != nullptr,
            result.transportError.isNotEmpty()))
    {
        const auto code = responseCode (result.body);
        const bool replacement = propertyBool (
            result.body, "session_replaced");

        if (replacement || code == "SESSION_REPLACED"
            || code == "SESSION_REPLACED_BY_NEW_DEVICE")
        {
            publishTerminal (
                classifySessionFailure (result.status, code, replacement),
                true,
                requestEpoch);
            return RequestOutcome::terminalFailure;
        }

        if (code.isNotEmpty() && code != "OK" && code != "SESSION_ACTIVE")
        {
            const auto decision = classifySessionFailure (
                result.status, code, false);
            if (decision.action == ServerAction::refreshAccessToken)
                return RequestOutcome::accessTokenExpired;
            if (decision.action == ServerAction::retryTransient)
                return RequestOutcome::transientFailure;

            publishTerminal (decision, true, requestEpoch);
            return RequestOutcome::terminalFailure;
        }

        juce::String expectedSessionId;
        if (! operationEpoch.runIfCurrent (
                requestEpoch,
                [&]
                {
                    const juce::ScopedLock scoped (lock);
                    expectedSessionId = runtime.secrets.sessionId.reveal();
                }))
            return RequestOutcome::superseded;
        ScopedOwnedJuceStringWiper expectedSessionWiper (
            expectedSessionId);
        if (! isValidRuntimeSuccessPayload (
                result.body, expectedSessionId))
        {
            juce::Logger::writeToLog (
                "LICENCA DESKTOP: resposta 2xx de runtime recusada por contrato semantico");
            return RequestOutcome::transientFailure;
        }

        if (! updateRuntimeFromOnlineResponse (result.body, requestEpoch))
            return RequestOutcome::superseded;

        juce::String saveError;
        if (! persistRuntimeSession (requestEpoch, saveError))
        {
            if (! operationEpoch.isCurrent (requestEpoch))
                return RequestOutcome::superseded;

            const auto invalidation =
                invalidatePersistentSession (requestEpoch);
            operationEpoch.runIfCurrent (
                requestEpoch,
                [&]
                {
                    const juce::ScopedLock scoped (lock);
                    runtime.remember = false;
                    runtime.persistedTrustedTimeHighWater = 0;
                });
            juce::Logger::writeToLog (
                invalidation.guaranteed
                    ? "LICENCA DESKTOP: heartbeat valido, mas cache seguro falhou; retomada persistida foi bloqueada"
                    : "LICENCA DESKTOP: heartbeat valido, mas invalidacao do cache local nao foi confirmada");
            if (! invalidation.guaranteed)
            {
                publishTerminal (
                    { ServerAction::terminateSession,
                      State::sessionRevoked,
                      "LOCAL_SESSION_INVALIDATION_FAILED",
                      "A sessao foi encerrada porque o armazenamento seguro nao confirmou o bloqueio da credencial antiga.",
                      true,
                      true },
                    true,
                    requestEpoch);
                return RequestOutcome::terminalFailure;
            }
        }
        else
        {
            if (! operationEpoch.runIfCurrent (
                    requestEpoch,
                    [&]
                    {
                        const juce::ScopedLock scoped (lock);
                        runtime.persistedTrustedTimeHighWater =
                            advanceTrustedTimeHighWater (
                                runtime.persistedTrustedTimeHighWater,
                                runtime.trustedTimeHighWater);
                    }))
                return RequestOutcome::superseded;
        }

        return publishActiveSnapshot (true, {}, requestEpoch)
                 ? RequestOutcome::success
                 : RequestOutcome::superseded;
    }

    if (result.transportError.isNotEmpty()
        || (result.status >= 200 && result.status < 300))
        return RequestOutcome::transientFailure;

    const auto decision = classifySessionFailure (
        result.status,
        responseCode (result.body),
        propertyBool (result.body, "session_replaced"));

    if (decision.action == ServerAction::retryTransient)
        return RequestOutcome::transientFailure;

    if (decision.action == ServerAction::refreshAccessToken)
        return RequestOutcome::accessTokenExpired;

    publishTerminal (decision, runtimeSessionIsActive(), requestEpoch);
    return RequestOutcome::terminalFailure;
}

bool Client::updateRuntimeFromOnlineResponse (
    const juce::var& body,
    OperationEpoch requestEpoch)
{
    if (! operationEpoch.isCurrent (requestEpoch))
        return false;
    const auto responseServerTime = propertyInt64 (body, "server_time");
    const auto responseHeartbeat = static_cast<int> (
        propertyInt64 (body, "heartbeat_interval_seconds"));
    const auto responseAccessToken = propertyString (body, "access_token");
    const auto responseExpiresIn = propertyInt64 (body, "expires_in");
    const auto responseLeaseToken = propertyString (body, "lease_token");
    const auto responseGeneration = propertyInt64 (body, "generation");

    juce::String expectedInstallationId;
    juce::int64 currentLeaseExpiresAt = 0;
    juce::int64 currentOfflineUntil = 0;
    if (! operationEpoch.runIfCurrent (
            requestEpoch,
            [&]
            {
                const juce::ScopedLock scoped (lock);
                expectedInstallationId = runtime.device.installationId;
                currentLeaseExpiresAt = runtime.leaseExpiresAt;
                currentOfflineUntil = runtime.offlineUntil;
            }))
        return false;
    const auto signedLeaseUpdate = validateSignedLeaseUpdate (
        responseLeaseToken,
        expectedInstallationId,
        trustedNowSeconds(),
        currentLeaseExpiresAt,
        currentOfflineUntil);

    if (responseLeaseToken.isNotEmpty() && ! signedLeaseUpdate.accepted)
        juce::Logger::writeToLog (
            "LICENCA DESKTOP: heartbeat trouxe lease novo invalido; limites assinados vigentes foram preservados");

    return operationEpoch.runIfCurrent (
        requestEpoch,
        [&]
        {
            const juce::ScopedLock scoped (lock);
            runtime.serverTime = responseServerTime;
            runtime.trustedTimeHighWater = advanceTrustedTimeHighWater (
                runtime.trustedTimeHighWater, runtime.serverTime);
            runtime.serverTimeAnchorMs = monotonicTimeMs();

            runtime.heartbeatIntervalSeconds = responseHeartbeat;
            if (responseAccessToken.isNotEmpty())
                runtime.secrets.accessToken = responseAccessToken;
            if (responseExpiresIn > 0)
                runtime.accessExpiresAt = runtime.serverTime
                                        + juce::jlimit<juce::int64> (
                                              60, 86400,
                                              responseExpiresIn);
            if (signedLeaseUpdate.accepted)
            {
                runtime.secrets.leaseToken = responseLeaseToken;
                runtime.leaseExpiresAt = signedLeaseUpdate.leaseExpiresAt;
                runtime.offlineUntil = signedLeaseUpdate.offlineUntil;
            }
            if (responseGeneration > 0)
                runtime.generation = responseGeneration;

            runtime.online = true;
            runtime.active = true;
        });
}

bool Client::publishActiveSnapshot (bool online,
                                    juce::String message,
                                    OperationEpoch expectedEpoch)
{
    Snapshot next;
    if (! operationEpoch.runIfCurrent (
            expectedEpoch,
            [&]
            {
                const juce::ScopedLock scoped (lock);
                runtime.online = online;
                next.state = online ? State::activeOnline
                                    : State::activeOffline;
                next.email = runtime.email;
                next.displayName = runtime.displayName;
                next.activationLabel = runtime.activationLabel;
                next.leaseExpiresAt = runtime.leaseExpiresAt;
                next.offlineUntil = runtime.offlineUntil;
            }))
        return false;

    next.reasonCode = online ? "SESSION_ACTIVE" : "SESSION_OFFLINE";
    next.canOpenApp = true;
    next.online = online;
    next.busy = false;
    next.message = message.isNotEmpty()
                 ? std::move (message)
                 : (online
                      ? juce::String ("Sessao online e protegida.")
                      : juce::String (
                            "Modo offline protegido. A sessao sera validada quando a conexao voltar."));
    return publish (std::move (next), expectedEpoch);
}

bool Client::publishTerminal (const ServerDecision& decision,
                              bool closeRunningApplication,
                              OperationEpoch expectedEpoch)
{
    const auto terminalEpoch = expectedEpoch != 0
        ? invalidateOperationEpochIfCurrent (expectedEpoch)
        : invalidateOperationEpoch();
    if (terminalEpoch == 0)
        return false;

    juce::String email;
    operationEpoch.runIfCurrent (
        terminalEpoch,
        [&]
        {
            const juce::ScopedLock scoped (lock);
            email = runtime.email.isNotEmpty() ? runtime.email
                                               : snapshot.email;
        });

    const auto invalidation = clearRuntimeSession (
        decision.clearSession, terminalEpoch);

    Snapshot next;
    next.state = decision.state;
    next.email = email;
    next.message = decision.message;
    next.reasonCode = decision.code;
    if (decision.clearSession && ! invalidation.guaranteed)
    {
        next.reasonCode = "LOCAL_SESSION_INVALIDATION_FAILED";
        next.message += " A sessao foi bloqueada, mas a remocao local nao pode ser confirmada.";
    }
    next.busy = false;
    next.canOpenApp = false;
    next.online = false;
    next.requiresAppClosure = closeRunningApplication;
    return publish (std::move (next), terminalEpoch);
}

LocalSessionInvalidationResult Client::clearRuntimeSession (
    bool invalidatePersistent,
    OperationEpoch expectedEpoch)
{
    LocalSessionInvalidationResult result;
    result.guaranteed = ! invalidatePersistent;
    if (! operationEpoch.runIfCurrent (
            expectedEpoch,
            [&]
            {
                const juce::ScopedLock scoped (lock);
                runtime.clear();
                pendingPassword.clear();
                forceStatusRequested.store (false);
                checkpointRequested.store (false);
            }))
    {
        result.guaranteed = false;
        result.error = "Operacao terminal substituida";
        return result;
    }

    if (invalidatePersistent)
        result = invalidatePersistentSession (expectedEpoch);
    return result;
}

bool Client::enqueuePendingLogout (PendingLogout logout)
{
   #if defined(CHURCHDAW_LICENSE_ENABLE_TEST_HOOKS) \
       && CHURCHDAW_LICENSE_ENABLE_TEST_HOOKS
    CompensatingLogoutObserverForTest observer;
    if (logout.compensating)
    {
        const juce::ScopedLock scoped (lock);
        observer = compensatingLogoutObserverForTest;
    }
    if (observer != nullptr)
    {
        auto sessionId = logout.sessionId.reveal();
        ScopedOwnedJuceStringWiper sessionWiper (sessionId);
        observer (sessionId,
                  logout.accessToken.isNotEmpty(),
                  logout.refreshToken.isNotEmpty());
        logout.clear();
        return true;
    }
   #endif

    if (logoutWorker == nullptr)
    {
        logout.clear();
        return false;
    }
    return logoutWorker->enqueue (std::move (logout));
}

bool Client::queueCompensatingLogoutFromAuthResponse (
    const HttpResult& result,
    const DeviceIdentity& device,
    const juce::String& expectedSessionId,
    OperationEpoch requestEpoch)
{
    if (result.status < 200 || result.status >= 300
        || result.body.getDynamicObject() == nullptr)
        return false;

    ParsedCompensatingLogout parsed;
    if (! parseCompensatingLogoutPayload (
            result.body, expectedSessionId, parsed))
        return false;

    PendingLogout logout;
    logout.accessToken = std::move (parsed.accessToken);
    logout.refreshToken = std::move (parsed.refreshToken);
    logout.sessionId = std::move (parsed.sessionId);
    logout.installationId = device.installationId;
    logout.clientDeviceId = device.clientDeviceId;
    logout.deviceSecretBase64.assign (device.secretBase64);
    logout.epoch = requestEpoch;
    logout.compensating = true;

    if (! logout.canContactServer())
    {
        logout.clear();
        return false;
    }

    juce::Logger::writeToLog (
        "LICENCA DESKTOP: resposta auth supersedida; logout compensatorio enfileirado");
    return enqueuePendingLogout (std::move (logout));
}

bool Client::performBestEffortLogout (
    const PendingLogout& logout,
    const std::atomic<bool>* externalCancellation)
{
    const auto cancellationRequested = [&]
    {
        return shutdownRequested.load()
            || (externalCancellation != nullptr
                && externalCancellation->load());
    };

    if (cancellationRequested())
        return false;

    if (! shouldAttemptRemoteLogout (
            logout.accessToken.isNotEmpty(),
            logout.refreshToken.isNotEmpty(),
            logout.deviceSecretBase64.isNotEmpty()))
    {
        return false;
    }

    auto accessToken = logout.accessToken.reveal();
    auto refreshToken = logout.refreshToken.reveal();
    auto sessionId = logout.sessionId.reveal();
    auto deviceSecret = logout.deviceSecretBase64.reveal();
    ScopedOwnedJuceStringWiper accessWiper (accessToken);
    ScopedOwnedJuceStringWiper refreshWiper (refreshToken);
    ScopedOwnedJuceStringWiper sessionWiper (sessionId);
    ScopedOwnedJuceStringWiper deviceSecretWiper (deviceSecret);

    auto payload = makeObject();
    ScopedSensitiveJsonWiper payloadWiper (payload);
    if (sessionId.isNotEmpty())
        setProperty (payload, "session_id", sessionId);
    setProperty (payload, "installation_id", logout.installationId);
    if (logout.clientDeviceId.isNotEmpty())
        setProperty (payload, "client_device_id", logout.clientDeviceId);
    setProperty (payload, "timestamp", unixTimeNow());

    // A troca de epoch protege o estado local, mas nao deve cancelar o POST da
    // sessao antiga. Se o usuario iniciar outro login imediatamente, este
    // bearer continua limitado ao session_id capturado antes da limpeza; a
    // resposta antiga nunca publica estado na nova sessao.
    bool confirmed = false;
    if (accessToken.isNotEmpty())
    {
        auto result = requestJson (
            "/license/logout", payload, accessToken, "POST",
            externalCancellation);
        ScopedSensitiveJsonWiper resultWiper (result.body);
        confirmed = result.status >= 200 && result.status < 300;
        secureClearOwnedJuceString (accessToken);
    }

    if (! confirmed
        && ! cancellationRequested()
        && refreshToken.isNotEmpty()
        && deviceSecret.isNotEmpty())
    {
        const auto timestamp = unixTimeNow();
        auto nonce = randomToken (24);
        ScopedOwnedJuceStringWiper nonceWiper (nonce);
        if (nonce.isNotEmpty())
        {
            auto proofMessage = logout.installationId + "|"
                              + juce::String (timestamp) + "|"
                              + nonce + "|" + refreshToken;
            ScopedOwnedJuceStringWiper proofMessageWiper (proofMessage);
            auto proof = hmacSha256Base64 (
                deviceSecret, proofMessage);
            ScopedOwnedJuceStringWiper proofWiper (proof);
            setProperty (payload, "refresh_token", refreshToken);
            setProperty (payload, "timestamp", timestamp);
            setProperty (payload, "nonce", nonce);
            setProperty (payload, "proof", proof);
            auto result = requestJson (
                "/license/logout", payload, {}, "POST",
                externalCancellation);
            ScopedSensitiveJsonWiper resultWiper (result.body);
            confirmed = result.status >= 200 && result.status < 300;
        }
    }

    juce::Logger::writeToLog (
        confirmed
            ? "LICENCA DESKTOP: logout confirmado pelo servidor"
            : "LICENCA DESKTOP: logout local protegido; confirmacao remota indisponivel");

    return confirmed;
}

bool Client::checkpointTrustedTimeHighWater (
    bool forceWrite,
    OperationEpoch expectedEpoch)
{
    if (expectedEpoch == 0)
        expectedEpoch = operationEpoch.capture();

    juce::int64 highWaterToPersist = 0;
    bool needsWrite = false;
    if (! operationEpoch.runIfCurrent (
            expectedEpoch,
            [&]
            {
                const juce::ScopedLock scoped (lock);
                if (! runtime.active || ! runtime.remember)
                    return;

                const auto estimatedNow = estimatedTrustedNowSecondsLocked();
                runtime.trustedTimeHighWater = advanceTrustedTimeHighWater (
                    runtime.trustedTimeHighWater, estimatedNow);
                const auto reservedHighWater = reserveTrustedTimeHighWater (
                    runtime.trustedTimeHighWater,
                    estimatedNow,
                    trustedTimeReservationSeconds);

                if (! forceWrite
                    && reservedHighWater
                         < runtime.persistedTrustedTimeHighWater
                             + trustedTimeCheckpointSeconds)
                    return;

                runtime.trustedTimeHighWater = reservedHighWater;
                highWaterToPersist = reservedHighWater;
                needsWrite = true;
            }))
        return false;

    if (! needsWrite)
        return true;

    juce::String saveError;
    if (! persistRuntimeSession (expectedEpoch, saveError))
    {
        if (! operationEpoch.isCurrent (expectedEpoch))
            return false;

        const auto invalidation =
            invalidatePersistentSession (expectedEpoch);
        operationEpoch.runIfCurrent (
            expectedEpoch,
            [&]
            {
                const juce::ScopedLock scoped (lock);
                runtime.remember = false;
                runtime.persistedTrustedTimeHighWater = 0;
            });
        juce::Logger::writeToLog (
            invalidation.guaranteed
                ? "LICENCA DESKTOP: high-water nao pode ser protegido; tombstone desativou a retomada offline"
                : "LICENCA DESKTOP: high-water e invalidacao local nao puderam ser confirmados");
        if (! invalidation.guaranteed)
        {
            publishTerminal (
                { ServerAction::terminateSession,
                  State::sessionRevoked,
                  "LOCAL_SESSION_INVALIDATION_FAILED",
                  "A sessao foi encerrada porque o armazenamento seguro nao confirmou o bloqueio da credencial antiga.",
                  true,
                  true },
                true,
                expectedEpoch);
        }
        return false;
    }

    return operationEpoch.runIfCurrent (
        expectedEpoch,
        [&]
        {
            const juce::ScopedLock scoped (lock);
            runtime.persistedTrustedTimeHighWater =
                advanceTrustedTimeHighWater (
                    runtime.persistedTrustedTimeHighWater,
                    highWaterToPersist);
        });
}

juce::int64 Client::estimatedTrustedNowSecondsLocked() const
{
    if (runtime.serverTime <= 0 || runtime.serverTimeAnchorMs <= 0.0)
        return unixTimeNow();

    return runtime.serverTime + static_cast<juce::int64> (
        juce::jmax (0.0, monotonicTimeMs() - runtime.serverTimeAnchorMs)
        / 1000.0);
}

juce::int64 Client::trustedNowSeconds() const
{
    const juce::ScopedLock scoped (lock);
    return advanceTrustedTimeHighWater (
        runtime.trustedTimeHighWater,
        estimatedTrustedNowSecondsLocked());
}

bool Client::runtimeSessionIsActive() const
{
    const juce::ScopedLock scoped (lock);
    return runtime.active;
}

Client::OperationEpoch Client::invalidateOperationEpoch()
{
    const juce::ScopedLock dispatch (callbackDispatch->dispatchLock);
    const auto nextEpoch = operationEpoch.invalidate();
    callbackDispatch->currentEpoch.store (
        nextEpoch, std::memory_order_release);
    return nextEpoch;
}

Client::OperationEpoch Client::invalidateOperationEpochIfCurrent (
    OperationEpoch expectedEpoch)
{
    const juce::ScopedLock dispatch (callbackDispatch->dispatchLock);
    const auto nextEpoch = operationEpoch.invalidateIfCurrent (expectedEpoch);
    if (nextEpoch != 0)
        callbackDispatch->currentEpoch.store (
            nextEpoch, std::memory_order_release);
    return nextEpoch;
}

bool Client::publish (Snapshot next, OperationEpoch expectedEpoch)
{
    if (expectedEpoch == 0)
        expectedEpoch = operationEpoch.capture();

    std::function<void (const Snapshot&)> callback;
    if (! operationEpoch.runIfCurrent (
            expectedEpoch,
            [&]
            {
                const juce::ScopedLock scoped (lock);
                snapshot = next;
                callback = onSnapshot;
            }))
        return false;

    auto dispatch = callbackDispatch;
    juce::MessageManager::callAsync (
        [dispatch = std::move (dispatch), callback = std::move (callback),
         next = std::move (next), expectedEpoch]
        {
            const juce::ScopedLock scoped (dispatch->dispatchLock);
            if (dispatch->alive
                && dispatch->currentEpoch.load (
                       std::memory_order_acquire) == expectedEpoch
                && callback != nullptr)
                callback (next);
        });
    return true;
}

juce::String Client::randomToken (size_t bytes)
{
    return secureRandomToken (bytes);
}

juce::String Client::hmacSha256Base64 (const juce::String& keyBase64,
                                       const juce::String& message)
{
    // O servidor usa os bytes UTF-8 do token Base64URL como chave HMAC.
    juce::MemoryBlock key (
        keyBase64.toRawUTF8(),
        static_cast<size_t> (keyBase64.getNumBytesAsUTF8()));
    ScopedMemoryBlockWiper keyWiper (key);

    constexpr size_t blockSize = 64;
    juce::MemoryBlock normalised (blockSize, true);
    ScopedMemoryBlockWiper normalisedWiper (normalised);

    if (key.getSize() > blockSize)
    {
        auto digest = juce::SHA256 (key).getRawData();
        ScopedMemoryBlockWiper digestWiper (digest);
        std::memcpy (normalised.getData(), digest.getData(),
                     juce::jmin (digest.getSize(), blockSize));
    }
    else
    {
        std::memcpy (normalised.getData(), key.getData(), key.getSize());
    }

    std::array<juce::uint8, blockSize> innerPad {};
    std::array<juce::uint8, blockSize> outerPad {};
    ScopedRawMemoryWiper innerPadWiper (
        innerPad.data(), innerPad.size());
    ScopedRawMemoryWiper outerPadWiper (
        outerPad.data(), outerPad.size());
    const auto* normalisedBytes =
        static_cast<const juce::uint8*> (normalised.getData());

    for (size_t i = 0; i < blockSize; ++i)
    {
        innerPad[i] = static_cast<juce::uint8> (normalisedBytes[i] ^ 0x36);
        outerPad[i] = static_cast<juce::uint8> (normalisedBytes[i] ^ 0x5c);
    }

    juce::MemoryBlock inner;
    ScopedMemoryBlockWiper innerWiper (inner);
    inner.append (innerPad.data(), innerPad.size());
    inner.append (message.toRawUTF8(),
                  static_cast<size_t> (message.getNumBytesAsUTF8()));
    auto innerDigest = juce::SHA256 (inner).getRawData();
    ScopedMemoryBlockWiper innerDigestWiper (innerDigest);

    juce::MemoryBlock outer;
    ScopedMemoryBlockWiper outerWiper (outer);
    outer.append (outerPad.data(), outerPad.size());
    outer.append (innerDigest.getData(), innerDigest.getSize());
    auto result = juce::SHA256 (outer).getRawData();
    ScopedMemoryBlockWiper resultWiper (result);

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
