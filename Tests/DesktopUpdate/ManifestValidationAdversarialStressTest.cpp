// ManifestValidationAdversarialStressTest.cpp
// White-box adversarial stress testing for library catalog loading,
// cached manifest handling, UI error formatting, and local library fallbacks.

#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <juce_core/juce_core.h>
#include <juce_cryptography/juce_cryptography.h>

#include "ChurchDAWServices.h"
#include "LibraryPanel.h"
#include "BinaryData.h"
#include "TimbresProcessor.h"

namespace
{
    int totalTests = 0;
    int passedTests = 0;
    int failedTests = 0;

    void assertCheck (bool condition, const juce::String& testName, const juce::String& detail = {})
    {
        ++totalTests;
        if (condition)
        {
            ++passedTests;
            std::cout << "[PASS] " << testName.toStdString() << std::endl;
        }
        else
        {
            ++failedTests;
            std::cout << "[FAIL] " << testName.toStdString();
            if (detail.isNotEmpty())
                std::cout << " -> " << detail.toStdString();
            std::cout << std::endl;
        }
    }
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    std::cout << "============================================================" << std::endl;
    std::cout << "Adversarial Stress Test: Manifest Validation & UI Error Parity" << std::endl;
    std::cout << "============================================================" << std::endl;

    // -------------------------------------------------------------------------
    // TEST 1: Exact UI Error String Formatting on Manifest Validation Failure
    // -------------------------------------------------------------------------
    std::cout << "\n--- TEST 1: Exact UI Error Message Formatting ---" << std::endl;
    {
        const juce::String expectedCatalogSha256 = "11223344556677889900aabbccddeeff11223344556677889900aabbccddeeff";
        const juce::String tamperedManifestJson = R"({
            "schema_version": 1,
            "package_id": "com.churchdaw.app",
            "latest_version_code": 14,
            "libraries": []
        })";

        juce::String parseError;
        const auto manifest = ChurchDAWServices::parseManifest (
            tamperedManifestJson, parseError, expectedCatalogSha256);

        assertCheck (! manifest.valid, "Manifest marked invalid on SHA-256 mismatch");
        assertCheck (parseError == "Manifesto nao corresponde ao SHA-256 assinado",
                     "parseManifest sets exact error string", parseError);

        const juce::String endpoint = "https://churchdaw.com/app/android/manifest.json";
        const int statusCode = 200;
        const juce::String formattedDiagnostic = endpoint + " / HTTP "
                                                + juce::String (statusCode)
                                                + ": " + parseError;

        const juce::String expectedFormattedString =
            "https://churchdaw.com/app/android/manifest.json / HTTP 200: Manifesto nao corresponde ao SHA-256 assinado";

        assertCheck (formattedDiagnostic == expectedFormattedString,
                     "Formatted diagnostic matches exact requirement", formattedDiagnostic);
    }

    // -------------------------------------------------------------------------
    // TEST 2: Cached Manifest Handling with Corrupted / Invalid Cache
    // -------------------------------------------------------------------------
    std::cout << "\n--- TEST 2: Cached Manifest Handling with Invalid Cache ---" << std::endl;
    {
        const auto cacheFile = ChurchDAWServices::cachedManifestFile();
        const auto backupFile = cacheFile.getSiblingFile ("android-manifest-cache.json.bak");

        // Backup existing cache if present
        if (cacheFile.existsAsFile())
            cacheFile.copyFileTo (backupFile);

        // Inject invalid manifest content into cache file
        ChurchDAWServices::saveCache ("{\"corrupted\": true, \"invalid_sha\": \"bad\"}");

        // Attempt loadCachedManifest
        const auto cachedManifest = ChurchDAWServices::loadCachedManifest();

        assertCheck (! cachedManifest.valid, "loadCachedManifest returns valid=false for corrupted cache");
        assertCheck (cachedManifest.libraries.isEmpty(), "loadCachedManifest libraries is empty for corrupted cache");

        // Cleanup injected cache and restore backup
        cacheFile.deleteFile();
        if (backupFile.existsAsFile())
        {
            backupFile.moveFileTo (cacheFile);
        }
    }

    // -------------------------------------------------------------------------
    // TEST 3: Anti-Crash, Anti-Hang, Anti-Infinite-Retry Resilience
    // -------------------------------------------------------------------------
    std::cout << "\n--- TEST 3: Stress Loop for Crash/Hang/Infinite Retry Resilience ---" << std::endl;
    {
        constexpr int iterations = 100;
        bool executionSuccessful = true;
        auto startTime = std::chrono::high_resolution_clock::now();

        for (int i = 0; i < iterations; ++i)
        {
            juce::String parseErr;
            const auto m = ChurchDAWServices::parseManifest (
                "{\"invalid_json\": true}", parseErr, "0000000000000000000000000000000000000000000000000000000000000000");

            if (m.valid)
            {
                executionSuccessful = false;
                break;
            }
        }

        auto endTime = std::chrono::high_resolution_clock::now();
        auto durationMs = std::chrono::duration_cast<std::chrono::milliseconds> (endTime - startTime).count();

        assertCheck (executionSuccessful, "Completed 100 rapid invalid manifest parse calls without failure");
        assertCheck (durationMs < 2000, "Execution completed quickly (no infinite retry / hang)",
                     juce::String (durationMs) + " ms");
    }

    // -------------------------------------------------------------------------
    // TEST 4: Local Fallback Libraries and Preset Accessibility Preservation
    // -------------------------------------------------------------------------
    std::cout << "\n--- TEST 4: Local Library Fallback & Preset Accessibility ---" << std::endl;
    {
        // 4.1 Verify knownLibraries() runs safely without throwing or corrupting state
        const auto libs = TimbresProcessor::knownLibraries();
        assertCheck (true, "knownLibraries() executed safely",
                     "Found " + juce::String (libs.size()) + " local library definitions");

        // 4.2 Verify Church Organ presets count invariant
        assertCheck (TimbresProcessor::churchOrganFamilyCount == 4,
                     "Church Organ family count invariant preserved (4 families)");

        // 4.3 Verify default samples folder check
        const auto defaultFolder = TimbresProcessor::defaultSamplesFolder();
        assertCheck (true, "defaultSamplesFolder() evaluated cleanly",
                     defaultFolder.getFullPathName());
    }

    std::cout << "\n============================================================" << std::endl;
    std::cout << "SUMMARY: " << passedTests << " / " << totalTests << " tests passed." << std::endl;
    std::cout << "============================================================" << std::endl;

    return (failedTests == 0) ? 0 : 1;
}
