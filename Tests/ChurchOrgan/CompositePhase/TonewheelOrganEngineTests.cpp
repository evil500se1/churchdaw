#include "TonewheelOrganEngine.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>

namespace
{
    std::atomic<std::uint64_t> allocationCount { 0 };
    std::size_t checks = 0;

    void require (bool condition, const char* message)
    {
        ++checks;
        if (! condition)
        {
            std::cerr << "FAIL: " << message << '\n';
            std::exit (1);
        }
    }

    void configureNeutral (
        churchorgan::TonewheelOrganEngine& engine)
    {
        using namespace churchorgan;
        auto& p = engine.parameters();
        p.setPlain (OrganParameterId::scannerMode, 0.0f);
        p.setPlain (OrganParameterId::scannerMix, 0.0f);
        p.setPlain (OrganParameterId::drive, 0.0f);
        p.setPlain (OrganParameterId::transformer, 0.0f);
        p.setPlain (OrganParameterId::rotaryMix, 0.0f);
        p.setPlain (OrganParameterId::leakage, 0.0f);
        p.setPlain (OrganParameterId::crosstalk, 0.0f);
        p.setPlain (OrganParameterId::hum, 0.0f);
        p.setPlain (OrganParameterId::keyClick, 0.0f);
        p.setPlain (OrganParameterId::masterGainDb, 0.0f);
    }

    template <std::size_t N>
    double energy (const std::array<float, N>& buffer)
    {
        double result = 0.0;
        for (const auto sample : buffer)
            result += static_cast<double> (sample) * sample;
        return result;
    }
}

void* operator new (std::size_t size)
{
    allocationCount.fetch_add (1, std::memory_order_relaxed);
    if (auto* result = std::malloc (size))
        return result;
    throw std::bad_alloc();
}

void* operator new[] (std::size_t size)
{
    return ::operator new (size);
}

void operator delete (void* memory) noexcept
{
    std::free (memory);
}

void operator delete[] (void* memory) noexcept
{
    std::free (memory);
}

void operator delete (void* memory, std::size_t) noexcept
{
    std::free (memory);
}

void operator delete[] (void* memory, std::size_t) noexcept
{
    std::free (memory);
}

