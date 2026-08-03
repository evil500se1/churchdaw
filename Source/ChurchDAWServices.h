#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_cryptography/juce_cryptography.h>
#include "AppPaths.h"

#if JUCE_ANDROID
 #include <jni.h>
 #include <juce_core/native/juce_JNIHelpers_android.h>
#elif JUCE_IOS
 #include "IOSPolicySignatureVerifier.h"
#elif JUCE_WINDOWS || JUCE_MAC
 // Verificador RS256 nativo: no desktop nao existe JNI para chamar o
 // ChurchDAWSignedPolicyVerifier.java. Mesma chave, mesmo algoritmo.
 #include "DesktopUpdater.h"
#endif

#include <atomic>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>

//==============================================================================
// Infraestrutura de distribuicao do ChurchDAW.
//
// O binario Android e distribuido pelo dominio oficial (mesmo pacote,
// assinatura e versionCode crescente). Uma politica pequena decide quando uma
// versao deve ser bloqueada; o manifesto separado publica o catalogo de
// bibliotecas. Falha de DNS/servidor nunca cria um bloqueio novo: so uma
// politica HTTPS valida, recebida ou cacheada recentemente, pode elevar o piso
// obrigatorio.
//==============================================================================
namespace ChurchDAWServices
{
    inline constexpr int currentVersionCode = 14;
    inline constexpr const char* currentVersionName = "1.5.8";
    inline constexpr const char* packageId = "com.churchdaw.app";
    inline constexpr const char* updatePolicyUrl =
        "https://churchdaw.com/app/android/update-policy-v2.json";
    inline constexpr const char* fallbackUpdatePolicyUrl =
        "https://www.churchdaw.com/app/android/update-policy-v2.json";
    inline constexpr const char* manifestUrl = "https://churchdaw.com/app/android/manifest.json";
    inline constexpr const char* fallbackManifestUrl =
        "https://www.churchdaw.com/app/android/manifest.json";
    // URL de escape usada quando a policy nao carrega (celular sem internet no
    // domingo, policy vencida). Ela TEM de terminar em "-signed.apk": a ponte
    // Java exige isso em ChurchDAWUpdateBridge.isOfficialApkUrl e devolve false
    // sem nem tentar abrir. Com o nome sem "-signed" o botao "Atualizar" ficava
    // morto e, no gate obrigatorio, o cliente ficava preso — foi o que
    // aconteceu em 31/07/2026.
    inline constexpr const char* defaultPlayStoreUrl =
        "https://churchdaw.com/app/android/ChurchDAW-1.5.8-signed.apk";
    inline constexpr int maximumUpdatePolicyBytes = 32 * 1024;
    inline constexpr int maximumSignedPolicyPayloadBytes = 24 * 1024;
    inline constexpr int maximumVersionNameCharacters = 48;
    inline constexpr int maximumUpdateMessageCharacters = 640;
    // A secao comercial ja existe visualmente, mas conteudo digital nao pode
    // abrir checkout externo na versao da Play. So habilitar compras depois de
    // integrar Play Billing, validar entitlement e usar URLs temporarias.
    inline constexpr bool paidLibraryPurchasesEnabled = false;
    inline constexpr int maximumLibraryPartCount = 128;
    inline constexpr int maximumLibraryEntryCount = 250000;
    inline constexpr juce::int64 maximumPackDownloadBytes =
        (juce::int64) 80 * 1024 * 1024 * 1024;
    inline constexpr juce::int64 maximumPackInstalledBytes =
        (juce::int64) 80 * 1024 * 1024 * 1024;
    inline constexpr juce::int64 maximumLibraryEntryBytes =
        (juce::int64) 1024 * 1024 * 1024;
    inline constexpr juce::int64 installerStorageReserve =
        (juce::int64) 256 * 1024 * 1024;

    inline bool checkedAddNonNegative (juce::int64 first, juce::int64 second,
                                       juce::int64& result) noexcept
    {
        if (first < 0 || second < 0
             || first > std::numeric_limits<juce::int64>::max() - second)
            return false;

        result = first + second;
        return true;
    }

    inline bool readBoundedInteger (const juce::var& value, juce::int64 maximum,
                                    juce::int64& result) noexcept
    {
        if (! value.isInt() && ! value.isInt64() && ! value.isDouble())
            return false;

        const auto numeric = (double) value;

        if (! std::isfinite (numeric) || numeric < 0.0
             || numeric > (double) maximum || std::floor (numeric) != numeric)
            return false;

        result = (juce::int64) numeric;
        return true;
    }

    inline juce::File serviceFolder()
    {
        return AppPaths::managedDataFolder().getChildFile ("Services");
    }

   #if JUCE_ANDROID
    inline jclass findAndroidAppClass (
        JNIEnv* env,
        jobject activity,
        const char* dottedClassName)
    {
        if (env == nullptr || activity == nullptr || dottedClassName == nullptr)
            return nullptr;

        jclass activityClass = env->GetObjectClass (activity);
        jmethodID getClassLoader = activityClass != nullptr
            ? env->GetMethodID (
                activityClass, "getClassLoader", "()Ljava/lang/ClassLoader;")
            : nullptr;
        jobject classLoader = getClassLoader != nullptr
            ? env->CallObjectMethod (activity, getClassLoader)
            : nullptr;
        jclass classLoaderClass = classLoader != nullptr
            ? env->GetObjectClass (classLoader)
            : nullptr;
        jmethodID loadClass = classLoaderClass != nullptr
            ? env->GetMethodID (
                classLoaderClass, "loadClass",
                "(Ljava/lang/String;)Ljava/lang/Class;")
            : nullptr;
        jstring className = loadClass != nullptr
            ? env->NewStringUTF (dottedClassName)
            : nullptr;
        jclass result = className != nullptr
            ? static_cast<jclass> (
                env->CallObjectMethod (classLoader, loadClass, className))
            : nullptr;

        const bool ok = ! env->ExceptionCheck() && result != nullptr;
        if (! ok)
        {
            env->ExceptionClear();
            if (result != nullptr)
                env->DeleteLocalRef (result);
            result = nullptr;
        }

        if (className != nullptr)
            env->DeleteLocalRef (className);
        if (classLoaderClass != nullptr)
            env->DeleteLocalRef (classLoaderClass);
        if (classLoader != nullptr)
            env->DeleteLocalRef (classLoader);
        if (activityClass != nullptr)
            env->DeleteLocalRef (activityClass);

        return result;
    }
   #endif

    inline juce::File cachedManifestFile()
    {
        return serviceFolder().getChildFile ("android-manifest-cache.json");
    }

    inline juce::File cachedManifestTimeFile()
    {
        return serviceFolder().getChildFile ("android-manifest-cache.time");
    }

    inline juce::File cachedUpdatePolicyFile()
    {
        return serviceFolder().getChildFile ("android-update-policy-cache.json");
    }

    inline juce::File cachedUpdatePolicyTimeFile()
    {
        return serviceFolder().getChildFile ("android-update-policy-cache.time");
    }

    inline juce::File acceptedUpdatePolicyStateFile()
    {
        return serviceFolder().getChildFile ("android-update-policy-state.json");
    }

    inline std::mutex& updatePolicyStateMutex()
    {
        static std::mutex mutex;
        return mutex;
    }

    inline std::mutex& libraryAuthorizationMutex()
    {
        static std::mutex mutex;
        return mutex;
    }

    inline juce::String& libraryAuthorizationStorage()
    {
        static juce::String token;
        return token;
    }

    inline void setLibraryAuthorizationToken (juce::String token)
    {
        const std::lock_guard<std::mutex> lock (
            libraryAuthorizationMutex());
        libraryAuthorizationStorage() = std::move (token);
    }

    inline juce::String libraryAuthorizationHeader()
    {
        const std::lock_guard<std::mutex> lock (
            libraryAuthorizationMutex());
        const auto token = libraryAuthorizationStorage();
        return token.isNotEmpty()
            ? "Authorization: Bearer " + token + "\n"
            : juce::String();
    }

    inline juce::File starterChoiceCompletedFile()
    {
        return serviceFolder().getChildFile ("starter-choice-completed");
    }

    inline bool starterChoiceWasCompleted()
    {
        return starterChoiceCompletedFile().existsAsFile();
    }

    inline void rememberStarterChoiceCompleted()
    {
        serviceFolder().createDirectory();
        starterChoiceCompletedFile().replaceWithText ("1");
    }

    inline juce::File legacyLibraryPromptHandledFile()
    {
        return serviceFolder().getChildFile ("legacy-library-prompt-handled");
    }

    inline bool legacyLibraryPromptWasHandled()
    {
        return legacyLibraryPromptHandledFile().existsAsFile();
    }

    inline void rememberLegacyLibraryPromptHandled()
    {
        serviceFolder().createDirectory();
        legacyLibraryPromptHandledFile().replaceWithText ("1");
    }

    inline bool isSafeIdentifier (const juce::String& text)
    {
        if (text.isEmpty() || text.length() > 64)
            return false;

        for (const auto c : text)
            if (! juce::CharacterFunctions::isLetterOrDigit (c) && c != '-' && c != '_')
                return false;

        return true;
    }

    inline bool isSafeUpdateVersionName (const juce::String& text);

    inline bool isHttpsUrl (const juce::String& text)
    {
        return text.startsWithIgnoreCase ("https://") && juce::URL (text).isWellFormed();
    }

    // A URL do APK faz parte do payload RSA. Mesmo assim, restringi-la ao
    // diretorio oficial evita que uma policy malformada abra checkout, outro
    // host, esquema ou arquivo que nao seja um update ChurchDAW.
    inline bool isSafeWebsiteUpdateUrl (const juce::String& text)
    {
        static constexpr const char* prefix =
            "https://churchdaw.com/app/android/ChurchDAW-";

        if (! text.startsWith (prefix) || text.length() > 320
             || ! text.endsWith ("-signed.apk")
             || text.containsAnyOf ("?#\r\n"))
            return false;

        auto name = text.fromFirstOccurrenceOf (prefix, false, false)
                        .dropLastCharacters (
                            (int) juce::String ("-signed.apk").length());

        return isSafeUpdateVersionName (name);
    }

    inline bool isSafeUpdateVersionName (const juce::String& text)
    {
        if (text.isEmpty() || text.length() > maximumVersionNameCharacters)
            return false;

        for (const auto character : text)
        {
            const auto c = (juce::juce_wchar) character;
            const bool asciiAlphaNumeric =
                (c >= '0' && c <= '9')
                || (c >= 'a' && c <= 'z')
                || (c >= 'A' && c <= 'Z');

            if (! asciiAlphaNumeric && c != '.' && c != '-'
                 && c != '_' && c != '+')
                return false;
        }

        return true;
    }

    inline bool isSafeUpdateMessage (const juce::String& text)
    {
        if (text.length() > maximumUpdateMessageCharacters)
            return false;

        for (const auto character : text)
        {
            const auto c = (juce::juce_wchar) character;
            const bool control = c < 0x20 || c == 0x7f;
            const bool invisibleFormatting =
                c == 0x061c
                || (c >= 0x200b && c <= 0x200f)
                || (c >= 0x202a && c <= 0x202e)
                || (c >= 0x2060 && c <= 0x206f)
                || c == 0xfeff;

            if (control || invisibleFormatting)
                return false;
        }

        return true;
    }

    inline bool isInternalLibraryDirectory (const juce::File& folder)
    {
        const auto name = folder.getFileName();
        return name.startsWithChar ('.')
            || name.startsWithIgnoreCase ("installing-")
            || name.startsWithIgnoreCase ("backup-");
    }

    inline bool containsSupportedLibrarySample (const juce::File& folder)
    {
        if (! folder.isDirectory())
            return false;

        // Iteracao lazy: basta um sample real para validar a biblioteca, sem
        // materializar a arvore inteira (que pode conter centenas de milhares
        // de arquivos).
        for (const auto& entry : juce::RangedDirectoryIterator (
                 folder, true, "*", juce::File::findFiles,
                 juce::File::FollowSymlinks::no))
        {
            const auto extension = entry.getFile().getFileExtension();

            if (entry.getFileSize() > 0
                 && (extension.equalsIgnoreCase (".flac")
                 || extension.equalsIgnoreCase (".wav")
                 || extension.equalsIgnoreCase (".aif")
                 || extension.equalsIgnoreCase (".aiff")))
                return true;
        }

        return false;
    }

    inline int validLibraryMarkerVersion (const juce::File& folder,
                                          const juce::String& expectedId)
    {
        if (! folder.isDirectory() || ! isSafeIdentifier (expectedId))
            return 0;

        const auto parsed = juce::JSON::parse (
            folder.getChildFile (".churchdaw-library.json"));

        if (auto* object = parsed.getDynamicObject())
        {
            juce::int64 version = 0;

            if (object->getProperty ("id").toString() == expectedId
                 && readBoundedInteger (object->getProperty ("version"),
                                        1000000000, version)
                 && version >= 1
                 && containsSupportedLibrarySample (folder))
                return (int) version;
        }

        return 0;
    }

    inline bool hasValidLibraryMarker (const juce::File& folder)
    {
        const auto parsed = juce::JSON::parse (
            folder.getChildFile (".churchdaw-library.json"));

        if (auto* object = parsed.getDynamicObject())
        {
            const auto id = object->getProperty ("id").toString();
            return isSafeIdentifier (id)
                && validLibraryMarkerVersion (folder, id) > 0;
        }

        return false;
    }

    inline juce::CriticalSection& libraryTransactionMutex()
    {
        static juce::CriticalSection mutex;
        return mutex;
    }

    struct LibraryTransactionState
    {
        juce::String id, installFolder, stagingName, backupName, phase;
        int targetVersion = 0;
        bool previousWasValid = false;

        bool isValid() const
        {
            const auto hasSeparator = [] (const juce::String& name)
            {
                return name.containsChar ('/') || name.containsChar ('\\');
            };

            return isSafeIdentifier (id)
                && isSafeIdentifier (installFolder)
                && targetVersion >= 1
                && targetVersion <= 1000000000
                && stagingName.startsWith (".installing-" + id + "-")
                && backupName.startsWith (".backup-" + id + "-")
                && stagingName.length() <= 180
                && backupName.length() <= 180
                && ! hasSeparator (stagingName)
                && ! hasSeparator (backupName);
        }
    };

    inline juce::File libraryTransactionJournal (const juce::File& root,
                                                  const juce::String& id)
    {
        return root.getChildFile (".transaction-" + id + ".json");
    }

    inline bool writeLibraryTransaction (const juce::File& root,
                                         const LibraryTransactionState& state,
                                         const juce::String& phase)
    {
        if (! state.isValid()
             || (phase != "prepared"
                 && phase != "backed_up"
                 && phase != "activated"))
            return false;

        auto object = std::make_unique<juce::DynamicObject>();
        object->setProperty ("schema_version", 1);
        object->setProperty ("id", state.id);
        object->setProperty ("install_folder", state.installFolder);
        object->setProperty ("target_version", state.targetVersion);
        object->setProperty ("previous_was_valid", state.previousWasValid);
        object->setProperty ("staging_name", state.stagingName);
        object->setProperty ("backup_name", state.backupName);
        object->setProperty ("phase", phase);

        return libraryTransactionJournal (root, state.id).replaceWithText (
            juce::JSON::toString (juce::var (object.release()), true));
    }

    inline bool readLibraryTransaction (const juce::File& journal,
                                         LibraryTransactionState& state)
    {
        if (! journal.existsAsFile() || journal.getSize() <= 0
             || journal.getSize() > 64 * 1024)
            return false;

        const auto parsed = juce::JSON::parse (journal);
        auto* object = parsed.getDynamicObject();

        if (object == nullptr)
            return false;

        juce::int64 schemaVersion = 0;
        juce::int64 targetVersion = 0;
        const auto previousWasValid =
            object->getProperty ("previous_was_valid");

        if (! readBoundedInteger (object->getProperty ("schema_version"),
                                  1000000000, schemaVersion)
             || schemaVersion != 1
             || ! previousWasValid.isBool())
            return false;

        state.id = object->getProperty ("id").toString().trim();
        state.installFolder = object->getProperty ("install_folder").toString().trim();
        state.stagingName = object->getProperty ("staging_name").toString().trim();
        state.backupName = object->getProperty ("backup_name").toString().trim();
        state.phase = object->getProperty ("phase").toString().trim();
        state.previousWasValid = (bool) previousWasValid;

        if (! readBoundedInteger (object->getProperty ("target_version"),
                                  1000000000, targetVersion))
            return false;

        state.targetVersion = (int) targetVersion;
        return state.isValid()
            && (state.phase == "prepared"
                || state.phase == "backed_up"
                || state.phase == "activated")
            && journal.getFileName() == ".transaction-" + state.id + ".json";
    }

    inline bool removeInternalPath (const juce::File& root, const juce::File& path)
    {
        if (! path.exists())
            return true;

        if (! path.isAChildOf (root) || ! isInternalLibraryDirectory (path))
            return false;

        return path.deleteRecursively();
    }

