#include "NordSampleTone.h"

#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>

namespace
{
constexpr double sampleRate = 48000.0;

struct TestContext
{
    void expect (bool condition, const char* message)
    {
        if (condition)
            return;

        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }

    int failures = 0;
};

double filterRms (int type, double frequency,
                  float cutoff = 1000.0f,
                  float resonance = 0.0f,
                  int drive = 0)
{
    ChurchDAWNordSample::FilterSettings settings;
    settings.enabled = true;
    settings.type = type;
    settings.cutoffHz = cutoff;
    settings.resonance = resonance;
    settings.drive = drive;
    settings.envelopeAmount = 0.0f;

    ChurchDAWNordSample::VoiceFilter filter;
    filter.start (sampleRate, settings);

    constexpr int totalSamples = 48000;
    constexpr int discardSamples = 8000;
    double sumSquares = 0.0;
    int measured = 0;
    double phase = 0.0;
    const double increment = juce::MathConstants<double>::twoPi
                           * frequency / sampleRate;

    for (int sample = 0; sample < totalSamples; ++sample)
    {
        float left = 0.08f * (float) std::sin (phase);
        float right = left;
        phase += increment;
        filter.processStereo (left, right, settings, 60, 100);

        if (sample >= discardSamples)
        {
            sumSquares += (double) left * left;
            ++measured;
        }
    }

    return std::sqrt (sumSquares / (double) juce::jmax (1, measured));
}

double eqRms (double frequency,
              ChurchDAWNordSample::AmpEqSettings settings,
              bool checkSilentRight = false)
{
    constexpr int totalSamples = 48000;
    constexpr int discardSamples = 8000;
    juce::AudioBuffer<float> buffer (2, totalSamples);
    double phase = 0.0;
    const double increment = juce::MathConstants<double>::twoPi
                           * frequency / sampleRate;

    for (int sample = 0; sample < totalSamples; ++sample)
    {
        buffer.setSample (0, sample, 0.04f * (float) std::sin (phase));
        buffer.setSample (1, sample,
                          checkSilentRight ? 0.0f : buffer.getSample (0, sample));
        phase += increment;
    }

    ChurchDAWNordSample::AmpEq eq;
    eq.prepare (sampleRate, totalSamples, settings);
    eq.process (buffer, totalSamples, settings);

    if (checkSilentRight)
        for (int sample = 0; sample < totalSamples; ++sample)
            if (buffer.getSample (1, sample) != 0.0f)
                return std::numeric_limits<double>::infinity();

    double sumSquares = 0.0;
    for (int sample = discardSamples; sample < totalSamples; ++sample)
    {
        const double value = buffer.getSample (0, sample);
        sumSquares += value * value;
    }

    return std::sqrt (sumSquares / (double) (totalSamples - discardSamples));
}
}

