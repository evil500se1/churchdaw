#include "DiagnosticTestTone.h"
#include "FinalOutputGuard.h"
#include "GuardedAudioDeviceSelector.h"
#include <algorithm>
#include <iostream>
#include <limits>
#include <vector>

namespace
{
struct ToneRender
{
    std::vector<float> audio;
    float rawPeak = 0, outputPeak = 0, stereoError = 0;
    bool finite = true;
};

ToneRender renderDiagnostic (double rate, int block, int mode)
{
    DiagnosticTestTone tone;
    FinalOutputGuard guard;
    tone.prepare (rate); guard.prepare (rate, 64, 2);
    tone.requestStart();
    const int total = (int) (rate * 1.5);
    const int restart = (int) (rate * 0.25), stop = (int) (rate * 0.60);
    ToneRender result; result.audio.resize ((size_t) total);
    juce::AudioBuffer<float> buffer (2, block);
    for (int position = 0; position < total;)
    {
        int next = total;
        if (mode == 2)
        {
            if (position == restart) tone.requestStart();
            if (position == stop) tone.requestStop();
            if (position < restart) next = restart;
            else if (position < stop) next = stop;
        }
        const int count = std::min (block, next - position);
        // Mode 0: idle playback/empty graph. Mode 1: an ongoing loud mix,
        // already bounded by the app before a diagnostic tone is added.
        const float music = mode == 1 ? 0.78f : 0.0f;
        for (int channel = 0; channel < 2; ++channel)
            juce::FloatVectorOperations::fill (buffer.getWritePointer (channel), music, count);
        tone.processAdd (buffer.getArrayOfWritePointers(), 2, count);
        result.rawPeak = std::max (result.rawPeak, buffer.getMagnitude (0, count));
        guard.process (buffer.getArrayOfWritePointers(), 2, count);
        for (int i = 0; i < count; ++i)
        {
            const float sample = buffer.getSample (0, i);
            result.audio[(size_t) position + i] = sample;
            result.finite = result.finite && std::isfinite (sample)
                && std::isfinite (buffer.getSample (1, i));
            result.outputPeak = std::max (result.outputPeak, std::abs (sample));
            result.stereoError = std::max (result.stereoError,
                std::abs (sample - buffer.getSample (1, i)));
        }
        position += count;
    }
    return result;
}

bool testSelectorBinding()
{
    juce::Component hierarchy, panel;
    hierarchy.addAndMakeVisible (panel);
    juce::TextButton test (juce::translate ("Test"), juce::translate ("Plays a test tone"));
    juce::TextButton unrelated (juce::translate ("Test"), "Unrelated command");
    panel.addAndMakeVisible (test); panel.addAndMakeVisible (unrelated);
    int unsafeCalls = 0, protectedCalls = 0, otherCalls = 0;
    test.onClick = [&] { ++unsafeCalls; };
    unrelated.onClick = [&] { ++otherCalls; };
    const auto callback = [&] { ++protectedCalls; };
    bool ok = ChurchDAWAudioUI::bindOutputTest (hierarchy, callback) == 1;
    test.onClick(); unrelated.onClick();
    ok = ok && unsafeCalls == 0 && protectedCalls == 1 && otherCalls == 1;
    // A marked button remains safe even if presentation later changes labels.
    test.setButtonText ("Testar saída"); test.setTooltip ("Tom protegido");
    test.onClick = [&] { ++unsafeCalls; };
    ok = ChurchDAWAudioUI::bindOutputTest (hierarchy, callback) == 1 && ok;
    test.onClick();
    panel.removeChildComponent (&test);
    juce::TextButton replacement (juce::translate ("Test"), juce::translate ("Plays a test tone"));
    replacement.onClick = [&] { ++unsafeCalls; };
    panel.addAndMakeVisible (replacement);
    // The subclass performs this bind synchronously after JUCE recreates its
    // hierarchy; no cosmetic refresh timer is required to replace the callback.
    ok = ChurchDAWAudioUI::bindOutputTest (hierarchy, callback) == 1 && ok;
    replacement.onClick();
    ok = ok && unsafeCalls == 0 && protectedCalls == 3 && otherCalls == 1;
    std::cout << "DIAGNOSTIC_SELECTOR_REBIND " << (ok ? "PASS" : "FAIL") << '\n';
    return ok;
}
}

bool runDiagnosticTestToneTests()
{
    bool ok = testSelectorBinding();
    for (double rate : { 44100.0, 48000.0, 96000.0 })
        for (int mode : { 0, 1, 2 })
        {
            const auto reference = renderDiagnostic (rate, 64, mode);
            float error = 0;
            bool finite = reference.finite;
            for (int block : { 1, 257, 1024, 8192 })
            {
                const auto subject = renderDiagnostic (rate, block, mode);
                finite = finite && subject.finite;
                for (size_t sample = 0; sample < subject.audio.size(); ++sample)
                    error = std::max (error, std::abs (subject.audio[sample] - reference.audio[sample]));
            }
            const int end = (int) (rate * (mode == 2 ? 0.62 : 1.02));
            bool stopped = true;
            if (mode != 1)
                for (size_t sample = (size_t) end; sample < reference.audio.size(); ++sample)
                    stopped = stopped && reference.audio[sample] == 0.0f;
            const bool passed = finite && stopped && error <= 1e-7f
                && reference.stereoError <= 1e-7f && reference.outputPeak > 0.1f
                && reference.outputPeak <= juce::Decibels::decibelsToGain (-1.0f) + 1e-5f
                && (mode != 1 || reference.rawPeak > 1.0f);
            ok = ok && passed;
            std::cout << "DIAGNOSTIC_TONE rate=" << rate << " mode=" << mode
                << " raw_peak=" << reference.rawPeak << " protected_peak=" << reference.outputPeak
                << " partition_error=" << error << " stopped=" << stopped
                << " result=" << (passed ? "PASS" : "FAIL") << '\n';
        }
    DiagnosticTestTone tone;
    tone.prepare (48000); tone.requestStart(); tone.processAdd (nullptr, 0, 64);
    tone.reset();
    juce::AudioBuffer<float> silence (2, 256); silence.clear();
    tone.processAdd (silence.getArrayOfWritePointers(), 2, 256);
    ok = ok && silence.getMagnitude (0, 256) == 0.0f;
    tone.requestStart(); tone.requestStop();
    tone.processAdd (silence.getArrayOfWritePointers(), 2, 256);
    ok = ok && silence.getMagnitude (0, 256) == 0.0f;
    for (double invalidRate : { std::numeric_limits<double>::quiet_NaN(),
                                std::numeric_limits<double>::infinity(), 1.0e30 })
    {
        tone.prepare (invalidRate); tone.requestStart(); silence.clear();
        tone.processAdd (silence.getArrayOfWritePointers(), 2, 256);
        for (int sample = 0; sample < 256; ++sample)
            ok = ok && std::isfinite (silence.getSample (0, sample))
                    && std::abs (silence.getSample (0, sample)) <= 0.5f;
    }
    std::cout << "DIAGNOSTIC_TONE_TESTS " << (ok ? "PASS" : "FAIL") << '\n';
    return ok;
}
