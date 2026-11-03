#pragma once

#include "AppPaths.h"
#include "ChurchDAWServices.h"

#include <functional>
#include <memory>
#include <mutex>

//==============================================================================
// Catalogo de TIMBRES individuais (ex.: Black Upright, Pad Bell).
// Diferente do pack church-live de 13 GB: cada instrumento tem seu ZIP.
// URL publica: https://churchdaw.com/app/android/timbres/timbre-catalog-v1.json
//==============================================================================
namespace ChurchDAWTimbreCatalog
{
    inline constexpr const char* catalogUrl =
        "https://churchdaw.com/app/android/timbres/timbre-catalog-v1.json";
    inline constexpr const char* catalogUrlMirror =
        "https://churchdaw.com/app/android/timbres/timbre-catalog-v1.json?retry=1";

    inline bool isValidSha256 (const juce::String& value) noexcept
    {
        const auto hash = value.trim();
        return hash.length() == 64
            && hash.containsOnly ("0123456789abcdefABCDEF");
    }

    // Catalogos e ZIPs carregam nomes relativos ("Piano/C4.wav"). Construir
    // juce::File a partir deles dispara uma assertion em iOS/macOS, pois File
    // exige caminho absoluto. Para validar extensao basta analisar o texto.
    inline juce::String extensionFromPathText (juce::String value)
    {
        value = value.replaceCharacter ('\\', '/');
        const int slash = value.lastIndexOfChar ('/');
        const int dot = value.lastIndexOfChar ('.');
        return dot > slash ? value.substring (dot).toLowerCase()
                           : juce::String();
    }

    inline bool isOfficialTimbreDownloadUrl (const juce::String& value)
    {
        static constexpr const char* prefixes[] = {
            "https://churchdaw.com/app/android/timbres/",
            "https://www.churchdaw.com/app/android/timbres/"
        };

        const auto url = value.trim();
        int prefixLength = 0;

        for (const auto* prefix : prefixes)
            if (url.startsWith (prefix))
            {
                prefixLength = (int) std::char_traits<char>::length (prefix);
                break;
            }

        if (prefixLength == 0 || url.length() > 360
             || url.containsAnyOf ("?#\r\n"))
            return false;

        const auto fileName = url.substring (prefixLength);

        if (fileName.isEmpty() || fileName.length() > 200
             || fileName.containsAnyOf ("/\\")
             || ! fileName.containsOnly (
                 "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
                 "0123456789._-"))
            return false;

        if (fileName.endsWithIgnoreCase (".zip"))
            return true;

        const auto marker = fileName.lastIndexOfIgnoreCase (".zip.part");

        if (marker <= 0)
            return false;

        const auto suffix = fileName.substring (marker + 9);
        return suffix.length() == 2
            && suffix.containsOnly ("0123456789")
            && suffix.getIntValue() >= 1
            && suffix.getIntValue() <= 64;
    }

    inline bool isSafeInstallRelativePath (const juce::String& value)
    {
        auto path = value.trim().replaceCharacter ('\\', '/');

        if (path.isEmpty() || path.length() > 320
             || path.startsWithChar ('/') || path.containsChar (':')
             || path.containsAnyOf ("\r\n"))
            return false;

        juce::StringArray segments;
        segments.addTokens (path, "/", {});

        if (segments.isEmpty())
            return false;

        for (const auto& segment : segments)
            if (segment.isEmpty() || segment == "." || segment == "..")
                return false;

        return true;
    }

    inline bool isTimbreAudioFileName (const juce::String& fileName)
    {
        const auto extension = extensionFromPathText (fileName);
        return extension == ".flac" || extension == ".wav"
            || extension == ".aif" || extension == ".aiff";
    }

    inline bool calculateInstallerWorkspaceBytes (
        juce::int64 downloadBytes,
        juce::int64 installedBytes,
        juce::int64& requiredBytes) noexcept
    {
        requiredBytes = 0;

        if (! ChurchDAWServices::checkedAddNonNegative (
                downloadBytes,
                ChurchDAWServices::installerStorageReserve,
                requiredBytes))
            return false;

        return installedBytes <= 0
            || ChurchDAWServices::checkedAddNonNegative (
                requiredBytes, installedBytes, requiredBytes);
    }

    inline juce::File cachedCatalogFile()
    {
        return AppPaths::managedDataFolder()
            .getChildFile ("Services")
            .getChildFile ("timbre-catalog-v1.json");
    }

    // Parte de um ZIP grande (host limita ~500 MB por upload).
    struct Part
    {
        juce::String downloadUrl, sha256;
        juce::int64 downloadBytes = 0;

        bool isValid() const noexcept
        {
            return isOfficialTimbreDownloadUrl (downloadUrl)
                && isValidSha256 (sha256)
                && downloadBytes > 0
                && downloadBytes <= (juce::int64) 520 * 1024 * 1024;
        }
    };

    struct Instrument
    {
        juce::String id, name, rawLabel, category;
        juce::String libraryId { "church-live" };
        juce::String installRelative { "Imported" };
        juce::String downloadUrl, sha256;   // ZIP inteiro (sha do arquivo montado)
        juce::Array<Part> parts;            // se >1, baixar e concatenar
        int fileCount = 0;
        juce::int64 downloadBytes = 0;
        juce::int64 installedBytes = 0;     // opcional; tamanho expandido publicado
        bool included = true;
        bool ready = false;

        bool hasValidDownloadMetadata() const noexcept
        {
            if (! isValidSha256 (sha256)
                 || downloadBytes <= 0
                 || downloadBytes >= (juce::int64) 8 * 1024 * 1024 * 1024)
                return false;

            if (! ChurchDAWServices::isSafeIdentifier (id)
                 || ! ChurchDAWServices::isSafeIdentifier (libraryId)
                 || ! isSafeInstallRelativePath (installRelative))
                return false;

            if (! parts.isEmpty())
            {
                if (parts.size() > 64)
                    return false;

                juce::int64 sum = 0;

                for (const auto& p : parts)
                {
                    if (! p.isValid())
                        return false;

                    if (! ChurchDAWServices::checkedAddNonNegative (
                            sum, p.downloadBytes, sum))
                        return false;
                }

                return sum == downloadBytes;
            }

            return isOfficialTimbreDownloadUrl (downloadUrl);
        }

        bool canDownload() const noexcept
        {
            return ready && included && hasValidDownloadMetadata();
        }

        juce::File installFolder() const
        {
            return AppPaths::managedLibrariesFolder()
                .getChildFile (libraryId.isNotEmpty() ? libraryId : "church-live")
                .getChildFile (installRelative.isNotEmpty() ? installRelative
                                                            : "Imported");
        }

        juce::File installInProgressMarker() const
        {
            return installFolder().getChildFile (
                ".churchdaw-timbre-" + id + ".installing");
        }

        juce::File installedMarker() const
        {
            return installFolder().getChildFile (
                ".churchdaw-timbre-" + id + ".json");
        }

        bool usesSharedInstallFolder() const noexcept
        {
            return installRelative.trim().equalsIgnoreCase ("Imported");
        }

        int minimumInstalledAudioFiles() const noexcept
        {
            if (fileCount <= 0)
                return 8;

            return fileCount;
        }

        bool matchesInstalledStem (const juce::String& stem) const
        {
            const auto needle = rawLabel.isNotEmpty() ? rawLabel : name;
            return needle.isNotEmpty()
                && (stem.startsWithIgnoreCase (needle)
                    || stem.containsIgnoreCase (needle));
        }

        bool hasInstalledStems (const juce::StringArray& stems) const
        {
            int hits = 0;
            const int need = minimumInstalledAudioFiles();

            for (const auto& stem : stems)
                if (matchesInstalledStem (stem) && ++hits >= need)
                    return true;

            return false;
        }

        int countInstalledAudioPayload (const juce::File& folder,
                                        bool recursive) const
        {
            if (! folder.isDirectory())
                return 0;

            int hits = 0;
            const bool requireMatchingStem = usesSharedInstallFolder();

            for (const auto& entry : juce::RangedDirectoryIterator (
                     folder, recursive, "*.flac;*.wav;*.aif;*.aiff",
                     juce::File::findFiles))
            {
                const auto file = entry.getFile();

                if (file.getSize() <= 0)
                    continue;

                if (requireMatchingStem
                     && ! matchesInstalledStem (
                         file.getFileNameWithoutExtension()))
                    continue;

                ++hits;
            }

            return hits;
        }

        bool hasInstalledAudioPayload (const juce::File& folder,
                                       bool recursive) const
        {
            return countInstalledAudioPayload (folder, recursive)
                >= minimumInstalledAudioFiles();
        }

