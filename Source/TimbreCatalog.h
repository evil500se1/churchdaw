#pragma once

#include "AppPaths.h"
#include "ChurchDAWServices.h"

#include <functional>
#include <memory>

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
        "https://www.churchdaw.com/app/android/timbres/timbre-catalog-v1.json";

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
            return downloadUrl.startsWithIgnoreCase ("https://")
                && sha256.length() == 64
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
        bool included = true;
        bool ready = false;

        bool canDownload() const noexcept
        {
            if (! ready || ! included || sha256.length() != 64
                 || downloadBytes <= 0
                 || downloadBytes >= (juce::int64) 8 * 1024 * 1024 * 1024)
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

                    sum += p.downloadBytes;
                }

                return sum == downloadBytes;
            }

            return downloadUrl.startsWithIgnoreCase ("https://");
        }

        juce::File installFolder() const
        {
            return AppPaths::managedLibrariesFolder()
                .getChildFile (libraryId.isNotEmpty() ? libraryId : "church-live")
                .getChildFile (installRelative.isNotEmpty() ? installRelative
                                                            : "Imported");
        }

        // Heuristica: se ha samples locais com o prefixo do instrumento, ja
        // esta disponivel (nao precisa baixar de novo).
        bool isInstalledLocally() const
        {
            const auto folder = installFolder();

            if (! folder.isDirectory())
                return false;

            const auto needle = rawLabel.isNotEmpty() ? rawLabel : name;

            if (needle.isEmpty())
                return false;

            int hits = 0;
            const int need = juce::jmax (3, fileCount > 0 ? fileCount / 4 : 8);

            for (const auto& entry : juce::RangedDirectoryIterator (
                     folder, false, "*", juce::File::findFiles))
            {
                const auto stem = entry.getFile().getFileNameWithoutExtension();

                if (stem.startsWithIgnoreCase (needle)
                     || stem.containsIgnoreCase (needle))
                {
                    if (++hits >= need)
                        return true;
                }
            }

            // Pastas dedicadas (B3 Fabio, LEAD SAW, ...)
            if (installRelative.equalsIgnoreCase (rawLabel)
                 || installRelative.equalsIgnoreCase (name))
            {
                for (const auto& entry : juce::RangedDirectoryIterator (
                         folder, true, "*.flac;*.wav;*.aif;*.aiff",
                         juce::File::findFiles))
                {
                    juce::ignoreUnused (entry);
                    if (++hits >= need)
                        return true;
                }
            }

            return false;
        }
    };

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
                        inst.ready = inst.canDownload();

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
                            .withNumRedirectsToFollow (5)
                            .withStatusCode (&status);
            if (auto stream = u.createInputStream (opts))
            {
                auto body = stream->readEntireStreamAsString();
                if (status != 0 && status != 200)
                    juce::Logger::writeToLog (
                        "TIMBRE-CATALOG http=" + juce::String (status)
                        + " url=" + url);
                return body;
            }
            juce::Logger::writeToLog (
                "TIMBRE-CATALOG stream null url=" + url);
            return {};
        };

        juce::String body = tryUrl (catalogUrl);

        if (body.isEmpty())
            body = tryUrl (catalogUrlMirror);

        // Fallback com www / http se o host principal falhar em algum aparelho.
        if (body.isEmpty())
            body = tryUrl (
                "https://www.churchdaw.com/app/android/timbres/timbre-catalog-v1.json");

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
            signalThreadShouldExit();
            stopThread (120000);
        }

        void begin() { startThread (juce::Thread::Priority::normal); }

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
        juce::Result downloadUrlToFile (const juce::String& url,
                                        const juce::File& dest,
                                        juce::int64 expectedBytes,
                                        const juce::String& expectedSha,
                                        float progressStart,
                                        float progressEnd,
                                        const juce::String& label)
        {
            dest.deleteFile();

            juce::URL u (url);
            auto opts = juce::URL::InputStreamOptions (
                            juce::URL::ParameterHandling::inAddress)
                            .withConnectionTimeoutMs (45000)
                            .withNumRedirectsToFollow (5);

            auto in = u.createInputStream (opts);

            if (in == nullptr)
                return juce::Result::fail ("Falha ao baixar: " + label);

            auto out = dest.createOutputStream();

            if (out == nullptr)
                return juce::Result::fail ("Sem espaco para baixar: " + label);

            juce::int64 total = expectedBytes > 0 ? expectedBytes : 1;
            juce::MemoryBlock buffer (64 * 1024);
            juce::int64 done = 0;

            while (! threadShouldExit())
            {
                const auto n = in->read (buffer.getData(), (int) buffer.getSize());

                if (n <= 0)
                    break;

                if (! out->write (buffer.getData(), (size_t) n))
                    return juce::Result::fail ("Erro ao gravar: " + label);

                done += n;
                const double safeTotal = total > 0 ? (double) total : 1.0;
                const double r = (double) done / safeTotal;
                const float p = progressStart
                              + (progressEnd - progressStart)
                                    * (float) juce::jlimit (0.0, 1.0, r);
                report (p, label);
            }

            out.reset();

            if (threadShouldExit())
            {
                dest.deleteFile();
                return juce::Result::fail ("Cancelado");
            }

            if (done < 1024)
            {
                dest.deleteFile();
                return juce::Result::fail ("Download incompleto: " + label);
            }

            if (expectedBytes > 0 && done != expectedBytes)
            {
                dest.deleteFile();
                return juce::Result::fail ("Tamanho incorreto: " + label);
            }

            if (expectedSha.isNotEmpty())
            {
                const auto hash = juce::SHA256 (dest).toHexString().toLowerCase();

                if (hash != expectedSha)
                {
                    dest.deleteFile();
                    return juce::Result::fail ("Hash invalido: " + label);
                }
            }

            return juce::Result::ok();
        }

        juce::Result install()
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
                            + juce::String (nParts) + " · " + instrument.name);

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
                     staging, true, "*.flac;*.wav;*.aif;*.aiff",
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
            if (progress)
            {
                juce::MessageManager::callAsync (
                    [cb = progress, p, text] { cb (p, text); });
            }
        }

        Instrument instrument;
        Progress progress;
        Finished finished;
    };
}
