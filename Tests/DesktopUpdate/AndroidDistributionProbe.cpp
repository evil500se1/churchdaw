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
        "https://churchdaw.com/app/android/ChurchDAW-1.5.0-signed.apk";
    if (! ChurchDAWServices::isSafeWebsiteUpdateUrl (officialApkUrl))
        return fail ("URL oficial do APK foi rejeitada");
    if (ChurchDAWServices::isSafeWebsiteUpdateUrl (
            officialApkUrl + "?arquivo=outro")
        || ChurchDAWServices::isSafeWebsiteUpdateUrl (
            officialApkUrl + "\r\nhttps://example.com/arquivo.apk")
        || ChurchDAWServices::isSafeWebsiteUpdateUrl (
            "https://example.com/app/android/ChurchDAW-1.5.0-signed.apk"))
        return fail ("filtro de URL aceitou query, CRLF ou host externo");

    ChurchDAWServices::UpdatePolicy futurePolicy;
    futurePolicy.valid = true;
    futurePolicy.cryptographicallyVerified = true;
    futurePolicy.latestVersionCode =
        ChurchDAWServices::currentVersionCode + 1;
    futurePolicy.playStoreUrl =
        "https://churchdaw.com/app/android/"
        "ChurchDAW-9.9.9-signed.apk";

    if (ChurchDAWServices::requiredUpdateDownloadUrl (
            futurePolicy, futurePolicy.latestVersionCode)
            != futurePolicy.playStoreUrl)
        return fail ("gate descartou URL assinada da versao futura");

    futurePolicy.playStoreUrl =
        "https://example.com/app/android/ChurchDAW-9.9.9-signed.apk";
    if (ChurchDAWServices::requiredUpdateDownloadUrl (
            futurePolicy, futurePolicy.latestVersionCode)
            != ChurchDAWServices::defaultPlayStoreUrl)
        return fail ("gate aceitou URL externa como download obrigatorio");

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

    std::cout << "PASS: politica serial " << policy.policySerial
              << ", catalogo vinculado e "
              << manifest.libraries.size() << " biblioteca(s)"
              << std::endl;
    return 0;
}