        // O recibo .json ajuda no diagnostico, mas nunca prova que os samples
        // ainda existem. Instalacoes antigas sem recibo continuam validas se
        // o payload real estiver presente.
        bool isInstalledInFolder (const juce::File& folder) const
        {
            if (! folder.isDirectory())
                return false;

            if (folder.getChildFile (
                    ".churchdaw-timbre-" + id + ".installing")
                    .existsAsFile())
                return false;

            const auto receipt = folder.getChildFile (
                ".churchdaw-timbre-" + id + ".json");

            if (receipt.existsAsFile())
            {
                const auto parsed = juce::JSON::parse (
                    receipt.loadFileAsString());
                auto* object = parsed.getDynamicObject();

                // Um arquivo de recibo presente, mas ilegivel, indica uma
                // instalacao interrompida/corrompida. Nunca o trate como
                // legado: em Imported a heuristica pode contar outro timbre.
                if (object == nullptr)
                    return false;

                const int schemaVersion = (int) object->getProperty (
                    "schema_version");
                auto* files = object->getProperty ("files").getArray();

                if (schemaVersion >= 2)
                {
                    // Recibos da geracao transacional sao autoritativos.
                    // Se estiverem truncados/corrompidos, nunca recue para
                    // a heuristica legado da pasta Imported: ela pode contar
                    // samples pertencentes a outro timbre.
                    if (files == nullptr
                         || object->getProperty ("id").toString() != id
                         || files->isEmpty())
                        return false;

                        juce::StringArray names;
                        int audioFiles = 0;

                        for (const auto& item : *files)
                        {
                            auto* entry = item.getDynamicObject();

                            if (entry == nullptr)
                                return false;

                            const auto name = entry->getProperty (
                                "name").toString();
                            const auto bytes = entry->getProperty (
                                "bytes").toString().getLargeIntValue();
                            const auto key = name.toLowerCase();

                            if (name.isEmpty() || name.containsAnyOf ("/\\")
                                 || names.contains (key) || bytes < 0)
                                return false;

                            const auto installed = folder.getChildFile (name);

                            if (! installed.existsAsFile()
                                 || installed.getSize() != bytes)
                                return false;

                            names.add (key);

                            if (isTimbreAudioFileName (name))
                                ++audioFiles;
                        }

                    return fileCount > 0 ? audioFiles == fileCount
                                         : audioFiles > 0;
                }
            }

            // Recibos schema 1 e instalacoes sem recibo usam a heuristica de
            // stems somente para compatibilidade com bibliotecas antigas.
            return hasInstalledAudioPayload (
                folder, ! usesSharedInstallFolder());
        }

        bool isInstalledLocally() const
        {
            return isInstalledInFolder (installFolder());
        }
    };

    inline std::mutex& installerCommitMutex()
    {
        static std::mutex mutex;
        return mutex;
    }

    inline juce::String installerArtifactPrefix (
        const char* kind,
        const Instrument& instrument)
    {
        return "." + juce::String (kind) + "-" + instrument.id + "-";
    }

    inline juce::File installInProgressMarkerIn (
        const Instrument& instrument,
        const juce::File& folder)
    {
        return folder.getChildFile (
            ".churchdaw-timbre-" + instrument.id + ".installing");
    }

    inline bool safeDeleteFileWithRetry (const juce::File& file, int maxAttempts = 10, int delayMs = 80)
    {
        if (! file.exists())
            return true;

        if (file.deleteFile())
            return true;

        for (int i = 0; i < maxAttempts; ++i)
        {
            juce::Thread::sleep (delayMs);
            if (! file.exists() || file.deleteFile())
                return true;
        }

        return ! file.exists();
    }

    inline bool safeMoveFileWithRetry (const juce::File& src, const juce::File& dst, int maxAttempts = 10, int delayMs = 80)
    {
        if (! src.exists())
            return false;

        if (src == dst)
            return true;

        if (dst.exists())
            safeDeleteFileWithRetry (dst, maxAttempts, delayMs);

        if (src.moveFileTo (dst))
            return true;

        for (int i = 0; i < maxAttempts; ++i)
        {
            juce::Thread::sleep (delayMs);
            if (dst.exists())
                dst.deleteFile();
            if (src.moveFileTo (dst))
                return true;
        }

        if (src.copyFileTo (dst))
        {
            safeDeleteFileWithRetry (src, maxAttempts, delayMs);
            return true;
        }

        return false;
    }

    inline juce::Result removeNamespacedInstallerArtifacts (
        const Instrument& instrument,
        const juce::File& folder,
        const char* kind)
    {
        if (! folder.isDirectory())
            return juce::Result::ok();

        const auto prefix = installerArtifactPrefix (kind, instrument);
        const auto files = folder.findChildFiles (
            juce::File::findFiles, false, prefix + "*");

        for (const auto& file : files)
            if (file.getFileName().startsWith (prefix)
                 && file.existsAsFile() && ! safeDeleteFileWithRetry (file))
                return juce::Result::fail (
                    "Nao foi possivel limpar " + file.getFileName());

        return juce::Result::ok();
    }

    struct InstalledFileIdentity
    {
        juce::String name;
        juce::int64 bytes = 0;
    };

    struct CommitEntry
    {
        InstalledFileIdentity identity;
        juce::File source, target, temporary, backup;
        bool hadTarget = false;
        bool committed = false;
    };

    struct CommitFaultInjection
    {
        int failAfterBackupIndex = -1;
        int failAfterCommitIndex = -1;
        bool forceCopyFallback = false;
        juce::int64 availableBytesOverride = -1;
    };

    struct CommitOutcome
    {
        juce::Result result { juce::Result::ok() };
        juce::Array<InstalledFileIdentity> files;
        int audioFileCount = 0;
    };

    inline juce::Result validateStagingPayload (
        const Instrument& instrument,
        const juce::File& staging,
        juce::Array<juce::File>& files,
        juce::Array<InstalledFileIdentity>& identities,
        int& audioFileCount,
        juce::int64& largestFileBytes)
    {
        files.clearQuick();
        identities.clearQuick();
        audioFileCount = 0;
        largestFileBytes = 0;
        juce::StringArray names;

        if (! staging.isDirectory())
            return juce::Result::fail ("Staging de instalacao ausente");

        for (const auto& item : juce::RangedDirectoryIterator (
                 staging, false, "*", juce::File::findFiles))
        {
            const auto file = item.getFile();
            const auto name = file.getFileName();
            const auto key = name.toLowerCase();
            const auto bytes = file.getSize();

            if (name.isEmpty() || name.containsAnyOf ("/\\")
                 || names.contains (key) || bytes < 0)
                return juce::Result::fail (
                    "Staging contem arquivo invalido ou duplicado");

            InstalledFileIdentity identity;
            identity.name = name;
            identity.bytes = bytes;
            files.add (file);
            identities.add (std::move (identity));
            names.add (key);
            largestFileBytes = juce::jmax (largestFileBytes, bytes);

            if (isTimbreAudioFileName (name))
                ++audioFileCount;
        }

        if (files.isEmpty())
            return juce::Result::fail (
                "Nenhum arquivo extraido para instalar");

        const bool countMatches = instrument.fileCount > 0
            ? audioFileCount == instrument.fileCount
            : audioFileCount > 0;

        if (! countMatches)
            return juce::Result::fail (
                "Staging divergente: " + juce::String (audioFileCount)
                + "/" + juce::String (instrument.fileCount)
                + " samples");

        return juce::Result::ok();
    }

    inline juce::Result prepareStagedFileForCommit (
        const juce::File& source,
        const juce::File& temporary,
        juce::int64 largestStagedFileBytes,
        bool attemptMoveFirst = true,
        juce::int64 availableBytesOverride = -1)
    {
        if (temporary.exists() && ! safeDeleteFileWithRetry (temporary))
            return juce::Result::fail (
                "Nao foi possivel limpar o temporario de destino");

        if (attemptMoveFirst && safeMoveFileWithRetry (source, temporary))
            return juce::Result::ok();

        if (temporary.exists() && ! safeDeleteFileWithRetry (temporary))
            return juce::Result::fail (
                "Nao foi possivel reiniciar o temporario de destino");

        juce::int64 fallbackRequired = 0;

        if (! ChurchDAWServices::checkedAddNonNegative (
                largestStagedFileBytes,
                ChurchDAWServices::installerStorageReserve,
                fallbackRequired))
            return juce::Result::fail (
                "Tamanho do fallback excedeu o limite suportado");

        const auto available = availableBytesOverride >= 0
            ? availableBytesOverride
            : temporary.getParentDirectory().getBytesFreeOnVolume();

        if (available < fallbackRequired)
            return juce::Result::fail (
                "Espaco insuficiente para fallback seguro de copia");

        if (source.copyFileTo (temporary))
        {
            safeDeleteFileWithRetry (source);
            return juce::Result::ok();
        }

        safeDeleteFileWithRetry (temporary);
        return juce::Result::fail (
            "Nao foi possivel preparar o arquivo no destino");
    }

