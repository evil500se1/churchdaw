#include "ChannelRotarySpeaker.h"

#include <cmath>
#include <iostream>
#include <limits>

namespace
{
    [[noreturn]] void fail (const char* message)
    {
        std::cerr << "CHANNEL_ROTARY_FAIL: " << message << std::endl;
        std::exit (1);
    }

    void expect (bool condition, const char* message)
    {
        if (! condition)
            fail (message);
    }

    void fillSine (juce::AudioBuffer<float>& buffer,
                   double sampleRate,
                   double& phase,
                   float amplitude = 0.28f,
                   double frequency = 440.0)
    {
        const double step = juce::MathConstants<double>::twoPi * frequency / sampleRate;

        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
        {
            const float value = amplitude * (float) std::sin (phase);
            phase += step;

            if (phase >= juce::MathConstants<double>::twoPi)
                phase -= juce::MathConstants<double>::twoPi;

            for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
                buffer.setSample (channel, sample, value);
        }
    }

    void runForSeconds (ChurchDAWRotary::Speaker& rotary,
                        double sampleRate,
                        double seconds,
                        int mode,
                        float amount)
    {
        constexpr int block = 257;
        juce::AudioBuffer<float> audio (2, block);
        double phase = 0.0;
        int remaining = (int) std::round (sampleRate * seconds);

        while (remaining > 0)
        {
            const int count = juce::jmin (block, remaining);
            audio.setSize (2, count, false, false, true);
            fillSine (audio, sampleRate, phase);
            rotary.process (audio, count, amount, mode);
            remaining -= count;
        }
    }

    struct LevelMeasurement
    {
        double stereoRms = 0.0;
        double monoRms = 0.0;
        float peak = 0.0f;
    };

    LevelMeasurement measureLevel (double frequency,
                                   float amount,
                                   int mode,
                                   double rate = 48000.0,
                                   int channels = 2)
    {
        constexpr int blockSize = 256;
        channels = juce::jlimit (1, 2, channels);
        ChurchDAWRotary::Speaker rotary;
        rotary.prepare (rate, blockSize);
        juce::AudioBuffer<float> audio (channels, blockSize);
        double phase = 0.0;

        // Estabiliza tanto os motores quanto o smoother do Amount.
        int warmup = (int) (rate * 8.0);
        while (warmup > 0)
        {
            const int count = juce::jmin (blockSize, warmup);
            audio.setSize (channels, count, false, false, true);
            fillSine (audio, rate, phase, 0.28f, frequency);
            rotary.process (audio, count, amount, mode);
            warmup -= count;
        }

        double stereoEnergy = 0.0;
        double monoEnergy = 0.0;
        int measured = 0;
        LevelMeasurement result;
        int remaining = (int) (rate * 4.0);

        while (remaining > 0)
        {
            const int count = juce::jmin (blockSize, remaining);
            audio.setSize (channels, count, false, false, true);
            fillSine (audio, rate, phase, 0.28f, frequency);
            rotary.process (audio, count, amount, mode);

            for (int sample = 0; sample < count; ++sample)
            {
                const float l = audio.getSample (0, sample);
                const float r = channels > 1
                                    ? audio.getSample (1, sample)
                                    : l;
                const float mono = 0.5f * (l + r);
                stereoEnergy += 0.5 * ((double) l * l + (double) r * r);
                monoEnergy += (double) mono * mono;
                result.peak = juce::jmax (
                    result.peak, juce::jmax (std::abs (l), std::abs (r)));
                ++measured;
            }

            remaining -= count;
        }

        result.stereoRms = std::sqrt (
            stereoEnergy / (double) juce::jmax (1, measured));
        result.monoRms = std::sqrt (
            monoEnergy / (double) juce::jmax (1, measured));
        return result;
    }
}