int main()
{
    TestContext test;

    // Bypass is a hard contract: a disabled filter must not colour or even
    // round the sample. This also protects old sessions after the migration.
    ChurchDAWNordSample::FilterSettings bypass;
    ChurchDAWNordSample::VoiceFilter bypassFilter;
    bypassFilter.start (sampleRate, bypass);
    for (int sample = 0; sample < 4096; ++sample)
    {
        const float inputLeft = (float) std::sin ((double) sample * 0.117);
        const float inputRight = (float) std::cos ((double) sample * 0.071);
        float left = inputLeft;
        float right = inputRight;
        bypassFilter.processStereo (left, right, bypass, 64, 93);
        test.expect (left == inputLeft && right == inputRight,
                     "disabled voice filter is not bit-exact bypass");
    }

    const double lpLow = filterRms (ChurchDAWNordSample::lp24, 100.0);
    const double lpHigh = filterRms (ChurchDAWNordSample::lp24, 8000.0);
    test.expect (lpLow > lpHigh * 20.0,
                 "LP24 does not reject high frequencies strongly enough");

    const double hpLow = filterRms (ChurchDAWNordSample::highPass, 100.0);
    const double hpHigh = filterRms (ChurchDAWNordSample::highPass, 8000.0);
    test.expect (hpHigh > hpLow * 20.0,
                 "HP does not reject low frequencies strongly enough");

    const double bpCentre = filterRms (ChurchDAWNordSample::bandPass, 1000.0,
                                       1000.0f, 0.35f);
    const double bpLow = filterRms (ChurchDAWNordSample::bandPass, 80.0,
                                    1000.0f, 0.35f);
    const double bpHigh = filterRms (ChurchDAWNordSample::bandPass, 9000.0,
                                     1000.0f, 0.35f);
    test.expect (bpCentre > bpLow * 2.0 && bpCentre > bpHigh * 2.0,
                 "BP does not favour its centre band");

    for (int type = 0; type < ChurchDAWNordSample::numFilterTypes; ++type)
        for (int drive = 0; drive <= 3; ++drive)
        {
            const double rms = filterRms (type, 733.0, 1700.0f, 1.0f, drive);
            test.expect (std::isfinite (rms) && rms >= 0.0 && rms < 8.0,
                         "filter mode/drive produced invalid output");
        }

    // The sampler captures and bounds these controls once per audio block.
    // Prove that its optimized entry point is bit-identical to the public
    // safety wrapper for an already-bounded setting, including a moving ADSR.
    ChurchDAWNordSample::FilterSettings paritySettings;
    paritySettings.enabled = true;
    paritySettings.type = ChurchDAWNordSample::lp24;
    paritySettings.cutoffHz = 1840.0f;
    paritySettings.resonance = 0.42f;
    paritySettings.drive = 2;
    paritySettings.keyTrack = 3;
    paritySettings.envelopeAmount = 0.61f;
    paritySettings.velocity = true;
    paritySettings.bright = true;
    paritySettings.envelopeAttackSeconds = 0.007f;
    paritySettings.envelopeDecaySeconds = 0.091f;
    paritySettings.envelopeReleaseSeconds = 0.240f;

    ChurchDAWNordSample::VoiceFilter wrappedFilter;
    ChurchDAWNordSample::VoiceFilter boundedFilter;
    wrappedFilter.start (sampleRate, paritySettings);
    boundedFilter.start (sampleRate, paritySettings);

    for (int sample = 0; sample < 16384; ++sample)
    {
        if (sample == 9000)
        {
            wrappedFilter.release (paritySettings);
            boundedFilter.release (paritySettings);
        }

        float wrappedLeft = 0.13f * (float) std::sin ((double) sample * 0.071);
        float wrappedRight = 0.11f * (float) std::cos ((double) sample * 0.043);
        float boundedLeft = wrappedLeft;
        float boundedRight = wrappedRight;
        wrappedFilter.processStereo (
            wrappedLeft, wrappedRight, paritySettings, 67, 113);
        boundedFilter.processStereoBounded (
            boundedLeft, boundedRight, paritySettings, 67, 113);
        test.expect (wrappedLeft == boundedLeft && wrappedRight == boundedRight,
                     "bounded voice-filter path changed a rendered sample");
    }

    ChurchDAWNordSample::FilterSettings tracked;
    tracked.cutoffHz = 1000.0f;
    tracked.keyTrack = 3;
    const float middleC = ChurchDAWNordSample::cutoffFor (
        tracked, 60, 100, 0.0f, sampleRate);
    const float upperC = ChurchDAWNordSample::cutoffFor (
        tracked, 72, 100, 0.0f, sampleRate);
    test.expect (std::abs (upperC / middleC - 2.0f) < 0.001f,
                 "1:1 key tracking does not double cutoff per octave");

    tracked.keyTrack = 0;
    tracked.velocity = true;
    tracked.envelopeAmount = 0.5f;
    const float softCutoff = ChurchDAWNordSample::cutoffFor (
        tracked, 60, 1, 1.0f, sampleRate);
    const float hardCutoff = ChurchDAWNordSample::cutoffFor (
        tracked, 60, 127, 1.0f, sampleRate);
    test.expect (hardCutoff > softCutoff * 3.0f,
                 "filter velocity does not open the envelope sufficiently");

    // Amp EQ bypass must be equally transparent.
    juce::AudioBuffer<float> bypassBuffer (2, 1024);
    for (int channel = 0; channel < 2; ++channel)
        for (int sample = 0; sample < bypassBuffer.getNumSamples(); ++sample)
            bypassBuffer.setSample (
                channel, sample,
                0.17f * (float) std::sin ((double) (sample + channel * 13)));
    juce::AudioBuffer<float> reference;
    reference.makeCopyOf (bypassBuffer);
    ChurchDAWNordSample::AmpEq bypassEq;
    bypassEq.prepare (sampleRate, bypassBuffer.getNumSamples(), {});
    bypassEq.process (bypassBuffer, bypassBuffer.getNumSamples(), {});
    test.expect (std::memcmp (bypassBuffer.getReadPointer (0),
                             reference.getReadPointer (0),
                             (std::size_t) bypassBuffer.getNumSamples()
                                 * sizeof (float)) == 0
                 && std::memcmp (bypassBuffer.getReadPointer (1),
                                 reference.getReadPointer (1),
                                 (std::size_t) bypassBuffer.getNumSamples()
                                     * sizeof (float)) == 0,
                 "disabled Amp EQ is not bit-exact bypass");

    ChurchDAWNordSample::AmpEqSettings neutral;
    const double lowNeutral = eqRms (60.0, neutral);
    auto bassBoost = neutral;
    bassBoost.enabled = true;
    bassBoost.bassDb = 12.0f;
    test.expect (eqRms (60.0, bassBoost) > lowNeutral * 1.7,
                 "100 Hz bass shelf does not boost the low band");

    const double midNeutral = eqRms (1000.0, neutral);
    auto midCut = neutral;
    midCut.enabled = true;
    midCut.midHz = 1000.0f;
    midCut.midDb = -12.0f;
    test.expect (eqRms (1000.0, midCut) < midNeutral * 0.62,
                 "sweep mid does not cut its selected frequency");

    const double highNeutral = eqRms (8000.0, neutral);
    auto trebleBoost = neutral;
    trebleBoost.enabled = true;
    trebleBoost.trebleDb = 12.0f;
    test.expect (eqRms (8000.0, trebleBoost) > highNeutral * 1.7,
                 "4 kHz treble shelf does not boost the high band");
    test.expect (std::isfinite (eqRms (440.0, trebleBoost, true)),
                 "Amp EQ leaks one stereo channel into the other");

    if (test.failures != 0)
    {
        std::cerr << test.failures << " Nord sample tone test(s) failed\n";
        return 1;
    }

    std::cout << "PASS: Nord sample filters, tracking, drive and Amp EQ\n";
    return 0;
}