    inline juce::Result writeCommitJournal (
        const Instrument& instrument,
        const juce::File& folder,
        const juce::Array<CommitEntry>& entries,
        const juce::String& phase)
    {
        auto journal = std::make_unique<juce::DynamicObject>();
        journal->setProperty ("schema_version", 2);
        journal->setProperty ("id", instrument.id);
        journal->setProperty ("phase", phase);
        juce::Array<juce::var> serialized;

        for (const auto& entry : entries)
        {
            auto item = std::make_unique<juce::DynamicObject>();
            item->setProperty ("name", entry.identity.name);
            item->setProperty ("bytes", entry.identity.bytes);
            item->setProperty ("temporary", entry.temporary.getFileName());
            item->setProperty ("backup", entry.backup.getFileName());
            item->setProperty ("had_target", entry.hadTarget);
            serialized.add (juce::var (item.release()));
        }

        journal->setProperty ("entries", juce::var (serialized));

        if (! installInProgressMarkerIn (instrument, folder).replaceWithText (
                juce::JSON::toString (juce::var (journal.release()), true)))
            return juce::Result::fail (
                "Nao foi possivel gravar o journal de instalacao");

        return juce::Result::ok();
    }

    inline juce::Result rollbackCommitEntries (
        juce::Array<CommitEntry>& entries,
        bool recoverFromJournal)
    {
        juce::String failure;

        for (int index = entries.size(); --index >= 0;)
        {
            auto& entry = entries.getReference (index);

            if (entry.temporary.existsAsFile()
                 && ! safeDeleteFileWithRetry (entry.temporary) && failure.isEmpty())
                failure = "Falha ao limpar " + entry.temporary.getFileName();

            const bool newTargetMayExist = recoverFromJournal
                ? ! entry.hadTarget || entry.backup.existsAsFile()
                : entry.committed;

            if (newTargetMayExist && entry.target.existsAsFile()
                 && ! safeDeleteFileWithRetry (entry.target) && failure.isEmpty())
                failure = "Falha ao remover commit parcial "
                    + entry.target.getFileName();

            if (entry.backup.existsAsFile())
            {
                if (entry.target.existsAsFile())
                    safeDeleteFileWithRetry (entry.target);

                if (! safeMoveFileWithRetry (entry.backup, entry.target)
                     && failure.isEmpty())
                    failure = "Falha ao restaurar "
                        + entry.target.getFileName();
            }
        }

        return failure.isEmpty() ? juce::Result::ok()
                                 : juce::Result::fail (failure);
    }

    inline juce::Result recoverInterruptedInstallUnlocked (
        const Instrument& instrument,
        const juce::File& folder)
    {
        const auto marker = installInProgressMarkerIn (instrument, folder);

        if (! marker.existsAsFile())
            return removeNamespacedInstallerArtifacts (
                instrument, folder, "installing");

        const auto parsed = juce::JSON::parse (marker.loadFileAsString());
        auto* journal = parsed.getDynamicObject();

        if (journal == nullptr
             || (int) journal->getProperty ("schema_version") < 2)
        {
            const auto cleanup = removeNamespacedInstallerArtifacts (
                instrument, folder, "installing");

            if (cleanup.failed())
                return cleanup;

            return marker.deleteFile()
                ? juce::Result::ok()
                : juce::Result::fail (
                    "Nao foi possivel remover marcador legado interrompido");
        }

        if (journal->getProperty ("id").toString() != instrument.id)
            return juce::Result::fail ("Journal pertence a outro timbre");

        auto* serialized = journal->getProperty ("entries").getArray();

        if (serialized == nullptr || serialized->isEmpty())
            return juce::Result::fail ("Journal de instalacao invalido");

        juce::Array<CommitEntry> entries;

        for (const auto& value : *serialized)
        {
            auto* item = value.getDynamicObject();

            if (item == nullptr)
                return juce::Result::fail ("Entrada invalida no journal");

            CommitEntry entry;
            entry.identity.name = item->getProperty ("name").toString();
            entry.identity.bytes = item->getProperty (
                "bytes").toString().getLargeIntValue();
            const auto temporaryName = item->getProperty (
                "temporary").toString();
            const auto backupName = item->getProperty ("backup").toString();
            const auto temporaryPrefix = installerArtifactPrefix (
                "installing", instrument);
            const auto backupPrefix = installerArtifactPrefix (
                "backup", instrument);

            if (entry.identity.name.isEmpty()
                 || entry.identity.name.containsAnyOf ("/\\")
                 || ! temporaryName.startsWith (temporaryPrefix)
                 || ! backupName.startsWith (backupPrefix)
                 || temporaryName.containsAnyOf ("/\\")
                 || backupName.containsAnyOf ("/\\"))
                return juce::Result::fail (
                    "Journal contem caminho transacional invalido");

            entry.target = folder.getChildFile (entry.identity.name);
            entry.temporary = folder.getChildFile (temporaryName);
            entry.backup = folder.getChildFile (backupName);
            entry.hadTarget = (bool) item->getProperty ("had_target");
            entries.add (std::move (entry));
        }

        const bool committed = journal->getProperty (
            "phase").toString() == "committed";

        if (! committed)
        {
            const auto rollback = rollbackCommitEntries (entries, true);

            if (rollback.failed())
                return rollback;
        }

        for (const auto& entry : entries)
            for (const auto& artifact : { entry.temporary, entry.backup })
                if (artifact.existsAsFile() && ! safeDeleteFileWithRetry (artifact))
                    return juce::Result::fail (
                        "Nao foi possivel limpar " + artifact.getFileName());

        if (marker.existsAsFile() && ! safeDeleteFileWithRetry (marker))
            return juce::Result::fail (
                "Nao foi possivel finalizar recuperacao do journal");

        return juce::Result::ok();
    }

    inline juce::Result recoverInterruptedInstall (
        const Instrument& instrument,
        const juce::File& folder)
    {
        const std::lock_guard<std::mutex> lock (installerCommitMutex());
        return recoverInterruptedInstallUnlocked (instrument, folder);
    }

    inline juce::int64 remainingDownloadBytes (
        const Instrument& instrument,
        const juce::File& tempRoot,
        const juce::File& zipFile)
    {
        juce::int64 reusable = 0;

        if (instrument.parts.isEmpty())
        {
            const auto existing = zipFile.existsAsFile()
                ? zipFile.getSize() : 0;

            if (existing >= 0 && existing <= instrument.downloadBytes)
                reusable = existing;
        }
        else
        {
            const auto existingZip = zipFile.existsAsFile()
                ? zipFile.getSize() : 0;
            juce::int64 assembledBoundary = 0;
            int completedParts = 0;

            for (int index = 0; index < instrument.parts.size(); ++index)
            {
                juce::int64 nextBoundary = 0;

                if (! ChurchDAWServices::checkedAddNonNegative (
                        assembledBoundary,
                        instrument.parts.getReference (index).downloadBytes,
                        nextBoundary)
                     || existingZip < nextBoundary)
                    break;

                assembledBoundary = nextBoundary;
                ++completedParts;
            }

            reusable = assembledBoundary;

            for (int index = completedParts;
                 index < instrument.parts.size(); ++index)
            {
                const auto partFile = tempRoot.getChildFile (
                    instrument.id + "-p"
                    + juce::String (index + 1).paddedLeft ('0', 2)
                    + ".download.part");
                const auto existing = partFile.existsAsFile()
                    ? partFile.getSize() : 0;
                const auto expected = instrument.parts.getReference (
                    index).downloadBytes;

                if (existing >= 0 && existing <= expected
                     && ! ChurchDAWServices::checkedAddNonNegative (
                         reusable, existing, reusable))
                    return instrument.downloadBytes;
            }
        }

        return juce::jmax ((juce::int64) 0,
                           instrument.downloadBytes
                               - juce::jmin (instrument.downloadBytes,
                                             reusable));
    }