    inline bool recoverLibraryTransaction (const juce::File& root,
                                            const juce::File& journal)
    {
        LibraryTransactionState state;

        if (! readLibraryTransaction (journal, state))
            return false;

        const auto destination = root.getChildFile (state.installFolder);
        const auto staging = root.getChildFile (state.stagingName);
        const auto backup = root.getChildFile (state.backupName);

        auto destinationVersion = validLibraryMarkerVersion (destination, state.id);
        auto stagingVersion = validLibraryMarkerVersion (staging, state.id);
        auto backupVersion = validLibraryMarkerVersion (backup, state.id);

        auto backupIsAcceptable = [&]()
        {
            return backup.exists()
                && (! state.previousWasValid || backupVersion > 0);
        };

        auto finishCleanup = [&]()
        {
            const bool stagingRemoved = removeInternalPath (root, staging);
            const bool backupRemoved = removeInternalPath (root, backup);
            const bool clean = stagingRemoved && backupRemoved;
            const auto resumeJournal = root.getChildFile (
                ".install-resume-" + state.id + ".json");

            if (clean && resumeJournal.existsAsFile()
                 && ! resumeJournal.deleteFile())
                return false;

            if (clean && journal.existsAsFile() && ! journal.deleteFile())
                return false;

            return clean;
        };

        if (destinationVersion >= state.targetVersion)
            return finishCleanup();

        if (stagingVersion >= state.targetVersion)
        {
            if (destination.exists())
            {
                if (backup.exists())
                    return false;

                if (! destination.moveFileTo (backup))
                    return false;

                backupVersion = validLibraryMarkerVersion (backup, state.id);

                if (! backupIsAcceptable()
                     || ! writeLibraryTransaction (root, state, "backed_up"))
                {
                    const bool restored = destination.exists()
                        || (backup.exists() && backup.moveFileTo (destination));

                    if (! restored)
                        juce::Logger::writeToLog (
                            "BIBLIOTECA: falha ao restaurar backup durante recuperacao");

                    return false;
                }
            }
            else if (backup.exists())
            {
                // O processo pode ter caido depois do rename, antes de atualizar
                // a fase do journal. Um destino anterior corrompido continua
                // sendo um rollback valido, desde que o journal o identifique.
                backupVersion = validLibraryMarkerVersion (backup, state.id);

                if (! backupIsAcceptable()
                     || ! writeLibraryTransaction (root, state, "backed_up"))
                    return false;
            }
            else if (state.previousWasValid)
            {
                // Nunca ative sobre uma biblioteca anteriormente valida sem
                // conseguir comprovar onde ficou o rollback dela.
                return false;
            }

            if (! staging.moveFileTo (destination))
            {
                const bool restored = destination.exists()
                    || (backupIsAcceptable()
                        && backup.moveFileTo (destination));

                if (! restored)
                    juce::Logger::writeToLog (
                        "BIBLIOTECA: biblioteca anterior permaneceu somente no backup");

                return false;
            }

            destinationVersion = validLibraryMarkerVersion (destination, state.id);

            if (destinationVersion < state.targetVersion)
            {
                bool restored = false;
                const bool rollbackAvailable = backupIsAcceptable();

                if (! staging.exists() && destination.moveFileTo (staging))
                {
                    restored = rollbackAvailable
                        ? backup.moveFileTo (destination)
                        : ! state.previousWasValid;
                }

                if (! restored)
                    juce::Logger::writeToLog (
                        "BIBLIOTECA: ativacao invalida preservada para diagnostico");

                return false;
            }

            if (! writeLibraryTransaction (root, state, "activated"))
                return false;

            return finishCleanup();
        }

        if (backupIsAcceptable())
        {
            bool destinationMovedAside = false;

            if (destination.exists())
            {
                if (! removeInternalPath (root, staging)
                     || ! destination.moveFileTo (staging))
                    return false;

                destinationMovedAside = true;
            }

            if (! backup.moveFileTo (destination))
            {
                if (destinationMovedAside && ! destination.exists()
                     && ! staging.moveFileTo (destination))
                    juce::Logger::writeToLog (
                        "BIBLIOTECA: falha ao devolver destino durante recuperacao");

                return false;
            }

            if (state.previousWasValid
                 && validLibraryMarkerVersion (destination, state.id) <= 0)
                return false;

            return finishCleanup();
        }

        // A biblioteca antiga continua canonica e valida: a transacao nunca
        // passou da preparacao. O staging incompleto pode ser descartado.
        if (destinationVersion > 0)
            return finishCleanup();

        // Quando o destino anterior ja era invalido (ou inexistente), a
        // recuperacao deve voltar a esse estado e liberar uma nova tentativa
        // de Reparar, em vez de deixar um journal impossivel de concluir.
        if (! state.previousWasValid && ! backup.exists())
            return finishCleanup();

        // Nenhuma copia comprovadamente valida: preserve tudo para diagnostico.
        return false;
    }

    inline bool recoverLibraryTransactions()
    {
        const juce::ScopedLock lock (libraryTransactionMutex());
        const auto root = AppPaths::managedLibrariesFolder();

        if (! root.isDirectory())
            return true;

        bool recovered = true;

        for (const auto& journal : root.findChildFiles (
                 juce::File::findFiles, false, ".transaction-*.json"))
            recovered = recoverLibraryTransaction (root, journal) && recovered;

        return recovered;
    }

    inline juce::String humanBytes (juce::int64 bytes)
    {
        if (bytes <= 0)
            return {};

        // Os rótulos são GB/MB/KB, portanto usam unidades decimais. Além de ser
        // a convenção exibida pelo Android, isso evita anunciar 12,5 GB para um
        // pacote de 13,4 bilhões de bytes.
        constexpr double gb = 1000.0 * 1000.0 * 1000.0;
        constexpr double mb = 1000.0 * 1000.0;
        constexpr double kb = 1000.0;

        if ((double) bytes >= gb)
            return juce::String ((double) bytes / gb,
                                 (double) bytes >= 100.0 * gb ? 0 : 1) + " GB";

        if ((double) bytes >= mb)
            return juce::String ((double) bytes / mb, 0) + " MB";

        return juce::String (juce::jmax (1, juce::roundToInt ((double) bytes / kb))) + " KB";
    }

    inline bool isOfficialLibraryDownloadUrl (const juce::String& value)
    {
        static constexpr const char* protectedPrefix =
            "https://churchdaw.com/api/v1/library/files/";
        static constexpr const char* publicBasePrefix =
            "https://churchdaw.com/app/android/packs/";
        const auto trimmed = value.trim();
        bool isPublicBasePart = false;
        int prefixLength = 0;

        if (trimmed.startsWith (protectedPrefix))
            prefixLength = (int) std::char_traits<char>::length (
                protectedPrefix);
        else if (trimmed.startsWith (publicBasePrefix))
        {
            prefixLength = (int) std::char_traits<char>::length (
                publicBasePrefix);
            isPublicBasePart = true;
        }
        else
            return false;

        const auto fileName = trimmed.substring (
            prefixLength);

        const bool safeFileName = fileName.isNotEmpty()
            && fileName.length() <= 180
            && fileName.endsWithIgnoreCase (".zip")
            && fileName.containsOnly (
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
                "0123456789._-");

        if (! safeFileName || ! isPublicBasePart)
            return safeFileName;

        // A pasta publica nao exige token, entao o nome do arquivo e a
        // unica barreira: so aceitamos os conjuntos que NOS publicamos.
        static constexpr const char* basePartPrefix =
            "church-live-v1-segment-";
        const auto partPrefixLength = (int) std::char_traits<char>::length (
            basePartPrefix);
        const auto partNumberText = fileName.substring (
            partPrefixLength, partPrefixLength + 2);
        const auto partNumber = partNumberText.getIntValue();

        if (fileName.startsWith (basePartPrefix)
             && partNumberText.length() == 2
             && partNumberText.containsOnly ("0123456789")
             && partNumber >= 1
             && partNumber <= 30
             && fileName == juce::String (basePartPrefix)
                 + juce::String (partNumber).paddedLeft ('0', 2)
                 + ".zip")
            return true;

        // Pad Continuo (30/07): uma parte por tom natural, publica como o
        // Church Live. "pad-continuo-v1-<tom>.zip", tom em c..b minusculo.
        static constexpr const char* padPartPrefix = "pad-continuo-v1-";
        const auto padPrefixLength = (int) std::char_traits<char>::length (
            padPartPrefix);

        if (fileName.startsWith (padPartPrefix))
        {
            const auto tone = fileName.substring (
                padPrefixLength, padPrefixLength + 1);

            return tone.length() == 1
                && juce::String ("cdefgab").containsChar (tone[0])
                && fileName == juce::String (padPartPrefix) + tone + ".zip";
        }

        return false;
    }

    struct LibraryPart
    {
        static constexpr juce::int64 maximumDownloadBytes =
            (juce::int64) 3900 * 1024 * 1024;

        juce::String downloadUrl, sha256;
        juce::int64 downloadBytes = 0;

        bool isValid() const
        {
            return isOfficialLibraryDownloadUrl (downloadUrl)
                && downloadBytes > 0
                && downloadBytes <= maximumDownloadBytes
                && sha256.length() == 64
                && sha256.containsOnly ("0123456789abcdefABCDEF");
        }
    };

    struct LibraryPack
    {
        juce::String id, name, description, versionName, installFolder;
        juce::String downloadUrl, sha256, priceLabel, purchaseUrl;
        juce::Array<LibraryPart> parts;
        int version = 1;
        juce::int64 downloadBytes = 0, installedBytes = 0;
        bool included = false, featured = false;

        juce::File installedFolder() const
        {
            return AppPaths::managedLibrariesFolder().getChildFile (installFolder);
        }

        // O catalogo atual instala o Pad em Libraries/Pads. Builds antigas
        // usaram Libraries/pad-continuo/Pads e deixaram o marker no diretorio
        // pai. O player ja reconhece esse layout; a loja precisa consultar o
        // mesmo local para nao oferecer download de algo que esta instalado.
        // O destino de novas instalacoes continua sendo installedFolder().
        juce::File detectedInstalledFolderAt (const juce::File& librariesRoot) const
        {
            const auto canonical = librariesRoot.getChildFile (installFolder);

            if (canonical.exists())
                return canonical;

            if (id == "pad-continuo")
            {
                const auto legacy =
                    librariesRoot.getChildFile (id);

                if (legacy.isDirectory())
                    return legacy;
            }

            return canonical;
        }

        juce::File detectedInstalledFolder() const
        {
            return detectedInstalledFolderAt (AppPaths::managedLibrariesFolder());
        }

        juce::File markerFile() const
        {
            return installedFolder().getChildFile (".churchdaw-library.json");
        }

        int installedVersion() const
        {
            if (const auto managedVersion = validLibraryMarkerVersion (
                    detectedInstalledFolder(), id);
                managedVersion > 0)
                return managedVersion;

           
#if JUCE_ANDROID
            // Quem atualiza da 1.1 ja possui o Church Live em Music/ChurchDAW,
            // anterior aos marcadores do instalador. A loja nao pode anunciar
            // como "indisponivel" uma biblioteca que esta carregada e tocando.
            if (id == "church-live"
                && juce::RuntimePermissions::isGranted (
                       juce::RuntimePermissions::readMediaAudio)
                && containsSupportedLibrarySample (
                       AppPaths::legacySharedDataFolder().getChildFile ("Library")))
                return 1;
           #endif

            return 0;
        }

        bool isInstalled() const
        {
            // installedVersion() tambem reconhece migracoes anteriores aos
            // marcadores. Exigir primeiro a pasta gerenciada anulava justamente
            // essa compatibilidade e fazia o Church Live carregado aparecer
            // como indisponivel no catalogo.
            return installedVersion() >= version;
        }

        bool canDownload() const
        {
            if (! parts.isEmpty())
            {
                juce::int64 total = 0;

                if (parts.size() > maximumLibraryPartCount)
                    return false;

                for (const auto& part : parts)
                {
                    if (! part.isValid())
                        return false;

                    if (! checkedAddNonNegative (total, part.downloadBytes, total)
                         || total > maximumPackDownloadBytes)
                        return false;
                }

                return total == downloadBytes
                    && installedBytes > 0
                    && installedBytes <= maximumPackInstalledBytes;
            }

            LibraryPart part { downloadUrl, sha256, downloadBytes };
            return part.isValid()
                && installedBytes > 0
                && installedBytes <= maximumPackInstalledBytes;
        }
    };

    // Reconstrói somente os metadados necessários para exibir uma biblioteca
    // que já foi instalada e validada no disco. Isso é deliberadamente
    // independente do catálogo remoto: perder a conexão ou o cache nunca pode
    // fazer um pack de 13 GB parecer apagado.
    inline bool readInstalledLibraryPack (const juce::File& folder,
                                          LibraryPack& pack)
    {
        if (! folder.isDirectory()
             || isInternalLibraryDirectory (folder)
             || ! isSafeIdentifier (folder.getFileName()))
            return false;

        const auto parsed = juce::JSON::parse (
            folder.getChildFile (".churchdaw-library.json"));
        auto* object = parsed.getDynamicObject();

        if (object == nullptr)
            return false;

        const auto id = object->getProperty ("id").toString().trim();
        auto name = object->getProperty ("name").toString().trim();
        auto versionName =
            object->getProperty ("version_name").toString().trim();
        juce::int64 version = 0;
        juce::int64 installedBytes = 0;

        if (! isSafeIdentifier (id)
             || ! readBoundedInteger (object->getProperty ("version"),
                                      1000000000, version)
             || version < 1
             || validLibraryMarkerVersion (folder, id) != (int) version)
            return false;

        if (name.isEmpty())
            name = id;

        if (name.length() > 80 || ! isSafeUpdateMessage (name))
            return false;

        if (versionName.isNotEmpty()
             && ! isSafeUpdateVersionName (versionName))
            versionName.clear();

        const auto installedBytesValue =
            object->getProperty ("installed_bytes");

        if (! installedBytesValue.isVoid()
             && ! installedBytesValue.isUndefined()
             && ! readBoundedInteger (installedBytesValue,
                                      maximumPackInstalledBytes,
                                      installedBytes))
            return false;

        LibraryPack local;
        local.id = id;
        local.name = name;
        local.description =
            "Instalada neste computador. O catalogo online sera atualizado "
            "quando a conexao voltar.";
        local.versionName = versionName;
        local.installFolder = folder.getFileName();
        local.version = (int) version;
        local.installedBytes = installedBytes;
        local.included = true;
        pack = std::move (local);
        return true;
    }

    inline juce::Array<LibraryPack> loadInstalledLibraryPacks()
    {
        juce::Array<LibraryPack> installed;
        recoverLibraryTransactions();

        const auto root = AppPaths::managedLibrariesFolder();

        if (! root.isDirectory())
            return installed;

        juce::StringArray ids;

        for (const auto& folder :
             root.findChildFiles (juce::File::findDirectories, false))
        {
            LibraryPack pack;

            if (readInstalledLibraryPack (folder, pack)
                 && ! ids.contains (pack.id))
            {
                ids.add (pack.id);
                installed.add (std::move (pack));
            }
        }

        return installed;
    }

    struct LibraryInstallResumePart
    {
        LibraryPart descriptor;
        bool extracted = false;
        int entryCount = 0;
        int fileCount = 0;
        juce::int64 expandedBytes = 0;

        bool isValid() const
        {
            if (! descriptor.isValid()
                 || entryCount < 0 || entryCount > maximumLibraryEntryCount
                 || fileCount < 0 || fileCount > entryCount
                 || expandedBytes < 0 || expandedBytes > maximumPackInstalledBytes)
                return false;

            return extracted
                ? entryCount > 0 && fileCount > 0
                : entryCount == 0 && fileCount == 0 && expandedBytes == 0;
        }
    };

    struct LibraryInstallResumeState
    {
        juce::String id, installFolder, stagingName;
        int targetVersion = 0;
        juce::int64 installedBytes = 0;
        juce::Array<LibraryInstallResumePart> parts;

        bool isValid() const
        {
            const auto hasSeparator = [] (const juce::String& name)
            {
                return name.containsChar ('/') || name.containsChar ('\\');
            };

            if (! isSafeIdentifier (id)
                 || ! isSafeIdentifier (installFolder)
                 || targetVersion < 1 || targetVersion > 1000000000
                 || installedBytes <= 0
                 || installedBytes > maximumPackInstalledBytes
                 || ! stagingName.startsWith (".installing-" + id + "-")
                 || stagingName.length() > 180
                 || hasSeparator (stagingName)
                 || parts.isEmpty()
                 || parts.size() > maximumLibraryPartCount)
                return false;

            juce::int64 totalDownloadBytes = 0;
            juce::int64 completedExpandedBytes = 0;
            int completedEntryCount = 0;

            for (const auto& part : parts)
            {
                if (! part.isValid()
                     || ! checkedAddNonNegative (
                            totalDownloadBytes,
                            part.descriptor.downloadBytes,
                            totalDownloadBytes)
                     || totalDownloadBytes > maximumPackDownloadBytes
                     || ! checkedAddNonNegative (
                            completedExpandedBytes,
                            part.expandedBytes,
                            completedExpandedBytes)
                     || completedExpandedBytes > installedBytes
                     || completedEntryCount > maximumLibraryEntryCount
                                              - part.entryCount)
                    return false;

                completedEntryCount += part.entryCount;
            }

            return true;
        }

        juce::int64 completedExpandedBytes() const
        {
            juce::int64 result = 0;

            for (const auto& part : parts)
                if (! checkedAddNonNegative (result, part.expandedBytes, result))
                    return -1;

            return result;
        }

        int completedEntryCount() const
        {
            int result = 0;

            for (const auto& part : parts)
            {
                if (result > maximumLibraryEntryCount - part.entryCount)
                    return -1;

                result += part.entryCount;
            }

            return result;
        }

        int completedFileCount() const
        {
            int result = 0;

            for (const auto& part : parts)
            {
                if (result > maximumLibraryEntryCount - part.fileCount)
                    return -1;

                result += part.fileCount;
            }

            return result;
        }
    };

    inline juce::Array<LibraryPart> downloadPartsFor (const LibraryPack& pack)
    {
        auto parts = pack.parts;

        if (parts.isEmpty())
            parts.add ({ pack.downloadUrl, pack.sha256, pack.downloadBytes });

        return parts;
    }