int main()
{
    constexpr double sampleRate = 48000.0;
    constexpr int block = 256;

    // Estado antigo migrado usa Amount=0 e precisa ser bit-transparent.
    ChurchDAWRotary::Speaker dry;
    dry.prepare (sampleRate, block);
    juce::AudioBuffer<float> original (2, block);
    double dryPhase = 0.0;
    fillSine (original, sampleRate, dryPhase);
    juce::AudioBuffer<float> stopped;
    stopped.makeCopyOf (original);
    dry.process (stopped, block, 0.0f, ChurchDAWRotary::Speaker::stopMode);

    for (int channel = 0; channel < 2; ++channel)
        for (int sample = 0; sample < block; ++sample)
            expect (stopped.getSample (channel, sample)
                        == original.getSample (channel, sample),
                    "Amount zero must be bit-transparent");

    ChurchDAWRotary::Speaker rotary;
    rotary.prepare (sampleRate, block);
    runForSeconds (rotary, sampleRate, 6.0,
                   ChurchDAWRotary::Speaker::slowMode, 0.65f);

    expect (rotary.getCurrentHornRateHzForTest() > 0.78
             && rotary.getCurrentHornRateHzForTest() < 0.81,
            "slow horn rate outside expected range");
    expect (rotary.getCurrentDrumRateHzForTest() > 0.55
             && rotary.getCurrentDrumRateHzForTest() < 0.68,
            "slow drum inertia/rate outside expected range");

    runForSeconds (rotary, sampleRate, 12.0,
                   ChurchDAWRotary::Speaker::fastMode, 0.65f);

    expect (rotary.getCurrentHornRateHzForTest() > 6.5,
            "fast horn did not accelerate");
    expect (rotary.getCurrentDrumRateHzForTest() > 5.4,
            "fast drum did not accelerate");

    // STOP e o freio fisico, nao um bypass instantaneo.
    const double fastHorn = rotary.getCurrentHornRateHzForTest();
    const double fastDrum = rotary.getCurrentDrumRateHzForTest();
    runForSeconds (rotary, sampleRate, 0.10,
                   ChurchDAWRotary::Speaker::stopMode, 0.65f);
    expect (rotary.getCurrentHornRateHzForTest() > 0.1
             && rotary.getCurrentHornRateHzForTest() < fastHorn,
            "STOP did not brake horn with inertia");
    expect (rotary.getCurrentDrumRateHzForTest() > 0.1
             && rotary.getCurrentDrumRateHzForTest() < fastDrum,
            "STOP did not brake drum with inertia");

    // A cadeia do canal e pulada depois de 0,6 s sem audio. Mesmo assim a
    // fisica precisa continuar: FAST->STOP nao pode congelar ate a proxima nota.
    int silentSamples = (int) (sampleRate * 30.0);

    while (silentSamples > 0)
    {
        const int count = juce::jmin (block, silentSamples);
        rotary.advanceSilently (
            count, ChurchDAWRotary::Speaker::stopMode);
        silentSamples -= count;
    }

    expect (rotary.getCurrentHornRateHzForTest() < 0.03,
            "silent STOP froze the horn above rest");
    expect (rotary.getCurrentDrumRateHzForTest() < 0.08,
            "silent STOP froze the drum above rest");

    silentSamples = (int) (sampleRate * 12.0);

    while (silentSamples > 0)
    {
        const int count = juce::jmin (block, silentSamples);
        rotary.advanceSilently (
            count, ChurchDAWRotary::Speaker::fastMode);
        silentSamples -= count;
    }

    expect (rotary.getCurrentHornRateHzForTest() > 6.5,
            "silent FAST did not accelerate the horn");
    expect (rotary.getCurrentDrumRateHzForTest() > 5.4,
            "silent FAST did not accelerate the drum");

    // Bypass limpa o delay: nenhum pedaco do orgao anterior pode reaparecer.
    ChurchDAWRotary::Speaker history;
    history.prepare (sampleRate, block);
    juce::AudioBuffer<float> impulse (2, block);
    impulse.clear();
    impulse.setSample (0, 0, 0.5f);
    impulse.setSample (1, 0, 0.5f);
    history.process (impulse, block, 1.0f,
                     ChurchDAWRotary::Speaker::fastMode);

    for (int i = 0; i < 80; ++i)
    {
        impulse.clear();
        history.process (impulse, block, 0.0f,
                         ChurchDAWRotary::Speaker::stopMode);
    }

    impulse.clear();
    history.process (impulse, block, 1.0f,
                     ChurchDAWRotary::Speaker::stopMode);
    expect (impulse.getMagnitude (0, 0, block) < 1.0e-7f
             && impulse.getMagnitude (1, 0, block) < 1.0e-7f,
            "bypass leaked stale delay history");

    // Movimento estereo, estabilidade numerica e continuidade na troca Slow/Fast.
    rotary.reset();
    juce::AudioBuffer<float> audio (2, block);
    double phase = 0.0;
    double stereoEnergy = 0.0;
    double outputEnergy = 0.0;
    float peak = 0.0f;
    float previousL = 0.0f;
    bool havePrevious = false;
    int zeroCrossings = 0;
    float previousMono = 0.0f;
    int measuredSamples = 0;
    const int totalBlocks = (int) (sampleRate * 14.0 / block);

    for (int b = 0; b < totalBlocks; ++b)
    {
        fillSine (audio, sampleRate, phase);
        rotary.process (audio, block, 0.72f,
                        b < totalBlocks / 2
                            ? ChurchDAWRotary::Speaker::slowMode
                            : ChurchDAWRotary::Speaker::fastMode);

        for (int sample = 0; sample < block; ++sample)
        {
            const float left = audio.getSample (0, sample);
            const float right = audio.getSample (1, sample);
            expect (std::isfinite (left) && std::isfinite (right),
                    "non-finite rotary output");

            if (havePrevious)
                expect (std::abs (left - previousL) < 0.35f,
                        "click/discontinuity during speed transition");

            previousL = left;
            havePrevious = true;
            peak = juce::jmax (peak,
                               juce::jmax (std::abs (left), std::abs (right)));

            if (b >= totalBlocks - (int) (sampleRate * 4.0 / block))
            {
                const float mono = 0.5f * (left + right);
                const float difference = left - right;
                stereoEnergy += (double) difference * difference;
                outputEnergy += 0.5 * ((double) left * left + (double) right * right);

                if (previousMono <= 0.0f && mono > 0.0f)
                    ++zeroCrossings;

                previousMono = mono;
                ++measuredSamples;
            }
        }
    }

    const double stereoRms = std::sqrt (stereoEnergy / juce::jmax (1, measuredSamples));
    const double outputRms = std::sqrt (outputEnergy / juce::jmax (1, measuredSamples));
    const double measuredSeconds = (double) measuredSamples / sampleRate;
    const double measuredFrequency = zeroCrossings / measuredSeconds;

    expect (stereoRms > 0.004, "rotary did not create stereo movement");
    expect (outputRms > 0.04 && outputRms < 0.45, "rotary RMS outside safe range");
    expect (peak < 0.75f, "rotary peak gain is unsafe");
    expect (std::abs (measuredFrequency - 440.0) < 2.5,
            "rotary introduced long-term pitch drift");

    // Relatório de ganho por Amount/frequência. As asserções de estabilidade
    // são aplicadas abaixo; manter os números no log torna regressões de
    // loudness e cancelamento mono diagnosticáveis.
    constexpr double dryRms = 0.28 / juce::MathConstants<double>::sqrt2;
    for (double frequency : { 80.0, 196.0, 220.0, 233.0, 240.0, 247.0,
                              262.0, 440.0, 800.0, 1000.0, 2500.0,
                              6000.0 })
        for (float amount : { 0.0f, 0.25f, 0.50f, 0.75f, 1.0f })
        {
            const auto level = measureLevel (
                frequency, amount, ChurchDAWRotary::Speaker::fastMode);
            const auto stereoDb = juce::Decibels::gainToDecibels (
                (float) (level.stereoRms / dryRms), -120.0f);
            const auto monoDb = juce::Decibels::gainToDecibels (
                (float) (level.monoRms / dryRms), -120.0f);
            std::cout << "ROTARY_LEVEL"
                      << " hz=" << frequency
                      << " amount=" << amount
                      << " stereo_db=" << stereoDb
                      << " mono_db=" << monoDb
                      << " peak=" << level.peak
                      << '\n';

            expect (stereoDb >= -2.0f && stereoDb <= 2.0f,
                    "rotary stereo loudness changed by more than 2 dB");
            expect (monoDb >= -2.0f && monoDb <= 2.0f,
                    "rotary mono loudness changed by more than 2 dB");
            expect (level.peak < 0.70f,
                    "rotary frequency probe peak is unsafe");
        }

    // Adversarial matrix omitted by the original test: true mono, every
    // half-moon state and every supported production sample rate. These are
    // short musical-register probes rather than a permissive absolute-RMS
    // floor, so a reintroduced comb notch fails deterministically.
    for (double rate : { 44100.0, 48000.0, 96000.0 })
        for (int mode : { ChurchDAWRotary::Speaker::slowMode,
                          ChurchDAWRotary::Speaker::stopMode,
                          ChurchDAWRotary::Speaker::fastMode })
            for (int channels : { 1, 2 })
                for (double frequency : { 196.0, 220.0, 240.0, 262.0,
                                          440.0, 800.0 })
                    for (float amount : { 0.25f, 0.50f, 1.0f })
                    {
                        const auto level = measureLevel (
                            frequency, amount, mode, rate, channels);
                        const auto stereoDb =
                            juce::Decibels::gainToDecibels (
                                (float) (level.stereoRms / dryRms),
                                -120.0f);
                        const auto monoDb =
                            juce::Decibels::gainToDecibels (
                                (float) (level.monoRms / dryRms),
                                -120.0f);
                        if (stereoDb < -2.0f || stereoDb > 2.0f
                            || monoDb < -2.0f || monoDb > 2.0f
                            || level.peak >= 0.70f)
                            std::cerr << "ROTARY_MATRIX"
                                      << " rate=" << rate
                                      << " mode=" << mode
                                      << " channels=" << channels
                                      << " hz=" << frequency
                                      << " amount=" << amount
                                      << " stereo_db=" << stereoDb
                                      << " mono_db=" << monoDb
                                      << " peak=" << level.peak
                                      << '\n';
                        expect (stereoDb >= -2.0f && stereoDb <= 2.0f,
                                "rotary matrix stereo level outside +/-2 dB");
                        expect (monoDb >= -2.0f && monoDb <= 2.0f,
                                "rotary matrix mono level outside +/-2 dB");
                        expect (level.peak < 0.70f,
                                "rotary matrix peak is unsafe");
                    }

    // Sample rates e blocos variados, inclusive entrada invalida.
    for (double rate : { 44100.0, 48000.0, 96000.0 })
    {
        ChurchDAWRotary::Speaker varied;
        varied.prepare (rate, 1024);
        double variedPhase = 0.0;

        for (int size : { 31, 64, 127, 256, 511, 1024 })
        {
            juce::AudioBuffer<float> variedBuffer (2, size);
            fillSine (variedBuffer, rate, variedPhase);
            variedBuffer.setSample (0, size / 2,
                                    std::numeric_limits<float>::quiet_NaN());
            varied.process (variedBuffer, size, 1.0f,
                            ChurchDAWRotary::Speaker::fastMode);

            for (int channel = 0; channel < 2; ++channel)
                for (int sample = 0; sample < size; ++sample)
                    expect (std::isfinite (variedBuffer.getSample (channel, sample)),
                            "rate/block fuzz produced non-finite output");
        }
    }

    std::cout << "CHANNEL_ROTARY_OK"
              << " stereo_rms=" << stereoRms
              << " output_rms=" << outputRms
              << " peak=" << peak
              << " frequency=" << measuredFrequency
              << std::endl;
    return 0;
}