    inline CommitOutcome commitStagingTransaction (
        const Instrument& instrument,
        const juce::File& staging,
        const juce::File& destination,
        const std::function<bool()>& shouldCancel = {},
        const std::function<void (int, int)>& progress = {},
        const CommitFaultInjection* fault = nullptr)
    {
        CommitOutcome outcome;
        juce::Array<juce::File> stagedFiles;
        juce::int64 largestFileBytes = 0;
        const auto stagingResult = validateStagingPayload (
            instrument, staging, stagedFiles, outcome.files,
            outcome.audioFileCount, largestFileBytes);

        if (stagingResult.failed())
        {
            outcome.result = stagingResult;
            return outcome;
        }

        const auto createResult = destination.createDirectory();

        if (createResult.failed())
        {
            outcome.result = juce::Result::fail (
                "Nao foi possivel criar a pasta do timbre");
            return outcome;
        }

        const std::lock_guard<std::mutex> lock (installerCommitMutex());
        const auto recovery = recoverInterruptedInstallUnlocked (
            instrument, destination);

        if (recovery.failed())
        {
            outcome.result = recovery;
            return outcome;
        }

        juce::Array<CommitEntry> entries;

        for (int index = 0; index < stagedFiles.size(); ++index)
        {
            CommitEntry entry;
            entry.identity = outcome.files.getReference (index);
            entry.source = stagedFiles.getReference (index);
            entry.target = destination.getChildFile (entry.identity.name);
            entry.temporary = destination.getChildFile (
                installerArtifactPrefix ("installing", instrument)
                + juce::Uuid().toString());
            entry.backup = destination.getChildFile (
                installerArtifactPrefix ("backup", instrument)
                + juce::Uuid().toString());
            entry.hadTarget = entry.target.existsAsFile();
            entries.add (std::move (entry));
        }

        CommitEntry receiptEntry;
        receiptEntry.identity.name = instrument.installedMarker().getFileName();
        receiptEntry.target = destination.getChildFile (
            receiptEntry.identity.name);
        receiptEntry.temporary = destination.getChildFile (
            installerArtifactPrefix ("installing", instrument)
            + juce::Uuid().toString());
        receiptEntry.backup = destination.getChildFile (
            installerArtifactPrefix ("backup", instrument)
            + juce::Uuid().toString());
        receiptEntry.hadTarget = receiptEntry.target.existsAsFile();
        entries.add (std::move (receiptEntry));

        auto result = writeCommitJournal (
            instrument, destination, entries, "committing");

        if (result.failed())
        {
            outcome.result = result;
            return outcome;
        }

        const auto failAndRollback = [&] (juce::Result failure)
        {
            const auto rollback = rollbackCommitEntries (entries, false);

            if (rollback.wasOk())
            {
                const auto temporaryCleanup =
                    removeNamespacedInstallerArtifacts (
                        instrument, destination, "installing");
                const auto marker = installInProgressMarkerIn (
                    instrument, destination);

                if (temporaryCleanup.failed())
                    failure = temporaryCleanup;
                else if (marker.existsAsFile() && ! marker.deleteFile())
                    failure = juce::Result::fail (
                        "Rollback concluido, mas o journal nao pode ser removido");
            }
            else
            {
                failure = juce::Result::fail (
                    failure.getErrorMessage() + ". "
                    + rollback.getErrorMessage());
            }

            outcome.result = failure;
            return outcome;
        };

        const int sampleEntryCount = stagedFiles.size();

        for (int index = 0; index < sampleEntryCount; ++index)
        {
            if (shouldCancel && shouldCancel())
                return failAndRollback (juce::Result::fail ("Cancelado"));

            auto& entry = entries.getReference (index);
            const auto prepare = prepareStagedFileForCommit (
                entry.source,
                entry.temporary,
                largestFileBytes,
                fault == nullptr || ! fault->forceCopyFallback,
                fault != nullptr ? fault->availableBytesOverride : -1);

            if (prepare.failed())
                return failAndRollback (juce::Result::fail (
                    "Falha ao preparar " + entry.identity.name + ": "
                    + prepare.getErrorMessage()));

            if (entry.hadTarget
                 && ! safeMoveFileWithRetry (entry.target, entry.backup))
                return failAndRollback (juce::Result::fail (
                    "Falha ao criar rollback de " + entry.identity.name));

            if (fault != nullptr && fault->failAfterBackupIndex == index)
                return failAndRollback (juce::Result::fail (
                    "Falha injetada depois do backup"));

            if (! safeMoveFileWithRetry (entry.temporary, entry.target))
                return failAndRollback (juce::Result::fail (
                    "Falha ao instalar " + entry.identity.name));

            entry.committed = true;

            if (fault != nullptr && fault->failAfterCommitIndex == index)
                return failAndRollback (juce::Result::fail (
                    "Falha injetada depois do commit"));

            if (progress)
                progress (index + 1, sampleEntryCount);
        }

        for (const auto& identity : outcome.files)
        {
            const auto installed = destination.getChildFile (identity.name);

            if (! installed.existsAsFile()
                 || installed.getSize() != identity.bytes)
                return failAndRollback (juce::Result::fail (
                    "Commit final divergente: " + identity.name));
        }

        auto receipt = std::make_unique<juce::DynamicObject>();
        receipt->setProperty ("schema_version", 2);
        receipt->setProperty ("id", instrument.id);
        receipt->setProperty ("name", instrument.name);
        receipt->setProperty ("sha256", instrument.sha256);
        receipt->setProperty ("download_bytes", instrument.downloadBytes);
        receipt->setProperty ("audio_file_count", outcome.audioFileCount);
        receipt->setProperty (
            "installed_at_ms",
            juce::Time::getCurrentTime().toMilliseconds());
        juce::Array<juce::var> serializedFiles;

        for (const auto& identity : outcome.files)
        {
            auto file = std::make_unique<juce::DynamicObject>();
            file->setProperty ("name", identity.name);
            file->setProperty ("bytes", identity.bytes);
            serializedFiles.add (juce::var (file.release()));
        }

        receipt->setProperty ("files", juce::var (serializedFiles));
        auto& receiptCommit = entries.getReference (sampleEntryCount);
        const auto receiptText = juce::JSON::toString (
            juce::var (receipt.release()), true);

        // Este arquivo ja e o temporario da transacao. Grava-lo diretamente
        // evita o segundo TemporaryFile interno de replaceWithText(), que pode
        // falhar em caminhos reais mais longos no Windows.
        {
            juce::FileOutputStream output (receiptCommit.temporary);

            if (output.failedToOpen())
                return failAndRollback (juce::Result::fail (
                    "Falha ao abrir recibo temporario: "
                    + output.getStatus().getErrorMessage()));

            const auto truncate = output.truncate();

            if (truncate.failed()
                 || ! output.write (receiptText.toRawUTF8(),
                                    receiptText.getNumBytesAsUTF8()))
                return failAndRollback (juce::Result::fail (
                    "Falha ao gravar recibo temporario"));

            output.flush();

            if (output.getStatus().failed())
                return failAndRollback (juce::Result::fail (
                    "Falha ao confirmar recibo temporario: "
                    + output.getStatus().getErrorMessage()));
        }

        receiptCommit.identity.bytes = receiptCommit.temporary.getSize();

        if (receiptCommit.hadTarget
             && ! safeMoveFileWithRetry (receiptCommit.target, receiptCommit.backup))
            return failAndRollback (juce::Result::fail (
                "Falha ao preservar recibo anterior"));

        if (! safeMoveFileWithRetry (receiptCommit.temporary, receiptCommit.target))
            return failAndRollback (juce::Result::fail (
                "Falha ao registrar o timbre instalado"));

        receiptCommit.committed = true;
        result = writeCommitJournal (
            instrument, destination, entries, "committed");

        if (result.failed())
            return failAndRollback (result);

        // Somente depois de samples + recibo + journal committed os backups
        // deixam de ser necessarios. Se a limpeza falhar, o journal committed
        // permite ao proximo inicio preservar os novos targets e apenas limpar.
        for (const auto& entry : entries)
            if (entry.backup.existsAsFile() && ! safeDeleteFileWithRetry (entry.backup))
            {
                outcome.result = juce::Result::fail (
                    "Instalado, mas o backup nao pode ser limpo: "
                    + entry.backup.getFileName());
                return outcome;
            }

        result = removeNamespacedInstallerArtifacts (
            instrument, destination, "installing");

        if (result.failed())
        {
            outcome.result = result;
            return outcome;
        }

        const auto marker = installInProgressMarkerIn (
            instrument, destination);

        if (marker.existsAsFile() && ! safeDeleteFileWithRetry (marker))
        {
            outcome.result = juce::Result::fail (
                "Instalado, mas o journal nao pode ser removido");
            return outcome;
        }

        outcome.result = juce::Result::ok();
        return outcome;
    }

    struct Catalog
    {
        bool valid = false;
        juce::String libraryId, libraryName, diagnostic;
        juce::Array<Instrument> instruments;
    };