    inline juce::File libraryInstallResumeJournal (const juce::File& root,
                                                    const juce::String& id)
    {
        return root.getChildFile (".install-resume-" + id + ".json");
    }

    inline bool writeLibraryInstallResumeState (
        const juce::File& root, const LibraryInstallResumeState& state)
    {
        if (! state.isValid())
            return false;

        auto object = std::make_unique<juce::DynamicObject>();
        object->setProperty ("schema_version", 2);
        object->setProperty ("id", state.id);
        object->setProperty ("install_folder", state.installFolder);
        object->setProperty ("target_version", state.targetVersion);
        object->setProperty ("installed_bytes", state.installedBytes);
        object->setProperty ("staging_name", state.stagingName);

        juce::Array<juce::var> serializedParts;

        for (int partIndex = 0; partIndex < state.parts.size(); ++partIndex)
        {
            const auto& part = state.parts.getReference (partIndex);
            auto partObject = std::make_unique<juce::DynamicObject>();
            partObject->setProperty ("index", partIndex);
            partObject->setProperty ("download_url", part.descriptor.downloadUrl);
            partObject->setProperty ("sha256", part.descriptor.sha256.toLowerCase());
            partObject->setProperty ("download_bytes",
                                     part.descriptor.downloadBytes);
            partObject->setProperty ("extracted", part.extracted);
            partObject->setProperty ("entry_count", part.entryCount);
            partObject->setProperty ("file_count", part.fileCount);
            partObject->setProperty ("expanded_bytes", part.expandedBytes);
            serializedParts.add (juce::var (partObject.release()));
        }

        object->setProperty ("parts", juce::var (serializedParts));
        return libraryInstallResumeJournal (root, state.id).replaceWithText (
            juce::JSON::toString (juce::var (object.release()), true));
    }

    inline bool readLibraryInstallResumeState (
        const juce::File& journal, LibraryInstallResumeState& state)
    {
        if (! journal.existsAsFile() || journal.getSize() <= 0
             || journal.getSize() > 512 * 1024)
            return false;

        const auto parsed = juce::JSON::parse (journal);
        auto* object = parsed.getDynamicObject();

        if (object == nullptr)
            return false;

        juce::int64 schemaVersion = 0;
        juce::int64 targetVersion = 0;
        juce::int64 installedBytes = 0;
        auto* serializedParts = object->getProperty ("parts").getArray();

        if (! readBoundedInteger (object->getProperty ("schema_version"),
                                  1000000000, schemaVersion)
             || schemaVersion != 2
             || ! readBoundedInteger (object->getProperty ("target_version"),
                                      1000000000, targetVersion)
             || ! readBoundedInteger (object->getProperty ("installed_bytes"),
                                      maximumPackInstalledBytes, installedBytes)
             || serializedParts == nullptr
             || serializedParts->isEmpty()
             || serializedParts->size() > maximumLibraryPartCount)
            return false;

        state = {};
        state.id = object->getProperty ("id").toString().trim();
        state.installFolder =
            object->getProperty ("install_folder").toString().trim();
        state.stagingName =
            object->getProperty ("staging_name").toString().trim();
        state.targetVersion = (int) targetVersion;
        state.installedBytes = installedBytes;

        for (int partIndex = 0; partIndex < serializedParts->size(); ++partIndex)
        {
            auto* partObject =
                serializedParts->getReference (partIndex).getDynamicObject();
            juce::int64 serializedIndex = 0;
            juce::int64 downloadBytes = 0;
            juce::int64 entryCount = 0;
            juce::int64 fileCount = 0;
            juce::int64 expandedBytes = 0;

            if (partObject == nullptr
                 || ! readBoundedInteger (partObject->getProperty ("index"),
                                          maximumLibraryPartCount,
                                          serializedIndex)
                 || serializedIndex != partIndex
                 || ! readBoundedInteger (
                        partObject->getProperty ("download_bytes"),
                        LibraryPart::maximumDownloadBytes, downloadBytes)
                 || ! readBoundedInteger (
                        partObject->getProperty ("entry_count"),
                        maximumLibraryEntryCount, entryCount)
                 || ! readBoundedInteger (
                        partObject->getProperty ("file_count"),
                        maximumLibraryEntryCount, fileCount)
                 || ! readBoundedInteger (
                        partObject->getProperty ("expanded_bytes"),
                        maximumPackInstalledBytes, expandedBytes)
                 || ! partObject->getProperty ("extracted").isBool())
                return false;

            LibraryInstallResumePart part;
            part.descriptor.downloadUrl =
                partObject->getProperty ("download_url").toString().trim();
            part.descriptor.sha256 =
                partObject->getProperty ("sha256").toString().trim().toLowerCase();
            part.descriptor.downloadBytes = downloadBytes;
            part.extracted = (bool) partObject->getProperty ("extracted");
            part.entryCount = (int) entryCount;
            part.fileCount = (int) fileCount;
            part.expandedBytes = expandedBytes;
            state.parts.add (std::move (part));
        }

        return state.isValid()
            && journal.getFileName()
                == ".install-resume-" + state.id + ".json";
    }

    inline bool libraryInstallResumeMatchesPack (
        const LibraryInstallResumeState& state, const LibraryPack& pack)
    {
        if (! state.isValid()
             || state.id != pack.id
             || state.installFolder != pack.installFolder
             || state.targetVersion != pack.version
             || state.installedBytes != pack.installedBytes)
            return false;

        const auto expectedParts = downloadPartsFor (pack);

        if (state.parts.size() != expectedParts.size())
            return false;

        for (int partIndex = 0; partIndex < expectedParts.size(); ++partIndex)
        {
            const auto& expected = expectedParts.getReference (partIndex);
            const auto& actual =
                state.parts.getReference (partIndex).descriptor;

            if (actual.downloadUrl != expected.downloadUrl
                 || actual.sha256 != expected.sha256.toLowerCase()
                 || actual.downloadBytes != expected.downloadBytes)
                return false;
        }

        return true;
    }

    inline juce::int64 estimatedStreamingInstallPeakBytes (
        const LibraryPack& pack)
    {
        if (! pack.canDownload())
            return 0;

        juce::int64 largestPart = 0;

        for (const auto& part : downloadPartsFor (pack))
            largestPart = juce::jmax (largestPart, part.downloadBytes);

        juce::int64 result = 0;
        juce::int64 maximumEntryMetadata = 0;

        if (! checkedAddNonNegative (
                0, (juce::int64) maximumLibraryEntryCount * 4096,
                maximumEntryMetadata)
             || ! checkedAddNonNegative (
                    pack.installedBytes, largestPart, result)
             || ! checkedAddNonNegative (
                    result, maximumEntryMetadata, result)
             || ! checkedAddNonNegative (
                    result, installerStorageReserve, result))
            return std::numeric_limits<juce::int64>::max();

        return result;
    }

    // A politica de atualizacao e deliberadamente pequena e independente do
    // catalogo. Assim um pack novo/invalido ou uma futura evolucao do schema de
    // bibliotecas nunca apaga a autoridade de minimum_version_code.
    struct UpdatePolicy
    {
        int schemaVersion = 0;
        int policySerial = 0;
        int revokesPolicySerial = 0;
        int latestVersionCode = currentVersionCode;
        int minimumVersionCode = 1;
        juce::int64 issuedAt = 0;
        juce::int64 expiresAt = 0;
        juce::int64 validatedAtEpoch = 0;
        juce::String latestVersionName { currentVersionName };
        juce::String updateMessage;
        juce::String playStoreUrl { defaultPlayStoreUrl };
        juce::String mode;
        juce::String payloadSha256;
        juce::String catalogSha256;
        bool cryptographicallyVerified = false;
        bool valid = false;
    };

    enum class AndroidUpdatePromptDecision
    {
        noPrompt,
        optionalUpdate,
        requiredUpdate
    };

    // Decisao pura compartilhada pela UI e pelo probe de distribuicao. O piso
    // efetivo pode vir de uma policy assinada aceita anteriormente; por isso o
    // gate obrigatorio continua valendo mesmo quando a consulta atual falha.
    // A oferta opcional, ao contrario, exige a policy atual/cacheada completa,
    // criptograficamente verificada e com URL oficial valida.
    inline AndroidUpdatePromptDecision decideAndroidUpdatePrompt (
        const UpdatePolicy& policy,
        int installedVersionCode,
        int effectiveMinimum)
    {
        if (installedVersionCode < effectiveMinimum)
            return AndroidUpdatePromptDecision::requiredUpdate;

        if (installedVersionCode < 1
             || ! policy.valid
             || ! policy.cryptographicallyVerified
             || policy.minimumVersionCode > installedVersionCode
             || policy.latestVersionCode <= installedVersionCode
             || ! isSafeWebsiteUpdateUrl (policy.playStoreUrl))
            return AndroidUpdatePromptDecision::noPrompt;

        return AndroidUpdatePromptDecision::optionalUpdate;
    }

    inline juce::String requiredUpdateDownloadUrl (
        const UpdatePolicy& policy,
        int effectiveMinimum)
    {
        if (policy.valid
             && policy.cryptographicallyVerified
             && policy.latestVersionCode >= effectiveMinimum
             && isSafeWebsiteUpdateUrl (policy.playStoreUrl))
            return policy.playStoreUrl;

        return defaultPlayStoreUrl;
    }

    struct AcceptedUpdatePolicyState
    {
        int schemaVersion = 0;
        int policySerial = 0;
        int minimumVersionCode = 1;
        juce::int64 trustedTimeEpoch = 0;
        juce::String payloadSha256;
        juce::String catalogSha256;
        bool valid = false;
    };

    struct RemoteManifest
    {
        int schemaVersion = 0;
        int latestVersionCode = currentVersionCode;
        int minimumVersionCode = 1;
        juce::String latestVersionName { currentVersionName };
        juce::String updateMessage;
        juce::String playStoreUrl { defaultPlayStoreUrl };
        juce::Array<LibraryPack> libraries;
        bool valid = false;

        const LibraryPack* starterPack() const
        {
            // Culto offline primeiro: pack leve (Pad) permite tocar sem baixar
            // o Church Live de ~12 GB e sem Wi-Fi no domingo.
            auto isOfflineCulto = [] (const LibraryPack& pack)
            {
                return pack.id.equalsIgnoreCase ("pad-continuo")
                    || pack.id.equalsIgnoreCase ("culto-offline")
                    || pack.id.equalsIgnoreCase ("culto-offline-v1");
            };

            for (const auto& pack : libraries)
                if (pack.included && pack.canDownload() && isOfflineCulto (pack))
                    return &pack;

            for (const auto& pack : libraries)
                if (pack.included && pack.featured && pack.canDownload()
                     && ! pack.id.equalsIgnoreCase ("church-live"))
                    return &pack;

            for (const auto& pack : libraries)
                if (pack.included && pack.featured)
                    return &pack;

            for (const auto& pack : libraries)
                if (pack.included && pack.canDownload()
                     && ! pack.id.equalsIgnoreCase ("church-live"))
                    return &pack;

            for (const auto& pack : libraries)
                if (pack.included)
                    return &pack;

            return nullptr;
        }

        bool requiresUpdate() const noexcept
        {
            return valid && currentVersionCode < minimumVersionCode;
        }
    };

    inline bool parsePack (const juce::var& value, LibraryPack& pack,
                           juce::String& error)
    {
        auto* object = value.getDynamicObject();

        if (object == nullptr)
        {
            error = "Entrada de biblioteca precisa ser um objeto";
            return false;
        }

        pack.id             = object->getProperty ("id").toString().trim();
        pack.name           = object->getProperty ("name").toString().trim();
        pack.description    = object->getProperty ("description").toString().trim();
        pack.versionName    = object->getProperty ("version_name").toString().trim();
        pack.installFolder  = object->getProperty ("install_folder").toString().trim();
        pack.downloadUrl    = object->getProperty ("download_url").toString().trim();
        pack.sha256         = object->getProperty ("sha256").toString().trim().toLowerCase();
        pack.priceLabel     = object->getProperty ("price_label").toString().trim();
        pack.purchaseUrl    = object->getProperty ("purchase_url").toString().trim();
        pack.included       = (bool) object->getProperty ("included");
        pack.featured       = (bool) object->getProperty ("featured");

        juce::int64 version = 0;

        if (! readBoundedInteger (object->getProperty ("version"),
                                  1000000000, version)
             || version < 1
             || ! readBoundedInteger (object->getProperty ("download_bytes"),
                                      maximumPackDownloadBytes, pack.downloadBytes)
             || ! readBoundedInteger (object->getProperty ("installed_bytes"),
                                      maximumPackInstalledBytes, pack.installedBytes))
        {
            error = "Versao ou tamanhos invalidos na biblioteca " + pack.id;
            return false;
        }

        pack.version = (int) version;
        const auto partsValue = object->getProperty ("parts");

        if (! partsValue.isVoid() && ! partsValue.isUndefined())
        {
            auto* partList = partsValue.getArray();

            if (partList == nullptr || partList->isEmpty()
                 || partList->size() > maximumLibraryPartCount)
            {
                error = "Lista de partes invalida na biblioteca " + pack.id;
                return false;
            }

            juce::int64 total = 0;
            juce::StringArray partUrls;

            for (const auto& partValue : *partList)
            {
                auto* partObject = partValue.getDynamicObject();

                if (partObject == nullptr)
                {
                    error = "Parte nao-objeto na biblioteca " + pack.id;
                    return false;
                }

                LibraryPart part;
                part.downloadUrl = partObject->getProperty ("download_url").toString().trim();
                part.sha256 = partObject->getProperty ("sha256").toString().trim().toLowerCase();

                if (! readBoundedInteger (partObject->getProperty ("download_bytes"),
                                          LibraryPart::maximumDownloadBytes,
                                          part.downloadBytes)
                     || ! part.isValid()
                     || partUrls.contains (part.downloadUrl)
                     || ! checkedAddNonNegative (total, part.downloadBytes, total)
                     || total > maximumPackDownloadBytes)
                {
                    error = "Parte invalida ou duplicada na biblioteca " + pack.id;
                    return false;
                }

                partUrls.add (part.downloadUrl);
                pack.parts.add (std::move (part));
            }

            if (pack.downloadBytes != total)
            {
                error = "Soma das partes diverge na biblioteca " + pack.id;
                return false;
            }

            pack.downloadUrl.clear();
            pack.sha256.clear();
        }

        const bool declaresDownload = ! pack.parts.isEmpty()
                                   || pack.downloadUrl.isNotEmpty()
                                   || pack.sha256.isNotEmpty()
                                   || pack.downloadBytes > 0;

        if (declaresDownload && ! pack.canDownload())
        {
            error = "Download ou tamanho instalado invalido na biblioteca " + pack.id;
            return false;
        }

        if (! declaresDownload && pack.installedBytes != 0)
        {
            error = "Biblioteca sem download declarou tamanho instalado";
            return false;
        }

        return true;
    }

    inline bool parseLegacyManifestPolicyObject (
        const juce::DynamicObject* object,
        UpdatePolicy& policy,
        juce::String& error)
    {
        if (object == nullptr)
        {
            error = "Raiz da politica precisa ser um objeto";
            return false;
        }

        juce::int64 schemaVersion = 0;
        juce::int64 latestVersionCode = 0;
        juce::int64 minimumVersionCode = 0;

        if (! readBoundedInteger (object->getProperty ("schema_version"),
                                  1000000000, schemaVersion)
             || ! readBoundedInteger (object->getProperty ("latest_version_code"),
                                      1000000000, latestVersionCode)
             || ! readBoundedInteger (object->getProperty ("minimum_version_code"),
                                      1000000000, minimumVersionCode))
        {
            error = "Campos de versao invalidos";
            return false;
        }

        policy.schemaVersion = (int) schemaVersion;
        policy.latestVersionCode = (int) latestVersionCode;
        policy.minimumVersionCode = (int) minimumVersionCode;
        policy.latestVersionName =
            object->getProperty ("latest_version_name").toString().trim();
        policy.updateMessage =
            object->getProperty ("update_message").toString().trim();
        policy.playStoreUrl =
            object->getProperty ("play_store_url").toString().trim();

        const auto policyPackage =
            object->getProperty ("package_id").toString().trim();

        if (policy.schemaVersion != 1
             || policyPackage != packageId
             || policy.minimumVersionCode < 1
             || policy.latestVersionCode < policy.minimumVersionCode
             || policy.latestVersionCode > 1000000000
             || ! isSafeUpdateVersionName (policy.latestVersionName)
             || ! isSafeUpdateMessage (policy.updateMessage)
             || ! isSafeWebsiteUpdateUrl (policy.playStoreUrl))
        {
            error = "Campos de versao, pacote, texto ou URL rejeitados";
            return false;
        }

        policy.valid = true;
        return true;
    }

