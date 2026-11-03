#include <iostream>

#include <juce_core/juce_core.h>

#include "ChurchDAWServices.h"

namespace
{
int fail (const juce::String& message)
{
    std::cerr << message << std::endl;
    return 1;
}
}

int main (int argc, char** argv)
{
    if (argc != 4)
        return fail (
            "uso: probe <update-policy-v2.json> <manifest.json> <unix-time>");

    const juce::String officialApkUrl =
        "https://churchdaw.com/app/android/ChurchDAW-1.6.24-r2-signed.apk";
    if (! ChurchDAWServices::isSafeWebsiteUpdateUrl (officialApkUrl))
        return fail ("URL oficial do APK foi rejeitada");
    if (ChurchDAWServices::isSafeWebsiteUpdateUrl (
            "https://churchdaw.com/app/android/ChurchDAW-1.6.24-r2.apk")
        || ChurchDAWServices::isSafeWebsiteUpdateUrl (
            officialApkUrl + "?arquivo=outro")
        || ChurchDAWServices::isSafeWebsiteUpdateUrl (
            officialApkUrl + "\r\nhttps://example.com/arquivo.apk")
        || ChurchDAWServices::isSafeWebsiteUpdateUrl (
            "https://example.com/app/android/ChurchDAW-1.5.0-signed.apk"))
        return fail ("filtro de URL aceitou query, CRLF ou host externo");

    if (! ChurchDAWServices::isSafeAndroidUpdateUrl (
            ChurchDAWServices::defaultPlayStoreUrl))
        return fail ("ficha oficial da Play Store foi rejeitada");

    const juce::String publicBasePartUrl =
        "https://churchdaw.com/app/android/packs/"
        "church-live-v1-segment-01.zip";
    if (! ChurchDAWServices::isOfficialLibraryDownloadUrl (
            publicBasePartUrl)
        || ChurchDAWServices::isOfficialLibraryDownloadUrl (
            "https://churchdaw.com/app/android/packs/"
            "church-live-v1-segment-31.zip")
        || ChurchDAWServices::isOfficialLibraryDownloadUrl (
            "https://churchdaw.com/app/android/packs/"
            "worship-keys-paid.zip")
        || ChurchDAWServices::isOfficialLibraryDownloadUrl (
            "https://example.com/app/android/packs/"
            "church-live-v1-segment-01.zip"))
        return fail (
            "filtro da biblioteca publica aceitou parte fora da allowlist");

    ChurchDAWServices::UpdatePolicy futurePolicy;
    futurePolicy.valid = true;
    futurePolicy.cryptographicallyVerified = true;
    futurePolicy.latestVersionCode =
        ChurchDAWServices::currentVersionCode + 1;
    futurePolicy.playStoreUrl =
        "https://churchdaw.com/app/android/"
        "ChurchDAW-9.9.9-r1-signed.apk";

    if (ChurchDAWServices::requiredUpdateDownloadUrl (
            futurePolicy, futurePolicy.latestVersionCode)
            != futurePolicy.playStoreUrl)
        return fail ("gate descartou URL assinada da versao futura");

    futurePolicy.playStoreUrl =
        "https://example.com/app/android/ChurchDAW-9.9.9-signed.apk";
    if (ChurchDAWServices::requiredUpdateDownloadUrl (
            futurePolicy, futurePolicy.latestVersionCode).isNotEmpty())
        return fail ("gate aceitou URL externa como download obrigatorio");

    using UpdateDecision =
        ChurchDAWServices::AndroidUpdatePromptDecision;

    futurePolicy.minimumVersionCode =
        ChurchDAWServices::currentVersionCode - 1;
    futurePolicy.playStoreUrl =
        "https://churchdaw.com/app/android/"
        "ChurchDAW-9.9.9-r1-signed.apk";

    if (ChurchDAWServices::decideAndroidUpdatePrompt (
            futurePolicy, ChurchDAWServices::currentVersionCode,
            futurePolicy.minimumVersionCode)
        != UpdateDecision::optionalUpdate)
        return fail ("policy assinada futura nao gerou oferta opcional");

    auto candidate = futurePolicy;
    candidate.cryptographicallyVerified = false;
    if (ChurchDAWServices::decideAndroidUpdatePrompt (
            candidate, ChurchDAWServices::currentVersionCode,
            candidate.minimumVersionCode)
        != UpdateDecision::noPrompt)
        return fail ("policy sem assinatura gerou oferta opcional");

    candidate = futurePolicy;
    candidate.valid = false;
    if (ChurchDAWServices::decideAndroidUpdatePrompt (
            candidate, ChurchDAWServices::currentVersionCode,
            candidate.minimumVersionCode)
        != UpdateDecision::noPrompt)
        return fail ("falha de rede/cache invalido bloqueou a inicializacao");

    candidate = futurePolicy;
    candidate.playStoreUrl =
        "https://example.com/app/android/ChurchDAW-9.9.9-signed.apk";
    if (ChurchDAWServices::decideAndroidUpdatePrompt (
            candidate, ChurchDAWServices::currentVersionCode,
            candidate.minimumVersionCode)
        != UpdateDecision::noPrompt)
        return fail ("URL externa gerou oferta opcional");

    candidate = futurePolicy;
    candidate.latestVersionCode = ChurchDAWServices::currentVersionCode;
    if (ChurchDAWServices::decideAndroidUpdatePrompt (
            candidate, ChurchDAWServices::currentVersionCode,
            candidate.minimumVersionCode)
        != UpdateDecision::noPrompt)
        return fail ("versao atual gerou oferta opcional");

    candidate = futurePolicy;
    candidate.minimumVersionCode =
        ChurchDAWServices::currentVersionCode + 1;
    if (ChurchDAWServices::decideAndroidUpdatePrompt (
            candidate, ChurchDAWServices::currentVersionCode,
            candidate.minimumVersionCode)
        != UpdateDecision::requiredUpdate)
        return fail ("versao abaixo do piso nao preservou gate obrigatorio");

    candidate.valid = false;
    if (ChurchDAWServices::decideAndroidUpdatePrompt (
            candidate, ChurchDAWServices::currentVersionCode,
            candidate.minimumVersionCode)
        != UpdateDecision::requiredUpdate)
        return fail ("falha de rede apagou piso obrigatorio ja confirmado");

    candidate = futurePolicy;
    candidate.minimumVersionCode =
        ChurchDAWServices::currentVersionCode + 1;
    if (ChurchDAWServices::decideAndroidUpdatePrompt (
            candidate, ChurchDAWServices::currentVersionCode, 1)
        != UpdateDecision::noPrompt)
        return fail ("policy com minimo futuro nao armado virou oferta opcional");

    const auto policyFile = juce::File (juce::String::fromUTF8 (argv[1]));
    const auto manifestFile = juce::File (juce::String::fromUTF8 (argv[2]));
    const auto trustedNow = juce::String::fromUTF8 (argv[3]).getLargeIntValue();

    if (! policyFile.existsAsFile() || ! manifestFile.existsAsFile()
        || trustedNow <= 0)
        return fail ("entrada do probe invalida");

    juce::String policyError;
    auto policy = ChurchDAWServices::parseUpdatePolicy (
        policyFile.loadFileAsString(), policyError,
        ChurchDAWServices::maximumUpdatePolicyBytes, trustedNow);

    if (! policy.valid)
        return fail ("politica invalida: " + policyError);

    juce::String manifestError;
    const auto manifest = ChurchDAWServices::parseManifest (
        manifestFile.loadFileAsString(), manifestError,
        policy.catalogSha256);

    if (! manifest.valid)
        return fail ("catalogo invalido: " + manifestError);

    // Validação explicita de normalizeManifestText e paridade SHA-256 no probe
    const juce::String rawManifestText = manifestFile.loadFileAsString();
    const juce::String normalizedDirect = ChurchDAWServices::normalizeManifestText (
        juce::String::fromUTF8 ("\xef\xbb\xbf") + juce::String ("a\r\nb\rc"));
    if (normalizedDirect != "a\nb\nc")
        return fail ("normalizeManifestText falhou no probe");

    const juce::String manifestLF = rawManifestText.replace ("\r\n", "\n").replace ("\r", "\n");
    const juce::String manifestCRLF = manifestLF.replace ("\n", "\r\n");
    juce::String manifestMixed = manifestLF;
    manifestMixed = manifestMixed.replaceSection (manifestMixed.indexOfChar ('\n'), 1, "\r\n");
    const juce::String manifestBOM = juce::String::fromUTF8 ("\xef\xbb\xbf") + manifestCRLF;

    const auto normLF = ChurchDAWServices::normalizeManifestText (manifestLF);
    const auto normCRLF = ChurchDAWServices::normalizeManifestText (manifestCRLF);
    const auto normMixed = ChurchDAWServices::normalizeManifestText (manifestMixed);
    const auto normBOM = ChurchDAWServices::normalizeManifestText (manifestBOM);

    const auto hashLF = juce::SHA256 (normLF.toRawUTF8(), (size_t) normLF.getNumBytesAsUTF8()).toHexString().toLowerCase();
    const auto hashCRLF = juce::SHA256 (normCRLF.toRawUTF8(), (size_t) normCRLF.getNumBytesAsUTF8()).toHexString().toLowerCase();
    const auto hashMixed = juce::SHA256 (normMixed.toRawUTF8(), (size_t) normMixed.getNumBytesAsUTF8()).toHexString().toLowerCase();
    const auto hashBOM = juce::SHA256 (normBOM.toRawUTF8(), (size_t) normBOM.getNumBytesAsUTF8()).toHexString().toLowerCase();

    if (hashLF != policy.catalogSha256 || hashCRLF != policy.catalogSha256 || hashMixed != policy.catalogSha256 || hashBOM != policy.catalogSha256)
        return fail ("paridade de hash SHA-256 entre line endings falhou no probe");

    juce::String errCRLF, errLF, errMixed, errBOM;
    if (! ChurchDAWServices::parseManifest (manifestCRLF, errCRLF, policy.catalogSha256).valid
         || ! ChurchDAWServices::parseManifest (manifestLF, errLF, policy.catalogSha256).valid
         || ! ChurchDAWServices::parseManifest (manifestMixed, errMixed, policy.catalogSha256).valid
         || ! ChurchDAWServices::parseManifest (manifestBOM, errBOM, policy.catalogSha256).valid)
        return fail ("parseManifest com variantes de line endings/BOM falhou no probe");

    juce::String wrongHashErr;
    if (ChurchDAWServices::parseManifest (manifestCRLF, wrongHashErr, "0000000000000000000000000000000000000000000000000000000000000000").valid
         || wrongHashErr != "Manifesto nao corresponde ao SHA-256 assinado")
        return fail ("parseManifest aceitou hash incorreto ou devolveu mensagem de erro errada");

    // Um catálogo indisponível nunca pode apagar visualmente um pack que já foi
    // instalado. O painel reconstrói seu card a partir deste marker local.
    const auto fixtureRoot =
        juce::File::getSpecialLocation (juce::File::tempDirectory)
            .getNonexistentChildFile (
                "churchdaw-installed-pack-probe", {}, true);
    const auto installedFolder = fixtureRoot.getChildFile ("church-live");

    if (installedFolder.createDirectory().failed()
         || ! installedFolder.getChildFile ("sample.wav")
                .replaceWithData ("x", 1))
        return fail ("nao foi possivel criar fixture de biblioteca instalada");

    auto marker = std::make_unique<juce::DynamicObject>();
    marker->setProperty ("schema_version", 1);
    marker->setProperty ("id", "church-live");
    marker->setProperty ("name", "Church Live");
    marker->setProperty ("version", 1);
    marker->setProperty ("version_name", "1.0");
    marker->setProperty ("installed_bytes", 1);

    if (! installedFolder.getChildFile (".churchdaw-library.json")
             .replaceWithText (
                 juce::JSON::toString (
                     juce::var (marker.release()), true)))
        return fail ("nao foi possivel gravar marker da fixture");

    ChurchDAWServices::LibraryPack installedPack;
    const bool installedPackOk =
        ChurchDAWServices::readInstalledLibraryPack (
            installedFolder, installedPack)
        && installedPack.id == "church-live"
        && installedPack.name == "Church Live"
        && installedPack.version == 1
        && installedPack.installFolder == "church-live";

    fixtureRoot.deleteRecursively();

    if (! installedPackOk)
        return fail (
            "marker local valido nao reconstruiu a biblioteca instalada");

    // O Pad das builds antigas ficava em Libraries/pad-continuo/Pads,
    // enquanto o catalogo atual declara install_folder=Pads. O card precisa
    // consultar o pai legado (onde esta o marker), sem mudar o destino de uma
    // futura reparacao/atualizacao.
    const auto legacyPadFolder = fixtureRoot.getChildFile ("pad-continuo");
    const auto legacyPadSamples = legacyPadFolder.getChildFile ("Pads");

    if (legacyPadSamples.createDirectory().failed()
         || ! legacyPadSamples.getChildFile ("pad_C.wav")
                .replaceWithData ("x", 1))
        return fail ("nao foi possivel criar fixture legada do Pad");

    auto padMarker = std::make_unique<juce::DynamicObject>();
    padMarker->setProperty ("schema_version", 1);
    padMarker->setProperty ("id", "pad-continuo");
    padMarker->setProperty ("name", "Pad Continuo");
    padMarker->setProperty ("version", 1);
    padMarker->setProperty ("version_name", "1.0");
    padMarker->setProperty ("installed_bytes", 7);

    if (! legacyPadFolder.getChildFile (".churchdaw-library.json")
             .replaceWithText (juce::JSON::toString (
                 juce::var (padMarker.release()), true)))
        return fail ("nao foi possivel gravar marker legado do Pad");

    ChurchDAWServices::LibraryPack catalogPad;
    catalogPad.id = "pad-continuo";
    catalogPad.installFolder = "Pads";
    catalogPad.version = 1;

    if (catalogPad.detectedInstalledFolderAt (fixtureRoot) != legacyPadFolder
         || ChurchDAWServices::validLibraryMarkerVersion (
                catalogPad.detectedInstalledFolderAt (fixtureRoot), catalogPad.id) != 1)
        return fail ("catalogo nao reconheceu a instalacao legada do Pad");

    fixtureRoot.deleteRecursively();

    std::cout << "PASS: politica serial " << policy.policySerial
              << ", catalogo vinculado e "
              << manifest.libraries.size()
              << " biblioteca(s), fallback local validado"
              << std::endl;
    return 0;
}