    inline Catalog parseCatalogJson (const juce::String& jsonText)
    {
        Catalog catalog;
        const auto parsed = juce::JSON::parse (jsonText);

        if (auto* root = parsed.getDynamicObject())
        {
            catalog.libraryId = root->getProperty ("library_id").toString().trim();
            catalog.libraryName = root->getProperty ("library_name").toString().trim();

            if (auto* arr = root->getProperty ("instruments").getArray())
            {
                for (const auto& item : *arr)
                {
                    auto* o = item.getDynamicObject();

                    if (o == nullptr)
                        continue;

                    Instrument inst;
                    inst.id = o->getProperty ("id").toString().trim();
                    inst.name = o->getProperty ("name").toString().trim();
                    inst.rawLabel = o->getProperty ("raw_label").toString().trim();
                    inst.category = o->getProperty ("category").toString().trim();
                    inst.libraryId = o->getProperty ("library_id").toString().trim();
                    inst.installRelative =
                        o->getProperty ("install_relative").toString().trim();
                    inst.downloadUrl =
                        o->getProperty ("download_url").toString().trim();
                    inst.sha256 =
                        o->getProperty ("sha256").toString().trim().toLowerCase();
                    inst.fileCount = (int) o->getProperty ("file_count");
                    inst.downloadBytes =
                        (juce::int64) o->getProperty ("download_bytes");
                    inst.installedBytes =
                        (juce::int64) o->getProperty ("installed_bytes");

                    if (inst.installedBytes < 0
                         || inst.installedBytes
                                > ChurchDAWServices::maximumPackInstalledBytes)
                        inst.installedBytes = 0;

                    inst.included = (bool) o->getProperty ("included");
                    inst.ready = (bool) o->getProperty ("ready");

                    if (auto* partArr = o->getProperty ("parts").getArray())
                    {
                        for (const auto& partVal : *partArr)
                        {
                            auto* po = partVal.getDynamicObject();

                            if (po == nullptr)
                                continue;

                            Part part;
                            part.downloadUrl =
                                po->getProperty ("download_url").toString().trim();
                            part.sha256 = po->getProperty ("sha256")
                                              .toString()
                                              .trim()
                                              .toLowerCase();
                            part.downloadBytes =
                                (juce::int64) po->getProperty ("download_bytes");
                            inst.parts.add (std::move (part));
                        }
                    }

                    if (inst.id.isEmpty() || inst.name.isEmpty())
                        continue;

                    if (inst.libraryId.isEmpty())
                        inst.libraryId = "church-live";

                    if (inst.installRelative.isEmpty())
                        inst.installRelative = "Imported";

                    // ready pode vir false no catalogo se o ZIP ainda nao
                    // subiu; recalcula com o que temos.
                    if (! inst.ready)
                        inst.ready = inst.hasValidDownloadMetadata();

                    catalog.instruments.add (std::move (inst));
                }
            }

            catalog.valid = ! catalog.instruments.isEmpty();
        }

        if (! catalog.valid)
            catalog.diagnostic = "Catalogo de timbres vazio ou invalido";

        return catalog;
    }

    inline Catalog loadCachedCatalog()
    {
        const auto file = cachedCatalogFile();

        if (! file.existsAsFile())
            return {};

        return parseCatalogJson (file.loadFileAsString());
    }

    inline Catalog fetchRemoteCatalog()
    {
        auto tryUrl = [] (const juce::String& url) -> juce::String
        {
            juce::URL u (url);
            // httpStatusCode + timeout generoso: redes moveis e MITM de OEMs
            // as vezes demoram no primeiro handshake TLS.
            int status = 0;
            auto opts = juce::URL::InputStreamOptions (
                            juce::URL::ParameterHandling::inAddress)
                            .withConnectionTimeoutMs (30000)
                            // O host www responde 301. Nao interpretar o corpo
                            // HTML do redirect como se fosse o catalogo JSON.
                            .withNumRedirectsToFollow (0)
                            .withStatusCode (&status);
            if (auto stream = u.createInputStream (opts))
            {
                if (status != 0 && (status < 200 || status >= 300))
                {
                    juce::Logger::writeToLog (
                        "TIMBRE-CATALOG http=" + juce::String (status)
                        + " url=" + url);
                    return {};
                }

                juce::String body;
                if (! ChurchDAWServices::readStreamWithLimit (
                        *stream, 2 * 1024 * 1024, body))
                {
                    juce::Logger::writeToLog (
                        "TIMBRE-CATALOG resposta excedeu 2 MB url=" + url);
                    return {};
                }

                return body;
            }
            juce::Logger::writeToLog (
                "TIMBRE-CATALOG stream null url=" + url);
            return {};
        };

        juce::String body = tryUrl (catalogUrl);

        if (body.isEmpty())
            body = tryUrl (catalogUrlMirror);

        if (body.isEmpty())
        {
            Catalog failed;
            failed.diagnostic = "Sem conexao com o catalogo de timbres";
            juce::Logger::writeToLog ("TIMBRE-CATALOG: " + failed.diagnostic);
            return failed;
        }

        auto catalog = parseCatalogJson (body);

        if (catalog.valid)
        {
            cachedCatalogFile().getParentDirectory().createDirectory();
            cachedCatalogFile().replaceWithText (body);
            juce::Logger::writeToLog (
                "TIMBRE-CATALOG ok n="
                + juce::String (catalog.instruments.size()));
        }
        else
        {
            juce::Logger::writeToLog (
                "TIMBRE-CATALOG parse falhou bytes="
                + juce::String (body.length()));
        }

        return catalog;
    }

    //--------------------------------------------------------------------------
    // Instalador de UM timbre (ZIP unico ou varias partes <= 500 MB).
    //--------------------------------------------------------------------------
    class Installer final : public juce::Thread
    {
    public:
        using Progress = std::function<void (float, const juce::String&)>;
        using Finished = std::function<void (juce::Result)>;

        Installer (Instrument instrumentToInstall, Progress progressCallback,
                   Finished finishedCallback)
            : juce::Thread ("ChurchDAW timbre installer"),
              instrument (std::move (instrumentToInstall)),
              progress (std::move (progressCallback)),
              finished (std::move (finishedCallback))
        {
        }

        ~Installer() override
        {
            cancel();
            stopThread (-1);
        }

        bool begin()
        {
            return startThread (juce::Thread::Priority::normal);
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
            if (finished)
            {
                juce::MessageManager::callAsync (
                    [cb = finished, result] { cb (result); });
            }
        }

    private:
        juce::Result requireFreeSpace (const juce::File& volumeRoot,
                                       juce::int64 required,
                                       const juce::String& operation) const
        {
            const auto freeBytes = volumeRoot.getBytesFreeOnVolume();

            if (freeBytes <= 0)
                return juce::Result::fail (
                    "Nao foi possivel confirmar o espaco livre para "
                    + operation);

            if (required < 0 || freeBytes < required)
                return juce::Result::fail (
                    "Espaco insuficiente para " + operation
                    + ". Libere pelo menos "
                    + ChurchDAWServices::humanBytes (
                        juce::jmax ((juce::int64) 0, required - freeBytes))
                    + ".");

            return juce::Result::ok();
        }

        juce::Result preflightDownloadSpace (
            const juce::File& tempRoot,
            juce::int64 downloadBytesRemaining) const
        {
            juce::int64 required = 0;

            // Downloads, staging e biblioteca gerenciada ficam no mesmo volume.
            // A instalacao move staging -> destino e portanto precisa de apenas
            // bytes de rede restantes + uma copia expandida + reserva. Parciais
            // validos ja ocupam o volume e nao podem ser cobrados uma segunda vez.
            if (! calculateInstallerWorkspaceBytes (
                    downloadBytesRemaining,
                    instrument.installedBytes,
                    required))
                return juce::Result::fail (
                    "Tamanho da instalacao excede o limite suportado");

            return requireFreeSpace (tempRoot, required, "baixar este timbre");
        }

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
                Installer& ownerToUse,
                std::shared_ptr<juce::WebInputStream> streamToUse)
                : owner (ownerToUse), stream (std::move (streamToUse))
            {
                owner.setActiveDownloadStream (stream);
            }

            ~ActiveDownloadRegistration()
            {
                owner.clearActiveDownloadStream (stream);
            }