    inline bool verifyAndroidSignedPolicyEnvelope (
        const juce::String& payloadBase64,
        const juce::String& signatureBase64,
        const juce::String& keyId)
    {
       #if JUCE_ANDROID
        auto* env = juce::getEnv();

        if (env == nullptr)
            return false;

        const auto activity = juce::getCurrentActivity();
        const auto clazz = findAndroidAppClass (
            env, activity.get(),
            "com.churchdaw.app.ChurchDAWSignedPolicyVerifier");

        if (clazz == nullptr)
        {
            env->ExceptionClear();
            return false;
        }

        const auto method = env->GetStaticMethodID (
            clazz, "verify",
            "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Z");

        if (method == nullptr)
        {
            env->ExceptionClear();
            env->DeleteLocalRef (clazz);
            return false;
        }

        const auto payload = juce::javaString (payloadBase64);
        const auto signature = juce::javaString (signatureBase64);
        const auto key = juce::javaString (keyId);
        const bool verified = env->CallStaticBooleanMethod (
            clazz, method, payload.get(), signature.get(), key.get()) == JNI_TRUE;
        const bool ok = ! env->ExceptionCheck();

        if (! ok)
            env->ExceptionClear();

        env->DeleteLocalRef (clazz);
        return ok && verified;
       #elif JUCE_IOS
        // O iOS nao possui a bridge Java do Android nem compila o updater
        // desktop. A mesma chave publica e validada pelo Security.framework.
        return ChurchDAWIOSPolicy::verifySignedPolicyEnvelope (
            payloadBase64, signatureBase64, keyId);
       #elif JUCE_WINDOWS || JUCE_MAC
        // Sem isto o desktop nunca obtem um catalog_sha256 confiavel, e
        // checkRemoteManifest() aborta com "Catalogo bloqueado" antes mesmo de
        // pedir o manifest.json -- ou seja, ninguem no Windows/Mac consegue
        // baixar timbre nenhum. A verificacao e a mesma do Android: RSA-3072,
        // SHA256withRSA, chave churchdaw-update-2026-01.
        return ChurchDAWDesktopUpdate::verifySignedPolicyEnvelope (
            payloadBase64, signatureBase64, keyId);
       #else
        juce::ignoreUnused (payloadBase64, signatureBase64, keyId);
        return false;
       #endif
    }

