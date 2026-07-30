#include "TonewheelCharacter.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
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

    TonewheelBank bankA;
    TonewheelBank bankB;
    TonewheelCharacter characterA;
    TonewheelCharacter characterB;

    TonewheelCharacterParameters parameters;
    parameters.leakage = 0.55f;
    parameters.crosstalk = 0.48f;
    parameters.hum = 0.30f;
    parameters.age = 0.65f;
    parameters.mechanicalNoise = 0.22f;
    parameters.transformer = 0.58f;

    require (bankA.prepare (48000.0, 0x1234, 0.2f), "bank A prepare");
    require (bankB.prepare (48000.0, 0x1234, 0.2f), "bank B prepare");
    require (characterA.prepare (48000.0, 0x5678, parameters),
             "character A prepare");
    require (characterB.prepare (48000.0, 0x5678, parameters),
             "character B prepare");
    require (! characterA.prepare (12000.0, 1, parameters),
             "bad sample rate rejected");
    require (characterA.prepare (48000.0, 0x5678, parameters),
             "character A reprepare");

    double leakageEnergy = 0.0;
    double crosstalkEnergy = 0.0;
    double transformerDifference = 0.0;
    float peak = 0.0f;

    const auto allocationsBefore =
        allocationCount.load (std::memory_order_relaxed);

    for (int sample = 0; sample < 96000; ++sample)
    {
        characterA.beginSample (bankA);
        characterB.beginSample (bankB);

        const auto leakageA = characterA.leakageSample();
        const auto leakageB = characterB.leakageSample();
        require (leakageA == leakageB, "deterministic leakage");

        const auto crosstalkA =
            characterA.crosstalkSample (bankA, 44, 0.5f);
        const auto crosstalkB =
            characterB.crosstalkSample (bankB, 44, 0.5f);
        require (crosstalkA == crosstalkB,
                 "deterministic crosstalk");

        const auto input = bankA.sample (48) * 0.52f;
        const auto outputA = characterA.processTransformer (
            input + leakageA + crosstalkA);
        const auto outputB = characterB.processTransformer (
            input + leakageB + crosstalkB);
        require (outputA == outputB, "deterministic transformer");
        require (std::isfinite (outputA), "finite output");

        leakageEnergy += static_cast<double> (leakageA) * leakageA;
        crosstalkEnergy +=
            static_cast<double> (crosstalkA) * crosstalkA;
        transformerDifference += std::abs (
            static_cast<double> (outputA) - input);
        peak = std::max (peak, std::abs (outputA));

        bankA.advance();
        bankB.advance();
    }

    const auto allocationsAfter =
        allocationCount.load (std::memory_order_relaxed);

    require (allocationsAfter == allocationsBefore,
             "render path allocates nothing");
    require (leakageEnergy > 1.0e-4, "leakage is audible");
    require (crosstalkEnergy > 1.0e-5, "crosstalk is audible");
    require (transformerDifference > 1.0, "transformer changes signal");
    require (peak < 1.2f, "bounded output");

    TonewheelCharacterParameters bypass;
    bypass.leakage = 0.0f;
    bypass.crosstalk = 0.0f;
    bypass.hum = 0.0f;
    bypass.age = 0.0f;
    bypass.mechanicalNoise = 0.0f;
    bypass.transformer = 0.0f;
    bypass.smoothingMilliseconds = 1.0f;

    TonewheelCharacter clean;
    TonewheelBank cleanBank;
    require (cleanBank.prepare (44100.0, 2, 0.0f), "clean bank prepare");
    require (clean.prepare (44100.0, 3, bypass),
             "clean character prepare");

    for (int sample = 0; sample < 2048; ++sample)
    {
        clean.beginSample (cleanBank);
        require (clean.leakageSample() == 0.0f, "zero leakage bypass");
        require (
            clean.crosstalkSample (cleanBank, 20, 0.2f) == 0.0f,
            "zero crosstalk bypass");
        const auto input = cleanBank.sample (30) * 0.3f;
        require (clean.processTransformer (input) == input,
                 "transformer exact bypass");
        cleanBank.advance();
    }

    std::cout << "CHURCH_ORGAN_CHARACTER_TESTS_OK checks="
              << checks
              << " allocations="
              << (allocationsAfter - allocationsBefore)
              << " peak=" << peak
              << '\n';
    return 0;
}
