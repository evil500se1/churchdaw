#include "TimbreAudition.h"
#include "ParameterEditHistory.h"
#include "PluginProductIdentity.h"
#include <iostream>

int runFullUpgradeTests()
{
    int failures = 0;
    const auto expect = [&] (bool ok, const char* message)
    { if (! ok) { ++failures; std::cerr << "FAIL: " << message << '\n'; } };

    juce::PluginDescription au, vst;
    au.name = "Piano 8 (AU)"; au.manufacturerName = "Maker"; au.isInstrument = true;
    au.pluginFormatName = "AudioUnit";
    vst = au; vst.name = "Piano 8"; vst.pluginFormatName = "VST3";
    expect (ChurchDAWPluginProduct::key (au) == ChurchDAWPluginProduct::key (vst), "AU/VST3 must share a product");
    vst.name = "Piano 9";
    expect (ChurchDAWPluginProduct::key (au) != ChurchDAWPluginProduct::key (vst), "different major versions must stay distinct");
    vst = au; vst.isInstrument = false;
    expect (ChurchDAWPluginProduct::key (au) != ChurchDAWPluginProduct::key (vst), "instrument and FX editions must stay distinct");
    vst = au; vst.manufacturerName = "Other";
    expect (ChurchDAWPluginProduct::key (au) != ChurchDAWPluginProduct::key (vst), "same name from different makers must stay distinct");

    juce::AudioParameterFloat volume ("volume", "Volume", 0.0f, 1.0f, 0.3f);
    ParameterEditHistory history ({ &volume });
    volume.setValueNotifyingHost (0.45f);
    expect (! history.canUndo(), "MIDI/restore updates without GUI gestures must not enter undo");
    volume.beginChangeGesture(); volume.setValueNotifyingHost (0.8f); volume.endChangeGesture();
    expect (history.canUndo(), "a completed user gesture must be undoable");
    history.undo();
    expect (std::abs (volume.get() - 0.45f) < 0.00001f, "undo must restore the pre-drag value");
    expect (! history.canUndo() && history.canRedo(), "undo must not record itself");
    history.redo();
    expect (std::abs (volume.get() - 0.8f) < 0.00001f, "redo must restore the edited value");
    history.undo();
    volume.beginChangeGesture(); volume.setValueNotifyingHost (0.6f); volume.endChangeGesture();
    expect (! history.canRedo(), "new edits must discard the abandoned redo branch");
    history.clear(); volume.beginChangeGesture(); volume.endChangeGesture();
    expect (! history.canUndo(), "touch without a value change must not create undo");

    const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
        .getChildFile ("churchdaw-audition-" + juce::Uuid().toString() + ".wav");
    {
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (file.createOutputStream().release(), 48000, 2, 24, {}, 0));
        juce::AudioBuffer<float> sample (2, 48000);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < sample.getNumSamples(); ++i)
                sample.setSample (c, i, 0.8f * std::sin (i * 0.04f));
        expect (writer != nullptr && writer->writeFromAudioSampleBuffer (sample, 0, sample.getNumSamples()), "audition fixture must be written");
    }
    {
        TimbreAudition audition;
        audition.request (file);
        for (int i = 0; i < 500 && audition.getStatus() == TimbreAudition::Status::loading; ++i) juce::Thread::sleep (2);
        expect (audition.getStatus() == TimbreAudition::Status::playing, "local sample must decode asynchronously");
        juce::AudioBuffer<float> out (2, 512);
        out.clear(); audition.renderAdd (out, 44100);
        expect (out.getMagnitude (0, out.getNumSamples()) > 0, "preview must resample into the audio output");
        expect (out.getMagnitude (0, out.getNumSamples()) <= 0.0632f, "default preview peak must be at most -24 dBFS");
        audition.setLevelDb (100); out.clear(); audition.renderAdd (out, 44100);
        expect (out.getMagnitude (0, out.getNumSamples()) <= 0.2513f, "preview ceiling must clamp excessive gain");
        audition.stop (true); out.clear(); audition.renderAdd (out, 44100);
        expect (out.getMagnitude (0, out.getNumSamples()) == 0, "panic/device release must immediately silence preview");
        audition.request (file); audition.stop (true);
        juce::Thread::sleep (30); out.clear(); audition.renderAdd (out, 48000);
        expect (out.getMagnitude (0, out.getNumSamples()) == 0 && ! audition.isPlaying(), "cancelled decode must not resurrect audio");
        audition.request (file.getSiblingFile ("missing-" + juce::Uuid().toString() + ".wav"));
        for (int i = 0; i < 500 && audition.getStatus() == TimbreAudition::Status::loading; ++i) juce::Thread::sleep (2);
        expect (audition.getStatus() == TimbreAudition::Status::failed, "missing sample must produce a visible error");
        audition.request (file);
        for (int i = 0; i < 500 && audition.getStatus() == TimbreAudition::Status::loading; ++i) juce::Thread::sleep (2);
        for (int i = 0; i < 200; ++i) { out.clear(); audition.renderAdd (out, 48000); }
        expect (! audition.isPlaying(), "preview must stop at sample end without looping");
        out.clear(); audition.renderAdd (out, 48000);
        expect (out.getMagnitude (0, out.getNumSamples()) == 0, "finished audition must stay silent");
    }
    file.deleteFile();

    if (! failures) std::cout << "FULL_UPGRADE_OK\n";
    return failures ? 1 : 0;
}
