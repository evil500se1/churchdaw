#include "ChurchOrganLayerProcessor.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>

namespace
{
    constexpr double testSampleRate = 48000.0;
    constexpr std::size_t blockSize = 128;
    constexpr std::uint64_t seed = 0x5452414e53464143ULL;
    constexpr float expectedOptInVibratoDepth = 0.55f;

    class TestRunner
    {
    public:
        void expect (bool condition, const std::string& message)
        {
            ++checks;
            if (! condition)
            {
                ++failures;
                std::cerr << "FAIL: " << message << '\n';
            }
        }

        void expectNear (
            double actual,
            double expected,
            double tolerance,
            const std::string& message)
        {
            expect (
                std::isfinite (actual)
                    && std::abs (actual - expected) <= tolerance,
                message);
        }

        int finish() const
        {
            if (failures != 0)
            {
                std::cerr
                    << "TRANSISTOR_FACTORY_DEFAULTS_FAILED checks="
                    << checks << " failures=" << failures << '\n';
                return 1;
            }

            std::cout << "TRANSISTOR_FACTORY_DEFAULTS_OK checks="
                      << checks << '\n';
            return 0;
        }

    private:
        int checks = 0;
        int failures = 0;
    };

    churchorgan::ChurchOrganLayerPrepareSpec transistorSpec()
    {
        churchorgan::ChurchOrganLayerPrepareSpec spec;
        spec.sampleRate = testSampleRate;
        spec.maximumBlockSize = blockSize;
        spec.deterministicSeed = seed;
        spec.initialFamily =
            churchorgan::ChurchOrganFamily::transistor;
        return spec;
    }

    juce::ValueTree transistorState (
        churchorgan::ChurchOrganLayerProcessor& processor)
    {
        processor.setProcessingSuspended (true);
        return processor.createState().getChildWithName ("Transistor");
    }

    void testIntegratedFactoryIsNeutral (TestRunner& test)
    {
        const churchorgan::TransistorOrganPrepareSpec engineDefaults;
        test.expect (
            ! engineDefaults.vibratoEnabled,
            "standalone Transistor spec must not opt into pitch modulation");
        test.expectNear (
            engineDefaults.vibratoDepth,
            expectedOptInVibratoDepth,
            0.0,
            "neutral spec retains a useful depth for explicit opt-in");

        churchorgan::ChurchOrganLayerProcessor processor;
        test.expect (
            processor.prepare (transistorSpec()),
            "integrated Transistor factory prepares");
        test.expect (
            ! processor.transistorEngine().vibratoEnabled(),
            "integrated factory leaves internal vibrato disabled beside channel rotary");

        const auto state = transistorState (processor);
        test.expect (state.isValid(), "factory writes Transistor state");
        test.expect (
            ! static_cast<bool> (
                state.getProperty ("vibratoEnabled", true)),
            "factory state persists the neutral vibrato switch");
        test.expectNear (
            static_cast<double> (
                state.getProperty ("vibratoDepth", 0.0)),
            expectedOptInVibratoDepth,
            1.0e-7,
            "factory state keeps the musical opt-in depth");
    }

    void testExplicitControlRoundTrips (TestRunner& test)
    {
        churchorgan::ChurchOrganLayerProcessor source;
        test.expect (
            source.prepare (transistorSpec()),
            "explicit-control source prepares");
        source.setTransistorVibrato (
            true, expectedOptInVibratoDepth);
        source.setProcessingSuspended (true);
        const auto saved = source.createState();

        churchorgan::ChurchOrganLayerProcessor restored;
        test.expect (
            restored.prepare (transistorSpec()),
            "explicit-control destination prepares");
        restored.setProcessingSuspended (true);
        test.expect (
            restored.restoreState (saved),
            "explicit Transistor vibrato state restores");
        test.expect (
            restored.transistorEngine().vibratoEnabled(),
            "explicit vibrato enable survives a marked-state round trip");
        test.expectNear (
            restored.transistorEngine().vibratoDepth(),
            expectedOptInVibratoDepth,
            1.0e-6,
            "marked 0.55 vibrato survives without legacy migration");
    }

    void testLegacyFactoryDefaultMigratesOnce (TestRunner& test)
    {
        churchorgan::ChurchOrganLayerProcessor templateProcessor;
        test.expect (
            templateProcessor.prepare (transistorSpec()),
            "legacy template prepares");
        templateProcessor.setProcessingSuspended (true);

        auto legacyFactoryState = templateProcessor.createState();
        legacyFactoryState.setProperty ("schema", 2, nullptr);
        auto legacyTransistor =
            legacyFactoryState.getChildWithName ("Transistor");
        legacyTransistor.removeProperty (
            "vibratoIntentVersion", nullptr);
        legacyTransistor.setProperty (
            "vibratoEnabled", true, nullptr);
        legacyTransistor.setProperty (
            "vibratoDepth",
            expectedOptInVibratoDepth,
            nullptr);

        churchorgan::ChurchOrganLayerProcessor migrated;
        test.expect (
            migrated.prepare (transistorSpec()),
            "legacy migration destination prepares");
        migrated.setProcessingSuspended (true);
        test.expect (
            migrated.restoreState (legacyFactoryState),
            "legacy factory state restores");
        test.expect (
            ! migrated.transistorEngine().vibratoEnabled(),
            "legacy automatic 0.55 vibrato migrates to neutral");

        const auto migratedRoot = migrated.createState();
        test.expect (
            static_cast<int> (
                migratedRoot.getProperty ("schema", 0))
                == static_cast<int> (
                    churchorgan::ChurchOrganLayerProcessor::
                        stateSchemaVersion),
            "migrated state is stamped with the current schema");
        test.expect (
            ! static_cast<bool> (
                migratedRoot.getChildWithName ("Transistor")
                    .getProperty ("vibratoEnabled", true)),
            "one-time migration persists the neutral result");

        auto legacyExplicitState = legacyFactoryState.createCopy();
        legacyExplicitState.getChildWithName ("Transistor")
            .setProperty ("vibratoDepth", 0.73f, nullptr);

        churchorgan::ChurchOrganLayerProcessor explicitLegacy;
        test.expect (
            explicitLegacy.prepare (transistorSpec()),
            "explicit legacy destination prepares");
        explicitLegacy.setProcessingSuspended (true);
        test.expect (
            explicitLegacy.restoreState (legacyExplicitState),
            "explicit legacy state restores");
        test.expect (
            explicitLegacy.transistorEngine().vibratoEnabled(),
            "non-default legacy vibrato choice remains enabled");
        test.expectNear (
            explicitLegacy.transistorEngine().vibratoDepth(),
            0.73,
            1.0e-6,
            "non-default legacy vibrato depth is preserved");
    }
}

int main()
{
    TestRunner test;
    testIntegratedFactoryIsNeutral (test);
    testExplicitControlRoundTrips (test);
    testLegacyFactoryDefaultMigratesOnce (test);
    return test.finish();
}