            Installer& owner;
            std::shared_ptr<juce::WebInputStream> stream;
        };

        static juce::String responseHeader (
            const juce::StringPairArray& headers,
            const juce::String& name)
        {
            const auto& keys = headers.getAllKeys();
            const auto& values = headers.getAllValues();

            for (int i = 0; i < keys.size(); ++i)
                if (keys[i].equalsIgnoreCase (name))
                    return values[i];

            return {};
        }

        static juce::int64 responseContentLength (
            const juce::StringPairArray& headers)
        {
            const auto value = responseHeader (headers, "Content-Length").trim();

            if (value.isEmpty() || ! value.containsOnly ("0123456789"))
                return -1;

            return value.getLargeIntValue();
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

            return expectedTotal > 0
                && startText.getLargeIntValue() == expectedStart
                && endText.getLargeIntValue() == expectedTotal - 1
                && totalText.getLargeIntValue() == expectedTotal;
        }

        juce::Result verifyFileHash (const juce::File& file,
                                     const juce::String& expectedSha,
                                     const juce::String& label)
        {
            if (threadShouldExit())
                return juce::Result::fail ("Cancelado");

            const auto actual = juce::SHA256 (file).toHexString().toLowerCase();

            if (threadShouldExit())
                return juce::Result::fail ("Cancelado");

            if (! actual.equalsIgnoreCase (expectedSha))
                return juce::Result::fail ("Hash invalido: " + label);

            return juce::Result::ok();
        }

        juce::Result downloadUrlToFile (const juce::String& url,
                                        const juce::File& dest,
                                        juce::int64 expectedBytes,
                                        const juce::String& expectedSha,
                                        float progressStart,
                                        float progressEnd,
                                        const juce::String& label)
        {
            if (! isOfficialTimbreDownloadUrl (url)
                 || expectedBytes <= 0 || ! isValidSha256 (expectedSha))
                return juce::Result::fail (
                    "Catalogo de download invalido: " + label);

            const auto parentResult = dest.getParentDirectory().createDirectory();

            if (parentResult.failed())
                return juce::Result::fail (
                    "Nao foi possivel preparar o download: " + label);

            juce::int64 resumeAt = dest.existsAsFile() ? dest.getSize() : 0;

            if (resumeAt < 0 || resumeAt > expectedBytes)
            {
                if (dest.exists() && ! dest.deleteFile())
                    return juce::Result::fail (
                        "Nao foi possivel reiniciar: " + label);

                resumeAt = 0;
            }

            if (resumeAt == expectedBytes)
            {
                report (progressEnd, "Verificando download retomado...");
                const auto existing = verifyFileHash (dest, expectedSha, label);

                if (existing.wasOk())
                    return existing;

                if (threadShouldExit())
                    return existing;

                if (! dest.deleteFile())
                    return juce::Result::fail (
                        "Download corrompido nao pode ser reiniciado: " + label);

                resumeAt = 0;
            }

            auto headers = juce::String ("Accept: application/zip\n");

            if (resumeAt > 0)
                headers += "Range: bytes=" + juce::String (resumeAt) + "-\n";

            report (progressStart,
                    resumeAt > 0 ? "Retomando " + label : "Conectando " + label);

            auto input = std::make_shared<juce::WebInputStream> (
                juce::URL (url), false);
            input->withConnectionTimeout (45000)
                 .withNumRedirectsToFollow (0)
                 .withExtraHeaders (headers);
            ActiveDownloadRegistration activeDownload (*this, input);

            if (threadShouldExit())
            {
                input->cancel();
                return juce::Result::fail ("Cancelado");
            }

            const bool connected = input->connect (nullptr);
            const int status = input->getStatusCode();
            const auto responseHeaders = input->getResponseHeaders();

            if (threadShouldExit())
                return juce::Result::fail ("Cancelado");

            if (! connected || input->isError()
                 || (status != 200 && status != 206))
            {
                if (status == 416 && resumeAt > 0)
                {
                    input->cancel();

                    if (! dest.deleteFile())
                        return juce::Result::fail (
                            "O servidor rejeitou a retomada e o parcial nao pode ser removido");
                }

                return juce::Result::fail (
                    "Falha ao baixar " + label + " (HTTP "
                    + juce::String (status) + ")");
            }

            if (status == 206
                 && ! contentRangeMatches (
                     responseHeader (responseHeaders, "Content-Range"),
                     resumeAt, expectedBytes))
            {
                input->cancel();
                return juce::Result::fail (
                    "Servidor respondeu uma faixa invalida: " + label);
            }

            if (status == 200 && resumeAt > 0)
            {
                resumeAt = 0;

                if (! dest.deleteFile())
                    return juce::Result::fail (
                        "Nao foi possivel reiniciar: " + label);
            }

            const auto contentLength = responseContentLength (responseHeaders);
            const auto expectedResponseBytes = expectedBytes - resumeAt;

            if (contentLength >= 0 && contentLength != expectedResponseBytes)
            {
                input->cancel();
                return juce::Result::fail (
                    "Tamanho HTTP divergente: " + label);
            }

            juce::int64 done = resumeAt;

            {
                juce::FileOutputStream output (dest);

                if (output.failedToOpen() || ! output.setPosition (resumeAt))
                    return juce::Result::fail (
                        "Sem espaco para baixar: " + label);

                const auto copyResult = ChurchDAWServices::copyExactDownloadBytes (
                    *input,
                    output,
                    resumeAt,
                    expectedBytes,
                    [this] { return threadShouldExit(); },
                    [this, expectedBytes, progressStart, progressEnd, &label]
                    (juce::int64 downloaded)
                    {
                        const auto ratio = (double) downloaded
                                         / (double) expectedBytes;
                        report (progressStart
                                    + (progressEnd - progressStart)
                                        * (float) juce::jlimit (0.0, 1.0, ratio),
                                label);
                    },
                    done);

                if (copyResult.wasOk())
                    input->cancel();

                output.flush();

                if (output.getStatus().failed())
                    return juce::Result::fail (
                        "Erro ao confirmar no disco: " + label);

                if (threadShouldExit())
                    return juce::Result::fail ("Cancelado");

                if (copyResult.failed())
                    return juce::Result::fail (
                        "Download incompleto: " + label + ". "
                        + copyResult.getErrorMessage());
            }

            if (done != expectedBytes || dest.getSize() != expectedBytes)
                return juce::Result::fail (
                    "Tamanho incorreto: " + label);

            report (progressEnd, "Verificando " + label);
            const auto hashResult = verifyFileHash (dest, expectedSha, label);

            if (hashResult.failed() && ! threadShouldExit())
                dest.deleteFile();

            return hashResult;
        }

        juce::File partFileFor (const juce::File& tempRoot, int index) const
        {
            return tempRoot.getChildFile (
                instrument.id + "-p"
                + juce::String (index + 1).paddedLeft ('0', 2)
                + ".download.part");
        }

        juce::Result prepareMultipartAssembly (
            const juce::File& tempRoot,
            const juce::File& zipFile,
            int& completedParts,
            juce::int64& assembledBytes)
        {
            const auto existingBytes = zipFile.existsAsFile()
                                         ? zipFile.getSize() : 0;
            completedParts = 0;
            assembledBytes = 0;

            for (int i = 0; i < instrument.parts.size(); ++i)
            {
                juce::int64 nextBoundary = 0;

                if (! ChurchDAWServices::checkedAddNonNegative (
                        assembledBytes,
                        instrument.parts.getReference (i).downloadBytes,
                        nextBoundary))
                    return juce::Result::fail (
                        "Tamanho multipart excedeu o limite");

                if (existingBytes < nextBoundary)
                    break;

                assembledBytes = nextBoundary;
                ++completedParts;
            }

            // Se o processo caiu no meio da concatenacao, volta somente ao
            // ultimo limite confirmado. O .download.part completo continua
            // disponivel e nao precisa ser baixado de novo.
            if (existingBytes != assembledBytes)
            {
                juce::FileOutputStream output (zipFile);

                if (output.failedToOpen()
                     || ! output.setPosition (assembledBytes))
                    return juce::Result::fail (
                        "Nao foi possivel retomar a montagem do ZIP");

                const auto truncateResult = output.truncate();

                if (truncateResult.failed())
                    return juce::Result::fail (
                        "Nao foi possivel reparar a montagem interrompida");
            }

            for (int i = 0; i < completedParts; ++i)
                partFileFor (tempRoot, i).deleteFile();

            return juce::Result::ok();
        }

        juce::Result appendPartToArchive (
            const juce::File& partFile,
            const juce::File& zipFile,
            juce::int64 assembledBytes,
            const Part& part,
            int partIndex,
            int partCount)
        {
            juce::FileInputStream input (partFile);

            if (input.failedToOpen() || partFile.getSize() != part.downloadBytes)
                return juce::Result::fail (
                    "Parte validada nao pode ser aberta para montagem");

            juce::int64 copied = 0;

            {
                juce::FileOutputStream output (zipFile);

                if (output.failedToOpen()
                     || ! output.setPosition (assembledBytes))
                    return juce::Result::fail (
                        "Sem espaco para montar o timbre");

                const auto copyResult = ChurchDAWServices::copyExactDownloadBytes (
                    input,
                    output,
                    0,
                    part.downloadBytes,
                    [this] { return threadShouldExit(); },
                    [this, partIndex, partCount] (juce::int64)
                    {
                        report (0.73f,
                                "Montando parte "
                                    + juce::String (partIndex + 1) + "/"
                                    + juce::String (partCount) + "...");
                    },
                    copied);

                output.flush();

                if (output.getStatus().failed())
                    return juce::Result::fail (
                        "Falha ao confirmar a montagem no disco");

                if (copyResult.failed())
                    return copyResult;
            }

            juce::int64 expectedSize = 0;

            if (! ChurchDAWServices::checkedAddNonNegative (
                    assembledBytes, part.downloadBytes, expectedSize)
                 || zipFile.getSize() != expectedSize)
                return juce::Result::fail (
                    "Montagem incompleta do timbre");

            return juce::Result::ok();
        }

        static bool isSafeArchivePath (juce::String path)
        {
            path = path.replaceCharacter ('\\', '/');

            if (path.isEmpty() || path.length() > 640
                 || path.startsWithChar ('/') || path.containsChar (':')
                 || path.containsAnyOf ("\r\n"))
                return false;

            juce::StringArray segments;
            segments.addTokens (path, "/", juce::String());

            for (const auto& segment : segments)
                if (segment == "." || segment == "..")
                    return false;

            return true;
        }

        static bool isSupportedPayloadFile (const juce::String& fileName)
        {
            const auto extension = extensionFromPathText (fileName);
            return extension == ".flac" || extension == ".wav"
                || extension == ".aif" || extension == ".aiff"
                || extension == ".json" || extension == ".sfz"
                || extension == ".xml" || extension == ".txt";
        }

        static bool isAudioPayloadFile (const juce::String& fileName)
        {
            const auto extension = extensionFromPathText (fileName);
            return extension == ".flac" || extension == ".wav"
                || extension == ".aif" || extension == ".aiff";
        }

        juce::Result extractArchive (const juce::File& zipFile,
                                     const juce::File& staging)
        {
            struct EntryToExtract
            {
                int index = -1;
                juce::String fileName;
                juce::int64 bytes = 0;
            };

            if (staging.exists() && ! staging.deleteRecursively())
            {
                for (int i = 0; staging.exists() && i < 10; ++i)
                {
                    juce::Thread::sleep (80);
                    staging.deleteRecursively();
                }

                if (staging.exists())
                    return juce::Result::fail (
                        "Nao foi possivel limpar a extracao anterior");
            }

            const auto createResult = staging.createDirectory();

            if (createResult.failed())
                return juce::Result::fail (
                    "Nao foi possivel preparar a extracao");

            juce::ZipFile archive (zipFile);

            if (archive.getNumEntries() <= 0
                 || archive.getNumEntries()
                        > ChurchDAWServices::maximumLibraryEntryCount)
                return juce::Result::fail ("ZIP vazio ou invalido");

            juce::Array<EntryToExtract> entries;
            juce::StringArray names;
            juce::int64 totalExpandedBytes = 0;
            int audioFiles = 0;

            for (int i = 0; i < archive.getNumEntries(); ++i)
            {
                const auto* entry = archive.getEntry (i);

                if (entry == nullptr)
                    return juce::Result::fail ("Entrada ZIP invalida");

                auto path = entry->filename.replaceCharacter ('\\', '/');

                if (! isSafeArchivePath (path))
                    return juce::Result::fail (
                        "ZIP contem um caminho inseguro");

                if (path.endsWithChar ('/') || ! isSupportedPayloadFile (path))
                    continue;

                const auto slash = path.lastIndexOfChar ('/');
                const auto fileName = path.substring (slash + 1);
                const auto key = fileName.toLowerCase();

                if (fileName.isEmpty() || fileName.length() > 240
                     || names.contains (key))
                    return juce::Result::fail (
                        "ZIP contem nomes duplicados ou invalidos");

                if (entry->uncompressedSize < 0
                     || entry->uncompressedSize
                            > ChurchDAWServices::maximumLibraryEntryBytes
                     || ! ChurchDAWServices::checkedAddNonNegative (
                         totalExpandedBytes, entry->uncompressedSize,
                         totalExpandedBytes)
                     || totalExpandedBytes
                            > ChurchDAWServices::maximumPackInstalledBytes)
                    return juce::Result::fail (
                        "ZIP excede os limites de extracao");

                EntryToExtract item;
                item.index = i;
                item.fileName = fileName;
                item.bytes = entry->uncompressedSize;
                names.add (key);
                entries.add (std::move (item));

                if (isAudioPayloadFile (fileName))
                    ++audioFiles;
            }

            if (entries.isEmpty() || audioFiles <= 0)
                return juce::Result::fail (
                    "Nenhum sample no pacote do timbre");

            juce::int64 extractionRequired = 0;

            // O ZIP ja ocupa o disco e esta descontado de getBytesFreeOnVolume.
            // Como staging e destino compartilham o volume, o commit move cada
            // arquivo e nao duplica o payload expandido inteiro.
            if (! ChurchDAWServices::checkedAddNonNegative (
                    totalExpandedBytes,
                    ChurchDAWServices::installerStorageReserve,
                    extractionRequired))
                return juce::Result::fail (
                    "Tamanho expandido excede o limite suportado");

            const auto spaceResult = requireFreeSpace (
                staging.getParentDirectory(), extractionRequired,
                "extrair e instalar este timbre");

            if (spaceResult.failed())
                return spaceResult;

            juce::int64 completedBytes = 0;

            for (const auto& item : entries)
            {
                if (threadShouldExit())
                    return juce::Result::fail ("Cancelado");

                auto input = std::unique_ptr<juce::InputStream> (
                    archive.createStreamForEntry (item.index));
                const auto outputFile = staging.getChildFile (item.fileName);
                juce::FileOutputStream output (outputFile);

                if (input == nullptr || output.failedToOpen())
                    return juce::Result::fail (
                        "Falha ao abrir " + item.fileName + " na extracao");

                const auto truncateResult = output.truncate();

                if (truncateResult.failed())
                    return juce::Result::fail (
                        "Falha ao preparar " + item.fileName);

                juce::int64 extracted = 0;
                juce::Result copyResult = juce::Result::ok();

                if (item.bytes > 0)
                    copyResult = ChurchDAWServices::copyExactDownloadBytes (
                        *input,
                        output,
                        0,
                        item.bytes,
                        [this] { return threadShouldExit(); },
                        [this, completedBytes, totalExpandedBytes]
                        (juce::int64 current)
                        {
                            const auto ratio = totalExpandedBytes > 0
                                ? (double) (completedBytes + current)
                                    / (double) totalExpandedBytes
                                : 1.0;
                            report (0.80f + 0.12f * (float) ratio,
                                    "Extraindo...");
                        },
                        extracted);

                output.flush();

                if (copyResult.failed() || output.getStatus().failed()
                     || outputFile.getSize() != item.bytes)
                    return juce::Result::fail (
                        "Falha ao extrair " + item.fileName + ": "
                        + (copyResult.failed()
                               ? copyResult.getErrorMessage()
                               : juce::String ("tamanho divergente")));

                completedBytes += item.bytes;
            }

            report (0.92f, "Extracao concluida");
            return juce::Result::ok();
        }

        juce::Result installExtractedFiles (const juce::File& staging)
        {
            const auto dest = instrument.installFolder();
            const auto outcome = commitStagingTransaction (
                instrument,
                staging,
                dest,
                [this] { return threadShouldExit(); },
                [this] (int committed, int total)
                {
                    report (0.92f
                                + 0.07f * (float) committed / (float) total,
                            "Instalando " + juce::String (committed) + "/"
                                + juce::String (total) + "...");
                });
            return outcome.result;
        }

        juce::Result install()
        {
            if (! instrument.canDownload())
                return juce::Result::fail (
                    "Timbre ainda nao disponivel para download individual");

            const auto recoveryResult = recoverInterruptedInstall (
                instrument, instrument.installFolder());

            if (recoveryResult.failed())
                return recoveryResult;

            if (instrument.isInstalledLocally())
            {
                report (1.0f, "Pronto");
                return juce::Result::ok();
            }

            report (0.02f, "Preparando download...");

            const auto tempRoot = AppPaths::managedDataFolder()
                                      .getChildFile ("Downloads")
                                      .getChildFile ("timbres");
            const auto tempResult = tempRoot.createDirectory();

            if (tempResult.failed())
                return juce::Result::fail (
                    "Nao foi possivel criar a pasta de downloads");

            const auto zipFile =
                tempRoot.getChildFile (instrument.id + "-download.zip");
            const auto staging =
                tempRoot.getChildFile (instrument.id + "-staging");

            // A extracao nao e retomavel. Remover staging antigo antes do
            // preflight impede cobrar espaco livre com bytes obsoletos ocupando
            // o mesmo volume e deixa explicito que apenas rede/multipart retoma.
            if (staging.exists() && ! staging.deleteRecursively())
                return juce::Result::fail (
                    "Nao foi possivel limpar a extracao interrompida");

            const auto bytesRemaining = remainingDownloadBytes (
                instrument, tempRoot, zipFile);

            const auto spaceResult = preflightDownloadSpace (
                tempRoot, bytesRemaining);

            if (spaceResult.failed())
                return spaceResult;

            if (! instrument.parts.isEmpty())
            {
                int completedParts = 0;
                juce::int64 assembled = 0;
                auto result = prepareMultipartAssembly (
                    tempRoot, zipFile, completedParts, assembled);

                if (result.failed())
                    return result;

                const int partCount = instrument.parts.size();

                for (int i = completedParts; i < partCount; ++i)
                {
                    if (threadShouldExit())
                        return juce::Result::fail ("Cancelado");

                    const auto& part = instrument.parts.getReference (i);
                    juce::int64 nextBoundary = 0;

                    if (! ChurchDAWServices::checkedAddNonNegative (
                            assembled, part.downloadBytes, nextBoundary))
                        return juce::Result::fail (
                            "Tamanho multipart excedeu o limite");

                    const auto p0 = 0.05f + 0.67f
                        * (float) ((double) assembled
                                   / (double) instrument.downloadBytes);
                    const auto p1 = 0.05f + 0.67f
                        * (float) ((double) nextBoundary
                                   / (double) instrument.downloadBytes);
                    const auto partFile = partFileFor (tempRoot, i);
                    const auto label = "Parte " + juce::String (i + 1)
                                     + "/" + juce::String (partCount)
                                     + " - " + instrument.name;

                    result = downloadUrlToFile (
                        part.downloadUrl, partFile, part.downloadBytes,
                        part.sha256, p0, p1, label);

                    if (result.failed())
                        return result;

                    result = appendPartToArchive (
                        partFile, zipFile, assembled, part, i, partCount);

                    if (result.failed())
                        return result;

                    assembled = nextBoundary;

                    if (! safeDeleteFileWithRetry (partFile))
                        juce::Logger::writeToLog (
                            "TIMBRE-DOWNLOAD: parte validada sera limpa depois "
                            + partFile.getFullPathName());
                }

                if (assembled != instrument.downloadBytes
                     || zipFile.getSize() != instrument.downloadBytes)
                    return juce::Result::fail (
                        "Montagem incompleta do timbre");
            }
            else
            {
                const auto single = downloadUrlToFile (
                    instrument.downloadUrl,
                    zipFile,
                    instrument.downloadBytes,
                    instrument.sha256,
                    0.05f,
                    0.72f,
                    "Baixando " + instrument.name);

                if (single.failed())
                    return single;
            }

            report (0.76f, "Verificando arquivo completo...");
            const auto hashResult = verifyFileHash (
                zipFile, instrument.sha256, instrument.name);

            if (hashResult.failed())
            {
                if (! threadShouldExit())
                    safeDeleteFileWithRetry (zipFile);

                return hashResult;
            }

            report (0.80f, "Extraindo...");
            auto result = extractArchive (zipFile, staging);

            if (result.failed())
                return result;

            report (0.92f, "Instalando...");
            result = installExtractedFiles (staging);

            if (result.failed())
                return result;

            staging.deleteRecursively();
            safeDeleteFileWithRetry (zipFile);

            for (int i = 0; i < instrument.parts.size(); ++i)
                safeDeleteFileWithRetry (partFileFor (tempRoot, i));

            report (1.0f, "Pronto");
            return juce::Result::ok();
        }

        juce::Result installLegacyUnused()
        {
            if (! instrument.canDownload())
                return juce::Result::fail ("Timbre ainda nao disponivel para download individual");

            report (0.02f, "Conectando...");

            const auto tempRoot = AppPaths::managedDataFolder()
                                      .getChildFile ("Downloads")
                                      .getChildFile ("timbres");
            tempRoot.createDirectory();

            const auto zipFile =
                tempRoot.getChildFile (instrument.id + "-download.zip");
            const auto staging =
                tempRoot.getChildFile (instrument.id + "-staging");

            if (staging.isDirectory())
                staging.deleteRecursively();

            zipFile.deleteFile();

            // Download: ZIP unico ou N partes concatenadas (limite host 500 MB)
            if (! instrument.parts.isEmpty())
            {
                auto out = zipFile.createOutputStream();

                if (out == nullptr)
                    return juce::Result::fail ("Sem espaco para montar o timbre");

                const int nParts = instrument.parts.size();
                juce::int64 assembled = 0;

                for (int i = 0; i < nParts; ++i)
                {
                    if (threadShouldExit())
                    {
                        out.reset();
                        zipFile.deleteFile();
                        return juce::Result::fail ("Cancelado");
                    }

                    const auto& part = instrument.parts.getReference (i);
                    const auto partFile =
                        tempRoot.getChildFile (instrument.id + "-p"
                                               + juce::String (i + 1).paddedLeft ('0', 2)
                                               + ".bin");

                    const float p0 = 0.05f + 0.70f * ((float) i / (float) nParts);
                    const float p1 = 0.05f + 0.70f * ((float) (i + 1) / (float) nParts);

                    auto partResult = downloadUrlToFile (
                        part.downloadUrl,
                        partFile,
                        part.downloadBytes,
                        part.sha256,
                        p0,
                        p1,
                        "Parte " + juce::String (i + 1) + "/"
                            + juce::String (nParts)
                            + juce::String::fromUTF8 (u8" · ")
                            + instrument.name);

                    if (partResult.failed())
                    {
                        out.reset();
                        zipFile.deleteFile();
                        partFile.deleteFile();
                        return partResult;
                    }

                    // Concatena no ZIP final
                    if (auto in = partFile.createInputStream())
                    {
                        juce::MemoryBlock buffer (256 * 1024);
                        while (! threadShouldExit())
                        {
                            const auto n = in->read (buffer.getData(),
                                                     (int) buffer.getSize());
                            if (n <= 0)
                                break;

                            if (! out->write (buffer.getData(), (size_t) n))
                            {
                                out.reset();
                                zipFile.deleteFile();
                                partFile.deleteFile();
                                return juce::Result::fail ("Falha ao montar o ZIP");
                            }

                            assembled += n;
                        }
                    }

                    partFile.deleteFile();
                }

                out.reset();

                if (assembled != instrument.downloadBytes)
                {
                    zipFile.deleteFile();
                    return juce::Result::fail ("Montagem incompleta do timbre");
                }
            }
            else
            {
                auto single = downloadUrlToFile (
                    instrument.downloadUrl,
                    zipFile,
                    instrument.downloadBytes,
                    {}, // hash verificado depois no arquivo final
                    0.05f,
                    0.75f,
                    "Baixando " + instrument.name + "...");

                if (single.failed())
                    return single;
            }

            report (0.78f, "Verificando...");

            {
                const auto hash =
                    juce::SHA256 (zipFile).toHexString().toLowerCase();

                if (hash != instrument.sha256)
                {
                    zipFile.deleteFile();
                    return juce::Result::fail ("Arquivo corrompido (hash invalido)");
                }
            }

            report (0.85f, "Extraindo...");

            {
                juce::ZipFile zip (zipFile);

                if (zip.getNumEntries() <= 0)
                {
                    zipFile.deleteFile();
                    return juce::Result::fail ("ZIP vazio");
                }

                staging.createDirectory();
                const auto res = zip.uncompressTo (staging);

                if (res.failed())
                {
                    staging.deleteRecursively();
                    zipFile.deleteFile();
                    return juce::Result::fail ("Falha ao extrair: " + res.getErrorMessage());
                }
            }

            report (0.93f, "Instalando...");

            const auto dest = instrument.installFolder();
            dest.createDirectory();

            // Copia samples para o destino final (Imported ou pasta do instrumento)
            int copied = 0;
            for (const auto& entry : juce::RangedDirectoryIterator (
                     staging, true, "*.flac;*.wav;*.aif;*.aiff;*.json;*.sfz;*.xml;*.txt",
                     juce::File::findFiles))
            {
                if (threadShouldExit())
                    break;

                const auto src = entry.getFile();
                const auto target = dest.getChildFile (src.getFileName());

                if (! src.copyFileTo (target))
                {
                    // tenta sobrescrever
                    target.deleteFile();
                    if (! src.copyFileTo (target))
                        return juce::Result::fail ("Falha ao instalar "
                                                   + src.getFileName());
                }

                ++copied;
            }

            staging.deleteRecursively();
            zipFile.deleteFile();

            if (copied <= 0)
                return juce::Result::fail ("Nenhum sample no pacote do timbre");

            report (1.0f, "Pronto");
            return juce::Result::ok();
        }

        void report (float p, const juce::String& text)
        {
            if (! progress)
                return;

            const auto nowMs = juce::Time::getMillisecondCounterHiRes();
            const bool terminal = p >= 0.9999f;
            const bool meaningfulStep = lastReportedProgress < 0.0f
                || std::abs (p - lastReportedProgress) >= 0.002f;
            const bool enoughTime = nowMs - lastReportedProgressMs >= 200.0;

            if (! terminal && ! meaningfulStep && ! enoughTime)
                return;

            lastReportedProgress = p;
            lastReportedProgressMs = nowMs;
            juce::MessageManager::callAsync (
                [cb = progress, p, text] { cb (p, text); });
        }

        Instrument instrument;
        Progress progress;
        Finished finished;
        std::mutex downloadStreamMutex;
        std::shared_ptr<juce::WebInputStream> activeDownloadStream;
        float lastReportedProgress = -1.0f;
        double lastReportedProgressMs = 0.0;
    };
}