int main()
{
    using namespace churchorgan;

    TonewheelOrganPrepareSpec spec;
    spec.sampleRate = 48000.0;
    spec.maximumBlockSize = 1024;
    spec.deterministicSeed = 0x1122334455667788ULL;
    spec.quality = OrganQualityProfile::standard;

    TonewheelOrganEngine invalid;
    auto bad = spec;
    bad.sampleRate = 12000.0;
    require (! invalid.prepare (bad), "invalid sample rate rejected");
    bad = spec;
    bad.maximumBlockSize =
        TonewheelOrganEngine::maximumBlockCapacity + 1;
    require (! invalid.prepare (bad), "oversized block rejected");

    TonewheelOrganEngine control;
    TonewheelOrganEngine offset;
    require (control.prepare (spec), "control prepare");
    require (offset.prepare (spec), "offset prepare");
    configureNeutral (control);
    configureNeutral (offset);

    std::array<float, 1024> controlLeft {};
    std::array<float, 1024> controlRight {};
    std::array<float, 1024> offsetLeft {};
    std::array<float, 1024> offsetRight {};

    require (offset.queueNoteOn (Manual::upper, 60, 110, 37),
             "offset note queued");
    require (
        control.render ({ controlLeft.data(), controlRight.data(), 1024 }),
        "control render");
    require (
        offset.render ({ offsetLeft.data(), offsetRight.data(), 1024 }),
        "offset render");

    for (std::size_t sample = 0; sample < 37; ++sample)
    {
        require (controlLeft[sample] == offsetLeft[sample],
                 "note offset preserves prefix left");
        require (controlRight[sample] == offsetRight[sample],
                 "note offset preserves prefix right");
    }

    double postOffsetDifference = 0.0;
    for (std::size_t sample = 37; sample < 1024; ++sample)
        postOffsetDifference += std::abs (
            static_cast<double> (controlLeft[sample])
            - offsetLeft[sample]);
    require (postOffsetDifference > 0.01,
             "note begins after requested offset");

    TonewheelOrganEngine whole;
    TonewheelOrganEngine partitioned;
    require (whole.prepare (spec), "whole prepare");
    require (partitioned.prepare (spec), "partitioned prepare");
    configureNeutral (whole);
    configureNeutral (partitioned);

    auto queueSequence = [] (TonewheelOrganEngine& engine)
    {
        require (engine.queueNoteOn (Manual::upper, 60, 100, 0),
                 "sequence upper");
        require (engine.queueNoteOn (Manual::lower, 48, 90, 73),
                 "sequence lower");
        require (engine.queueNoteOn (Manual::pedal, 36, 120, 211),
                 "sequence pedal");
        require (engine.queueExpression (74, 401),
                 "sequence expression");
        require (engine.queueSustain (Manual::upper, true, 507),
                 "sequence sustain down");
        require (engine.queueNoteOff (Manual::upper, 60, 601),
                 "sequence note off");
        require (engine.queueSustain (Manual::upper, false, 777),
                 "sequence sustain up");
    };

    queueSequence (whole);
    queueSequence (partitioned);

    std::array<float, 1024> wholeLeft {};
    std::array<float, 1024> wholeRight {};
    std::array<float, 1024> partitionedLeft {};
    std::array<float, 1024> partitionedRight {};
    require (
        whole.render ({ wholeLeft.data(), wholeRight.data(), 1024 }),
        "whole block render");

    for (std::size_t block = 0; block < 4; ++block)
        require (
            partitioned.render ({
                partitionedLeft.data() + block * 256,
                partitionedRight.data() + block * 256,
                256 }),
            "partition render");

    require (
        std::memcmp (
            wholeLeft.data(), partitionedLeft.data(),
            sizeof (wholeLeft)) == 0,
        "left block partition invariance");
    require (
        std::memcmp (
            wholeRight.data(), partitionedRight.data(),
            sizeof (wholeRight)) == 0,
        "right block partition invariance");

    TonewheelOrganEngine deterministicA;
    TonewheelOrganEngine deterministicB;
    require (deterministicA.prepare (spec), "deterministic A prepare");
    require (deterministicB.prepare (spec), "deterministic B prepare");
    deterministicA.parameters().setPlain (
        OrganParameterId::scannerMode, 5.0f);
    deterministicB.parameters().setPlain (
        OrganParameterId::scannerMode, 5.0f);
    deterministicA.parameters().setPlain (
        OrganParameterId::drive, 0.61f);
    deterministicB.parameters().setPlain (
        OrganParameterId::drive, 0.61f);
    deterministicA.parameters().setPlain (
        OrganParameterId::rotaryMode, 2.0f);
    deterministicB.parameters().setPlain (
        OrganParameterId::rotaryMode, 2.0f);

    for (const int note : { 48, 55, 60, 64, 67, 72 })
    {
        require (deterministicA.queueNoteOn (
                     Manual::upper, note, 100, note - 48),
                 "deterministic A note");
        require (deterministicB.queueNoteOn (
                     Manual::upper, note, 100, note - 48),
                 "deterministic B note");
    }

    std::array<float, 1024> deterministicLeftA {};
    std::array<float, 1024> deterministicRightA {};
    std::array<float, 1024> deterministicLeftB {};
    std::array<float, 1024> deterministicRightB {};
    for (int block = 0; block < 8; ++block)
    {
        require (
            deterministicA.render ({
                deterministicLeftA.data(),
                deterministicRightA.data(), 1024 }),
            "deterministic A render");
        require (
            deterministicB.render ({
                deterministicLeftB.data(),
                deterministicRightB.data(), 1024 }),
            "deterministic B render");
        require (
            std::memcmp (
                deterministicLeftA.data(), deterministicLeftB.data(),
                sizeof (deterministicLeftA)) == 0,
            "deterministic left");
        require (
            std::memcmp (
                deterministicRightA.data(), deterministicRightB.data(),
                sizeof (deterministicRightA)) == 0,
            "deterministic right");
    }
    require (
        std::memcmp (
            deterministicLeftA.data(), deterministicRightA.data(),
            sizeof (deterministicLeftA)) != 0,
        "rotary creates coherent stereo");

    for (const auto manual : { Manual::upper, Manual::lower, Manual::pedal })
    {
        TonewheelOrganEngine engine;
        require (engine.prepare (spec), "manual engine prepare");
        configureNeutral (engine);
        require (engine.queueNoteOn (manual, manual == Manual::pedal ? 36 : 60,
                                     100, 0),
                 "manual note queue");
        std::array<float, 1024> left {};
        std::array<float, 1024> right {};
        require (engine.render ({ left.data(), right.data(), left.size() }),
                 "manual render");
        require (energy (left) > 1.0e-6, "manual is audible");
    }

    TonewheelOrganEngine sustained;
    require (sustained.prepare (spec), "sustain prepare");
    configureNeutral (sustained);
    require (sustained.queueNoteOn (Manual::upper, 64, 100, 0),
             "sustain note on");
    require (sustained.queueSustain (Manual::upper, true, 64),
             "sustain down");
    require (sustained.queueNoteOff (Manual::upper, 64, 128),
             "sustain note off");
    std::array<float, 1024> sustainLeft {};
    std::array<float, 1024> sustainRight {};
    require (sustained.render ({
                 sustainLeft.data(), sustainRight.data(), 1024 }),
             "sustain held render");
    require (sustained.diagnostics().activeUpperNotes > 0,
             "sustain holds note");
    require (sustained.queueSustain (Manual::upper, false, 0),
             "sustain release");
    for (int block = 0; block < 20; ++block)
        require (sustained.render ({
                     sustainLeft.data(), sustainRight.data(), 1024 }),
                 "sustain tail render");
    require (sustained.diagnostics().activeUpperNotes == 0,
             "sustain releases note");

    TonewheelOrganEngine extreme;
    require (extreme.prepare (spec), "extreme prepare");
    extreme.parameters().setPlain (OrganParameterId::masterGainDb, 6.0f);
    extreme.parameters().setPlain (OrganParameterId::drive, 1.0f);
    extreme.parameters().setPlain (OrganParameterId::rotaryMode, 2.0f);
    for (int note = 0; note < 128; ++note)
        require (extreme.queueNoteOn (Manual::upper, note, 127, 0),
                 "extreme note queued");
    std::array<float, 1024> extremeLeft {};
    std::array<float, 1024> extremeRight {};
    for (int block = 0; block < 4; ++block)
        require (extreme.render ({
                     extremeLeft.data(), extremeRight.data(), 1024 }),
                 "extreme render");
    const auto extremeDiagnostics = extreme.diagnostics();
    require (extremeDiagnostics.invalidOutputSamples == 0,
             "extreme output finite");
    require (extremeDiagnostics.lastBlockPeak < 2.0f,
             "extreme output bounded");

    for (const auto rate : {
             32000.0,
             44100.0,
             48000.0,
             96000.0,
             192000.0,
             384000.0 })
    {
        TonewheelOrganEngine rateEngine;
        auto rateSpec = spec;
        rateSpec.sampleRate = rate;
        rateSpec.maximumBlockSize = 256;
        rateSpec.quality = OrganQualityProfile::eco;
        require (rateEngine.prepare (rateSpec),
                 "sample-rate matrix prepare");
        rateEngine.parameters().setPlain (
            OrganParameterId::scannerMode, 5.0f);
        rateEngine.parameters().setPlain (
            OrganParameterId::drive, 0.42f);
        rateEngine.parameters().setPlain (
            OrganParameterId::rotaryMode, 2.0f);
        for (const int note : { 48, 55, 60, 64, 67 })
            require (rateEngine.queueNoteOn (
                         Manual::upper, note, 100, 0),
                     "sample-rate note");

        std::array<float, 256> rateLeft {};
        std::array<float, 256> rateRight {};
        const auto blocks = std::max (
            4,
            static_cast<int> (
                std::ceil (rate * 0.04 / rateLeft.size())));
        for (int block = 0; block < blocks; ++block)
        {
            require (rateEngine.render ({
                         rateLeft.data(),
                         rateRight.data(),
                         rateLeft.size() }),
                     "sample-rate render");
            for (std::size_t sample = 0;
                 sample < rateLeft.size();
                 ++sample)
            {
                require (
                    std::isfinite (rateLeft[sample])
                        && std::isfinite (rateRight[sample]),
                    "sample-rate output finite");
                require (
                    std::abs (rateLeft[sample]) < 2.0f
                        && std::abs (rateRight[sample]) < 2.0f,
                    "sample-rate output bounded");
            }
        }
        require (
            rateEngine.diagnostics().invalidOutputSamples == 0,
            "sample-rate diagnostics finite");
    }

    TonewheelOrganEngine allocationEngine;
    auto allocationSpec = spec;
    allocationSpec.maximumBlockSize = 128;
    allocationSpec.quality = OrganQualityProfile::eco;
    require (allocationEngine.prepare (allocationSpec),
             "allocation engine prepare");
    for (const int note : { 48, 55, 60, 64, 67 })
        require (allocationEngine.queueNoteOn (
                     Manual::upper, note, 100, 0),
                 "allocation note");
    std::array<float, 128> allocationLeft {};
    std::array<float, 128> allocationRight {};

    const auto allocationsBefore =
        allocationCount.load (std::memory_order_relaxed);
    for (int block = 0; block < 4000; ++block)
    {
        if ((block % 127) == 0)
        {
            allocationEngine.parameters().setNormalised (
                OrganParameterId::drive,
                static_cast<float> (block % 1000) / 999.0f);
            require (allocationEngine.queueExpression (
                         block % 128, 17),
                     "allocation expression");
        }

        require (allocationEngine.render ({
                     allocationLeft.data(), allocationRight.data(), 128 }),
                 "allocation render");
    }
    const auto allocationsAfter =
        allocationCount.load (std::memory_order_relaxed);
    require (allocationsAfter == allocationsBefore,
             "composite render allocates nothing");

    TonewheelOrganEngine overflow;
    require (overflow.prepare (spec), "overflow prepare");
    for (std::size_t event = 0;
         event < TonewheelOrganEngine::maximumEvents;
         ++event)
        require (overflow.queueExpression (
                     static_cast<int> (event % 128),
                     static_cast<std::uint32_t> (event)),
                 "queue capacity");
    require (! overflow.queueExpression (100, 0), "queue overflow rejected");

    const auto diagnostics = allocationEngine.diagnostics();
    require (diagnostics.totalRenderedSamples == 4000u * 128u,
             "rendered sample diagnostics");
    require (diagnostics.invalidOutputSamples == 0,
             "no invalid samples");

    std::cout << "CHURCH_ORGAN_COMPOSITE_TESTS_OK checks="
              << checks
              << " allocations="
              << (allocationsAfter - allocationsBefore)
              << " extreme_peak=" << extremeDiagnostics.lastBlockPeak
              << '\n';
    return 0;
}
