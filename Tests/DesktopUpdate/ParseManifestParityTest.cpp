// ParseManifestParityTest.cpp
// Empirical verification test suite for ChurchDAWServices::parseManifest
// Validates cross-platform line ending hash parity (CRLF vs LF vs CR vs BOM)
// and strict rejection of corrupted/tampered manifest files.

#include <iostream>
#include <juce_core/juce_core.h>
#include "ChurchDAWServices.h"

namespace
{
    int testCount = 0;
    int passCount = 0;
    int failCount = 0;

    void assertTest (bool condition, const juce::String& testName, const juce::String& detail = {})
    {
        ++testCount;
        if (condition)
        {
            ++passCount;
            std::cout << "[PASS] " << testName << std::endl;
        }
        else
        {
            ++failCount;
            std::cout << "[FAIL] " << testName;
            if (detail.isNotEmpty())
                std::cout << " -> " << detail;
            std::cout << std::endl;
        }
    }
}

int main (int argc, char** argv)
{
    if (argc < 3)
    {
        std::cerr << "Usage: ParseManifestParityTest <path-to-update-policy-v2.json> <path-to-manifest.json>" << std::endl;
        return 2;
    }

    const juce::File policyFile (juce::String::fromUTF8 (argv[1]));
    const juce::File manifestFile (juce::String::fromUTF8 (argv[2]));

    std::cout << "====================================================" << std::endl;
    std::cout << "Running Empirical ParseManifest Parity Test Suite" << std::endl;
    std::cout << "Policy file: " << policyFile.getFullPathName() << std::endl;
    std::cout << "Manifest file: " << manifestFile.getFullPathName() << std::endl;
    std::cout << "====================================================" << std::endl;

    assertTest (policyFile.existsAsFile(), "Policy file exists");
    assertTest (manifestFile.existsAsFile(), "Manifest file exists");

    if (! policyFile.existsAsFile() || ! manifestFile.existsAsFile())
    {
        std::cerr << "Required input files not found!" << std::endl;
        return 1;
    }

    const juce::String rawPolicyText = policyFile.loadFileAsString();
    const juce::String rawManifestText = manifestFile.loadFileAsString();

    juce::String policyError;
    const auto policy = ChurchDAWServices::parseUpdatePolicy (
        rawPolicyText, policyError,
        ChurchDAWServices::maximumUpdatePolicyBytes);

    assertTest (policy.valid, "Parse signed update policy", policyError);
    assertTest (policy.catalogSha256.length() == 64, "Signed policy contains 64-char catalogSha256", policy.catalogSha256);

    juce::String actualManifestError;
    const auto actualManifest = ChurchDAWServices::parseManifest (
        rawManifestText, actualManifestError, policy.catalogSha256);

    assertTest (actualManifest.valid,
                "Parse real manifest bound to signed policy",
                actualManifestError);
    assertTest (actualManifestError.isEmpty(),
                "No error for real policy-bound manifest",
                actualManifestError);

    // Construct a fully valid sample manifest JSON matching all schema, URL, and identifier constraints
    const juce::String validManifestJson = 
R"({
  "schema_version": 1,
  "package_id": "com.churchdaw.app",
  "latest_version_code": 14,
  "minimum_version_code": 9,
  "latest_version_name": "1.5.8",
  "update_message": "ChurchDAW 1.5.8",
  "play_store_url": "https://churchdaw.com/app/android/ChurchDAW-1.5.8-signed.apk",
  "libraries": [
    {
      "id": "church-live",
      "name": "Church Live",
      "description": "Pianos, pads e timbres",
      "version": 1,
      "version_name": "1.0",
      "install_folder": "church-live",
      "download_bytes": 447204118,
      "installed_bytes": 447204118,
      "parts": [
        {
          "download_url": "https://churchdaw.com/app/android/packs/church-live-v1-segment-01.zip",
          "sha256": "1c7937e598fffaf640715cf571c76f5ceacc321f7f242b72788697fc3801fdb1",
          "download_bytes": 447204118
        }
      ],
      "included": true,
      "featured": true,
      "price_label": "Inclusa",
      "purchase_url": ""
    }
  ]
})";

    const auto canonicalNormalizedText = ChurchDAWServices::normalizeManifestText (validManifestJson);
    const juce::String expectedHash = juce::SHA256 (
        canonicalNormalizedText.toRawUTF8(),
        (size_t) canonicalNormalizedText.getNumBytesAsUTF8()).toHexString().toLowerCase();

    assertTest (expectedHash.length() == 64, "Computed canonical SHA-256 hash has 64 hex chars", expectedHash);

    // Build line ending and BOM variants
    const juce::String lfText = validManifestJson.replace ("\r\n", "\n").replace ("\r", "\n");
    const juce::String crlfText = lfText.replace ("\n", "\r\n");
    const juce::String crText = lfText.replace ("\n", "\r");

    const juce::String bomLfText = juce::String::fromUTF8 ("\xef\xbb\xbf") + lfText;
    const juce::String bomCrlfText = juce::String::fromUTF8 ("\xef\xbb\xbf") + crlfText;
    const juce::String bomCrText = juce::String::fromUTF8 ("\xef\xbb\xbf") + crText;

    // Mixed line endings variant (\r\n mixed with \n and \r)
    juce::String mixedText = lfText;
    mixedText = mixedText.replaceSection (mixedText.indexOfChar ('\n'), 1, "\r\n");
    if (mixedText.lastIndexOfChar ('\n') > 0)
        mixedText = mixedText.replaceSection (mixedText.lastIndexOfChar ('\n'), 1, "\r");

    std::cout << "\n--- TEST GROUP 1: Cross-Platform Line Ending & BOM Hash Parity ---" << std::endl;

    struct VariantCase
    {
        juce::String name;
        juce::String content;
    };

    const VariantCase variants[] = {
        { "LF Line Endings (macOS / Linux)", lfText },
        { "CRLF Line Endings (Windows)", crlfText },
        { "CR Line Endings (Legacy Mac)", crText },
        { "UTF-8 BOM + LF", bomLfText },
        { "UTF-8 BOM + CRLF", bomCrlfText },
        { "UTF-8 BOM + CR", bomCrText },
        { "Mixed Line Endings (CRLF + LF + CR)", mixedText }
    };

    for (const auto& v : variants)
    {
        juce::String manifestError;
        const auto manifest = ChurchDAWServices::parseManifest (v.content, manifestError, expectedHash);

        assertTest (manifest.valid, "parseManifest validation with " + v.name, manifestError);
        assertTest (manifestError.isEmpty(), "No error for " + v.name, manifestError);

        // Explicitly compute normalized SHA256 and confirm exact match across all variants
        const auto norm = ChurchDAWServices::normalizeManifestText (v.content);
        const auto computedHash = juce::SHA256 (norm.toRawUTF8(), (size_t) norm.getNumBytesAsUTF8()).toHexString().toLowerCase();
        assertTest (computedHash == expectedHash, "Hash parity check for " + v.name, computedHash + " vs " + expectedHash);
    }

    {
        // Compatibilidade com politicas publicadas pelo gerador antigo, que
        // assinava o SHA dos bytes CRLF em vez do texto normalizado.
        const auto legacyRawCrlfHash = juce::SHA256 (
            crlfText.toRawUTF8(),
            (size_t) crlfText.getNumBytesAsUTF8()).toHexString().toLowerCase();
        juce::String legacyError;
        const auto legacyManifest = ChurchDAWServices::parseManifest (
            crlfText, legacyError, legacyRawCrlfHash);

        assertTest (legacyManifest.valid,
                    "Accepts legacy signed raw-CRLF catalog hash",
                    legacyError);
        assertTest (legacyError.isEmpty(),
                    "No error for legacy signed raw-CRLF catalog hash",
                    legacyError);
    }

    std::cout << "\n--- TEST GROUP 2: Adversarial & Tampered Manifest Rejection ---" << std::endl;

    {
        // 2.1 Tampered hash (modify last character of expectedHash)
        juce::String tamperedHash = expectedHash;
        const char lastC = tamperedHash[63];
        tamperedHash = tamperedHash.dropLastCharacters (1) + (lastC == 'a' ? "b" : "a");

        juce::String err;
        const auto m = ChurchDAWServices::parseManifest (crlfText, err, tamperedHash);
        assertTest (! m.valid, "Rejects manifest when expected hash is modified");
        assertTest (err == "Manifesto nao corresponde ao SHA-256 assinado", "Correct error message for hash mismatch", err);
    }

    {
        // 2.2 Tampered manifest JSON content (modify latest_version_code)
        juce::String tamperedContent = crlfText.replace ("\"latest_version_code\": 14", "\"latest_version_code\": 15");
        if (tamperedContent == crlfText)
            tamperedContent = crlfText.replace ("\"latest_version_code\":14", "\"latest_version_code\":15");

        assertTest (tamperedContent != crlfText, "Preparation: tampered JSON content created");

        juce::String err;
        const auto m = ChurchDAWServices::parseManifest (tamperedContent, err, expectedHash);
        assertTest (! m.valid, "Rejects tampered manifest content");
        assertTest (err == "Manifesto nao corresponde ao SHA-256 assinado", "Correct error message for tampered manifest", err);
    }

    {
        // 2.3 Truncated hash (length < 64)
        juce::String shortHash = expectedHash.dropLastCharacters (4);
        juce::String err;
        const auto m = ChurchDAWServices::parseManifest (crlfText, err, shortHash);
        assertTest (! m.valid, "Rejects truncated hash string");
        assertTest (err == "Manifesto nao corresponde ao SHA-256 assinado", "Correct error for short hash", err);
    }

    {
        // 2.4 Hash with invalid characters (non-hex)
        juce::String invalidHexHash = expectedHash.dropLastCharacters (2) + "ZZ";
        juce::String err;
        const auto m = ChurchDAWServices::parseManifest (crlfText, err, invalidHexHash);
        assertTest (! m.valid, "Rejects non-hex hash string");
        assertTest (err == "Manifesto nao corresponde ao SHA-256 assinado", "Correct error for non-hex hash", err);
    }

    {
        // 2.5 Uppercase expected hash (parseManifest requires lower-case hex)
        juce::String upperHash = expectedHash.toUpperCase();
        juce::String err;
        const auto m = ChurchDAWServices::parseManifest (crlfText, err, upperHash);
        assertTest (! m.valid, "Rejects uppercase hash string (strict lowercase requirement)");
        assertTest (err == "Manifesto nao corresponde ao SHA-256 assinado", "Correct error for uppercase hash", err);
    }

    {
        // 2.6 Empty JSON string
        juce::String err;
        const auto m = ChurchDAWServices::parseManifest ("", err, expectedHash);
        assertTest (! m.valid, "Rejects empty manifest string");
        assertTest (err == "Manifesto vazio ou grande demais", "Correct error for empty manifest", err);
    }

    {
        // 2.7 Excessively large JSON string (> 2MB)
        juce::String hugeJson = "{" + juce::String::repeatedString ("\"a\":\"b\",", 300000) + "\"x\":\"y\"}";
        juce::String err;
        const auto m = ChurchDAWServices::parseManifest (hugeJson, err, expectedHash);
        assertTest (! m.valid, "Rejects manifest exceeding 2MB size limit");
        assertTest (err == "Manifesto vazio ou grande demais", "Correct error for huge manifest", err);
    }

    {
        // 2.8 Invalid JSON syntax with matching hash
        juce::String invalidJsonText = "{ schema_version: 1, package_id: com.churchdaw.app "; // missing closing braces & quotes
        const auto normInvalid = ChurchDAWServices::normalizeManifestText (invalidJsonText);
        const auto hashInvalid = juce::SHA256 (normInvalid.toRawUTF8(), (size_t) normInvalid.getNumBytesAsUTF8()).toHexString().toLowerCase();

        juce::String err;
        const auto m = ChurchDAWServices::parseManifest (invalidJsonText, err, hashInvalid);
        assertTest (! m.valid, "Rejects malformed JSON syntax even when SHA256 matches");
        assertTest (err.startsWith ("JSON invalido:"), "Correct error prefix for malformed JSON syntax", err);
    }

    std::cout << "\n====================================================" << std::endl;
    std::cout << "SUMMARY: " << passCount << " / " << testCount << " tests passed." << std::endl;
    std::cout << "====================================================" << std::endl;

    return (failCount == 0) ? 0 : 1;
}