    inline juce::int64 parseHttpDateEpochSeconds (
        const juce::String& value)
    {
        juce::StringArray fields;
        fields.addTokens (value.trim(), " ,:", {});
        fields.removeEmptyStrings();

        if (fields.size() != 8
             || fields[0].length() != 3
             || fields[7] != "GMT")
            return 0;

        for (const int index : { 1, 3, 4, 5, 6 })
            if (! fields[index].containsOnly ("0123456789"))
                return 0;

        const juce::StringArray months {
            "Jan", "Feb", "Mar", "Apr", "May", "Jun",
            "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
        };
        const int month = months.indexOf (fields[2]);
        const int day = fields[1].getIntValue();
        const int year = fields[3].getIntValue();
        const int hour = fields[4].getIntValue();
        const int minute = fields[5].getIntValue();
        const int second = fields[6].getIntValue();
        const bool leapYear =
            (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
        const int daysInMonth[] {
            31, leapYear ? 29 : 28, 31, 30, 31, 30,
            31, 31, 30, 31, 30, 31
        };

        if (year < 2020 || year > 2100
             || month < 0
             || day < 1 || day > daysInMonth[month]
             || hour < 0 || hour > 23
             || minute < 0 || minute > 59
             || second < 0 || second > 60)
            return 0;

        return juce::Time (
            year, month, day, hour, minute,
            juce::jmin (second, 59), 0, false).toMilliseconds() / 1000;
    }

    inline UpdatePolicy parseUpdatePolicy (
        const juce::String& json,
        juce::String& error,
        int maximumBytes = maximumUpdatePolicyBytes,
        juce::int64 trustedNowEpochSeconds = 0)
    {
        UpdatePolicy policy;

        if (json.isEmpty() || maximumBytes <= 0
             || json.getNumBytesAsUTF8() > maximumBytes)
        {
            error = "Politica vazia ou grande demais";
            return policy;
        }

        juce::var parsed;
        const auto parseResult = juce::JSON::parse (json, parsed);

        if (parseResult.failed())
        {
            error = "JSON invalido: " + parseResult.getErrorMessage();
            return policy;
        }

        auto* envelope = parsed.getDynamicObject();

        if (envelope == nullptr || envelope->getProperties().size() != 5)
        {
            error = "Envelope assinado invalido";
            return policy;
        }

        juce::int64 envelopeSchema = 0;
        const auto algorithm =
            envelope->getProperty ("algorithm").toString().trim();
        const auto keyId =
            envelope->getProperty ("key_id").toString().trim();
        const auto payloadBase64 =
            envelope->getProperty ("payload").toString().trim();
        const auto signatureBase64 =
            envelope->getProperty ("signature").toString().trim();

        if (! readBoundedInteger (
                envelope->getProperty ("schema_version"),
                1000000000, envelopeSchema)
             || envelopeSchema != 2
             || algorithm != "SHA256withRSA"
             || keyId != "churchdaw-update-2026-01"
             || payloadBase64.isEmpty()
             || payloadBase64.length() > maximumSignedPolicyPayloadBytes * 2
             || signatureBase64.length() != 512
             || payloadBase64.length() % 4 != 0
             || ! payloadBase64.containsOnly (
                    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
                    "0123456789+/=")
             || ! signatureBase64.containsOnly (
                    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
                    "0123456789+/="))
        {
            error = "Algoritmo, chave ou Base64 da politica rejeitados";
            return policy;
        }

        juce::MemoryOutputStream decodedPayload;

        if (! juce::Base64::convertFromBase64 (
                decodedPayload, payloadBase64)
             || decodedPayload.getDataSize() == 0
             || decodedPayload.getDataSize()
                    > (size_t) maximumSignedPolicyPayloadBytes)
        {
            error = "Payload Base64 invalido";
            return policy;
        }

        if (! verifyAndroidSignedPolicyEnvelope (
                payloadBase64, signatureBase64, keyId))
        {
            error = "Assinatura criptografica da politica invalida";
            return policy;
        }

        const auto payloadJson = juce::String::fromUTF8 (
            static_cast<const char*> (decodedPayload.getData()),
            (int) decodedPayload.getDataSize());
        juce::var parsedPayload;
        const auto payloadParseResult =
            juce::JSON::parse (payloadJson, parsedPayload);
        auto* object = parsedPayload.getDynamicObject();

        if (payloadParseResult.failed()
             || object == nullptr
             || object->getProperties().size() != 13)
        {
            error = "Payload JSON assinado invalido";
            return policy;
        }

        juce::int64 schemaVersion = 0;
        juce::int64 policySerial = 0;
        juce::int64 revokesPolicySerial = 0;
        juce::int64 latestVersionCode = 0;
        juce::int64 minimumVersionCode = 0;
        juce::int64 issuedAt = 0;
        juce::int64 expiresAt = 0;
        constexpr juce::int64 maximumEpochSeconds = 4102444800LL;

        if (! readBoundedInteger (
                object->getProperty ("schema_version"),
                1000000000, schemaVersion)
             || ! readBoundedInteger (
                object->getProperty ("policy_serial"),
                1000000000, policySerial)
             || ! readBoundedInteger (
                object->getProperty ("revokes_policy_serial"),
                1000000000, revokesPolicySerial)
             || ! readBoundedInteger (
                object->getProperty ("latest_version_code"),
                1000000000, latestVersionCode)
             || ! readBoundedInteger (
                object->getProperty ("minimum_version_code"),
                1000000000, minimumVersionCode)
             || ! readBoundedInteger (
                object->getProperty ("issued_at"),
                maximumEpochSeconds, issuedAt)
             || ! readBoundedInteger (
                object->getProperty ("expires_at"),
                maximumEpochSeconds, expiresAt))
        {
            error = "Numeros do payload assinado invalidos";
            return policy;
        }

        policy.schemaVersion = (int) schemaVersion;
        policy.policySerial = (int) policySerial;
        policy.revokesPolicySerial = (int) revokesPolicySerial;
        policy.latestVersionCode = (int) latestVersionCode;
        policy.minimumVersionCode = (int) minimumVersionCode;
        policy.issuedAt = issuedAt;
        policy.expiresAt = expiresAt;
        policy.latestVersionName =
            object->getProperty ("latest_version_name").toString().trim();
        policy.updateMessage =
            object->getProperty ("update_message").toString().trim();
        policy.playStoreUrl =
            object->getProperty ("play_store_url").toString().trim();
        policy.mode = object->getProperty ("mode").toString().trim();
        policy.payloadSha256 = juce::SHA256 (
            decodedPayload.getData(),
            decodedPayload.getDataSize()).toHexString().toLowerCase();
        policy.catalogSha256 =
            object->getProperty ("catalog_sha256")
                .toString().trim().toLowerCase();

        const auto policyPackage =
            object->getProperty ("package_id").toString().trim();
        const auto localNow =
            juce::Time::getCurrentTime().toMilliseconds() / 1000;
        const auto now = trustedNowEpochSeconds > 0
            ? trustedNowEpochSeconds
            : localNow;
        constexpr juce::int64 clockSkewAllowance = 24 * 60 * 60;
        constexpr juce::int64 maximumValidity = 180LL * 24 * 60 * 60;
        const bool rollback = policy.mode == "rollback";
        const bool normal = policy.mode == "normal";

        if (policy.schemaVersion != 2
             || policy.policySerial < 1
             || policyPackage != packageId
             || policy.minimumVersionCode < 1
             || policy.latestVersionCode < policy.minimumVersionCode
             || policy.latestVersionCode > 1000000000
             || policy.issuedAt > now + clockSkewAllowance
             || policy.expiresAt <= now
             || policy.expiresAt <= policy.issuedAt
             || policy.expiresAt - policy.issuedAt > maximumValidity
             || (! normal && ! rollback)
             || (normal && policy.revokesPolicySerial != 0)
             || (rollback
                  && (policy.revokesPolicySerial < 1
                       || policy.revokesPolicySerial >= policy.policySerial))
             || policy.catalogSha256.length() != 64
             || ! policy.catalogSha256.containsOnly ("0123456789abcdef")
             || ! isSafeUpdateVersionName (policy.latestVersionName)
             || ! isSafeUpdateMessage (policy.updateMessage)
             || ! isSafeWebsiteUpdateUrl (policy.playStoreUrl))
        {
            error = "Campos do payload assinado rejeitados";
            return {};
        }

        policy.cryptographicallyVerified = true;
        policy.validatedAtEpoch = now;
        policy.valid = true;
        return policy;
    }

    inline AcceptedUpdatePolicyState loadAcceptedUpdatePolicyState()
    {
        AcceptedUpdatePolicyState state;
        const auto file = acceptedUpdatePolicyStateFile();

        if (! file.existsAsFile() || file.getSize() <= 0
             || file.getSize() > 4096)
            return state;

        const auto parsed = juce::JSON::parse (file);
        auto* object = parsed.getDynamicObject();

        if (object == nullptr
             || (object->getProperties().size() != 5
                  && object->getProperties().size() != 6))
            return state;

        juce::int64 schemaVersion = 0;
        juce::int64 policySerial = 0;
        juce::int64 minimumVersionCode = 0;
        juce::int64 trustedTimeEpoch = 0;

        if (! readBoundedInteger (
                object->getProperty ("schema_version"),
                1000000000, schemaVersion)
             || ! readBoundedInteger (
                object->getProperty ("policy_serial"),
                1000000000, policySerial)
             || ! readBoundedInteger (
                object->getProperty ("minimum_version_code"),
                1000000000, minimumVersionCode)
             || ! readBoundedInteger (
                object->getProperty ("trusted_time_epoch"),
                4102444800LL, trustedTimeEpoch))
            return {};

        state.schemaVersion = (int) schemaVersion;
        state.policySerial = (int) policySerial;
        state.minimumVersionCode = (int) minimumVersionCode;
        state.trustedTimeEpoch = trustedTimeEpoch;
        state.payloadSha256 = object->getProperty (
            "payload_sha256").toString().trim().toLowerCase();
        if (object->hasProperty ("catalog_sha256"))
            state.catalogSha256 = object->getProperty (
                "catalog_sha256").toString().trim().toLowerCase();

        const bool legacyState = state.schemaVersion == 1
                              && object->getProperties().size() == 5
                              && state.catalogSha256.isEmpty();
        const bool currentState = state.schemaVersion == 2
                               && object->getProperties().size() == 6
                               && state.catalogSha256.length() == 64
                               && state.catalogSha256.containsOnly (
                                    "0123456789abcdef");

        if ((! legacyState && ! currentState)
             || state.policySerial < 1
             || state.minimumVersionCode < 1
             || state.trustedTimeEpoch < 1577836800LL
             || state.payloadSha256.length() != 64
             || ! state.payloadSha256.containsOnly ("0123456789abcdef"))
            return {};

        state.valid = true;
        return state;
    }

    inline bool saveAcceptedUpdatePolicyState (const UpdatePolicy& policy)
    {
        if (! policy.valid || ! policy.cryptographicallyVerified
             || policy.policySerial < 1
             || policy.minimumVersionCode < 1
             || policy.validatedAtEpoch < 1577836800LL
             || policy.payloadSha256.length() != 64
             || ! policy.payloadSha256.containsOnly ("0123456789abcdef")
             || policy.catalogSha256.length() != 64
             || ! policy.catalogSha256.containsOnly ("0123456789abcdef"))
            return false;

        auto* object = new juce::DynamicObject();
        object->setProperty ("schema_version", 2);
        object->setProperty ("policy_serial", policy.policySerial);
        object->setProperty (
            "minimum_version_code", policy.minimumVersionCode);
        object->setProperty (
            "trusted_time_epoch", policy.validatedAtEpoch);
        object->setProperty ("payload_sha256", policy.payloadSha256);
        object->setProperty ("catalog_sha256", policy.catalogSha256);

        auto folder = serviceFolder();
        if (folder.createDirectory().failed())
            return false;

        return acceptedUpdatePolicyStateFile().replaceWithText (
            juce::JSON::toString (juce::var (object), true));
    }

    inline bool acceptSignedUpdatePolicy (
        UpdatePolicy& policy,
        bool fetchedLive,
        juce::String& error)
    {
        if (! policy.valid || ! policy.cryptographicallyVerified)
        {
            error = "Politica sem assinatura verificada";
            policy.valid = false;
            return false;
        }

        const std::lock_guard<std::mutex> lock (updatePolicyStateMutex());
        const auto stateFile = acceptedUpdatePolicyStateFile();
        const auto previous = loadAcceptedUpdatePolicyState();

        // Um arquivo existente mas ilegivel nunca e sobrescrito
        // silenciosamente. Isso evita transformar corrupcao/tamper local em
        // perda do contador anti-replay.
        if (stateFile.existsAsFile() && ! previous.valid)
        {
            error = "Estado anti-replay da politica esta corrompido";
            policy.valid = false;
            return false;
        }

        if (! previous.valid)
        {
            // Cache sem estado duravel nao prova que esta politica ja foi
            // aceita ao vivo. O primeiro estado tambem nunca pode ser um
            // rollback, pois ainda nao existe serial para revogar.
            if (! fetchedLive || policy.mode != "normal"
                 || policy.revokesPolicySerial != 0)
            {
                error = "Primeira politica precisa ser normal e recebida ao vivo";
                policy.valid = false;
                return false;
            }
        }
        else if (fetchedLive
                  && policy.validatedAtEpoch < previous.trustedTimeEpoch)
        {
            error = "Relogio HTTPS regrediu em relacao ao estado confiavel";
            policy.valid = false;
            return false;
        }
        else if (policy.policySerial < previous.policySerial)
        {
            error = "Replay de uma politica antiga rejeitado";
            policy.valid = false;
            return false;
        }
        else if (policy.policySerial == previous.policySerial)
        {
            const bool exactSamePolicy =
                policy.payloadSha256 == previous.payloadSha256
                && policy.minimumVersionCode == previous.minimumVersionCode
                && policy.catalogSha256 == previous.catalogSha256;

            if (! exactSamePolicy)
            {
                error = "Serial reutilizado com payload diferente";
                policy.valid = false;
                return false;
            }

            if (fetchedLive
                 && policy.validatedAtEpoch > previous.trustedTimeEpoch
                 && ! saveAcceptedUpdatePolicyState (policy))
            {
                error = "Nao foi possivel atualizar o relogio confiavel";
                policy.valid = false;
                return false;
            }

            return true;
        }
        else
        {
            if (! fetchedLive)
            {
                error = "Cache tentou avancar o contador anti-replay";
                policy.valid = false;
                return false;
            }

            if (policy.mode == "normal")
            {
                if (policy.revokesPolicySerial != 0
                     || policy.minimumVersionCode
                            < previous.minimumVersionCode)
                {
                    error = "Reducao de piso exige politica rollback explicita";
                    policy.valid = false;
                    return false;
                }
            }
            else if (policy.mode == "rollback")
            {
                if (policy.revokesPolicySerial != previous.policySerial
                     || policy.minimumVersionCode
                            >= previous.minimumVersionCode)
                {
                    error = "Rollback nao revoga exatamente o estado atual";
                    policy.valid = false;
                    return false;
                }
            }
            else
            {
                error = "Modo de politica desconhecido";
                policy.valid = false;
                return false;
            }
        }

        if (! saveAcceptedUpdatePolicyState (policy))
        {
            error = "Nao foi possivel persistir o contador anti-replay";
            policy.valid = false;
            return false;
        }

        return true;
    }

    inline RemoteManifest parseManifest (
        const juce::String& json,
        juce::String& error,
        const juce::String& expectedCatalogSha256)
    {
        RemoteManifest manifest;

        if (json.isEmpty() || json.getNumBytesAsUTF8() > 2 * 1024 * 1024)
        {
            error = "Manifesto vazio ou grande demais";
            return manifest;
        }

        const auto catalogSha256 = juce::SHA256 (
            json.toRawUTF8(),
            (size_t) json.getNumBytesAsUTF8()).toHexString().toLowerCase();

        if (expectedCatalogSha256.length() != 64
             || ! expectedCatalogSha256.containsOnly ("0123456789abcdef")
             || catalogSha256 != expectedCatalogSha256)
        {
            error = "Manifesto nao corresponde ao SHA-256 assinado";
            return manifest;
        }

        juce::var parsed;
        const auto parseResult = juce::JSON::parse (json, parsed);

        if (parseResult.failed())
        {
            error = "JSON invalido: " + parseResult.getErrorMessage();
            return manifest;
        }

        auto* object = parsed.getDynamicObject();

        if (object == nullptr)
        {
            error = "Raiz do manifesto precisa ser um objeto";
            return manifest;
        }

        UpdatePolicy embeddedPolicy;

        if (! parseLegacyManifestPolicyObject (object, embeddedPolicy, error))
            return {};

        manifest.schemaVersion = embeddedPolicy.schemaVersion;
        manifest.latestVersionCode = embeddedPolicy.latestVersionCode;
        manifest.minimumVersionCode = embeddedPolicy.minimumVersionCode;
        manifest.latestVersionName = embeddedPolicy.latestVersionName;
        manifest.updateMessage = embeddedPolicy.updateMessage;
        manifest.playStoreUrl = embeddedPolicy.playStoreUrl;

        juce::StringArray libraryIds, installFolders;
        const auto librariesValue = object->getProperty ("libraries");

        if (! librariesValue.isVoid() && ! librariesValue.isUndefined()
             && ! librariesValue.isArray())
        {
            error = "Campo libraries precisa ser um array";
            return {};
        }

        if (auto* list = librariesValue.getArray())
            for (const auto& entry : *list)
            {
                LibraryPack pack;

                if (! parsePack (entry, pack, error))
                    return {};

                if (! isSafeIdentifier (pack.id)
                     || ! isSafeIdentifier (pack.installFolder)
                     || pack.name.isEmpty()
                     || pack.name.length() > 80
                     || pack.description.length() > 320
                     || pack.downloadBytes < 0
                     || pack.downloadBytes > maximumPackDownloadBytes
                     || pack.installedBytes < 0
                     || pack.installedBytes > maximumPackInstalledBytes)
                {
                    error = "Metadados rejeitados na biblioteca " + pack.id;
                    return {};
                }

                if (libraryIds.contains (pack.id)
                     || installFolders.contains (pack.installFolder))
                {
                    error = "ID ou pasta duplicada no catalogo: " + pack.id;
                    return {};
                }

                if (pack.purchaseUrl.isNotEmpty() && ! isHttpsUrl (pack.purchaseUrl))
                    pack.purchaseUrl.clear();

                if (! pack.included && ! paidLibraryPurchasesEnabled)
                    pack.purchaseUrl.clear();

                libraryIds.add (pack.id);
                installFolders.add (pack.installFolder);
                manifest.libraries.add (std::move (pack));
            }

        manifest.valid = true;
        return manifest;
    }

    inline bool readStreamWithLimit (juce::InputStream& stream,
                                     int maximumBytes,
                                     juce::String& text,
                                     double deadlineMs = 0.0)
    {
        if (maximumBytes <= 0)
            return false;

        juce::MemoryOutputStream output;
        juce::HeapBlock<char> buffer (16 * 1024);
        int total = 0;

        while (total <= maximumBytes)
        {
            if (deadlineMs > 0.0
                 && juce::Time::getMillisecondCounterHiRes() >= deadlineMs)
                return false;

            const int wanted = juce::jmin (16 * 1024,
                                           maximumBytes + 1 - total);
            const int received = stream.read (buffer.getData(), wanted);

            if (deadlineMs > 0.0
                 && juce::Time::getMillisecondCounterHiRes() >= deadlineMs)
                return false;

            if (received <= 0)
                break;

            total += received;

            if (total > maximumBytes
                 || ! output.write (buffer.getData(), (size_t) received))
                return false;
        }

        text = juce::String::fromUTF8 (
            static_cast<const char*> (output.getData()),
            (int) output.getDataSize());
        return true;
    }

    struct DistributionJsonResponse
    {
        int statusCode = 0;
        juce::int64 serverTime = 0;
        juce::String body;
        juce::String transportError;
    };

    inline DistributionJsonResponse fetchDistributionJson (
        const juce::String& endpoint,
        const juce::String& bearer,
        int maximumBytes,
        int timeoutMs,
        double deadlineMs)
    {
        DistributionJsonResponse response;

       #if JUCE_ANDROID
        auto* env = juce::getEnv();
        const auto activity = juce::getCurrentActivity();
        auto clazz = env != nullptr && activity != nullptr
            ? findAndroidAppClass (
                env, activity.get(),
                "com.churchdaw.app.ChurchDAWLicenseBridge")
            : nullptr;
        const auto method = clazz != nullptr
            ? env->GetStaticMethodID (
                clazz, "getDistributionJson",
                "(Ljava/lang/String;Ljava/lang/String;II)"
                "Ljava/lang/String;")
            : nullptr;
        auto endpointValue = method != nullptr
            ? env->NewStringUTF (endpoint.toRawUTF8()) : nullptr;
        auto bearerValue = method != nullptr
            ? env->NewStringUTF (bearer.toRawUTF8()) : nullptr;
        jstring result = nullptr;

        if (method != nullptr
             && endpointValue != nullptr
             && bearerValue != nullptr)
            result = static_cast<jstring> (
                env->CallStaticObjectMethod (
                    clazz, method, endpointValue, bearerValue,
                    (jint) maximumBytes, (jint) timeoutMs));

        juce::String envelopeText;
        if (env != nullptr && ! env->ExceptionCheck()
             && result != nullptr)
        {
            if (const auto* utf8 =
                    env->GetStringUTFChars (result, nullptr))
            {
                envelopeText = juce::String::fromUTF8 (utf8);
                env->ReleaseStringUTFChars (result, utf8);
            }
        }
        else if (env != nullptr)
        {
            env->ExceptionClear();
        }

        if (result != nullptr)
            env->DeleteLocalRef (result);
        if (endpointValue != nullptr)
            env->DeleteLocalRef (endpointValue);
        if (bearerValue != nullptr)
            env->DeleteLocalRef (bearerValue);
        if (clazz != nullptr)
            env->DeleteLocalRef (clazz);

        juce::var envelope;
        if (envelopeText.isEmpty()
             || juce::JSON::parse (envelopeText, envelope).failed())
        {
            response.transportError = "AndroidHttpsBridge";
            return response;
        }

        if (auto* object = envelope.getDynamicObject())
        {
            response.statusCode =
                (int) object->getProperty ("status");
            response.serverTime =
                (juce::int64) object->getProperty ("server_time");
            response.body =
                object->getProperty ("body").toString();
            response.transportError =
                object->getProperty ("transport_error").toString();
        }
       #else
        juce::StringPairArray responseHeaders;
        const auto options =
            juce::URL::InputStreamOptions (
                juce::URL::ParameterHandling::inAddress)
                .withConnectionTimeoutMs (timeoutMs)
                // Requisicoes de catalogo podem levar o bearer da biblioteca.
                // Nao siga redirects: a assinatura protege a integridade do
                // JSON, mas nao impediria o cabecalho Authorization de ser
                // encaminhado ao destino de um redirect. O endpoint www que
                // hoje responde 301 continua sendo apenas uma tentativa de
                // fallback; o endpoint apex acima serve o JSON diretamente.
                .withNumRedirectsToFollow (0)
                .withResponseHeaders (&responseHeaders)
                .withStatusCode (&response.statusCode)
                .withExtraHeaders (
                    "Accept: application/json\n"
                    "Cache-Control: no-cache\n"
                    + (bearer.isNotEmpty()
                        ? "Authorization: Bearer " + bearer + "\n"
                        : juce::String()));

        // Ver a nota em DesktopUpdater.cpp: a primeira conexao HTTPS do
        // processo falha no Windows enquanto o WinINet resolve o proxy do
        // sistema. Sem a repeticao, a politica de distribuicao (a primeira
        // coisa que o app busca) voltava "HTTPS indisponivel" mesmo com o
        // servidor respondendo em 110 ms — e o catalogo, buscado depois,
        // funcionava no mesmo arranque.
        std::unique_ptr<juce::InputStream> stream;

        for (int tentativa = 0; tentativa < 3 && stream == nullptr; ++tentativa)
        {
            if (tentativa > 0)
                juce::Thread::sleep (350);

            stream = juce::URL (endpoint).createInputStream (options);
        }

        if (stream != nullptr)
        {
            response.serverTime =
                parseHttpDateEpochSeconds (responseHeaders["Date"]);
            if (! readStreamWithLimit (
                    *stream, maximumBytes, response.body, deadlineMs))
                response.transportError =
                    "Resposta excedeu tamanho ou prazo";
        }
        else
        {
            response.transportError = "HTTPS indisponivel";
        }
       #endif

        return response;
    }

    struct StartupCheck
    {
        UpdatePolicy updatePolicy;
        RemoteManifest manifest;
        // Preenchidos pela thread Android depois do fetch. Nunca consultar a
        // Play no MessageManager: getAppUpdateInfo pode levar varios segundos.
        int installedVersionCode = 0;
        int immediateUpdateEligibility = 0;
        bool reachedServer = false;
        bool fromCache = false;
        bool reachedUpdateServer = false;
        bool updateFromCache = false;
        // Preflight local executado pelo mesmo worker da rede. Recuperar uma
        // instalacao interrompida pode tocar milhares de arquivos; a UI apenas
        // consome este snapshot.
        bool localLibraryChecked = false;
        bool localLibraryReady = false;
        juce::String diagnostic;
        juce::String updateDiagnostic;
    };

    struct UpdatePolicyCheck
    {
        UpdatePolicy policy;
        int installedVersionCode = 0;
        int immediateUpdateEligibility = 0;
        bool reachedServer = false;
        bool fromCache = false;
        bool localLibraryChecked = false;
        bool localLibraryReady = false;
        juce::String diagnostic;
    };

    inline bool saveCache (const juce::String& raw)
    {
        auto folder = serviceFolder();
        folder.createDirectory();

        return cachedManifestFile().replaceWithData (
                   raw.toRawUTF8(),
                   static_cast<size_t> (raw.getNumBytesAsUTF8()))
            && cachedManifestTimeFile().replaceWithText (
                 juce::String (juce::Time::getCurrentTime().toMilliseconds()));
    }

    inline bool saveUpdatePolicyCache (const juce::String& raw)
    {
        auto folder = serviceFolder();
        folder.createDirectory();

        return cachedUpdatePolicyFile().replaceWithData (
                   raw.toRawUTF8(),
                   static_cast<size_t> (raw.getNumBytesAsUTF8()))
            && cachedUpdatePolicyTimeFile().replaceWithText (
                 juce::String (juce::Time::getCurrentTime().toMilliseconds()));
    }

    inline bool saveUpdatePolicyCacheIfCurrent (
        const UpdatePolicy& policy,
        const juce::String& raw)
    {
        const std::lock_guard<std::mutex> lock (updatePolicyStateMutex());
        const auto state = loadAcceptedUpdatePolicyState();

        if (! state.valid
             || state.policySerial != policy.policySerial
             || state.minimumVersionCode != policy.minimumVersionCode
             || state.payloadSha256 != policy.payloadSha256
             || state.catalogSha256 != policy.catalogSha256)
            return false;

        return saveUpdatePolicyCache (raw);
    }

    inline bool cacheTimestampIsFresh (const juce::File& timestampFile,
                                       int maxAgeDays)
    {
        const auto text = timestampFile.loadFileAsString().trim();

        if (! text.containsOnly ("0123456789"))
            return false;

        const auto saved = text.getLargeIntValue();
        const auto age = juce::Time::getCurrentTime().toMilliseconds() - saved;
        return age >= 0 && age <= (juce::int64) maxAgeDays * 24 * 60 * 60 * 1000;
    }

    inline bool cacheIsFresh (int maxAgeDays = 7)
    {
        return cacheTimestampIsFresh (cachedManifestTimeFile(), maxAgeDays);
    }

    inline bool updatePolicyCacheIsFresh (int maxAgeDays = 7)
    {
        return cacheTimestampIsFresh (cachedUpdatePolicyTimeFile(), maxAgeDays);
    }

    inline UpdatePolicyCheck checkRemoteUpdatePolicy (int timeoutMs = 5000)
    {
        UpdatePolicyCheck result;
        const auto policyDeadline =
            juce::Time::getMillisecondCounterHiRes()
            + (double) juce::jmax (250, timeoutMs);

        for (const char* endpoint : { updatePolicyUrl, fallbackUpdatePolicyUrl })
        {
            const int remainingMs = (int) std::floor (
                policyDeadline - juce::Time::getMillisecondCounterHiRes());

            if (remainingMs <= 0)
                break;

            const auto fetched = fetchDistributionJson (
                endpoint, {}, maximumUpdatePolicyBytes,
                remainingMs, policyDeadline);

            if (fetched.transportError.isEmpty())
            {
                const auto& raw = fetched.body;
                juce::String parseError;
                UpdatePolicy parsed;
                const auto trustedServerTime = fetched.serverTime;

                if (trustedServerTime <= 0)
                    parseError = "Resposta HTTPS sem cabecalho Date confiavel";
                else if (raw.getNumBytesAsUTF8()
                            <= maximumUpdatePolicyBytes)
                    parsed = parseUpdatePolicy (
                        raw, parseError, maximumUpdatePolicyBytes,
                        trustedServerTime);
                else
                    parseError = "Politica excedeu tamanho ou prazo";

                if (fetched.statusCode >= 200
                     && fetched.statusCode < 300
                     && parsed.valid
                     && acceptSignedUpdatePolicy (
                            parsed, true, parseError))
                {
                    result.policy = std::move (parsed);
                    result.reachedServer = true;
                    saveUpdatePolicyCacheIfCurrent (result.policy, raw);
                    return result;
                }

                result.diagnostic = juce::String (endpoint) + " / HTTP "
                                  + juce::String (fetched.statusCode)
                                  + ": " + parseError;
            }
            else
            {
                result.diagnostic = juce::String (endpoint)
                                  + ": "
                                  + fetched.transportError;
            }
        }

        if (updatePolicyCacheIsFresh())
        {
            juce::String parseError;
            const auto localNow =
                juce::Time::getCurrentTime().toMilliseconds() / 1000;
            juce::int64 trustedCacheTime = localNow;

            {
                const std::lock_guard<std::mutex> lock (
                    updatePolicyStateMutex());
                const auto accepted = loadAcceptedUpdatePolicyState();

                if (accepted.valid)
                    trustedCacheTime = juce::jmax (
                        trustedCacheTime, accepted.trustedTimeEpoch);
            }

            auto cached = parseUpdatePolicy (
                cachedUpdatePolicyFile().loadFileAsString(), parseError,
                maximumUpdatePolicyBytes, trustedCacheTime);

            if (cached.valid
                 && acceptSignedUpdatePolicy (
                        cached, false, parseError))
            {
                result.policy = std::move (cached);
                result.fromCache = true;
            }
        }

        return result;
    }

    inline StartupCheck checkRemoteManifest (int timeoutMs = 7000)
    {
        StartupCheck result;
        auto updateCheck = checkRemoteUpdatePolicy (
            juce::jmin (5000, juce::jmax (250, timeoutMs)));
        result.updatePolicy = std::move (updateCheck.policy);
        result.reachedUpdateServer = updateCheck.reachedServer;
        result.updateFromCache = updateCheck.fromCache;
        result.updateDiagnostic = std::move (updateCheck.diagnostic);

        auto expectedCatalogSha256 =
            result.updatePolicy.catalogSha256.toLowerCase();

        if (expectedCatalogSha256.length() != 64
             || ! expectedCatalogSha256.containsOnly ("0123456789abcdef"))
        {
            const std::lock_guard<std::mutex> lock (
                updatePolicyStateMutex());
            const auto accepted = loadAcceptedUpdatePolicyState();

            if (accepted.valid)
                expectedCatalogSha256 = accepted.catalogSha256;
        }

        if (expectedCatalogSha256.length() != 64
             || ! expectedCatalogSha256.containsOnly ("0123456789abcdef"))
        {
            result.diagnostic =
                "Catalogo bloqueado: politica assinada sem SHA-256 confiavel";
            return result;
        }

        // O dominio oficial e sua variante www servem o mesmo conteudo por
        // HTTPS direto. Nenhum endpoint critico segue redirects.
        const auto catalogDeadline =
            juce::Time::getMillisecondCounterHiRes()
            + (double) juce::jmax (250, timeoutMs);

        for (const char* endpoint : { manifestUrl, fallbackManifestUrl })
        {
            const int remainingMs = (int) std::floor (
                catalogDeadline - juce::Time::getMillisecondCounterHiRes());

            if (remainingMs <= 0)
                break;

            auto bearer = libraryAuthorizationHeader()
                              .fromFirstOccurrenceOf (
                                  "Authorization: Bearer ", false, false)
                              .upToFirstOccurrenceOf ("\n", false, false);
            const auto fetched = fetchDistributionJson (
                endpoint, bearer, 2 * 1024 * 1024,
                remainingMs, catalogDeadline);

            if (fetched.transportError.isEmpty())
            {
                const auto& raw = fetched.body;
                juce::String parseError;
                RemoteManifest parsed;

                if (raw.getNumBytesAsUTF8() <= 2 * 1024 * 1024)
                    parsed = parseManifest (
                        raw, parseError, expectedCatalogSha256);
                else
                    parseError = "Manifesto excede 2 MB";

                if (fetched.statusCode >= 200
                     && fetched.statusCode < 300
                     && parsed.valid)
                {
                    result.manifest = std::move (parsed);
                    result.reachedServer = true;
                    saveCache (raw);
                    break;
                }

                result.diagnostic = juce::String (endpoint) + " / HTTP "
                                  + juce::String (fetched.statusCode)
                                  + ": " + parseError;
            }
            else
            {
                result.diagnostic = juce::String (endpoint)
                                  + ": "
                                  + fetched.transportError;
            }
        }

        if (! result.manifest.valid && cacheIsFresh())
        {
            juce::String parseError;
            const auto cachedRaw = cachedManifestFile().loadFileAsString();

            auto cached = parseManifest (
                cachedRaw, parseError, expectedCatalogSha256);

            if (cached.valid)
            {
                result.manifest = std::move (cached);
                result.fromCache = true;
            }
        }

        if (! result.reachedUpdateServer
             || ! result.reachedServer
             || ! result.manifest.valid)
            juce::Logger::writeToLog (
                "ChurchDAW distribution check: update="
                + result.updateDiagnostic
                + " catalog=" + result.diagnostic
                + " update_http="
                + juce::String (result.reachedUpdateServer ? 1 : 0)
                + " catalog_http="
                + juce::String (result.reachedServer ? 1 : 0));

        // Politica e catalogo falham de forma independente. O piso duravel, ja
        // conhecido pelo Main.cpp, continua valendo mesmo que ambos caiam.
        return result;
    }

    inline RemoteManifest loadCachedManifest()
    {
        juce::String error;
        juce::String expectedCatalogSha256;

        {
            const std::lock_guard<std::mutex> lock (
                updatePolicyStateMutex());
            const auto accepted = loadAcceptedUpdatePolicyState();

            if (accepted.valid)
                expectedCatalogSha256 = accepted.catalogSha256;
        }

        return parseManifest (
            cachedManifestFile().loadFileAsString(),
            error,
            expectedCatalogSha256);
    }

    inline bool hasLegacyInstalledLibrary()
    {
       
#if JUCE_ANDROID
        // Nunca sondeia Music/ChurchDAW antes de o usuario conceder acesso.
        // Em Android recente a arvore pode parecer existir mesmo quando a
        // abertura dos FLAC/WAV ainda sera negada pelo sistema.
        if (! juce::RuntimePermissions::isGranted (
                juce::RuntimePermissions::readMediaAudio))
            return false;
       #endif

        const auto shared = AppPaths::legacySharedDataFolder();

        if (shared.isDirectory())
            for (const char* name : { "Library", "ChurchLive Library", "SAMPLES",
                                     "Library2", "REDKEYS", "Library3", "Worship",
                                     "Library4", "BASELAB" })
                if (containsSupportedLibrarySample (shared.getChildFile (name)))
                    return true;

        return false;
    }

    inline bool hasAnyInstalledLibrary()
    {
        recoverLibraryTransactions();
        const auto managed = AppPaths::managedLibrariesFolder();

        if (managed.isDirectory())
            for (const auto& folder : managed.findChildFiles (juce::File::findDirectories, false))
                if (! isInternalLibraryDirectory (folder)
                     && hasValidLibraryMarker (folder))
                    return true;

        // Compatibilidade com o acervo instalado/copiado pelas versoes antigas.
        return hasLegacyInstalledLibrary();
    }

    inline bool setConfirmedWebsiteUpdateFloor (int requiredVersionCode)
    {
       
#if JUCE_ANDROID
        auto* env = juce::getEnv();
        const auto activity = juce::getCurrentActivity();

        if (env != nullptr && activity != nullptr)
        {
            const auto clazz = findAndroidAppClass (
                env, activity.get(),
                "com.churchdaw.app.ChurchDAWUpdateBridge");

            if (clazz != nullptr)
            {
                const auto method = env->GetStaticMethodID (
                    clazz, "setConfirmedRequiredVersion",
                    "(Landroid/app/Activity;I)I");

                if (method != nullptr)
                {
                    const auto confirmed = (int) env->CallStaticIntMethod (
                        clazz, method, activity.get(),
                        (jint) juce::jlimit (0, 1000000000, requiredVersionCode));
                    const bool ok = ! env->ExceptionCheck()
                                 && (requiredVersionCode <= 1
                                      ? confirmed == 1
                                      : confirmed == requiredVersionCode);

                    if (! ok)
                        env->ExceptionClear();

                    env->DeleteLocalRef (clazz);

                    if (ok)
                        return true;
                }
                else
                {
                    env->ExceptionClear();
                    env->DeleteLocalRef (clazz);
                }
            }
            else
            {
                env->ExceptionClear();
            }
        }
       #endif

        return false;
    }

    inline bool startWebsiteUpdate (const juce::String& updateUrl)
    {
        if (! isSafeWebsiteUpdateUrl (updateUrl))
            return false;
       
#if JUCE_ANDROID
        auto* env = juce::getEnv();
        const auto activity = juce::getCurrentActivity();

        if (env != nullptr && activity != nullptr)
        {
            const auto clazz = findAndroidAppClass (
                env, activity.get(),
                "com.churchdaw.app.ChurchDAWUpdateBridge");

            if (clazz != nullptr)
            {
                const auto method = env->GetStaticMethodID (
                    clazz, "openWebsiteUpdate",
                    "(Landroid/app/Activity;Ljava/lang/String;)Z");

                if (method != nullptr)
                {
                    const auto url = juce::javaString (updateUrl);
                    const bool started = env->CallStaticBooleanMethod (
                        clazz, method, activity.get(), url.get()) == JNI_TRUE;
                    const bool ok = ! env->ExceptionCheck() && started;

                    if (! ok)
                        env->ExceptionClear();

                    env->DeleteLocalRef (clazz);

                    if (ok)
                        return true;
                }
                else
                {
                    env->ExceptionClear();
                    env->DeleteLocalRef (clazz);
                }
            }
            else
            {
                env->ExceptionClear();
            }
        }
       #endif

        // O chamador decide se a tela e obrigatoria ou opcional. Falhar ao
        // entregar o Intent nunca abre uma URL alternativa nem muda esse estado.
        return false;
    }

    //==========================================================================
    class LibraryInstaller final : public juce::Thread
    {
    public:
        using Progress = std::function<void (float, const juce::String&)>;
        using Finished = std::function<void (juce::Result)>;

        LibraryInstaller (LibraryPack packToInstall, Progress progressCallback,
                          Finished finishedCallback)
            : juce::Thread ("ChurchDAW library installer"),
              pack (std::move (packToInstall)),
              progress (std::move (progressCallback)),
              finished (std::move (finishedCallback))
        {
        }

        ~LibraryInstaller() override
        {
            cancel();
            stopThread (-1);
        }

        void begin()
        {
            startThread (juce::Thread::Priority::normal);
        }

        void cancel()
        {
            signalThreadShouldExit();

            std::shared_ptr<juce::WebInputStream> stream;

            {
                const std::lock_guard<std::mutex> lock (downloadStreamMutex);
                stream = activeDownloadStream;
            }

            if (stream != nullptr)
                stream->cancel();
        }

        void run() override
        {
            const auto result = install();
            deliverFinished (result);
        }

    private:
        void setActiveDownloadStream (
            const std::shared_ptr<juce::WebInputStream>& stream)
        {
            const std::lock_guard<std::mutex> lock (downloadStreamMutex);
            activeDownloadStream = stream;
        }

        void clearActiveDownloadStream (
            const std::shared_ptr<juce::WebInputStream>& stream)
        {
            const std::lock_guard<std::mutex> lock (downloadStreamMutex);

            if (activeDownloadStream == stream)
                activeDownloadStream.reset();
        }

        struct ActiveDownloadRegistration
        {
            ActiveDownloadRegistration (
                LibraryInstaller& ownerToUse,
                std::shared_ptr<juce::WebInputStream> streamToUse)
                : owner (ownerToUse), stream (std::move (streamToUse))
            {
                owner.setActiveDownloadStream (stream);
            }

            ~ActiveDownloadRegistration()
            {
                owner.clearActiveDownloadStream (stream);
            }

            LibraryInstaller& owner;
            std::shared_ptr<juce::WebInputStream> stream;
        };

        class CancelableHashInputStream final : public juce::InputStream
        {
        public:
            CancelableHashInputStream (LibraryInstaller& ownerToUse,
                                       juce::InputStream& sourceToUse)
                : owner (ownerToUse), source (sourceToUse)
            {
            }

            juce::int64 getTotalLength() override
            {
                return source.getTotalLength();
            }

            juce::int64 getPosition() override
            {
                return source.getPosition();
            }

            bool setPosition (juce::int64 position) override
            {
                return source.setPosition (position);
            }

            int read (void* destination, int maximumBytesToRead) override
            {
                if (owner.threadShouldExit())
                {
                    cancelled = true;
                    return 0;
                }

                return source.read (destination, maximumBytesToRead);
            }

            bool isExhausted() override
            {
                return cancelled || source.isExhausted();
            }

            bool wasCancelled() const noexcept
            {
                return cancelled;
            }

        private:
            LibraryInstaller& owner;
            juce::InputStream& source;
            bool cancelled = false;
        };

        juce::Result calculatePartSha256 (const juce::File& file,
                                          juce::String& result)
        {
            result.clear();
            const auto expectedBytes = file.getSize();

            if (! file.existsAsFile() || expectedBytes < 0)
                return juce::Result::fail (
                    "Nao foi possivel abrir a parte para verificacao");

            juce::FileInputStream fileInput (file);

            if (fileInput.failedToOpen())
                return juce::Result::fail (
                    "Nao foi possivel abrir a parte para verificacao");

            // O SHA256 do JUCE le o InputStream em blocos. O buffer reduz I/O
            // pequeno, enquanto o wrapper consulta cancelamento entre blocos.
            juce::BufferedInputStream bufferedInput (fileInput, 1024 * 1024);
            CancelableHashInputStream cancelableInput (*this, bufferedInput);
            const juce::SHA256 hash (cancelableInput, expectedBytes);

            if (cancelableInput.wasCancelled() || threadShouldExit())
                return juce::Result::fail (
                    "Verificacao cancelada. A parte foi preservada para retomar.");

            if (cancelableInput.getPosition() != expectedBytes
                 || fileInput.getStatus().failed())
                return juce::Result::fail (
                    "Falha ao ler a parte durante a verificacao");

            result = hash.toHexString();
            return juce::Result::ok();
        }

        static bool contentRangeMatches (const juce::String& header,
                                         juce::int64 expectedStart,
                                         juce::int64 expectedTotal)
        {
            auto text = header.trim();

            if (! text.startsWithIgnoreCase ("bytes "))
                return false;

            text = text.substring (6).trim();
            const auto slash = text.indexOfChar ('/');
            const auto dash = text.indexOfChar ('-');

            if (dash <= 0 || slash <= dash + 1)
                return false;

            const auto startText = text.substring (0, dash).trim();
            const auto endText = text.substring (dash + 1, slash).trim();
            const auto totalText = text.substring (slash + 1).trim();

            if (! startText.containsOnly ("0123456789")
                 || ! endText.containsOnly ("0123456789")
                 || ! totalText.containsOnly ("0123456789"))
                return false;

            const auto start = startText.getLargeIntValue();
            const auto end = endText.getLargeIntValue();
            const auto total = totalText.getLargeIntValue();

            return expectedTotal > 0
                && start == expectedStart
                && end == expectedTotal - 1
                && total == expectedTotal;
        }

        static juce::File partFileFor (const juce::File& root,
                                       const LibraryPack& library,
                                       int partIndex, int partCount)
        {
            const auto suffix = partCount > 1
                ? ".part-" + juce::String (partIndex + 1)
                : juce::String();
            return root.getChildFile (
                "." + library.id + suffix + ".download.part");
        }

        static juce::Result removeOrphanedPartFiles (
            const juce::File& root, const LibraryPack& library, int partCount)
        {
            juce::StringArray expectedNames;

            for (int partIndex = 0; partIndex < partCount; ++partIndex)
                expectedNames.add (partFileFor (
                    root, library, partIndex, partCount).getFileName());

            const auto singlePartName =
                "." + library.id + ".download.part";
            const auto multipartPrefix =
                "." + library.id + ".part-";

            for (const auto& candidate : root.findChildFiles (
                     juce::File::findFiles, false,
                     "." + library.id + "*.download.part"))
            {
                const auto name = candidate.getFileName();
                const bool belongsToThisPack = name == singlePartName
                    || (name.startsWith (multipartPrefix)
                        && name.endsWith (".download.part"));

                if (belongsToThisPack && ! expectedNames.contains (name)
                     && ! candidate.deleteFile())
                    return juce::Result::fail (
                        "Nao foi possivel limpar uma parte antiga do download");
            }

            return juce::Result::ok();
        }

        static juce::Result removeAllPackPartFiles (
            const juce::File& root, const LibraryPack& library)
        {
            const auto singlePartName =
                "." + library.id + ".download.part";
            const auto multipartPrefix =
                "." + library.id + ".part-";

            for (const auto& candidate : root.findChildFiles (
                     juce::File::findFiles, false,
                     "." + library.id + "*.download.part"))
            {
                const auto name = candidate.getFileName();
                const bool belongsToThisPack = name == singlePartName
                    || (name.startsWith (multipartPrefix)
                        && name.endsWith (".download.part"));

                if (belongsToThisPack && ! candidate.deleteFile())
                    return juce::Result::fail (
                        "Nao foi possivel limpar uma parte antiga do download");
            }

            return juce::Result::ok();
        }

        static bool isSafeArchiveEntryName (const juce::String& entryName)
        {
            if (entryName.isEmpty() || entryName.length() > 4096
                 || entryName.startsWithChar ('/')
                 || entryName.startsWithChar ('\\')
                 || entryName.containsChar ('\\')
                 || entryName.containsChar (':')
                 || entryName.containsChar ('\0'))
                return false;

            juce::StringArray components;
            components.addTokens (entryName, "/", {});

            if (components.isEmpty())
                return false;

            for (const auto& component : components)
            {
                if (component.isEmpty() || component == "." || component == "..")
                    return false;

                for (const auto character : component)
                    if ((juce::juce_wchar) character < 0x20
                         || (juce::juce_wchar) character == 0x7f)
                        return false;
            }

            return true;
        }

        struct StagingInspection
        {
            int fileCount = 0;
            juce::int64 expandedBytes = 0;
        };

        juce::Result inspectStagingContents (
            const juce::File& staging,
            StagingInspection& inspection)
        {
            if (! staging.isDirectory())
                return juce::Result::fail ("Staging da biblioteca esta ausente");

            inspection = {};

            for (const auto& entry : juce::RangedDirectoryIterator (
                     staging, true, "*",
                     juce::File::findFilesAndDirectories,
                     juce::File::FollowSymlinks::no))
            {
                if (threadShouldExit())
                    return juce::Result::fail (
                        "Validacao do staging cancelada");

                const auto file = entry.getFile();

                if (! file.isAChildOf (staging) || file.isSymbolicLink())
                    return juce::Result::fail (
                        "Staging contem caminho ou link nao permitido");

                if (file.isDirectory())
                    continue;

                if (file == staging.getChildFile (
                                ".churchdaw-library.json"))
                {
                    if (validLibraryMarkerVersion (
                            staging, pack.id) != pack.version)
                        return juce::Result::fail (
                            "Staging contem marker final invalido");

                    // O marker e criado imediatamente antes da ativacao. Uma
                    // queda nesse intervalo deve retomar a ativacao, sem
                    // transforma-lo em arquivo inesperado do ZIP.
                    continue;
                }

                const auto bytes = entry.getFileSize();

                if (bytes < 0
                     || bytes > maximumLibraryEntryBytes
                     || inspection.fileCount >= maximumLibraryEntryCount
                     || ! checkedAddNonNegative (
                            inspection.expandedBytes,
                            bytes,
                            inspection.expandedBytes)
                     || inspection.expandedBytes > pack.installedBytes)
                    return juce::Result::fail (
                        "Staging excede os limites declarados");

                ++inspection.fileCount;
            }

            return juce::Result::ok();
        }

        juce::Result discardResumeArtifacts (
            const juce::File& root,
            const LibraryInstallResumeState* parsedState = nullptr)
        {
            if (parsedState != nullptr)
            {
                const auto staging =
                    root.getChildFile (parsedState->stagingName);

                if (staging.exists()
                     && (! staging.isAChildOf (root)
                         || ! isInternalLibraryDirectory (staging)
                         || ! removeInternalPath (root, staging)))
                    return juce::Result::fail (
                        "Nao foi possivel descartar o staging incompativel");
            }

            for (const auto& stale : root.findChildFiles (
                     juce::File::findDirectories, false,
                     ".installing-" + pack.id + "-*"))
                if (! removeInternalPath (root, stale))
                    return juce::Result::fail (
                        "Nao foi possivel limpar uma instalacao interrompida");

            auto result = removeAllPackPartFiles (root, pack);

            if (result.failed())
                return result;

            const auto journal =
                libraryInstallResumeJournal (root, pack.id);

            if (journal.existsAsFile() && ! journal.deleteFile())
                return juce::Result::fail (
                    "Nao foi possivel limpar o journal de retomada");

            return juce::Result::ok();
        }

        juce::Result prepareOrResumeInstall (
            const juce::File& root,
            const juce::Array<LibraryPart>& downloadParts,
            LibraryInstallResumeState& state,
            juce::File& staging)
        {
            const auto journal =
                libraryInstallResumeJournal (root, pack.id);

            if (journal.existsAsFile())
            {
                LibraryInstallResumeState parsed;
                const bool parsedOk =
                    readLibraryInstallResumeState (journal, parsed);
                const bool compatible = parsedOk
                    && libraryInstallResumeMatchesPack (parsed, pack);

                if (! compatible)
                {
                    const auto cleanup = discardResumeArtifacts (
                        root, parsedOk ? &parsed : nullptr);

                    if (cleanup.failed())
                        return cleanup;

                    return juce::Result::fail (
                        "A retomada salva nao pertence a esta versao do pack. "
                        "Ela foi descartada com seguranca; tente novamente.");
                }

                staging = root.getChildFile (parsed.stagingName);

                if (! staging.isDirectory()
                     || ! staging.isAChildOf (root)
                     || ! isInternalLibraryDirectory (staging))
                {
                    const auto cleanup =
                        discardResumeArtifacts (root, &parsed);

                    if (cleanup.failed())
                        return cleanup;

                    return juce::Result::fail (
                        "O staging da retomada estava ausente ou invalido. "
                        "Ele foi descartado; tente novamente.");
                }

                StagingInspection actualStaging;
                const auto expectedExpanded =
                    parsed.completedExpandedBytes();
                const auto expectedFiles =
                    parsed.completedFileCount();
                const auto stagingValidation =
                    inspectStagingContents (staging, actualStaging);

                if (stagingValidation.failed()
                     || expectedExpanded < 0
                     || expectedFiles < 0
                     || actualStaging.expandedBytes != expectedExpanded
                     || actualStaging.fileCount != expectedFiles)
                {
                    if (threadShouldExit())
                        return stagingValidation;

                    const auto cleanup =
                        discardResumeArtifacts (root, &parsed);

                    if (cleanup.failed())
                        return cleanup;

                    return juce::Result::fail (
                        "O staging salvo nao corresponde ao journal. "
                        "A retomada foi descartada; tente novamente.");
                }

                state = std::move (parsed);

                for (const auto& stale : root.findChildFiles (
                         juce::File::findDirectories, false,
                         ".installing-" + pack.id + "-*"))
                    if (stale != staging
                         && ! removeInternalPath (root, stale))
                        return juce::Result::fail (
                            "Nao foi possivel limpar um staging obsoleto");

                auto result = removeOrphanedPartFiles (
                    root, pack, downloadParts.size());

                if (result.failed())
                    return result;

                for (int partIndex = 0;
                     partIndex < state.parts.size();
                     ++partIndex)
                {
                    if (! state.parts.getReference (partIndex).extracted)
                        continue;

                    const auto partFile = partFileFor (
                        root, pack, partIndex, state.parts.size());

                    if (partFile.existsAsFile() && ! partFile.deleteFile())
                        return juce::Result::fail (
                            "Parte ja instalada nao pode ser liberada do armazenamento");
                }

                return juce::Result::ok();
            }

            auto result = discardResumeArtifacts (root);

            if (result.failed())
                return result;

            state.id = pack.id;
            state.installFolder = pack.installFolder;
            state.targetVersion = pack.version;
            state.installedBytes = pack.installedBytes;
            state.stagingName = ".installing-" + pack.id + "-"
                + juce::String::toHexString (
                    juce::Random::getSystemRandom().nextInt64());

            for (const auto& descriptor : downloadParts)
            {
                LibraryInstallResumePart part;
                part.descriptor = descriptor;
                part.descriptor.sha256 =
                    part.descriptor.sha256.toLowerCase();
                state.parts.add (std::move (part));
            }

            staging = root.getChildFile (state.stagingName);
            result = staging.createDirectory();

            if (result.failed() || ! staging.isDirectory())
                return juce::Result::fail (
                    "Nao foi possivel preparar o staging persistente da biblioteca");

            if (! writeLibraryInstallResumeState (root, state))
            {
                removeInternalPath (root, staging);
                return juce::Result::fail (
                    "Nao foi possivel registrar a retomada segura da biblioteca");
            }

            return juce::Result::ok();
        }

        juce::Result ensureStreamingInstallSpace (
            const juce::File& root,
            const LibraryInstallResumeState& state)
        {
            const auto expanded = state.completedExpandedBytes();
            const auto entries = state.completedEntryCount();

            if (expanded < 0 || expanded > pack.installedBytes
                 || entries < 0 || entries > maximumLibraryEntryCount)
                return juce::Result::fail (
                    "Journal de instalacao possui contadores invalidos");

            juce::int64 largestMissingPart = 0;

            for (int partIndex = 0; partIndex < state.parts.size(); ++partIndex)
            {
                const auto& part = state.parts.getReference (partIndex);

                if (part.extracted)
                    continue;

                const auto partFile = partFileFor (
                    root, pack, partIndex, state.parts.size());
                auto existingBytes =
                    partFile.existsAsFile() ? partFile.getSize() : 0;

                if (existingBytes < 0
                     || existingBytes > part.descriptor.downloadBytes)
                    existingBytes = 0;

                largestMissingPart = juce::jmax (
                    largestMissingPart,
                    part.descriptor.downloadBytes - existingBytes);
            }

            juce::int64 remainingExpanded =
                pack.installedBytes - expanded;
            juce::int64 remainingMetadata = 0;
            juce::int64 required = 0;

            if (! checkedAddNonNegative (
                    0,
                    (juce::int64) (maximumLibraryEntryCount - entries) * 4096,
                    remainingMetadata)
                 || ! checkedAddNonNegative (
                        remainingExpanded, largestMissingPart, required)
                 || ! checkedAddNonNegative (
                        required, remainingMetadata, required)
                 || ! checkedAddNonNegative (
                        required, installerStorageReserve, required))
                return juce::Result::fail (
                    "Tamanho da instalacao excede o limite suportado");

            const auto freeBytes = root.getBytesFreeOnVolume();

            if (freeBytes <= 0)
                return juce::Result::fail (
                    "Nao foi possivel confirmar o espaco livre do aparelho");

            if (freeBytes < required)
                return juce::Result::fail (
                    "Espaco insuficiente. Libere pelo menos "
                    + humanBytes (required - freeBytes)
                    + " para continuar a instalacao por partes.");

            return juce::Result::ok();
        }

        struct ArchiveInspection
        {
            int entryCount = 0;
            int fileCount = 0;
            juce::int64 expandedBytes = 0;
        };

        float partProgress (int partIndex, float withinPart,
                            int partCount) const
        {
            const auto overall = ((float) partIndex
                                  + juce::jlimit (0.0f, 1.0f, withinPart))
                               / (float) juce::jmax (1, partCount);
            return 0.02f + 0.92f * overall;
        }

        juce::Result inspectArchive (
            const juce::File& partFile,
            const juce::File& staging,
            const LibraryInstallResumeState& state,
            ArchiveInspection& inspection)
        {
            juce::ZipFile archive (partFile);
            const int entries = archive.getNumEntries();
            const auto completedExpanded = state.completedExpandedBytes();
            const auto completedEntries = state.completedEntryCount();

            if (entries <= 0
                 || entries > maximumLibraryEntryCount
                 || completedEntries < 0
                 || completedEntries > maximumLibraryEntryCount - entries)
                return juce::Result::fail (
                    "Pack possui uma quantidade de arquivos nao permitida");

            inspection = {};
            inspection.entryCount = entries;

            for (int entryIndex = 0; entryIndex < entries; ++entryIndex)
            {
                if (threadShouldExit())
                    return juce::Result::fail (
                        "Instalacao cancelada. O ZIP validado e o staging "
                        "foram preservados para retomar.");

                const auto* entry = archive.getEntry (entryIndex);

                if (entry == nullptr
                     || entry->isSymbolicLink
                     || entry->uncompressedSize < 0
                     || entry->uncompressedSize > maximumLibraryEntryBytes
                     || ! isSafeArchiveEntryName (entry->filename)
                     || entry->filename.trimCharactersAtEnd ("/")
                            == ".churchdaw-library.json")
                    return juce::Result::fail (
                        "Pack contem uma entrada ZIP nao permitida");

                const auto output = staging.getChildFile (entry->filename);
                const bool directoryEntry =
                    entry->filename.endsWithChar ('/');

                if (! output.isAChildOf (staging))
                    return juce::Result::fail (
                        "Pack tentou gravar fora do staging");

                if (directoryEntry)
                {
                    if (entry->uncompressedSize != 0)
                        return juce::Result::fail (
                            "Diretorio ZIP declarou conteudo inesperado");
                }
                else
                {
                    ++inspection.fileCount;
                }

                if (! checkedAddNonNegative (
                        inspection.expandedBytes,
                        entry->uncompressedSize,
                        inspection.expandedBytes)
                     || inspection.expandedBytes > pack.installedBytes
                     || completedExpanded < 0
                     || completedExpanded > pack.installedBytes
                                               - inspection.expandedBytes)
                    return juce::Result::fail (
                        "Pack expandido excede o tamanho declarado");
            }

            if (inspection.fileCount <= 0)
                return juce::Result::fail (
                    "Pack nao contem nenhum arquivo instalavel");

            return juce::Result::ok();
        }

        juce::Result extractArchiveEntryWithLimits (
            juce::ZipFile& archive,
            int entryIndex,
            const juce::File& staging)
        {
            const auto* entry = archive.getEntry (entryIndex);

            if (entry == nullptr
                 || entry->isSymbolicLink
                 || entry->uncompressedSize < 0
                 || entry->uncompressedSize > maximumLibraryEntryBytes
                 || ! isSafeArchiveEntryName (entry->filename))
                return juce::Result::fail (
                    "Entrada ZIP mudou depois da validacao");

            const auto output = staging.getChildFile (entry->filename);
            const bool directoryEntry =
                entry->filename.endsWithChar ('/');

            if (! output.isAChildOf (staging))
                return juce::Result::fail (
                    "Pack tentou extrair fora do staging");

            if (directoryEntry)
            {
                if ((output.exists() && ! output.isDirectory())
                     || output.createDirectory().failed())
                    return juce::Result::fail (
                        "Nao foi possivel criar um diretorio seguro do pack");

                for (auto cursor = output;
                     cursor != staging;
                     cursor = cursor.getParentDirectory())
                {
                    if (! cursor.isAChildOf (staging)
                         || cursor.isSymbolicLink())
                        return juce::Result::fail (
                            "Pack encontrou um link no caminho de extracao");
                }

                return juce::Result::ok();
            }

            // Arquivos repetidos, inclusive entre partes, sao rejeitados em
            // vez de sobrescritos. Assim a soma journalada sempre representa
            // exatamente a arvore que sera ativada.
            if (output.exists())
                return juce::Result::fail (
                    "Pack contem caminho de arquivo duplicado");

            const auto parent = output.getParentDirectory();

            if (parent.createDirectory().failed())
                return juce::Result::fail (
                    "Nao foi possivel criar a pasta de extracao");

            for (auto cursor = parent;
                 cursor != staging;
                 cursor = cursor.getParentDirectory())
            {
                if (! cursor.isAChildOf (staging)
                     || cursor.isSymbolicLink())
                    return juce::Result::fail (
                        "Pack encontrou um link no caminho de extracao");
            }

            const auto temporary = output.getSiblingFile (
                ".extracting-" + juce::Uuid().toString());

            if (temporary.exists() || ! temporary.isAChildOf (staging))
                return juce::Result::fail (
                    "Arquivo temporario de extracao inseguro");

            std::unique_ptr<juce::InputStream> input (
                archive.createStreamForEntry (entryIndex));
            auto outputStream = temporary.createOutputStream();
            auto fail = [&temporary, &input, &outputStream] (
                const juce::String& message)
            {
                outputStream.reset();
                input.reset();
                temporary.deleteFile();
                return juce::Result::fail (message);
            };

            if (input == nullptr
                 || outputStream == nullptr
                 || outputStream->failedToOpen())
                return fail ("Nao foi possivel abrir a entrada ZIP");

            juce::HeapBlock<char> buffer (64 * 1024);
            juce::int64 written = 0;

            while (written < entry->uncompressedSize)
            {
                if (threadShouldExit())
                    return fail (
                        "Instalacao cancelada durante a extracao");

                const auto remaining =
                    entry->uncompressedSize - written;
                const int wanted = (int) (
                    remaining < (juce::int64) 64 * 1024
                        ? remaining
                        : (juce::int64) 64 * 1024);
                const int received =
                    input->read (buffer.getData(), wanted);

                if (received <= 0
                     || received > wanted
                     || ! outputStream->write (
                            buffer.getData(), (size_t) received))
                    return fail (
                        "ZIP terminou cedo ou falhou ao gravar");

                written += received;
            }

            char unexpectedByte = 0;
            const int trailingBytes =
                input->read (&unexpectedByte, 1);

            if (trailingBytes != 0)
                return fail (
                    "ZIP expandiu alem do tamanho declarado ou terminou com erro");

            outputStream->flush();

            if (outputStream->getStatus().failed())
                return fail ("Falha ao confirmar a escrita do pack");

            outputStream.reset();
            input.reset();

            if (temporary.getSize() != entry->uncompressedSize
                 || output.exists()
                 || ! temporary.moveFileTo (output))
                return fail (
                    "Arquivo extraido nao corresponde ao ZIP validado");

            return juce::Result::ok();
        }

        juce::Result extractValidatedPart (
            const juce::File& root,
            const juce::File& partFile,
            const juce::File& staging,
            LibraryInstallResumeState& state,
            int partIndex)
        {
            ArchiveInspection inspection;
            auto result = inspectArchive (
                partFile, staging, state, inspection);

            if (result.failed())
                return result;

            juce::int64 entryMetadata = 0;
            juce::int64 extractionRequired = 0;

            if (! checkedAddNonNegative (
                    0, (juce::int64) inspection.entryCount * 4096,
                    entryMetadata)
                 || ! checkedAddNonNegative (
                        inspection.expandedBytes,
                        entryMetadata,
                        extractionRequired)
                 || ! checkedAddNonNegative (
                        extractionRequired,
                        installerStorageReserve,
                        extractionRequired))
                return juce::Result::fail (
                    "Tamanho expandido excede o limite suportado");

            const auto freeBytes = root.getBytesFreeOnVolume();

            if (freeBytes <= 0 || freeBytes < extractionRequired)
                return juce::Result::fail (
                    "Espaco insuficiente para extrair esta parte com seguranca");

            // Ponteiro, e nao objeto de pilha, para o ZIP poder ser FECHADO
            // antes da limpeza la embaixo.
            //
            // O juce::ZipFile mantem um FileInputStream aberto no .zip. No
            // Windows nao se apaga um arquivo aberto, entao o deleteFile()
            // do fim desta funcao falhava SEMPRE -- o usuario tinha de clicar
            // "tentar novamente" uma vez por parte, 30 vezes no pack completo.
            // No Android passava despercebido: o POSIX permite unlink de
            // arquivo aberto, o nome some na hora e os dados so depois.
            auto archive = std::make_unique<juce::ZipFile> (partFile);

            for (int entryIndex = 0;
                 entryIndex < inspection.entryCount;
                 ++entryIndex)
            {
                if (threadShouldExit())
                    return juce::Result::fail (
                        "Instalacao cancelada. O ZIP validado e o staging "
                        "foram preservados para retomar.");

                result = extractArchiveEntryWithLimits (
                    *archive, entryIndex, staging);

                if (result.failed())
                    return juce::Result::fail (
                        "Falha ao extrair a parte "
                        + juce::String (partIndex + 1) + ": "
                        + result.getErrorMessage());

                if ((entryIndex & 31) == 0
                     || entryIndex + 1 == inspection.entryCount)
                    publish (
                        partProgress (
                            partIndex,
                            0.58f + 0.40f
                                * ((float) (entryIndex + 1)
                                   / (float) inspection.entryCount),
                            state.parts.size()),
                        "Instalando arquivo "
                            + juce::String (entryIndex + 1)
                            + " de "
                            + juce::String (inspection.entryCount)
                            + " da parte "
                            + juce::String (partIndex + 1) + "...");
            }

            // Fecha o .zip assim que ele deixa de ser necessario. Precisa vir
            // antes do deleteFile() -- ver a nota na abertura do arquivo.
            archive.reset();

            auto& completedPart =
                state.parts.getReference (partIndex);
            completedPart.extracted = true;
            completedPart.entryCount = inspection.entryCount;
            completedPart.fileCount = inspection.fileCount;
            completedPart.expandedBytes = inspection.expandedBytes;

            if (! state.isValid()
                 || ! writeLibraryInstallResumeState (root, state))
            {
                completedPart.extracted = false;
                completedPart.entryCount = 0;
                completedPart.fileCount = 0;
                completedPart.expandedBytes = 0;
                return juce::Result::fail (
                    "A parte foi extraida, mas nao foi possivel confirmar "
                    "o journal. O ZIP foi preservado para uma retomada segura.");
            }

            // O ZIP so deixa de existir depois que seu descritor completo
            // (pack/versao/URL/SHA256/tamanho) e a extracao concluida estao
            // confirmados no journal persistente.
            // No Windows a remocao pode falhar por alguns instantes: antivirus
            // e indexador abrem o arquivo recem-gravado para varredura assim
            // que ele termina de ser escrito. Tentar de novo por pouco mais de
            // um segundo resolve o caso comum.
            bool released = partFile.deleteFile();

            for (int attempt = 0; ! released && attempt < 10; ++attempt)
            {
                juce::Thread::sleep (150);
                released = partFile.deleteFile();
            }

            if (! released)
            {
                // A parte JA esta instalada e confirmada no journal. Derrubar
                // a instalacao inteira porque a faxina falhou e
                // desproporcional -- era isso que obrigava o usuario a clicar
                // "tentar novamente" uma vez por parte, 30 vezes no pack
                // completo, sem que nada estivesse de fato errado.
                //
                // O ZIP orfao e removido na proxima preparacao, que ja varre e
                // apaga partes antigas deste pack.
                juce::Logger::writeToLog (
                    "[biblioteca] o ZIP da parte "
                     + juce::String (partIndex + 1)
                     + " nao pode ser removido agora; sera limpo na proxima "
                       "preparacao");
            }

            return juce::Result::ok();
        }

        juce::Result prepareTransactionArea (const juce::File& root)
        {
            // Compatibilidade com o backup de nome fixo criado pelas builds
            // anteriores ao journal transacional.
            const auto legacyBackup = root.getChildFile (".backup-" + pack.id);
            const auto destination = pack.installedFolder();
            const auto backupVersion = validLibraryMarkerVersion (legacyBackup, pack.id);

            if (backupVersion > 0 && ! destination.exists())
            {
                if (! legacyBackup.moveFileTo (destination)
                     || validLibraryMarkerVersion (destination, pack.id) <= 0)
                    return juce::Result::fail (
                        "Nao foi possivel recuperar a biblioteca anterior");
            }
            else if (legacyBackup.exists()
                      && ! removeInternalPath (root, legacyBackup))
            {
                return juce::Result::fail (
                    "Nao foi possivel limpar um backup antigo");
            }

            return juce::Result::ok();
        }

        juce::Result activateStaging (const juce::File& root,
                                      const juce::File& staging)
        {
            const juce::ScopedLock transactionLock (libraryTransactionMutex());
            const auto token = juce::String::toHexString (
                juce::Random::getSystemRandom().nextInt64());
            const auto destination = pack.installedFolder();
            const auto backup = root.getChildFile (".backup-" + pack.id + "-" + token);
            const auto journal = libraryTransactionJournal (root, pack.id);

            LibraryTransactionState state;
            state.id = pack.id;
            state.installFolder = pack.installFolder;
            state.targetVersion = pack.version;
            state.stagingName = staging.getFileName();
            state.backupName = backup.getFileName();

            if (journal.exists() && ! recoverLibraryTransaction (root, journal))
                return juce::Result::fail (
                    "Existe uma instalacao anterior que nao pode ser recuperada");

            state.previousWasValid =
                validLibraryMarkerVersion (destination, pack.id) > 0;

            if (! writeLibraryTransaction (root, state, "prepared"))
                return juce::Result::fail (
                    "Nao foi possivel registrar a transacao da biblioteca");

            if (destination.exists())
            {
                const bool movedToBackup = destination.moveFileTo (backup);
                const bool backupPreserved = movedToBackup
                    && backup.exists()
                    && (! state.previousWasValid
                        || validLibraryMarkerVersion (backup, pack.id) > 0);

                if (! backupPreserved)
                {
                    const bool preserved = destination.exists()
                        || (backup.exists() && backup.moveFileTo (destination));

                    return juce::Result::fail (
                        preserved
                            ? "Nao foi possivel preservar a biblioteca anterior"
                            : "Falha critica ao restaurar a biblioteca anterior");
                }

                if (! writeLibraryTransaction (root, state, "backed_up"))
                {
                    const bool restored = destination.exists()
                        || (backup.exists() && backup.moveFileTo (destination));

                    return juce::Result::fail (
                        restored
                            ? "Nao foi possivel confirmar o backup da biblioteca"
                            : "Falha critica ao confirmar e restaurar o backup");
                }
            }

            if (! staging.moveFileTo (destination)
                 || validLibraryMarkerVersion (destination, pack.id) < pack.version)
            {
                bool rollbackOk = true;

                if (destination.exists() && ! staging.exists())
                    rollbackOk = destination.moveFileTo (staging);

                if (backup.exists() && ! destination.exists())
                    rollbackOk = backup.moveFileTo (destination) && rollbackOk;

                return juce::Result::fail (
                    rollbackOk ? "Nao foi possivel ativar a biblioteca"
                               : "Falha ao ativar e restaurar a biblioteca anterior");
            }

            if (! writeLibraryTransaction (root, state, "activated"))
            {
                // A nova pasta ja esta valida e canonica. O journal preparado
                // fica no disco para a recuperacao confirmar/limpar no inicio.
                return juce::Result::fail (
                    "Biblioteca ativada, mas a transacao nao foi finalizada");
            }

            const bool backupRemoved = removeInternalPath (root, backup);
            const bool journalRemoved = backupRemoved
                && (! journal.existsAsFile() || journal.deleteFile());

            if (! backupRemoved || ! journalRemoved)
                juce::Logger::writeToLog (
                    "BIBLIOTECA: ativada; limpeza transacional ficara para o proximo inicio");

            return juce::Result::ok();
        }

        juce::Result install()
        {
            if (! pack.canDownload()
                 || ! isSafeIdentifier (pack.id)
                 || ! isSafeIdentifier (pack.installFolder))
                return juce::Result::fail ("Pack sem URL segura ou checksum valido");

            auto root = AppPaths::managedLibrariesFolder();
            auto result = root.createDirectory();

            if (result.failed())
                return juce::Result::fail ("Nao foi possivel preparar o armazenamento");

            if (! recoverLibraryTransactions())
                return juce::Result::fail (
                    "Uma instalacao anterior precisa ser recuperada antes de continuar");

            const auto downloadParts = downloadPartsFor (pack);
            LibraryInstallResumeState resumeState;
            juce::File staging;

            {
                const juce::ScopedLock transactionLock (libraryTransactionMutex());
                result = prepareTransactionArea (root);
                if (result.wasOk())
                    result = prepareOrResumeInstall (
                        root, downloadParts, resumeState, staging);
            }

            if (result.failed())
                return result;

            if (threadShouldExit())
                return juce::Result::fail (
                    "Instalacao cancelada. O staging e o journal foram "
                    "preservados para retomar.");

            result = ensureStreamingInstallSpace (root, resumeState);
            if (result.failed())
                return result;

            for (int partIndex = 0; partIndex < downloadParts.size(); ++partIndex)
            {
                if (threadShouldExit())
                    return juce::Result::fail (
                        "Instalacao cancelada. Partes concluidas, staging e "
                        "journal foram preservados para retomar.");

                const auto& downloadPart = downloadParts.getReference (partIndex);
                const auto partFile = partFileFor (
                    root, pack, partIndex, downloadParts.size());

                if (resumeState.parts.getReference (partIndex).extracted)
                {
                    if (partFile.existsAsFile() && ! partFile.deleteFile())
                        return juce::Result::fail (
                            "Uma parte ja instalada nao pode ser liberada");

                    publish (
                        partProgress (
                            partIndex, 1.0f, downloadParts.size()),
                        "Parte " + juce::String (partIndex + 1)
                            + " retomada do staging");
                    continue;
                }

                result = ensureStreamingInstallSpace (root, resumeState);

                if (result.failed())
                    return result;

                juce::int64 resumeAt = partFile.existsAsFile() ? partFile.getSize() : 0;

                if (resumeAt < 0 || resumeAt > downloadPart.downloadBytes)
                {
                    if (partFile.exists() && ! partFile.deleteFile())
                        return juce::Result::fail (
                            "Nao foi possivel remover uma parte de tamanho invalido");

                    resumeAt = 0;
                }

                if (resumeAt == downloadPart.downloadBytes)
                {
                    publish (
                        partProgress (
                            partIndex, 0.52f, downloadParts.size()),
                        "Verificando parte concluida "
                            + juce::String (partIndex + 1) + " de "
                            + juce::String (downloadParts.size()) + "...");

                    juce::String actualSha256;
                    result = calculatePartSha256 (partFile, actualSha256);

                    if (result.failed())
                        return result;

                    if (actualSha256.equalsIgnoreCase (
                            downloadPart.sha256))
                    {
                        result = extractValidatedPart (
                            root, partFile, staging, resumeState, partIndex);

                        if (result.failed())
                            return result;

                        continue;
                    }

                    if (! partFile.deleteFile())
                        return juce::Result::fail (
                            "Nao foi possivel reiniciar uma parte corrompida");

                    resumeAt = 0;
                }

                auto headers = juce::String ("Accept: application/zip\n")
                             + libraryAuthorizationHeader();

                if (resumeAt > 0)
                    headers += "Range: bytes=" + juce::String (resumeAt) + "-\n";

                publish (
                    partProgress (
                        partIndex, 0.01f, downloadParts.size()),
                    downloadParts.size() > 1
                        ? "Conectando a parte "
                            + juce::String (partIndex + 1)
                            + " de " + juce::String (downloadParts.size()) + "..."
                        : "Conectando...");

                auto input = std::make_shared<juce::WebInputStream> (
                    juce::URL (downloadPart.downloadUrl), false);
                input->withConnectionTimeout (12000)
                     // O download pode carregar Authorization. A URL ja foi
                     // validada como oficial; seguir um redirect sem revalidar
                     // o host poderia vazar o bearer para outro destino.
                     .withNumRedirectsToFollow (0)
                     .withExtraHeaders (headers);
                ActiveDownloadRegistration activeDownload (*this, input);

                if (threadShouldExit())
                {
                    input->cancel();
                    return juce::Result::fail (
                        "Download cancelado. O arquivo parcial esta vinculado "
                        "ao journal e foi preservado para retomar.");
                }

                const bool connected = input->connect (nullptr);
                const int statusCode = input->getStatusCode();
                const auto responseHeaders = input->getResponseHeaders();

                if (threadShouldExit())
                    return juce::Result::fail (
                        "Download cancelado. O arquivo parcial esta vinculado "
                        "ao journal e foi preservado para retomar.");

                if (! connected || input->isError()
                     || (statusCode != 200 && statusCode != 206))
                {
                    if (statusCode == 416 && resumeAt > 0
                         && (! partFile.exists() || partFile.deleteFile()))
                        return juce::Result::fail (
                            "O servidor rejeitou a retomada. Tente novamente para reiniciar a parte.");

                    return juce::Result::fail ("Falha no download da parte "
                                              + juce::String (partIndex + 1)
                                              + " (HTTP " + juce::String (statusCode) + ")");
                }

                if (statusCode == 206
                     && ! contentRangeMatches (responseHeaders["Content-Range"],
                                               resumeAt,
                                               downloadPart.downloadBytes))
                {
                    input->cancel();

                    if (partFile.exists() && ! partFile.deleteFile())
                        return juce::Result::fail (
                            "Content-Range invalido e a parte antiga nao pode ser removida");

                    return juce::Result::fail (
                        "Servidor respondeu uma faixa invalida. A parte sera reiniciada.");
                }

                if (statusCode == 200)
                {
                    resumeAt = 0;

                    if (partFile.exists() && ! partFile.deleteFile())
                        return juce::Result::fail ("Nao foi possivel reiniciar o download");
                }

                juce::FileOutputStream output (partFile);

                if (output.failedToOpen() || ! output.setPosition (resumeAt))
                    return juce::Result::fail ("Nao foi possivel gravar o download");

                juce::HeapBlock<char> buffer (128 * 1024);
                auto total = resumeAt;

                while (! threadShouldExit())
                {
                    const int got = input->read (buffer.getData(), 128 * 1024);

                    if (got <= 0)
                        break;

                    if (! output.write (buffer.getData(), (size_t) got))
                        return juce::Result::fail ("Erro ao gravar os samples");

                    total += got;

                    if (total > downloadPart.downloadBytes)
                        return juce::Result::fail (
                            "Servidor enviou uma parte maior que o catalogo");

                    auto message = downloadParts.size() > 1
                        ? "Baixando parte " + juce::String (partIndex + 1)
                            + " de " + juce::String (downloadParts.size()) + " - "
                        : "Baixando ";
                    message += humanBytes (total) + " de "
                        + humanBytes (downloadPart.downloadBytes);
                    const auto fraction =
                        (float) ((double) total
                                 / (double) downloadPart.downloadBytes);
                    publish (
                        partProgress (
                            partIndex,
                            0.02f + 0.48f * fraction,
                            downloadParts.size()),
                        std::move (message));
                }

                output.flush();

                if (output.getStatus().failed())
                    return juce::Result::fail ("Erro ao confirmar os samples no armazenamento");

                if (threadShouldExit())
                    return juce::Result::fail (
                        "Download cancelado. O arquivo parcial esta vinculado "
                        "ao journal e foi preservado para retomar.");

                if (total != downloadPart.downloadBytes)
                    return juce::Result::fail ("Download incompleto na parte "
                                              + juce::String (partIndex + 1));

                publish (
                    partProgress (
                        partIndex, 0.52f, downloadParts.size()),
                    "Verificando parte " + juce::String (partIndex + 1)
                        + " de " + juce::String (downloadParts.size()) + "...");

                juce::String actualSha256;
                result = calculatePartSha256 (partFile, actualSha256);

                if (result.failed())
                    return result;

                if (! actualSha256.equalsIgnoreCase (
                        downloadPart.sha256))
                {
                    if (! partFile.deleteFile())
                        return juce::Result::fail (
                            "Checksum incorreto e a parte corrompida nao pode ser removida");

                    return juce::Result::fail (
                        "Checksum incorreto na parte " + juce::String (partIndex + 1)
                        + ". O arquivo foi descartado.");
                }

                if (threadShouldExit())
                    return juce::Result::fail (
                        "Instalacao cancelada. O ZIP validado e o staging "
                        "foram preservados para retomar.");

                publish (
                    partProgress (
                        partIndex, 0.56f, downloadParts.size()),
                    "Validando estrutura da parte "
                        + juce::String (partIndex + 1) + "...");

                result = extractValidatedPart (
                    root, partFile, staging, resumeState, partIndex);

                if (result.failed())
                    return result;
            }

            if (threadShouldExit())
                return juce::Result::fail (
                    "Instalacao cancelada. O staging completo foi preservado "
                    "antes da ativacao.");

            const auto expandedBytes =
                resumeState.completedExpandedBytes();
            const auto totalEntries =
                resumeState.completedEntryCount();
            const auto totalFiles =
                resumeState.completedFileCount();

            if (expandedBytes != pack.installedBytes
                 || totalEntries <= 0
                 || totalEntries > maximumLibraryEntryCount
                 || totalFiles <= 0
                 || totalFiles > totalEntries)
                return juce::Result::fail (
                    "Pack expandido nao corresponde ao tamanho declarado");

            StagingInspection actualStaging;
            result = inspectStagingContents (staging, actualStaging);

            if (result.failed())
                return result;

            if (actualStaging.expandedBytes != expandedBytes
                 || actualStaging.fileCount != totalFiles)
                return juce::Result::fail (
                    "Conteudo real do staging nao corresponde ao journal");

            if (! containsSupportedLibrarySample (staging))
                return juce::Result::fail (
                    "Pack instalado nao contem nenhum sample de audio suportado");

            auto marker = std::make_unique<juce::DynamicObject>();
            marker->setProperty ("schema_version", 1);
            marker->setProperty ("id", pack.id);
            marker->setProperty ("name", pack.name);
            marker->setProperty ("version", pack.version);
            marker->setProperty ("version_name", pack.versionName);
            marker->setProperty ("installed_bytes", expandedBytes);
            marker->setProperty ("file_count", totalFiles);
            marker->setProperty ("installed_at_ms",
                                 juce::Time::getCurrentTime().toMilliseconds());

            if (! staging.getChildFile (".churchdaw-library.json")
                    .replaceWithText (juce::JSON::toString (juce::var (marker.release()), true)))
                return juce::Result::fail (
                    "Falha ao finalizar o registro do pack");

            if (threadShouldExit())
                return juce::Result::fail (
                    "Instalacao cancelada. O staging completo e o journal "
                    "foram preservados antes da ativacao.");

            result = activateStaging (root, staging);

            if (result.failed())
            {
                const auto originalError = result.getErrorMessage();
                recoverLibraryTransactions();

                if (validLibraryMarkerVersion (pack.installedFolder(), pack.id)
                    < pack.version)
                {
                    return juce::Result::fail (originalError);
                }
            }

            const auto resumeJournal =
                libraryInstallResumeJournal (root, pack.id);

            if (resumeJournal.existsAsFile()
                 && ! resumeJournal.deleteFile())
                juce::Logger::writeToLog (
                    "BIBLIOTECA: instalada; journal de retomada sera limpo "
                    "na proxima manutencao");

            // VARREDURA FINAL DOS ZIPS DE PARTE.
            //
            // A limpeza de cada parte acontece logo depois de extrai-la, mas no
            // Windows ela pode falhar por instantes: o antivirus abre o arquivo
            // recem-gravado para varredura. Nesse caso a instalacao segue (a
            // parte ja esta extraida e journalada) e o ZIP fica para depois.
            //
            // "Depois" precisa ser AQUI. Quando a instalacao conclui, a rotina
            // de preparacao -- que e quem varre partes antigas -- nunca mais
            // roda para este pack, e os ZIPs ficam encalhados para sempre.
            //
            // Medido em 28/07/2026 numa instalacao real: 27 pedacos, 11,25 GB
            // parados no disco de um usuario que tinha 33 GB livres.
            //
            // Falhar aqui seria desproporcional: a biblioteca esta instalada e
            // funcionando. Registra e segue.
            const auto sweep = removeAllPackPartFiles (root, pack);

            if (sweep.failed())
                juce::Logger::writeToLog (
                    "BIBLIOTECA: instalada, mas sobraram partes no disco: "
                     + sweep.getErrorMessage());

            publish (1.0f, "Biblioteca instalada");
            return juce::Result::ok();
        }

        void publish (float value, juce::String text)
        {
            if (progress == nullptr)
                return;

            auto callback = progress;
            juce::MessageManager::callAsync ([callback = std::move (callback), value,
                                              text = std::move (text)]
            {
                callback (value, text);
            });
        }

        void deliverFinished (juce::Result result)
        {
            if (finished == nullptr)
                return;

            auto callback = finished;
            juce::MessageManager::callAsync ([callback = std::move (callback),
                                              result = std::move (result)] () mutable
            {
                callback (std::move (result));
            });
        }

        LibraryPack pack;
        Progress progress;
        Finished finished;
        std::mutex downloadStreamMutex;
        std::shared_ptr<juce::WebInputStream> activeDownloadStream;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LibraryInstaller)
    };
}
