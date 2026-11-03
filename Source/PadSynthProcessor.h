#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <vector>

//==============================================================================
// Motor nativo do PAD EDITOR.
//
// Diferente do Pad Continuo (loops WAV por tom), este processador e um
// sintetizador polifonico de quatro camadas. Todos os controles exibidos pelo
// editor sao parametros reais do AudioProcessor e entram no caminho de audio.
// O processador nasce DESLIGADO para que atualizar o ChurchDAW nunca acrescente
// uma camada sonora a um setup existente sem uma escolha explicita do musico.
//==============================================================================
class PadSynthProcessor final : public juce::AudioProcessor,
                                public juce::ChangeBroadcaster
{
public:
    static constexpr int numLayers = 4;
    static constexpr int numPrograms = 16;
    static constexpr int maxVoices = 24;

    struct LayerParameters
    {
        juce::AudioParameterBool* enabled = nullptr;
        juce::AudioParameterChoice* osc1Wave = nullptr;
        juce::AudioParameterChoice* osc2Wave = nullptr;
        juce::AudioParameterChoice* subWave = nullptr;
        juce::AudioParameterChoice* noiseType = nullptr;
        juce::AudioParameterChoice* filterType = nullptr;
        juce::AudioParameterInt* osc1Octave = nullptr;
        juce::AudioParameterInt* osc2Octave = nullptr;
        juce::AudioParameterInt* subOctave = nullptr;
        juce::AudioParameterFloat* osc2Tune = nullptr;
        juce::AudioParameterFloat* osc1Level = nullptr;
        juce::AudioParameterFloat* osc2Level = nullptr;
        juce::AudioParameterFloat* subLevel = nullptr;
        juce::AudioParameterFloat* noiseLevel = nullptr;
        juce::AudioParameterFloat* cutoff = nullptr;
        juce::AudioParameterFloat* resonance = nullptr;
        juce::AudioParameterFloat* filterEnv = nullptr;
        juce::AudioParameterFloat* drive = nullptr;
        juce::AudioParameterFloat* attack = nullptr;
        juce::AudioParameterFloat* decay = nullptr;
        juce::AudioParameterFloat* sustain = nullptr;
        juce::AudioParameterFloat* release = nullptr;
        juce::AudioParameterFloat* modAttack = nullptr;
        juce::AudioParameterFloat* modDecay = nullptr;
        juce::AudioParameterFloat* modSustain = nullptr;
        juce::AudioParameterFloat* modRelease = nullptr;
        juce::AudioParameterFloat* pan = nullptr;
        juce::AudioParameterChoice* lfoWave = nullptr;
        juce::AudioParameterFloat* lfoRate = nullptr;
        juce::AudioParameterFloat* lfoDepth = nullptr;
        juce::AudioParameterFloat* lfoFade = nullptr;
        juce::AudioParameterFloat* lfoPhase = nullptr;
    };

    PadSynthProcessor()
        : juce::AudioProcessor (
              BusesProperties().withOutput (
                  "Pad Engine", juce::AudioChannelSet::stereo(), true))
    {
        engineEnabled = addParam (new juce::AudioParameterBool (
            { "padEngineEnabled", 1 }, "Pad Engine Ativo", false));
        masterDb = addParam (new juce::AudioParameterFloat (
            { "padMasterDb", 1 }, "Pad Master",
            juce::NormalisableRange<float> (-60.0f, 6.0f, 0.1f), -9.0f));
        midiChannel = addParam (new juce::AudioParameterInt (
            { "padMidiChannel", 1 }, "Pad Canal MIDI", 0, 16, 0));
        transpose = addParam (new juce::AudioParameterInt (
            { "padTranspose", 1 }, "Pad Transpose", -24, 24, 0));
        velocityCurve = addParam (new juce::AudioParameterChoice (
            { "padVelocityCurve", 1 }, "Pad Curva de Velocidade",
            { "Suave", "Normal", "Forte", "Fixa" }, 1));
        fixedVelocity = addParam (new juce::AudioParameterInt (
            { "padFixedVelocity", 1 }, "Pad Velocidade Fixa", 1, 127, 100));
        padColour = addParam (new juce::AudioParameterChoice (
            { "padColour", 1 }, "Pad Cor",
            { "Vermelho", "Laranja", "Amarelo", "Verde", "Ciano",
              "Azul", "Violeta", "Rosa", "Lavanda", "Branco" }, 0));
        padGroup = addParam (new juce::AudioParameterChoice (
            { "padGroup", 1 }, "Pad Grupo",
            { "Ambiencias", "Worship", "Profeticos", "Texturas" }, 0));
        holdMode = addParam (new juce::AudioParameterBool (
            { "padHoldMode", 1 }, "Pad Hold", true));
        toggleMode = addParam (new juce::AudioParameterBool (
            { "padToggleMode", 1 }, "Pad Toggle", false));

        toneLow = addParam (new juce::AudioParameterFloat (
            { "padToneLow", 1 }, "Pad EQ Low", -12.0f, 12.0f, 0.0f));
        toneMid = addParam (new juce::AudioParameterFloat (
            { "padToneMid", 1 }, "Pad EQ Mid", -12.0f, 12.0f, 0.0f));
        toneHigh = addParam (new juce::AudioParameterFloat (
            { "padToneHigh", 1 }, "Pad EQ High", -12.0f, 12.0f, 0.0f));

        chorusMix = addParam (new juce::AudioParameterFloat (
            { "padChorusMix", 1 }, "Pad Chorus",
            juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.20f));
        reverbMix = addParam (new juce::AudioParameterFloat (
            { "padReverbMix", 1 }, "Pad Reverb",
            juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.38f));
        delayMix = addParam (new juce::AudioParameterFloat (
            { "padDelayMix", 1 }, "Pad Delay",
            juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.12f));
        delayTime = addParam (new juce::AudioParameterFloat (
            { "padDelayTime", 1 }, "Pad Delay Time",
            juce::NormalisableRange<float> (0.08f, 1.20f, 0.001f, 0.45f), 0.42f));
        delayFeedback = addParam (new juce::AudioParameterFloat (
            { "padDelayFeedback", 1 }, "Pad Delay Feedback",
            juce::NormalisableRange<float> (0.0f, 0.82f, 0.001f), 0.28f));
        eqEnabled = addParam (new juce::AudioParameterBool (
            { "padEqEnabled", 1 }, "Pad EQ Ativo", true));
        chorusEnabled = addParam (new juce::AudioParameterBool (
            { "padChorusEnabled", 1 }, "Pad Chorus Ativo", true));
        reverbEnabled = addParam (new juce::AudioParameterBool (
            { "padReverbEnabled", 1 }, "Pad Reverb Ativo", true));
        delayEnabled = addParam (new juce::AudioParameterBool (
            { "padDelayEnabled", 1 }, "Pad Delay Ativo", true));

        macroAmbience = addParam (new juce::AudioParameterFloat (
            { "padMacroAmbience", 1 }, "Pad Macro Ambiencia", 0.0f, 1.0f, 0.50f));
        macroReverb = addParam (new juce::AudioParameterFloat (
            { "padMacroReverb", 1 }, "Pad Macro Reverb", 0.0f, 1.0f, 0.55f));
        macroDelay = addParam (new juce::AudioParameterFloat (
            { "padMacroDelay", 1 }, "Pad Macro Delay", 0.0f, 1.0f, 0.30f));
        macroMaster = addParam (new juce::AudioParameterFloat (
            { "padMacroMaster", 1 }, "Pad Macro Master", 0.0f, 1.0f, 0.72f));
        xyX = addParam (new juce::AudioParameterFloat (
            { "padXYX", 1 }, "Pad XY X", 0.0f, 1.0f, 0.50f));
        xyY = addParam (new juce::AudioParameterFloat (
            { "padXYY", 1 }, "Pad XY Y", 0.0f, 1.0f, 0.48f));
        fadeIn = addParam (new juce::AudioParameterFloat (
            { "padFadeIn", 1 }, "Pad Fade In",
            juce::NormalisableRange<float> (0.01f, 5.0f, 0.01f, 0.45f), 0.55f));
        fadeOut = addParam (new juce::AudioParameterFloat (
            { "padFadeOut", 1 }, "Pad Fade Out",
            juce::NormalisableRange<float> (0.01f, 8.0f, 0.01f, 0.45f), 1.20f));
        utilityAttack = addParam (new juce::AudioParameterFloat (
            { "padUtilityAttack", 1 }, "Pad Utility Attack",
            juce::NormalisableRange<float> (0.25f, 4.0f, 0.01f, 0.5f), 1.0f));
        utilityRelease = addParam (new juce::AudioParameterFloat (
            { "padUtilityRelease", 1 }, "Pad Utility Release",
            juce::NormalisableRange<float> (0.25f, 4.0f, 0.01f, 0.5f), 1.0f));
        gateEnabled = addParam (new juce::AudioParameterBool (
            { "padGateEnabled", 1 }, "Pad Gate Ativo", false));
        gateRate = addParam (new juce::AudioParameterChoice (
            { "padGateRate", 1 }, "Pad Gate Rate",
            { "1/4", "1/8", "1/8T", "1/16" }, 1));
        gateDepth = addParam (new juce::AudioParameterFloat (
            { "padGateDepth", 1 }, "Pad Gate Depth", 0.0f, 1.0f, 0.65f));

        for (int l = 0; l < numLayers; ++l)
            createLayerParameters (l);

        createFactoryPrograms();
        applyProgram (3, false);
    }

    ~PadSynthProcessor() override = default;

    //==============================================================================
    const juce::String getName() const override { return "ChurchDAW Pad Engine"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 9.0; }
    bool hasEditor() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override
    {
        return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
    }

    int getNumPrograms() override { return numPrograms; }
    int getCurrentProgram() override { return currentProgram.load(); }

    void setCurrentProgram (int index) override
    {
        selectProgram (index);
    }

    const juce::String getProgramName (int index) override
    {
        return juce::isPositiveAndBelow (index, numPrograms)
                 ? programs[(size_t) index].name
                 : juce::String();
    }

    void changeProgramName (int index, const juce::String& name) override
    {
        if (! juce::isPositiveAndBelow (index, numPrograms))
            return;

        programs[(size_t) index].name = name.trim().substring (0, 40);
        if (programs[(size_t) index].name.isEmpty())
            programs[(size_t) index].name = "Pad " + juce::String (index + 1);
        sendChangeMessage();
    }

    void prepareToPlay (double newSampleRate, int maximumBlockSize) override
    {
        sampleRate = newSampleRate >= 1000.0 ? newSampleRate : 44100.0;
        preparedBlockSize = juce::jmax (32, maximumBlockSize);
        voices.fill ({});
        pitchWheelSemis.fill (0.0f);
        sustainDown.fill (false);
        engineGain = engineEnabled != nullptr && engineEnabled->get() ? 1.0f : 0.0f;
        voiceStealDeclickMultiplier = std::exp (
            std::log (0.001f)
            / (float) juce::jmax (1.0, sampleRate * 0.005));

        const int chorusSize = juce::jmax (256, (int) std::ceil (sampleRate * 0.08));
        const int delaySize = juce::jmax (512, (int) std::ceil (sampleRate * 2.5));
        for (auto& channel : chorusBuffer)
            channel.assign ((size_t) chorusSize, 0.0f);
        for (auto& channel : delayBuffer)
            channel.assign ((size_t) delaySize, 0.0f);
        chorusWrite = delayWrite = 0;
        chorusPhase = 0.0f;
        gatePhase = 0.0f;

        juce::dsp::ProcessSpec spec {
            sampleRate, (juce::uint32) preparedBlockSize, 2
        };
        reverb.prepare (spec);
        reverb.reset();
        toneStates.fill ({});
        effectsAreSilent = true;
        silentTailSamples = 0;
        updateReverbParameters();
        outputPeak.store (0.0f, std::memory_order_relaxed);
    }

    void releaseResources() override
    {
        for (auto& channel : chorusBuffer) channel.clear();
        for (auto& channel : delayBuffer) channel.clear();
        reverb.reset();
        effectsAreSilent = true;
        silentTailSamples = 0;
    }

    void processBlock (juce::AudioBuffer<float>& buffer,
                       juce::MidiBuffer& midi) override
    {
        juce::ScopedNoDenormals noDenormals;
        buffer.clear();

        if (buffer.getNumChannels() < 2 || buffer.getNumSamples() <= 0)
            return;

        const auto settings = readBlockSettings();
        collectMidi (midi, buffer.getNumSamples());

        if (panicRequested.exchange (false, std::memory_order_acq_rel))
            for (int channel = 1; channel <= 16; ++channel)
                allNotesOff (channel, true);

        const auto hasActiveVoices = [this]() noexcept
        {
            return std::any_of (voices.begin(), voices.end(),
                                [] (const Voice& voice) { return voice.active; });
        };

        // Depois que a cauda ficou realmente silenciosa, um bloco sem vozes
        // nao precisa atravessar osciladores, chorus, EQ e reverb. O ganho do
        // engine ainda avanca pelo numero exato de amostras, preservando o
        // estado que existiria quando a proxima nota chegar.
        if (midiEventCount == 0 && ! hasActiveVoices() && effectsAreSilent)
        {
            const float target = settings.engineEnabled ? 1.0f : 0.0f;
            const float step = 1.0f / (float) juce::jmax (
                1.0, sampleRate * (target > engineGain ? settings.fadeIn
                                                       : settings.fadeOut));
            if (engineGain < target)
                engineGain = juce::jmin (target,
                    engineGain + step * (float) buffer.getNumSamples());
            else if (engineGain > target)
                engineGain = juce::jmax (target,
                    engineGain - step * (float) buffer.getNumSamples());
            return;
        }

        prepareActiveVoiceIncrements (settings);

        auto* left = buffer.getWritePointer (0);
        auto* right = buffer.getWritePointer (1);
        int eventIndex = 0;
        float peak = 0.0f;
        const float target = settings.engineEnabled ? 1.0f : 0.0f;
        const float engineGainStepUp = 1.0f / (float) juce::jmax (
            1.0, sampleRate * settings.fadeIn);
        const float engineGainStepDown = 1.0f / (float) juce::jmax (
            1.0, sampleRate * settings.fadeOut);
        const float macroGain = juce::Decibels::decibelsToGain (
            settings.masterDb + juce::jmap (settings.macroMaster,
                                            -8.0f, 2.0f));

        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
        {
            while (eventIndex < midiEventCount
                    && midiEvents[(size_t) eventIndex].sample <= sample)
            {
                handleMidiEvent (midiEvents[(size_t) eventIndex], settings);
                ++eventIndex;
            }

            if (engineGain < target)
                engineGain = juce::jmin (target, engineGain + engineGainStepUp);
            else if (engineGain > target)
                engineGain = juce::jmax (target, engineGain - engineGainStepDown);

            float l = 0.0f, r = 0.0f;
            // Mesmo inaudivel, o motor precisa continuar avancando fases e
            // envelopes. Caso contrario uma release congela quando o fade do
            // engine chega a zero e a voz reaparece ao religar o PadSynth.
            renderVoices (settings, l, r);

            float gateGain = 1.0f;
            if (settings.gateEnabled)
            {
                static constexpr float rateMultipliers[4] = { 1.0f, 2.0f, 3.0f, 4.0f };
                gatePhase += (2.0f * rateMultipliers[juce::jlimit (0, 3, settings.gateRate)])
                           / (float) sampleRate; // referencia musical a 120 BPM
                gatePhase -= std::floor (gatePhase);
                const float pulse = 0.5f + 0.5f * std::sin (
                    juce::MathConstants<float>::twoPi * gatePhase);
                gateGain = juce::jmap (settings.gateDepth, 1.0f, pulse);
            }
            left[sample] = l * engineGain * macroGain * gateGain;
            right[sample] = r * engineGain * macroGain * gateGain;
        }

        processChorusAndDelay (buffer, settings);
        processTone (buffer, settings);

        updateReverbParameters (settings);
        {
            juce::dsp::AudioBlock<float> audioBlock (buffer);
            juce::dsp::ProcessContextReplacing<float> context (audioBlock);
            reverb.process (context);
        }

        for (int ch = 0; ch < 2; ++ch)
        {
            const auto* data = buffer.getReadPointer (ch);
            for (int i = 0; i < buffer.getNumSamples(); ++i)
                peak = juce::jmax (peak, std::abs (data[i]));
        }
        outputPeak.store (juce::jmax (outputPeak.load (std::memory_order_relaxed), peak),
                          std::memory_order_relaxed);

        if (hasActiveVoices() || peak > 0.0000001f)
        {
            silentTailSamples = 0;
            effectsAreSilent = false;
        }
        else
        {
            silentTailSamples += buffer.getNumSamples();
            if (silentTailSamples >= (int64_t) (sampleRate * 4.0))
            {
                effectsAreSilent = true;
                for (auto& channel : chorusBuffer) channel.clear();
                for (auto& channel : delayBuffer) channel.clear();
                toneStates.fill ({});
                reverb.reset();
            }
        }
    }

    //==============================================================================
    LayerParameters& layer (int index) noexcept
    {
        return layers[(size_t) juce::jlimit (0, numLayers - 1, index)];
    }

    const LayerParameters& layer (int index) const noexcept
    {
        return layers[(size_t) juce::jlimit (0, numLayers - 1, index)];
    }

    juce::RangedAudioParameter* parameter (const juce::String& id) const noexcept
    {
        const auto found = parameterById.find (id);
        return found != parameterById.end() ? found->second : nullptr;
    }

    bool isEngineEnabled() const noexcept
    {
        return engineEnabled != nullptr && engineEnabled->get();
    }

    void setEngineEnabled (bool enabled)
    {
        if (engineEnabled != nullptr)
            engineEnabled->setValueNotifyingHost (enabled ? 1.0f : 0.0f);
        sendChangeMessage();
    }

    // Message thread pede; a thread de audio executa no limite do bloco.
    // Assim o PANIC da tela Performance nao disputa o array de vozes com o DSP.
    void requestPanic() noexcept
    {
        panicRequested.store (true, std::memory_order_release);
    }

    float readAndResetOutputPeak() noexcept
    {
        return outputPeak.exchange (0.0f, std::memory_order_relaxed);
    }

    void selectProgram (int index)
    {
        index = juce::jlimit (0, numPrograms - 1, index);
        const int previous = currentProgram.load();
        if (juce::isPositiveAndBelow (previous, numPrograms))
            captureProgram (previous);
        applyProgram (index, true);
    }

    void saveCurrentProgram()
    {
        captureProgram (currentProgram.load());
        sendChangeMessage();
    }

    bool isProgramFavourite (int index) const noexcept
    {
        return juce::isPositiveAndBelow (index, numPrograms)
                && favouritePrograms[(size_t) index];
    }

    void setProgramFavourite (int index, bool favourite)
    {
        if (! juce::isPositiveAndBelow (index, numPrograms)) return;
        favouritePrograms[(size_t) index] = favourite;
        sendChangeMessage();
    }

    void copyCurrentProgram()
    {
        clipboard = captureParameterState();
    }

    bool canPasteProgram() const noexcept { return clipboard.isValid(); }

    void pasteProgram()
    {
        if (! clipboard.isValid())
            return;
        applyParameterState (clipboard);
        captureProgram (currentProgram.load());
        sendChangeMessage();
    }

    //==============================================================================
    void getStateInformation (juce::MemoryBlock& destData) override
    {
        juce::XmlElement root ("PADSYNTH");
        root.setAttribute ("version", 1);
        root.setAttribute ("currentProgram", currentProgram.load());
        appendStateXml (root, "PARAMETERS", captureParameterState());

        auto* bank = root.createNewChildElement ("PROGRAMS");
        for (int i = 0; i < numPrograms; ++i)
        {
            auto* programXml = bank->createNewChildElement ("PROGRAM");
            programXml->setAttribute ("index", i);
            programXml->setAttribute ("name", programs[(size_t) i].name);
            programXml->setAttribute ("favourite",
                                      favouritePrograms[(size_t) i]);
            appendStateXml (*programXml, "STATE", programs[(size_t) i].state);
        }

        copyXmlToBinary (root, destData);
    }

    void setStateInformation (const void* data, int sizeInBytes) override
    {
        auto root = getXmlFromBinary (data, sizeInBytes);
        if (root == nullptr || ! root->hasTagName ("PADSYNTH"))
            return;

        if (auto* bank = root->getChildByName ("PROGRAMS"))
            for (auto* p : bank->getChildIterator())
            {
                if (! p->hasTagName ("PROGRAM"))
                    continue;
                const int index = p->getIntAttribute ("index", -1);
                if (! juce::isPositiveAndBelow (index, numPrograms))
                    continue;
                const auto currentFactoryName = programs[(size_t) index].name;
                auto savedName = p->getStringAttribute (
                    "name", "Pad " + juce::String (index + 1));
                static const char* legacyFactoryNames[numPrograms] = {
                    "Ambience Heaven", "Worship Warm", "Prophetic Air", "Deep Prayer",
                    "Sunday Glow", "Cloud Motion", "Wide Shimmer", "Soft Sanctuary",
                    "Living Water", "Redemption", "Holy Atmosphere", "Quiet Altar",
                    "Open Skies", "Velvet Pad", "Majestic Rise", "Custom Pad"
                };
                if (savedName == legacyFactoryNames[index])
                    savedName = currentFactoryName;
                programs[(size_t) index].name = savedName;
                favouritePrograms[(size_t) index] =
                    p->getBoolAttribute ("favourite", false);
                if (auto* state = p->getChildByName ("STATE"))
                    programs[(size_t) index].state = readStateXml (*state);
            }

        if (auto* params = root->getChildByName ("PARAMETERS"))
            applyParameterState (readStateXml (*params));

        currentProgram.store (juce::jlimit (
            0, numPrograms - 1, root->getIntAttribute ("currentProgram", 0)));
        sendChangeMessage();
    }

    // Parametros globais usados pelo editor/attachments.
    juce::AudioParameterBool* engineEnabled = nullptr;
    juce::AudioParameterFloat* masterDb = nullptr;
    juce::AudioParameterInt* midiChannel = nullptr;
    juce::AudioParameterInt* transpose = nullptr;
    juce::AudioParameterChoice* velocityCurve = nullptr;
    juce::AudioParameterInt* fixedVelocity = nullptr;
    juce::AudioParameterChoice* padColour = nullptr;
    juce::AudioParameterChoice* padGroup = nullptr;
    juce::AudioParameterBool* holdMode = nullptr;
    juce::AudioParameterBool* toggleMode = nullptr;
    juce::AudioParameterFloat* toneLow = nullptr;
    juce::AudioParameterFloat* toneMid = nullptr;
    juce::AudioParameterFloat* toneHigh = nullptr;
    juce::AudioParameterFloat* chorusMix = nullptr;
    juce::AudioParameterFloat* reverbMix = nullptr;
    juce::AudioParameterFloat* delayMix = nullptr;
    juce::AudioParameterFloat* delayTime = nullptr;
    juce::AudioParameterFloat* delayFeedback = nullptr;
    juce::AudioParameterBool* eqEnabled = nullptr;
    juce::AudioParameterBool* chorusEnabled = nullptr;
    juce::AudioParameterBool* reverbEnabled = nullptr;
    juce::AudioParameterBool* delayEnabled = nullptr;
    juce::AudioParameterFloat* macroAmbience = nullptr;
    juce::AudioParameterFloat* macroReverb = nullptr;
    juce::AudioParameterFloat* macroDelay = nullptr;
    juce::AudioParameterFloat* macroMaster = nullptr;
    juce::AudioParameterFloat* xyX = nullptr;
    juce::AudioParameterFloat* xyY = nullptr;
    juce::AudioParameterFloat* fadeIn = nullptr;
    juce::AudioParameterFloat* fadeOut = nullptr;
    juce::AudioParameterFloat* utilityAttack = nullptr;
    juce::AudioParameterFloat* utilityRelease = nullptr;
    juce::AudioParameterBool* gateEnabled = nullptr;
    juce::AudioParameterChoice* gateRate = nullptr;
    juce::AudioParameterFloat* gateDepth = nullptr;

#if defined (CHURCHDAW_INTERNAL_AUDIO_TEST_HOOKS)
    friend struct PadSynthProcessorTestAccess;
#endif

private:
    template <typename Parameter>
    Parameter* addParam (Parameter* value)
    {
        addParameter (value);
        parameterById[value->getParameterID()] = value;
        return value;
    }

    static juce::String layerId (int layerIndex, const char* suffix)
    {
        return "padL" + juce::String (layerIndex + 1) + suffix;
    }

    void createLayerParameters (int index)
    {
        auto& p = layers[(size_t) index];
        const auto display = "Pad Camada " + juce::String (index + 1) + " ";
        const bool firstTwo = index < 2;

        p.enabled = addParam (new juce::AudioParameterBool (
            { layerId (index, "Enabled"), 1 }, display + "Ativa", firstTwo));
        p.osc1Wave = addParam (new juce::AudioParameterChoice (
            { layerId (index, "Osc1Wave"), 1 }, display + "OSC 1 Forma",
            { "Seno", "Triangulo", "Serra", "Quadrada", "Warm" },
            index == 0 ? 2 : index == 1 ? 1 : 0));
        p.osc2Wave = addParam (new juce::AudioParameterChoice (
            { layerId (index, "Osc2Wave"), 1 }, display + "OSC 2 Forma",
            { "Seno", "Triangulo", "Serra", "Quadrada", "Warm" },
            index == 0 ? 1 : 0));
        p.subWave = addParam (new juce::AudioParameterChoice (
            { layerId (index, "SubWave"), 1 }, display + "Sub Forma",
            { "Seno", "Triangulo", "Quadrada" }, 0));
        p.noiseType = addParam (new juce::AudioParameterChoice (
            { layerId (index, "NoiseType"), 1 }, display + "Noise Tipo",
            { "White", "Pink", "Digital" }, 0));
        p.filterType = addParam (new juce::AudioParameterChoice (
            { layerId (index, "FilterType"), 1 }, display + "Filtro Tipo",
            { "Low Pass 24dB", "Low Pass 12dB", "High Pass", "Band Pass" }, 0));
        p.osc1Octave = addParam (new juce::AudioParameterInt (
            { layerId (index, "Osc1Oct"), 1 }, display + "OSC 1 Oitava", -2, 2,
            index == 1 ? 1 : 0));
        p.osc2Octave = addParam (new juce::AudioParameterInt (
            { layerId (index, "Osc2Oct"), 1 }, display + "OSC 2 Oitava", -2, 2,
            index == 0 ? -1 : 0));
        p.subOctave = addParam (new juce::AudioParameterInt (
            { layerId (index, "SubOct"), 1 }, display + "Sub Oitava", -3, 0, -2));
        p.osc2Tune = addParam (new juce::AudioParameterFloat (
            { layerId (index, "Osc2Tune"), 1 }, display + "OSC 2 Tune",
            -50.0f, 50.0f, index == 0 ? 7.0f : -5.0f));
        p.osc1Level = addParam (new juce::AudioParameterFloat (
            { layerId (index, "Osc1Level"), 1 }, display + "OSC 1 Nivel",
            0.0f, 1.0f, firstTwo ? 0.62f : 0.45f));
        p.osc2Level = addParam (new juce::AudioParameterFloat (
            { layerId (index, "Osc2Level"), 1 }, display + "OSC 2 Nivel",
            0.0f, 1.0f, firstTwo ? 0.34f : 0.20f));
        p.subLevel = addParam (new juce::AudioParameterFloat (
            { layerId (index, "SubLevel"), 1 }, display + "Sub Nivel",
            0.0f, 1.0f, index == 0 ? 0.16f : 0.0f));
        p.noiseLevel = addParam (new juce::AudioParameterFloat (
            { layerId (index, "NoiseLevel"), 1 }, display + "Noise Nivel",
            0.0f, 1.0f, index == 1 ? 0.035f : 0.0f));
        p.cutoff = addParam (new juce::AudioParameterFloat (
            { layerId (index, "Cutoff"), 1 }, display + "Cutoff",
            juce::NormalisableRange<float> (70.0f, 18000.0f, 0.1f, 0.28f),
            index == 0 ? 2800.0f : 4200.0f));
        p.resonance = addParam (new juce::AudioParameterFloat (
            { layerId (index, "Resonance"), 1 }, display + "Resonancia",
            0.0f, 1.0f, 0.18f));
        p.filterEnv = addParam (new juce::AudioParameterFloat (
            { layerId (index, "FilterEnv"), 1 }, display + "Envelope do Filtro",
            -1.0f, 1.0f, 0.22f));
        p.drive = addParam (new juce::AudioParameterFloat (
            { layerId (index, "Drive"), 1 }, display + "Drive",
            0.0f, 1.0f, 0.0f));
        p.attack = addParam (new juce::AudioParameterFloat (
            { layerId (index, "Attack"), 1 }, display + "Attack",
            juce::NormalisableRange<float> (0.003f, 8.0f, 0.001f, 0.34f),
            index == 0 ? 0.75f : 1.25f));
        p.decay = addParam (new juce::AudioParameterFloat (
            { layerId (index, "Decay"), 1 }, display + "Decay",
            juce::NormalisableRange<float> (0.02f, 8.0f, 0.001f, 0.40f), 1.4f));
        p.sustain = addParam (new juce::AudioParameterFloat (
            { layerId (index, "Sustain"), 1 }, display + "Sustain",
            0.0f, 1.0f, 0.82f));
        p.release = addParam (new juce::AudioParameterFloat (
            { layerId (index, "Release"), 1 }, display + "Release",
            juce::NormalisableRange<float> (0.04f, 14.0f, 0.001f, 0.34f),
            index == 0 ? 4.2f : 5.8f));
        p.modAttack = addParam (new juce::AudioParameterFloat (
            { layerId (index, "ModAttack"), 1 }, display + "Mod Attack",
            juce::NormalisableRange<float> (0.003f, 8.0f, 0.001f, 0.34f), 0.50f));
        p.modDecay = addParam (new juce::AudioParameterFloat (
            { layerId (index, "ModDecay"), 1 }, display + "Mod Decay",
            juce::NormalisableRange<float> (0.02f, 8.0f, 0.001f, 0.40f), 0.80f));
        p.modSustain = addParam (new juce::AudioParameterFloat (
            { layerId (index, "ModSustain"), 1 }, display + "Mod Sustain",
            0.0f, 1.0f, 0.60f));
        p.modRelease = addParam (new juce::AudioParameterFloat (
            { layerId (index, "ModRelease"), 1 }, display + "Mod Release",
            juce::NormalisableRange<float> (0.04f, 14.0f, 0.001f, 0.34f), 1.50f));
        p.pan = addParam (new juce::AudioParameterFloat (
            { layerId (index, "Pan"), 1 }, display + "Pan",
            -1.0f, 1.0f, index == 0 ? -0.18f : index == 1 ? 0.18f : 0.0f));
        p.lfoWave = addParam (new juce::AudioParameterChoice (
            { layerId (index, "LfoWave"), 1 }, display + "LFO Forma",
            { "Seno", "Triangulo", "Serra", "Sample Hold" }, 0));
        p.lfoRate = addParam (new juce::AudioParameterFloat (
            { layerId (index, "LfoRate"), 1 }, display + "LFO Rate",
            juce::NormalisableRange<float> (0.02f, 12.0f, 0.001f, 0.33f),
            index == 0 ? 0.16f : 0.09f));
        p.lfoDepth = addParam (new juce::AudioParameterFloat (
            { layerId (index, "LfoDepth"), 1 }, display + "LFO Depth",
            0.0f, 1.0f, index == 0 ? 0.18f : 0.12f));
        p.lfoFade = addParam (new juce::AudioParameterFloat (
            { layerId (index, "LfoFade"), 1 }, display + "LFO Fade",
            juce::NormalisableRange<float> (0.0f, 8.0f, 0.01f, 0.45f), 1.5f));
        p.lfoPhase = addParam (new juce::AudioParameterFloat (
            { layerId (index, "LfoPhase"), 1 }, display + "LFO Phase",
            0.0f, 360.0f, 0.0f));
    }

    enum class EnvelopeStage : uint8_t { idle, attack, decay, sustain, release };

    struct Envelope
    {
        EnvelopeStage stage = EnvelopeStage::idle;
        float level = 0.0f;

        void noteOn() noexcept { stage = EnvelopeStage::attack; }
        void noteOff() noexcept
        {
            if (stage != EnvelopeStage::idle)
                stage = EnvelopeStage::release;
        }

        float next (float attack, float decay, float sustain, float release,
                    double sr) noexcept
        {
            switch (stage)
            {
                case EnvelopeStage::idle: return 0.0f;
                case EnvelopeStage::attack:
                    level += 1.0f / (float) juce::jmax (1.0, sr * attack);
                    if (level >= 1.0f) { level = 1.0f; stage = EnvelopeStage::decay; }
                    break;
                case EnvelopeStage::decay:
                    level -= (1.0f - sustain) /
                             (float) juce::jmax (1.0, sr * decay);
                    if (level <= sustain) { level = sustain; stage = EnvelopeStage::sustain; }
                    break;
                case EnvelopeStage::sustain:
                    level = sustain;
                    break;
                case EnvelopeStage::release:
                    level -= juce::jmax (0.0000001f, level) /
                             (float) juce::jmax (1.0, sr * release * 0.55);
                    if (level < 0.00002f) { level = 0.0f; stage = EnvelopeStage::idle; }
                    break;
            }
            return level;
        }

        bool active() const noexcept { return stage != EnvelopeStage::idle; }
    };

    struct FilterState { float s1 = 0.0f, s2 = 0.0f; };

    struct VoiceLayer
    {
        Envelope envelope;
        Envelope modEnvelope;
        FilterState filterL, filterR, filter2L, filter2R;
        float osc1Phase = 0.0f, osc2Phase = 0.0f, subPhase = 0.0f;
        float lfoPhase = 0.0f;
        float heldNoise = 0.0f;
        float pinkNoise = 0.0f;
        float osc1Increment = 0.0f, osc2Increment = 0.0f;
        float subIncrement = 0.0f, lfoIncrement = 0.0f;
    };

    struct Voice
    {
        bool active = false;
        bool keyDown = false;
        bool sustained = false;
        int note = -1;
        int channel = 1;
        float velocity = 0.0f;
        uint32_t age = 0;
        uint64_t samplesAlive = 0;
        uint32_t noise = 0x1234567u;
        float lastOutputL = 0.0f, lastOutputR = 0.0f;
        float stealDeclickL = 0.0f, stealDeclickR = 0.0f;
        std::array<VoiceLayer, numLayers> layers;
    };

    struct LayerSettings
    {
        bool enabled = false;
        int osc1Wave = 0, osc2Wave = 0, subWave = 0, noiseType = 0;
        int filterType = 0, osc1Octave = 0, osc2Octave = 0, subOctave = -2;
        int lfoWave = 0;
        float osc2Tune = 0.0f, osc1Level = 0.0f, osc2Level = 0.0f;
        float subLevel = 0.0f, noiseLevel = 0.0f;
        float cutoff = 1000.0f, resonance = 0.0f, filterEnv = 0.0f, drive = 0.0f;
        float filterR = 1.0f / 0.55f;
        float driveGain = 1.0f, driveDenominator = 1.0f;
        float attack = 0.1f, decay = 1.0f, sustain = 0.8f, release = 2.0f;
        float modAttack = 0.5f, modDecay = 0.8f, modSustain = 0.6f, modRelease = 1.5f;
        float pan = 0.0f, lfoRate = 0.1f, lfoDepth = 0.0f, lfoFade = 0.0f;
        float lfoPhase = 0.0f;
    };

    struct BlockSettings
    {
        bool engineEnabled = false;
        int midiChannel = 0, transpose = 0, velocityCurve = 1, fixedVelocity = 100;
        bool holdMode = true, toggleMode = false;
        float masterDb = -9.0f, chorus = 0.0f, reverb = 0.0f;
        bool eqEnabled = true, chorusEnabled = true;
        bool reverbEnabled = true, delayEnabled = true;
        float toneLow = 0.0f, toneMid = 0.0f, toneHigh = 0.0f;
        float delay = 0.0f, delayTime = 0.4f, delayFeedback = 0.25f;
        float macroAmbience = 0.5f, macroReverb = 0.5f;
        float macroDelay = 0.3f, macroMaster = 0.7f;
        float xyX = 0.5f, xyY = 0.5f, fadeIn = 0.5f, fadeOut = 1.0f;
        float utilityAttack = 1.0f, utilityRelease = 1.0f;
        bool gateEnabled = false;
        int gateRate = 1;
        float gateDepth = 0.65f;
        std::array<LayerSettings, numLayers> layer;
    };

    BlockSettings readBlockSettings() const noexcept
    {
        BlockSettings s;
        s.engineEnabled = engineEnabled->get();
        s.masterDb = masterDb->get();
        s.midiChannel = midiChannel->get();
        s.transpose = transpose->get();
        s.velocityCurve = velocityCurve->getIndex();
        s.fixedVelocity = fixedVelocity->get();
        s.holdMode = holdMode->get();
        s.toggleMode = toggleMode->get();
        s.toneLow = toneLow->get();
        s.toneMid = toneMid->get();
        s.toneHigh = toneHigh->get();
        s.chorus = chorusMix->get();
        s.reverb = reverbMix->get();
        s.delay = delayMix->get();
        s.delayTime = delayTime->get();
        s.delayFeedback = delayFeedback->get();
        s.eqEnabled = eqEnabled->get();
        s.chorusEnabled = chorusEnabled->get();
        s.reverbEnabled = reverbEnabled->get();
        s.delayEnabled = delayEnabled->get();
        s.macroAmbience = macroAmbience->get();
        s.macroReverb = macroReverb->get();
        s.macroDelay = macroDelay->get();
        s.macroMaster = macroMaster->get();
        s.xyX = xyX->get();
        s.xyY = xyY->get();
        s.fadeIn = fadeIn->get();
        s.fadeOut = fadeOut->get();
        s.utilityAttack = utilityAttack->get();
        s.utilityRelease = utilityRelease->get();
        s.gateEnabled = gateEnabled->get();
        s.gateRate = gateRate->getIndex();
        s.gateDepth = gateDepth->get();

        for (int l = 0; l < numLayers; ++l)
        {
            const auto& p = layers[(size_t) l];
            auto& d = s.layer[(size_t) l];
            d.enabled = p.enabled->get();
            d.osc1Wave = p.osc1Wave->getIndex();
            d.osc2Wave = p.osc2Wave->getIndex();
            d.subWave = p.subWave->getIndex();
            d.noiseType = p.noiseType->getIndex();
            d.filterType = p.filterType->getIndex();
            d.osc1Octave = p.osc1Octave->get();
            d.osc2Octave = p.osc2Octave->get();
            d.subOctave = p.subOctave->get();
            d.osc2Tune = p.osc2Tune->get();
            d.osc1Level = p.osc1Level->get();
            d.osc2Level = p.osc2Level->get();
            d.subLevel = p.subLevel->get();
            d.noiseLevel = p.noiseLevel->get();
            d.cutoff = p.cutoff->get();
            d.resonance = p.resonance->get();
            d.filterEnv = p.filterEnv->get();
            d.drive = p.drive->get();
            d.filterR = 1.0f / juce::jmap (
                juce::jlimit (0.0f, 1.0f, d.resonance), 0.55f, 7.5f);
            d.driveGain = 1.0f + d.drive * 14.0f;
            d.driveDenominator = d.drive > 0.0001f
                                   ? std::tanh (d.driveGain)
                                   : 1.0f;
            d.attack = p.attack->get();
            d.decay = p.decay->get();
            d.sustain = p.sustain->get();
            d.release = p.release->get();
            d.modAttack = p.modAttack->get();
            d.modDecay = p.modDecay->get();
            d.modSustain = p.modSustain->get();
            d.modRelease = p.modRelease->get();
            d.pan = p.pan->get();
            d.lfoWave = p.lfoWave->getIndex();
            d.lfoRate = p.lfoRate->get();
            d.lfoDepth = p.lfoDepth->get();
            d.lfoFade = p.lfoFade->get();
            d.lfoPhase = p.lfoPhase->get();
        }
        return s;
    }

    struct RawMidiEvent
    {
        int sample = 0;
        uint8_t status = 0, data1 = 0, data2 = 0;
    };

    void collectMidi (const juce::MidiBuffer& midi, int blockSamples) noexcept
    {
        midiEventCount = 0;
        for (const auto metadata : midi)
        {
            if (midiEventCount >= (int) midiEvents.size()
                || metadata.data == nullptr || metadata.numBytes <= 0)
                continue;
            RawMidiEvent e;
            e.sample = juce::jlimit (0, juce::jmax (0, blockSamples - 1),
                                     (int) metadata.samplePosition);
            e.status = metadata.data[0];
            e.data1 = metadata.numBytes > 1 ? metadata.data[1] : 0;
            e.data2 = metadata.numBytes > 2 ? metadata.data[2] : 0;
            midiEvents[(size_t) midiEventCount++] = e;
        }
    }

    bool acceptsChannel (int channel, const BlockSettings& s) const noexcept
    {
        return s.midiChannel == 0 || s.midiChannel == channel;
    }

    void handleMidiEvent (const RawMidiEvent& event,
                          const BlockSettings& settings) noexcept
    {
        if (event.status < 0x80 || event.status >= 0xf0)
            return;
        const int type = event.status & 0xf0;
        const int channel = (event.status & 0x0f) + 1;
        if (! acceptsChannel (channel, settings))
            return;

        if (type == 0x90 && event.data2 > 0)
        {
            if (settings.engineEnabled)
            {
                noteOn ((int) event.data1, channel,
                        velocityFor ((float) event.data2 / 127.0f,
                                     settings.velocityCurve,
                                     settings.fixedVelocity),
                        settings.toggleMode);
                prepareActiveVoiceIncrements (settings, channel);
            }
        }
        else if (type == 0x80 || (type == 0x90 && event.data2 == 0))
            noteOff ((int) event.data1, channel,
                     settings.holdMode || settings.toggleMode);
        else if (type == 0xb0 && event.data1 == 64)
            setSustain (channel, event.data2 >= 64);
        else if (type == 0xb0 && (event.data1 == 120 || event.data1 == 123))
            allNotesOff (channel, event.data1 == 120);
        else if (type == 0xe0)
        {
            const int value = ((int) event.data2 << 7) | event.data1;
            pitchWheelSemis[(size_t) (channel - 1)] =
                juce::jmap ((float) value, 0.0f, 16383.0f, -2.0f, 2.0f);
            prepareActiveVoiceIncrements (settings, channel);
        }
    }

    static float velocityFor (float value, int curve, int fixed) noexcept
    {
        value = juce::jlimit (0.0f, 1.0f, value);
        if (curve == 0) return std::sqrt (value);
        if (curve == 2) return value * value;
        if (curve == 3) return (float) juce::jlimit (1, 127, fixed) / 127.0f;
        return value;
    }

    void noteOn (int note, int channel, float velocity, bool toggle) noexcept
    {
        if (toggle)
            for (auto& activeVoice : voices)
                if (activeVoice.active && activeVoice.note == note
                     && activeVoice.channel == channel)
                {
                    activeVoice.keyDown = activeVoice.sustained = false;
                    for (auto& l : activeVoice.layers)
                    {
                        l.envelope.noteOff();
                        l.modEnvelope.noteOff();
                    }
                    return;
                }

        Voice* target = nullptr;
        for (auto& v : voices)
            if (! v.active) { target = &v; break; }

        if (target == nullptr)
            target = &*std::min_element (voices.begin(), voices.end(),
                [] (const Voice& a, const Voice& b) { return a.age < b.age; });

        // Carrega a ultima contribuicao da voz roubada para uma rampa curta.
        // O novo ataque ocupa o mesmo slot fixo, portanto nao ha alocacao nem
        // aumento da polifonia no callback.
        const float declickL = target->active ? target->lastOutputL : 0.0f;
        const float declickR = target->active ? target->lastOutputR : 0.0f;
        *target = {};
        target->active = target->keyDown = true;
        target->note = note;
        target->channel = channel;
        target->velocity = velocity;
        target->age = ++voiceAge;
        target->noise = 0x9e3779b9u ^ (uint32_t) (note * 65537 + channel * 97 + voiceAge);
        target->stealDeclickL = declickL;
        target->stealDeclickR = declickR;
        for (auto& l : target->layers)
        {
            l.envelope.noteOn();
            l.modEnvelope.noteOn();
        }
    }

    void noteOff (int note, int channel, bool latched) noexcept
    {
        if (latched)
            return;

        Voice* target = nullptr;

        for (auto& v : voices)
            if (v.active && v.note == note && v.channel == channel && v.keyDown)
                if (target == nullptr || v.age < target->age)
                    target = &v;

        if (target == nullptr)
            return;

        target->keyDown = false;
        if (sustainDown[(size_t) (channel - 1)])
            target->sustained = true;
        else
            for (auto& l : target->layers)
            {
                l.envelope.noteOff();
                l.modEnvelope.noteOff();
            }
    }

    void setSustain (int channel, bool down) noexcept
    {
        sustainDown[(size_t) (channel - 1)] = down;
        if (! down)
            for (auto& v : voices)
                if (v.active && v.channel == channel && v.sustained && ! v.keyDown)
                {
                    v.sustained = false;
                    for (auto& l : v.layers)
                    {
                        l.envelope.noteOff();
                        l.modEnvelope.noteOff();
                    }
                }
    }

    void allNotesOff (int channel, bool hard) noexcept
    {
        sustainDown[(size_t) (channel - 1)] = false;
        for (auto& v : voices)
            if (v.active && v.channel == channel)
            {
                v.keyDown = v.sustained = false;
                if (hard) v = {};
                else for (auto& l : v.layers)
                {
                    l.envelope.noteOff();
                    l.modEnvelope.noteOff();
                }
            }
    }

    static float polyBlep (float phase, float increment) noexcept
    {
        if (phase < increment)
        {
            const float t = phase / increment;
            return t + t - t * t - 1.0f;
        }
        if (phase > 1.0f - increment)
        {
            const float t = (phase - 1.0f) / increment;
            return t * t + t + t + 1.0f;
        }
        return 0.0f;
    }

    static float waveform (int type, float phase, float increment) noexcept
    {
        const float pi2 = juce::MathConstants<float>::twoPi;
        switch (type)
        {
            case 0: return std::sin (pi2 * phase);
            case 1: return 1.0f - 4.0f * std::abs (phase - 0.5f);
            case 2: return (2.0f * phase - 1.0f) - polyBlep (phase, increment);
            case 3:
            {
                float y = phase < 0.5f ? 1.0f : -1.0f;
                y += polyBlep (phase, increment);
                float shifted = std::fmod (phase + 0.5f, 1.0f);
                y -= polyBlep (shifted, increment);
                return y;
            }
            default:
                return 0.72f * std::sin (pi2 * phase)
                     + 0.20f * std::sin (pi2 * phase * 2.0f)
                     + 0.08f * std::sin (pi2 * phase * 3.0f);
        }
    }

    static float nextPhase (float& phase, float increment) noexcept
    {
        phase += increment;
        phase -= std::floor (phase);
        return phase;
    }

    static float lfoValue (int type, float phase, float& held,
                           uint32_t& noise) noexcept
    {
        switch (type)
        {
            case 0: return std::sin (juce::MathConstants<float>::twoPi * phase);
            case 1: return 1.0f - 4.0f * std::abs (phase - 0.5f);
            case 2: return 2.0f * phase - 1.0f;
            default:
                if (phase < 0.002f)
                {
                    noise ^= noise << 13; noise ^= noise >> 17; noise ^= noise << 5;
                    held = ((float) (noise & 0xffffu) / 32767.5f) - 1.0f;
                }
                return held;
        }
    }

    struct FilterCoefficients
    {
        float g = 0.0f;
        float r = 0.0f;
        float denominator = 1.0f;
    };

    static FilterCoefficients filterCoefficients (float cutoff,
                                                   float filterR,
                                                   double sr) noexcept
    {
        cutoff = juce::jlimit (30.0f, (float) sr * 0.44f, cutoff);
        const float g = std::tan (juce::MathConstants<float>::pi
                                  * cutoff / (float) sr);
        return { g, filterR,
                 1.0f + filterR * g + g * g };
    }

    static float processFilterStage (float input,
                                     const FilterCoefficients& coefficients,
                                     FilterState& state,
                                     int type) noexcept
    {
        const float g = coefficients.g;
        const float hp = (input - (coefficients.r + g) * state.s1 - state.s2)
                       / coefficients.denominator;
        const float bp = g * hp + state.s1;
        state.s1 = g * hp + bp;
        const float lp = g * bp + state.s2;
        state.s2 = g * bp + lp;
        if (type == 2) return hp;
        if (type == 3) return bp;
        return lp;
    }

    static float processFilter (float input,
                                const FilterCoefficients& coefficients,
                                FilterState& first,
                                FilterState& second, int type) noexcept
    {
        const float once = processFilterStage (
            input, coefficients, first, type);
        // A primeira opcao e o LP de 24 dB da referencia: duas secoes reais
        // em cascata. Os demais modos usam uma secao de 12 dB.
        return type == 0
                 ? processFilterStage (
                       once, coefficients, second, 1)
                 : once;
    }

    void prepareActiveVoiceIncrements (const BlockSettings& settings,
                                       int onlyChannel = 0) noexcept
    {
        const float inverseSampleRate = 1.0f / (float) sampleRate;
        for (auto& voice : voices)
        {
            if (! voice.active || (onlyChannel != 0 && voice.channel != onlyChannel))
                continue;

            const float baseNote = (float) voice.note
                                 + (float) settings.transpose
                                 + pitchWheelSemis[(size_t) (voice.channel - 1)];
            for (int layer = 0; layer < numLayers; ++layer)
            {
                const auto& s = settings.layer[(size_t) layer];
                auto& v = voice.layers[(size_t) layer];
                const auto incrementFor = [baseNote, inverseSampleRate] (float semitones)
                {
                    const float hz = 440.0f * std::pow (
                        2.0f, (baseNote + semitones - 69.0f) / 12.0f);
                    return juce::jlimit (0.0f, 0.45f, hz * inverseSampleRate);
                };
                v.osc1Increment = incrementFor (12.0f * (float) s.osc1Octave);
                v.osc2Increment = incrementFor (12.0f * (float) s.osc2Octave
                                                + s.osc2Tune / 100.0f);
                v.subIncrement = incrementFor (12.0f * (float) s.subOctave);
                v.lfoIncrement = s.lfoRate * inverseSampleRate;
            }
        }
    }

    void renderVoices (const BlockSettings& settings, float& outL,
                       float& outR) noexcept
    {
        for (auto& voice : voices)
        {
            if (! voice.active)
                continue;

            bool anyLayer = false;
            float voiceL = 0.0f, voiceR = 0.0f;
            for (int l = 0; l < numLayers; ++l)
            {
                const auto& s = settings.layer[(size_t) l];
                auto& v = voice.layers[(size_t) l];
                const float env = v.envelope.next (
                    s.attack * settings.utilityAttack, s.decay, s.sustain,
                    s.release * settings.utilityRelease, sampleRate);
                const float modEnv = v.modEnvelope.next (
                    s.modAttack, s.modDecay, s.modSustain, s.modRelease,
                    sampleRate);
                anyLayer |= v.envelope.active();
                if (! s.enabled || env <= 0.000001f)
                    continue;

                if (voice.samplesAlive == 0)
                    v.lfoPhase = s.lfoPhase / 360.0f;
                const float lfo = lfoValue (s.lfoWave, v.lfoPhase,
                                            v.heldNoise, voice.noise);
                nextPhase (v.lfoPhase, v.lfoIncrement);

                voice.noise ^= voice.noise << 13;
                voice.noise ^= voice.noise >> 17;
                voice.noise ^= voice.noise << 5;
                const float whiteNoise = ((float) (voice.noise & 0xffffu) / 32767.5f) - 1.0f;
                float noise = whiteNoise;
                if (s.noiseType == 1)
                {
                    v.pinkNoise += 0.045f * (whiteNoise - v.pinkNoise);
                    noise = juce::jlimit (-1.0f, 1.0f, v.pinkNoise * 2.4f);
                }
                else if (s.noiseType == 2)
                    noise = std::round (whiteNoise * 8.0f) * 0.125f;

                float value = waveform (s.osc1Wave, v.osc1Phase, v.osc1Increment) * s.osc1Level
                            + waveform (s.osc2Wave, v.osc2Phase, v.osc2Increment) * s.osc2Level
                            + waveform (s.subWave, v.subPhase, v.subIncrement) * s.subLevel
                            + noise * s.noiseLevel
                                  * juce::jmap (settings.macroAmbience, 0.45f, 1.5f);
                nextPhase (v.osc1Phase, v.osc1Increment);
                nextPhase (v.osc2Phase, v.osc2Increment);
                nextPhase (v.subPhase, v.subIncrement);

                const float fade = s.lfoFade <= 0.001f ? 1.0f
                    : juce::jlimit (0.0f, 1.0f,
                        (float) voice.samplesAlive
                            / (float) juce::jmax (1.0, sampleRate * s.lfoFade));
                const float motion = lfo * s.lfoDepth * fade
                    * juce::jmap (settings.xyX, 0.45f, 1.55f);
                const float brightness = juce::jmap (settings.xyX, 0.62f, 1.55f);
                const float envOctaves = s.filterEnv * (modEnv - 0.35f) * 4.0f;
                const float cutoff = s.cutoff * brightness
                    * std::pow (2.0f, envOctaves + motion * 1.5f);

                if (s.drive > 0.0001f)
                {
                    value = std::tanh (value * s.driveGain)
                          / s.driveDenominator;
                }
                value *= env * voice.velocity * 0.30f;
                const float pan = juce::jlimit (-1.0f, 1.0f, s.pan + motion * 0.12f);
                const float gainL = std::sqrt (0.5f * (1.0f - pan));
                const float gainR = std::sqrt (0.5f * (1.0f + pan));
                // Cutoff/resonancia sao comuns aos dois canais e, no LP24,
                // aos dois estagios. Recalcular tan(), Q e denominador quatro
                // vezes por voz/camada/amostra consumia a maior parte do DSP
                // em ARM. Reusar os mesmos coeficientes preserva exatamente a
                // topologia e a resposta sonora do filtro.
                const auto coefficients = filterCoefficients (
                    cutoff, s.filterR, sampleRate);
                voiceL += processFilter (
                    value * gainL, coefficients,
                    v.filterL, v.filter2L, s.filterType);
                voiceR += processFilter (
                    value * gainR, coefficients,
                    v.filterR, v.filter2R, s.filterType);
            }

            const float renderedL = voiceL + voice.stealDeclickL;
            const float renderedR = voiceR + voice.stealDeclickR;
            outL += renderedL;
            outR += renderedR;
            voice.lastOutputL = renderedL;
            voice.lastOutputR = renderedR;

            voice.stealDeclickL *= voiceStealDeclickMultiplier;
            voice.stealDeclickR *= voiceStealDeclickMultiplier;
            if (std::abs (voice.stealDeclickL) < 0.0000001f)
                voice.stealDeclickL = 0.0f;
            if (std::abs (voice.stealDeclickR) < 0.0000001f)
                voice.stealDeclickR = 0.0f;

            const bool declickActive = voice.stealDeclickL != 0.0f
                                    || voice.stealDeclickR != 0.0f;
            if (! anyLayer && ! declickActive)
                voice = {};
            else
                ++voice.samplesAlive;
        }
    }

    static float readInterpolated (const std::vector<float>& buffer,
                                   float position) noexcept
    {
        if (buffer.empty()) return 0.0f;
        while (position < 0.0f) position += (float) buffer.size();
        while (position >= (float) buffer.size()) position -= (float) buffer.size();
        const int a = (int) position;
        const int b = (a + 1) % (int) buffer.size();
        const float frac = position - (float) a;
        return buffer[(size_t) a] + frac * (buffer[(size_t) b] - buffer[(size_t) a]);
    }

    void processChorusAndDelay (juce::AudioBuffer<float>& buffer,
                                const BlockSettings& settings) noexcept
    {
        if (chorusBuffer[0].empty() || delayBuffer[0].empty())
            return;
        const float chorusWet = juce::jlimit (0.0f, 0.78f,
            (settings.chorusEnabled ? settings.chorus : 0.0f)
                * juce::jmap (settings.macroAmbience, 0.55f, 1.35f));
        const float delayWet = juce::jlimit (0.0f, 0.82f,
            (settings.delayEnabled ? settings.delay : 0.0f)
                * juce::jmap (settings.macroDelay, 0.2f, 1.65f)
                           * juce::jmap (settings.xyY, 0.65f, 1.30f));
        const float delaySamples = settings.delayTime * (float) sampleRate;
        auto* left = buffer.getWritePointer (0);
        auto* right = buffer.getWritePointer (1);

        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            const float dryL = left[i], dryR = right[i];
            const float modL = 0.5f + 0.5f * std::sin (chorusPhase);
            const float modR = 0.5f + 0.5f * std::sin (chorusPhase + 1.9f);
            const float chL = readInterpolated (chorusBuffer[0],
                (float) chorusWrite - (0.012f + 0.009f * modL) * (float) sampleRate);
            const float chR = readInterpolated (chorusBuffer[1],
                (float) chorusWrite - (0.014f + 0.008f * modR) * (float) sampleRate);
            chorusBuffer[0][(size_t) chorusWrite] = dryL;
            chorusBuffer[1][(size_t) chorusWrite] = dryR;
            chorusWrite = (chorusWrite + 1) % (int) chorusBuffer[0].size();
            chorusPhase += juce::MathConstants<float>::twoPi * 0.17f / (float) sampleRate;
            if (chorusPhase >= juce::MathConstants<float>::twoPi)
                chorusPhase -= juce::MathConstants<float>::twoPi;

            const float chorusedL = dryL + (chL - dryL) * chorusWet;
            const float chorusedR = dryR + (chR - dryR) * chorusWet;
            const float dl = readInterpolated (delayBuffer[0],
                                               (float) delayWrite - delaySamples);
            const float dr = readInterpolated (delayBuffer[1],
                                               (float) delayWrite - delaySamples * 1.013f);
            delayBuffer[0][(size_t) delayWrite] = chorusedL + dr * settings.delayFeedback;
            delayBuffer[1][(size_t) delayWrite] = chorusedR + dl * settings.delayFeedback;
            delayWrite = (delayWrite + 1) % (int) delayBuffer[0].size();
            left[i] = chorusedL + dl * delayWet;
            right[i] = chorusedR + dr * delayWet;
        }
    }

    struct ToneState
    {
        float low = 0.0f;
        float midLow = 0.0f;
        float midHigh = 0.0f;
        float highLow = 0.0f;
    };

    void processTone (juce::AudioBuffer<float>& buffer,
                      const BlockSettings& settings) noexcept
    {
        if (! settings.eqEnabled)
            return;

        const float lowGain = juce::Decibels::decibelsToGain (settings.toneLow);
        const float midGain = juce::Decibels::decibelsToGain (settings.toneMid);
        const float highGain = juce::Decibels::decibelsToGain (settings.toneHigh);
        const auto coefficient = [this] (float hz)
        {
            return 1.0f - std::exp (-juce::MathConstants<float>::twoPi
                                    * hz / (float) sampleRate);
        };
        const float aLow = coefficient (180.0f);
        const float aMidLow = coefficient (260.0f);
        const float aMidHigh = coefficient (2200.0f);
        const float aHigh = coefficient (4800.0f);

        for (int ch = 0; ch < juce::jmin (2, buffer.getNumChannels()); ++ch)
        {
            auto& state = toneStates[(size_t) ch];
            auto* data = buffer.getWritePointer (ch);
            for (int i = 0; i < buffer.getNumSamples(); ++i)
            {
                const float input = data[i];
                state.low += aLow * (input - state.low);
                state.midLow += aMidLow * (input - state.midLow);
                state.midHigh += aMidHigh * (input - state.midHigh);
                state.highLow += aHigh * (input - state.highLow);
                const float midBand = state.midHigh - state.midLow;
                const float highBand = input - state.highLow;
                data[i] = input
                        + state.low * (lowGain - 1.0f)
                        + midBand * (midGain - 1.0f)
                        + highBand * (highGain - 1.0f);
            }
        }
    }

    void updateReverbParameters()
    {
        updateReverbParameters (readBlockSettings());
    }

    void updateReverbParameters (const BlockSettings& settings)
    {
        juce::dsp::Reverb::Parameters p;
        const float space = juce::jlimit (0.0f, 1.0f,
            (settings.reverbEnabled ? settings.reverb : 0.0f)
                * juce::jmap (settings.macroReverb, 0.25f, 1.55f)
                            * juce::jmap (settings.xyY, 0.70f, 1.25f));
        p.roomSize = juce::jmap (space, 0.45f, 0.96f);
        p.damping = juce::jmap (settings.xyX, 0.72f, 0.28f);
        p.wetLevel = juce::jlimit (0.0f, 0.48f, space * 0.48f);
        p.dryLevel = 1.0f;
        p.width = 0.92f;
        p.freezeMode = 0.0f;
        reverb.setParameters (p);
    }

    struct Program
    {
        juce::String name;
        juce::ValueTree state { "PARAMS" };
    };

    static bool isRuntimeParameter (const juce::String& id)
    {
        return id == "padEngineEnabled" || id == "padMidiChannel";
    }

    juce::ValueTree captureParameterState() const
    {
        juce::ValueTree state ("PARAMS");
        for (const auto& pair : parameterById)
            if (! isRuntimeParameter (pair.first))
                state.setProperty (pair.first, pair.second->getValue(), nullptr);
        return state;
    }

    void applyParameterState (const juce::ValueTree& state)
    {
        if (! state.isValid()) return;
        for (int i = 0; i < state.getNumProperties(); ++i)
        {
            const auto id = state.getPropertyName (i).toString();
            if (const auto found = parameterById.find (id);
                found != parameterById.end() && ! isRuntimeParameter (id))
                found->second->setValueNotifyingHost (
                    juce::jlimit (0.0f, 1.0f, (float) state.getProperty (id)));
        }
    }

    void captureProgram (int index)
    {
        if (juce::isPositiveAndBelow (index, numPrograms))
            programs[(size_t) index].state = captureParameterState();
    }

    void applyProgram (int index, bool notify)
    {
        if (! juce::isPositiveAndBelow (index, numPrograms)) return;
        applyParameterState (programs[(size_t) index].state);
        currentProgram.store (index);
        if (notify) sendChangeMessage();
    }

    void setProgramActual (Program& program, const juce::String& id, float value)
    {
        if (auto* p = parameter (id))
            program.state.setProperty (id, p->convertTo0to1 (value), nullptr);
    }

    void createFactoryPrograms()
    {
        static const char* names[numPrograms] = {
            "Warm Pad", "Heaven Pad", "Soft Pad", "Ambience Heaven",
            "Deep Atmos", "Cloud Pad", "Dark Pad", "Bright Ambience",
            "Ethereal", "Ocean Pad", "Air Pad", "Choir Pad",
            "Drone Pad", "Shimmer", "Space Pad", "Infinity"
        };

        const auto base = captureParameterState();
        for (int i = 0; i < numPrograms; ++i)
        {
            auto& program = programs[(size_t) i];
            program.name = names[i];
            program.state = base.createCopy();

            const float shade = (float) (i % 4) / 3.0f;
            setProgramActual (program, "padMasterDb", -11.0f + shade * 3.0f);
            setProgramActual (program, "padReverbMix", 0.28f + shade * 0.34f);
            setProgramActual (program, "padChorusMix", 0.12f + (float) ((i + 1) % 4) * 0.07f);
            setProgramActual (program, "padDelayMix", (i % 3 == 0) ? 0.22f : 0.08f + shade * 0.10f);
            setProgramActual (program, "padMacroAmbience", 0.38f + shade * 0.40f);
            setProgramActual (program, "padMacroReverb", 0.42f + shade * 0.40f);

            for (int l = 0; l < numLayers; ++l)
            {
                setProgramActual (program, layerId (l, "Enabled"),
                                  l < 2 || (l == 2 && i >= 6) ? 1.0f : 0.0f);
                setProgramActual (program, layerId (l, "Cutoff"),
                                  1200.0f + 900.0f * (float) l + shade * 4200.0f);
                setProgramActual (program, layerId (l, "Attack"),
                                  0.35f + 0.30f * (float) l + shade * 0.8f);
                setProgramActual (program, layerId (l, "Release"),
                                  2.8f + (float) l + shade * 4.5f);
                setProgramActual (program, layerId (l, "LfoRate"),
                                  0.05f + 0.035f * (float) ((i + l) % 5));
                setProgramActual (program, layerId (l, "LfoDepth"),
                                  0.07f + 0.05f * (float) ((i + l) % 4));
                setProgramActual (program, layerId (l, "Pan"),
                                  l % 2 == 0 ? -0.12f - shade * 0.18f
                                             : 0.12f + shade * 0.18f);
            }

            setProgramActual (program, "padColour", (float) (i % 10));
            setProgramActual (program, "padGroup", (float) ((i / 4) % 4));
        }
    }

    static void appendStateXml (juce::XmlElement& parent, const char* tag,
                                const juce::ValueTree& state)
    {
        auto* xml = parent.createNewChildElement (tag);
        for (int i = 0; i < state.getNumProperties(); ++i)
        {
            auto* p = xml->createNewChildElement ("P");
            p->setAttribute ("id", state.getPropertyName (i).toString());
            p->setAttribute ("v", (double) state.getProperty (state.getPropertyName (i)));
        }
    }

    static juce::ValueTree readStateXml (const juce::XmlElement& xml)
    {
        juce::ValueTree state ("PARAMS");
        for (auto* p : xml.getChildIterator())
            if (p->hasTagName ("P"))
                state.setProperty (p->getStringAttribute ("id"),
                                   p->getDoubleAttribute ("v"), nullptr);
        return state;
    }

    std::array<LayerParameters, numLayers> layers;
    std::map<juce::String, juce::RangedAudioParameter*> parameterById;
    std::array<Program, numPrograms> programs;
    std::array<bool, numPrograms> favouritePrograms {};
    juce::ValueTree clipboard;
    std::atomic<int> currentProgram { 0 };

    std::array<Voice, maxVoices> voices;
    std::array<float, 16> pitchWheelSemis {};
    std::array<bool, 16> sustainDown {};
    uint32_t voiceAge = 0;
    std::array<RawMidiEvent, 256> midiEvents;
    int midiEventCount = 0;

    double sampleRate = 44100.0;
    int preparedBlockSize = 512;
    float engineGain = 0.0f;
    float voiceStealDeclickMultiplier = 0.0f;
    std::array<std::vector<float>, 2> chorusBuffer, delayBuffer;
    int chorusWrite = 0, delayWrite = 0;
    float chorusPhase = 0.0f;
    float gatePhase = 0.0f;
    juce::dsp::Reverb reverb;
    std::array<ToneState, 2> toneStates;
    bool effectsAreSilent = true;
    int64_t silentTailSamples = 0;
    std::atomic<float> outputPeak { 0.0f };
    std::atomic<bool> panicRequested { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PadSynthProcessor)
};
