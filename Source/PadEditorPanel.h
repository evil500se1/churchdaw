#pragma once

#include "PadSynthProcessor.h"
#include "SamplePadEditorPanel.h"
#include "NordLookAndFeel.h"

#include <array>
#include <memory>
#include <vector>

//==============================================================================
// Workspace premium do PAD EDITOR. O componente ocupa a area de trabalho da
// DAW; header, sidebar e footer continuam pertencendo ao MainComponent.
//==============================================================================
class PadEditorPanel final : public juce::Component,
                             private juce::Timer,
                             private juce::ChangeListener
{
public:
    explicit PadEditorPanel (PadSynthProcessor& processorToUse)
        : processor (processorToUse)
    {
        setOpaque (true);
        setWantsKeyboardFocus (true);
        processor.addChangeListener (this);

        configureButton (closeButton, "X", false);
        closeButton.setTooltip ("Fechar o PAD EDITOR");
        closeButton.onClick = [this]
        {
            if (onClose != nullptr) onClose();
        };
        addAndMakeVisible (closeButton);

        configureButton (synthSourceButton, "PAD SYNTH", true);
        synthSourceButton.onClick = [this]
        {
            if (synthSourceButton.getToggleState())
                setWorkspaceMode (WorkspaceMode::synth);
        };
        addAndMakeVisible (synthSourceButton);

        configureButton (quickPadButton,
                         juce::String::fromUTF8 ("PAD CONT\xc3\x8dNUO"), false);
        quickPadButton.setTooltip ("Abrir o Pad Continuo de loops por tom");
        quickPadButton.onClick = [this]
        {
            if (onQuickPadContinuo != nullptr) onQuickPadContinuo();
        };
        addAndMakeVisible (quickPadButton);

        configureButton (samplesSourceButton, "PADS DE SAMPLES", true);
        samplesSourceButton.onClick = [this]
        {
            setWorkspaceMode (WorkspaceMode::samples);
        };
        addAndMakeVisible (samplesSourceButton);

        configureButton (engineButton, "ENGINE", true);
        engineButton.onClick = [this]
        {
            engineButton.setButtonText (engineButton.getToggleState()
                                         ? "ENGINE ON" : "ENGINE");
            stateWasEdited();
        };
        addAndMakeVisible (engineButton);

        configureCombo (presetBox);
        for (int i = 0; i < PadSynthProcessor::numPrograms; ++i)
            presetBox.addItem (processor.getProgramName (i), i + 1);
        presetBox.onChange = [this]
        {
            if (syncing || presetBox.getSelectedId() <= 0) return;
            processor.selectProgram (presetBox.getSelectedId() - 1);
            stateWasEdited();
            rebuildLayerAttachments();
        };
        addAndMakeVisible (presetBox);

        configureButton (favouriteButton, juce::String::fromUTF8 ("\xe2\x98\x86"), true);
        favouriteButton.setTooltip ("Marcar preset como favorito");
        favouriteButton.onClick = [this]
        {
            processor.setProgramFavourite (processor.getCurrentProgram(),
                                            favouriteButton.getToggleState());
            favouriteButton.setButtonText (favouriteButton.getToggleState()
                ? juce::String::fromUTF8 ("\xe2\x98\x85")
                : juce::String::fromUTF8 ("\xe2\x98\x86"));
            stateWasEdited();
        };
        addAndMakeVisible (favouriteButton);

        configureButton (copyButton, "COPIAR", false);
        configureButton (pasteButton, "COLAR", false);
        configureButton (saveButton, "SALVAR", false);
        copyButton.onClick = [this]
        {
            processor.copyCurrentProgram();
            pasteButton.setEnabled (true);
            setStatus ("Preset copiado", false);
        };
        pasteButton.onClick = [this]
        {
            processor.pasteProgram();
            rebuildLayerAttachments();
            setStatus ("Preset colado e aplicado", false);
            stateWasEdited();
        };
        saveButton.onClick = [this]
        {
            processor.saveCurrentProgram();
            setStatus ("Preset salvo no banco", false);
            stateWasEdited();
        };
        pasteButton.setEnabled (processor.canPasteProgram());
        addAndMakeVisible (copyButton);
        addAndMakeVisible (pasteButton);
        addAndMakeVisible (saveButton);

        static const char* sectionNames[7] = {
            "SINTESE", "AMP", "FILTRO", "MODULACAO",
            "LFO", "MATRIZ", "ARP / GATE"
        };
        for (int i = 0; i < (int) sectionButtons.size(); ++i)
        {
            configureButton (sectionButtons[(size_t) i], sectionNames[i], false);
            sectionButtons[(size_t) i].onClick = [this, i]
            {
                selectedSection = i;
                syncSectionButtons();
                resized();
                repaint();
            };
            addAndMakeVisible (sectionButtons[(size_t) i]);
        }

        for (int i = 0; i < PadSynthProcessor::numLayers; ++i)
        {
            configureButton (layerButtons[(size_t) i],
                             juce::String (i + 1), false);
            layerButtons[(size_t) i].onClick = [this, i]
            {
                selectLayer (i);
            };
            addAndMakeVisible (layerButtons[(size_t) i]);
        }

        for (int i = 0; i < PadSynthProcessor::numPrograms; ++i)
        {
            configureButton (programButtons[(size_t) i],
                             juce::String (i + 1), false);
            programButtons[(size_t) i].onClick = [this, i]
            {
                processor.selectProgram (i);
                stateWasEdited();
                rebuildLayerAttachments();
            };
            addAndMakeVisible (programButtons[(size_t) i]);
        }

        configureButton (layerEnabledButton, "LAYER ON", true);
        layerEnabledButton.onClick = [this] { stateWasEdited(); };
        addAndMakeVisible (layerEnabledButton);

        configureCombo (midiChannelBox);
        midiChannelBox.addItem ("OMNI", 1);
        for (int channel = 1; channel <= 16; ++channel)
            midiChannelBox.addItem ("MIDI " + juce::String (channel), channel + 1);
        midiChannelBox.onChange = [this] { if (! syncing) stateWasEdited(); };
        addAndMakeVisible (midiChannelBox);

        configureCombo (velocityBox);
        velocityBox.addItemList ({ "SUAVE", "NORMAL", "FORTE", "FIXA" }, 1);
        velocityBox.onChange = [this] { if (! syncing) stateWasEdited(); };
        addAndMakeVisible (velocityBox);

        configureCombo (groupBox);
        groupBox.addItemList ({ "AMBIENCIAS", "WORSHIP", "PROFETICOS", "TEXTURAS" }, 1);
        groupBox.onChange = [this] { if (! syncing) stateWasEdited(); };
        addAndMakeVisible (groupBox);

        for (int i = 0; i < (int) colourButtons.size(); ++i)
        {
            configureButton (colourButtons[(size_t) i], {}, false);
            colourButtons[(size_t) i].onClick = [this, i]
            {
                if (processor.padColour != nullptr)
                    processor.padColour->setValueNotifyingHost (
                        processor.padColour->convertTo0to1 ((float) i));
                syncColourButtons();
                stateWasEdited();
            };
            addAndMakeVisible (colourButtons[(size_t) i]);
        }

        configureButton (holdButton, "HOLD", true);
        configureButton (toggleButton, "TOGGLE", true);
        holdButton.onClick = [this]
        {
            if (holdButton.getToggleState())
            {
                toggleButton.setToggleState (false, juce::sendNotificationSync);
                processor.toggleMode->setValueNotifyingHost (0.0f);
            }
            stateWasEdited();
        };
        toggleButton.onClick = [this]
        {
            if (toggleButton.getToggleState())
            {
                holdButton.setToggleState (false, juce::sendNotificationSync);
                processor.holdMode->setValueNotifyingHost (0.0f);
            }
            stateWasEdited();
        };
        addAndMakeVisible (holdButton);
        addAndMakeVisible (toggleButton);

        configureCombo (osc1WaveBox);
        configureCombo (osc2WaveBox);
        configureCombo (subWaveBox);
        configureCombo (noiseTypeBox);
        configureCombo (filterTypeBox);
        configureCombo (lfoWaveBox);
        osc1WaveBox.addItemList ({ "SENO", "TRIANGULO", "SERRA", "QUADRADA", "WARM" }, 1);
        osc2WaveBox.addItemList ({ "SENO", "TRIANGULO", "SERRA", "QUADRADA", "WARM" }, 1);
        subWaveBox.addItemList ({ "SENO", "TRIANGULO", "QUADRADA" }, 1);
        noiseTypeBox.addItemList ({ "WHITE", "PINK", "DIGITAL" }, 1);
        filterTypeBox.addItemList ({ "LOW PASS 24DB", "LOW PASS 12DB", "HIGH PASS", "BAND PASS" }, 1);
        lfoWaveBox.addItemList ({ "SENO", "TRIANGULO", "SERRA", "S&H" }, 1);
        for (auto* box : { &osc1WaveBox, &osc2WaveBox, &subWaveBox,
                           &noiseTypeBox, &filterTypeBox, &lfoWaveBox })
        {
            box->onChange = [this] { if (! syncing) stateWasEdited(); };
            addAndMakeVisible (*box);
        }

        addAndMakeVisible (osc1Wave);
        addAndMakeVisible (osc2Wave);
        addAndMakeVisible (filterGraph);
        addAndMakeVisible (ampEnvelopeView);
        addAndMakeVisible (modEnvelopeView);
        addAndMakeVisible (velocityGraph);
        velocityGraph.onFixedChanged = [this] (int value)
        {
            if (processor.fixedVelocity != nullptr)
                processor.fixedVelocity->setValueNotifyingHost (
                    processor.fixedVelocity->convertTo0to1 ((float) value));
            stateWasEdited();
        };

        setupLayerSliders();
        setupGlobalSliders();

        static const char* fxNames[4] = { "EQ", "REVERB", "DELAY", "CHORUS" };
        for (int i = 0; i < (int) effectButtons.size(); ++i)
        {
            configureButton (effectButtons[(size_t) i], fxNames[i], true);
            effectButtons[(size_t) i].onClick = [this] { stateWasEdited(); };
            addAndMakeVisible (effectButtons[(size_t) i]);
        }
        configureButton (gateButton, "GATE", true);
        gateButton.onClick = [this] { stateWasEdited(); };
        addAndMakeVisible (gateButton);
        configureCombo (gateRateBox);
        gateRateBox.addItemList ({ "1/4", "1/8", "1/8T", "1/16" }, 1);
        gateRateBox.onChange = [this] { if (! syncing) stateWasEdited(); };
        addAndMakeVisible (gateRateBox);

        addAndMakeVisible (xyPad);
        xyPad.onChanged = [this] (float x, float y)
        {
            if (processor.xyX != nullptr)
                processor.xyX->setValueNotifyingHost (
                    processor.xyX->convertTo0to1 (x));
            if (processor.xyY != nullptr)
                processor.xyY->setValueNotifyingHost (
                    processor.xyY->convertTo0to1 (y));
            stateWasEdited();
        };

        engineAttachment = std::make_unique<juce::ButtonParameterAttachment> (
            *processor.engineEnabled, engineButton);
        midiAttachment = std::make_unique<juce::ComboBoxParameterAttachment> (
            *processor.midiChannel, midiChannelBox);
        velocityAttachment = std::make_unique<juce::ComboBoxParameterAttachment> (
            *processor.velocityCurve, velocityBox);
        groupAttachment = std::make_unique<juce::ComboBoxParameterAttachment> (
            *processor.padGroup, groupBox);
        holdAttachment = std::make_unique<juce::ButtonParameterAttachment> (
            *processor.holdMode, holdButton);
        toggleAttachment = std::make_unique<juce::ButtonParameterAttachment> (
            *processor.toggleMode, toggleButton);
        createGlobalAttachments();
        sampleWorkspace.onStateChanged = [this]
        {
            stateWasEdited();
        };
        addChildComponent (sampleWorkspace);
        selectLayer (0);
        syncSectionButtons();
        syncColourButtons();
        syncProgramUi();
        // A aba PAD agora e dedicada ao editor real de pads sampleados. O
        // antigo sintetizador permanece apenas como compatibilidade interna de
        // estados antigos, sem rota de interface nesta central.
        setWorkspaceMode (WorkspaceMode::samples);
    }

    ~PadEditorPanel() override
    {
        stopTimer();
        processor.removeChangeListener (this);
    }

    std::function<void()> onClose;
    std::function<void()> onQuickPadContinuo;
    std::function<void()> onStateChanged;

    void setSampleProcessor (TimbresProcessor* timbres)
    {
        sampleWorkspace.setProcessor (timbres);
    }

    void setSourceChannel (int channelIndex, const juce::String& name)
    {
        sourceChannel = channelIndex;
        sourceName = name;
        if (channelIndex >= 0)
        {
            sampleWorkspace.setTargetLayer (channelIndex);
            setWorkspaceMode (WorkspaceMode::samples);
        }
        repaint (sourceBounds);
    }

    void refreshFromProcessor()
    {
        if (workspaceMode == WorkspaceMode::samples)
        {
            sampleWorkspace.refreshFromProcessor();
            return;
        }

        syncProgramUi();
        engineButton.setButtonText (processor.isEngineEnabled()
                                     ? "ENGINE ON" : "ENGINE");
        xyPad.setPosition (processor.xyX->get(), processor.xyY->get());
        filterGraph.setValues (processor.layer (selectedLayer).cutoff->get(),
                               processor.layer (selectedLayer).resonance->get());
        ampEnvelopeView.setValues (
            processor.layer (selectedLayer).attack->get(),
            processor.layer (selectedLayer).decay->get(),
            processor.layer (selectedLayer).sustain->get(),
            processor.layer (selectedLayer).release->get());
        modEnvelopeView.setValues (
            processor.layer (selectedLayer).modAttack->get(),
            processor.layer (selectedLayer).modDecay->get(),
            processor.layer (selectedLayer).modSustain->get(),
            processor.layer (selectedLayer).modRelease->get());
        velocityGraph.setValues (processor.velocityCurve->getIndex(),
                                 processor.fixedVelocity->get());
        syncColourButtons();
        sampleWorkspace.refreshFromProcessor();
    }

    void visibilityChanged() override
    {
        updateRefreshTimers();
    }

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds();
        g.fillAll (juce::Colour (0xff080a0d));

        g.setGradientFill (juce::ColourGradient (
            juce::Colour (0xff171a20), 0.0f, 0.0f,
            juce::Colour (0xff090b0e), (float) getWidth(), (float) getHeight(), false));
        g.fillRect (bounds);

        drawToolbar (g);
        g.setColour (juce::Colour (0xff0c0f13));
        g.fillRect (sourceNavigationBounds);
        g.setColour (juce::Colour (0xff2b3038));
        g.drawHorizontalLine (sourceNavigationBounds.getBottom() - 1,
                              (float) sourceNavigationBounds.getX(),
                              (float) sourceNavigationBounds.getRight());

        if (workspaceMode == WorkspaceMode::synth)
        {
            drawCard (g, bankBounds, "BANCO ATUAL", juce::Colour (0xffef214b));
            drawCard (g, settingsBounds, "PAD SETTINGS", juce::Colour (0xffd09a34));
            drawCard (g, synthBounds,
                      juce::String::fromUTF8 ("S\xc3\x8dNTESE / SOUND DESIGN"),
                      juce::Colour (0xffef214b));
            drawCard (g, effectsBounds, "EFEITOS", juce::Colour (0xffef214b));

            drawSubCard (g, osc1Bounds, "OSC 1");
            drawSubCard (g, osc2Bounds, "OSC 2");
            drawSubCard (g, mixerBounds, "MIXER");
            drawSubCard (g, filterBounds, "FILTRO");
            drawSubCard (g, lfoBounds, "LFO");
            drawSubCard (g, ampEnvBounds, "AMP ENV");
            drawSubCard (g, modEnvBounds, "MOD ENV");
            drawSubCard (g, xyBounds, "XY");
            drawSubCard (g, macroBounds, "MACRO CONTROLS");
            drawSubCard (g, utilityBounds, "PAD UTILITIES");
            drawSubCard (g, velocityBounds, "VELOCIDADE");

            juce::Rectangle<int> focused;
            switch (selectedSection)
            {
                case 1: focused = ampEnvBounds; break;
                case 2: focused = filterBounds; break;
                case 3: focused = modEnvBounds.getUnion (xyBounds); break;
                case 4: focused = lfoBounds; break;
                case 5: focused = xyBounds; break;
                case 6: focused = utilityBounds; break;
                default: focused = synthBounds; break;
            }
            if (! focused.isEmpty())
            {
                g.setColour (juce::Colour (0xffff3158).withAlpha (0.38f));
                g.drawRoundedRectangle (focused.toFloat().reduced (0.8f),
                                        6.0f, 1.2f);
            }

            drawComponentLabels (g);

            if (! sourceBounds.isEmpty() && sourceChannel >= 0)
            {
                g.setColour (juce::Colour (0xff11151a));
                g.fillRoundedRectangle (sourceBounds.toFloat(), 5.0f);
                g.setColour (juce::Colour (0xffef214b));
                g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
                g.drawText ("ABERTO PELO CANAL " + juce::String (sourceChannel + 1)
                            + "  /  " + sourceName.toUpperCase(),
                            sourceBounds.reduced (8, 0), juce::Justification::centredLeft);
            }
        }

        if (statusText.isNotEmpty())
        {
            g.setColour (statusWarning ? juce::Colour (0xffe3a62f)
                                       : juce::Colour (0xff55d789));
            g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
            g.drawText (statusText, statusBounds, juce::Justification::centredRight);
        }
    }

    void resized() override
    {
        auto area = getLocalBounds();
        const bool compactSamplesHeader = workspaceMode == WorkspaceMode::samples
                                       && getHeight() < 420;

        if (compactSamplesHeader)
        {
            toolbarBounds = {};
            saveButton.setBounds ({});
            pasteButton.setBounds ({});
            copyButton.setBounds ({});
            for (auto& button : layerButtons)
                button.setBounds ({});
            favouriteButton.setBounds ({});
            presetBox.setBounds ({});
            engineButton.setBounds ({});

            sourceNavigationBounds = area.removeFromTop (46);
            auto padTabs = sourceNavigationBounds.reduced (12, 4);
            closeButton.setBounds (padTabs.removeFromRight (42));
            padTabs.removeFromRight (8);
            const int padTabGap = 7;
            const int padTabWidth = juce::jmax (112,
                juce::jmin (210, (padTabs.getWidth() - padTabGap) / 2));
            synthSourceButton.setBounds ({});
            samplesSourceButton.setBounds (padTabs.removeFromLeft (padTabWidth));
            padTabs.removeFromLeft (padTabGap);
            quickPadButton.setBounds (padTabs.removeFromLeft (padTabWidth));
        }
        else
        {
            toolbarBounds = area.removeFromTop (58);
            auto toolbar = toolbarBounds.reduced (14, 9);
            closeButton.setBounds (toolbar.removeFromRight (38));
            toolbar.removeFromRight (7);
            saveButton.setBounds (toolbar.removeFromRight (72));
            toolbar.removeFromRight (5);
            pasteButton.setBounds (toolbar.removeFromRight (66));
            toolbar.removeFromRight (5);
            copyButton.setBounds (toolbar.removeFromRight (66));
            toolbar.removeFromRight (12);

            auto layers = toolbar.removeFromRight (178);
            layerLabelBounds = layers.removeFromLeft (46);
            for (auto& button : layerButtons)
            {
                button.setBounds (layers.removeFromLeft (30).reduced (1, 0));
                layers.removeFromLeft (2);
            }
            toolbar.removeFromRight (8);
            favouriteButton.setBounds (toolbar.removeFromRight (34));
            toolbar.removeFromRight (6);
            presetBox.setBounds (toolbar.removeFromRight (juce::jmin (245, toolbar.getWidth() / 3)));
            toolbar.removeFromRight (5);
            padSelectorBounds = toolbar.removeFromRight (58);
            statusBounds = toolbar.removeFromRight (juce::jmin (165, toolbar.getWidth()));
            engineButton.setBounds ({});

            // O sintetizador continua sem rota de interface, mas o Pad Continuo e
            // um produto diferente do editor de samples e precisa continuar
            // encontravel. Duas abas claras devolvem o acesso que desapareceu sem
            // reativar o antigo PAD SYNTH.
            sourceNavigationBounds = area.removeFromTop (44);
            auto padTabs = sourceNavigationBounds.reduced (14, 5);
            const int padTabGap = 7;
            const int padTabWidth = juce::jmax (120,
                juce::jmin (210, (padTabs.getWidth() - padTabGap) / 2));
            synthSourceButton.setBounds ({});
            samplesSourceButton.setBounds (padTabs.removeFromLeft (padTabWidth));
            padTabs.removeFromLeft (padTabGap);
            quickPadButton.setBounds (padTabs.removeFromLeft (padTabWidth));
        }

        updateWorkspaceChrome();

        if (workspaceMode == WorkspaceMode::samples)
        {
            for (auto& button : sectionButtons) button.setBounds ({});
            engineButton.setBounds ({});
            layerEnabledButton.setBounds ({});
            midiChannelBox.setBounds ({});
            sourceBounds = {};
            statusBounds = {};
            padSelectorBounds = {};
            layerLabelBounds = {};
            bankBounds = settingsBounds = synthBounds = effectsBounds = {};
            sampleWorkspace.setBounds (area);
            sampleWorkspace.setVisible (true);
            sampleWorkspace.toFront (false);
            synthSourceButton.toFront (false);
            quickPadButton.toFront (false);
            samplesSourceButton.toFront (false);
            closeButton.toFront (false);
            return;
        }

        sampleWorkspace.setVisible (false);
        sampleWorkspace.setBounds ({});

        auto tabs = area.removeFromTop (36).reduced (14, 3);
        const int tabW = juce::jmax (68, juce::jmin (104,
                                                     tabs.getWidth() / 10));
        for (auto& button : sectionButtons)
        {
            button.setBounds (tabs.removeFromLeft (tabW));
            tabs.removeFromLeft (5);
        }
        tabs.removeFromLeft (8);
        engineButton.setBounds (tabs.removeFromLeft (82).reduced (0, 2));
        tabs.removeFromLeft (5);
        layerEnabledButton.setBounds (tabs.removeFromLeft (82).reduced (0, 2));
        tabs.removeFromLeft (5);
        midiChannelBox.setBounds (tabs.removeFromLeft (96).reduced (0, 2));
        tabs.removeFromLeft (5);
        sourceBounds = tabs.reduced (6, 2);

        auto content = area.reduced (10, 5);
        const int gap = 8;
        const int leftW = juce::jlimit (188, 248, content.getWidth() / 6);
        const int rightW = juce::jlimit (222, 292, content.getWidth() / 5);
        auto left = content.removeFromLeft (leftW);
        content.removeFromLeft (gap);
        auto right = content.removeFromRight (rightW);
        content.removeFromRight (gap);
        auto centre = content;

        // O banco ocupava mais da metade da coluna e deixava PAD SETTINGS com
        // menos altura que a soma de grupo + cores + transpose + modos. O texto
        // TRANSPOSE acabava por cima das bolinhas. O banco continua 4x4, mas
        // cede espaco suficiente para todos os controles abaixo respirarem.
        bankBounds = left.removeFromTop (juce::jmax (190,
                                                     left.getHeight() * 43 / 100));
        left.removeFromTop (gap);
        settingsBounds = left;
        synthBounds = centre;
        effectsBounds = right;

        layoutBank();
        layoutSettings();
        layoutSynth();
        layoutEffects();
    }

private:
    enum class WorkspaceMode { synth, samples };

    void setWorkspaceMode (WorkspaceMode mode)
    {
        juce::ignoreUnused (mode);
        workspaceMode = WorkspaceMode::samples;
        updateWorkspaceChrome();
        resized();
        updateRefreshTimers();
        repaint();
    }

    void updateRefreshTimers()
    {
        const bool showingSamples = isShowing()
                                 && workspaceMode == WorkspaceMode::samples;
        sampleWorkspace.setEditorActive (showingSamples);

        if (isShowing() && workspaceMode == WorkspaceMode::synth)
            startTimerHz (24);
        else
            stopTimer();
    }

    void updateWorkspaceChrome()
    {
        constexpr bool synth = false;
        synthSourceButton.setToggleState (false, juce::dontSendNotification);
        samplesSourceButton.setToggleState (true, juce::dontSendNotification);
        synthSourceButton.setVisible (false);
        quickPadButton.setVisible (true);
        samplesSourceButton.setVisible (true);

        presetBox.setVisible (synth);
        favouriteButton.setVisible (synth);
        copyButton.setVisible (synth);
        pasteButton.setVisible (synth);
        saveButton.setVisible (synth);
        for (auto& button : layerButtons) button.setVisible (synth);
        for (auto& button : sectionButtons) button.setVisible (synth);
        engineButton.setVisible (synth);
        layerEnabledButton.setVisible (synth);
        midiChannelBox.setVisible (synth);
    }

    //==============================================================================
    class ProgramButton final : public juce::TextButton
    {
    public:
        void paintButton (juce::Graphics& g, bool over, bool down) override
        {
            auto r = getLocalBounds().toFloat();
            auto base = findColour (juce::TextButton::buttonColourId);
            if (over) base = base.brighter (0.10f);
            if (down) base = base.darker (0.12f);
            g.setColour (base);
            g.fillRoundedRectangle (r, 5.0f);
            g.setColour (juce::Colour (0xff343a43));
            g.drawRoundedRectangle (r.reduced (0.5f), 5.0f, 1.0f);

            const auto parts = juce::StringArray::fromLines (getButtonText());
            auto inner = getLocalBounds().reduced (6, 4);
            g.setColour (findColour (juce::TextButton::textColourOffId));
            g.setFont (juce::Font (juce::FontOptions (8.0f)));
            g.drawText (parts[0], inner.removeFromTop (11),
                        juce::Justification::centredLeft);
            if (parts.size() > 1)
            {
                g.setFont (juce::Font (juce::FontOptions (7.6f,
                                                          juce::Font::bold)));
                g.drawFittedText (parts[1], inner.reduced (0, 2),
                                  juce::Justification::centred, 2, 0.68f);
            }

            g.setColour (juce::Colour (0xffff3158).withAlpha (
                base.getRed() > 80 ? 0.90f : 0.28f));
            g.fillRoundedRectangle (r.getX() + 8.0f, r.getBottom() - 3.0f,
                                    r.getWidth() - 16.0f, 2.0f, 1.0f);
        }
    };

    class WaveDisplay final : public juce::Component
    {
    public:
        void setWave (int newWave) { wave = newWave; repaint(); }
        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat().reduced (4.0f);
            g.setColour (juce::Colour (0xff090b0e));
            g.fillRoundedRectangle (r, 4.0f);
            g.setColour (juce::Colour (0xff4b515b).withAlpha (0.35f));
            g.drawHorizontalLine ((int) r.getCentreY(), r.getX(), r.getRight());
            juce::Path p;
            for (int i = 0; i <= (int) r.getWidth(); ++i)
            {
                const float phase = (float) i / juce::jmax (1.0f, r.getWidth());
                float y = 0.0f;
                switch (wave)
                {
                    case 0: y = std::sin (phase * juce::MathConstants<float>::twoPi * 2.0f); break;
                    case 1: y = 1.0f - 4.0f * std::abs (std::fmod (phase * 2.0f, 1.0f) - 0.5f); break;
                    case 2: y = 2.0f * std::fmod (phase * 2.0f, 1.0f) - 1.0f; break;
                    case 3: y = std::fmod (phase * 2.0f, 1.0f) < 0.5f ? 1.0f : -1.0f; break;
                    default: y = 0.75f * std::sin (phase * juce::MathConstants<float>::twoPi * 2.0f)
                                       + 0.20f * std::sin (phase * juce::MathConstants<float>::twoPi * 4.0f); break;
                }
                const float px = r.getX() + (float) i;
                const float py = r.getCentreY() - y * r.getHeight() * 0.32f;
                if (i == 0) p.startNewSubPath (px, py); else p.lineTo (px, py);
            }
            g.setColour (juce::Colour (0xffff3158));
            g.strokePath (p, juce::PathStrokeType (1.5f));
        }
    private:
        int wave = 0;
    };

    class FilterGraph final : public juce::Component
    {
    public:
        void setValues (float cutoffToUse, float resonanceToUse)
        {
            cutoff = cutoffToUse; resonance = resonanceToUse; repaint();
        }
        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat().reduced (5.0f);
            g.setColour (juce::Colour (0xff090b0e));
            g.fillRoundedRectangle (r, 4.0f);
            g.setColour (juce::Colour (0xff343a43));
            for (int i = 1; i < 4; ++i)
                g.drawVerticalLine ((int) (r.getX() + r.getWidth() * i / 4.0f), r.getY(), r.getBottom());
            const float norm = std::log (juce::jlimit (70.0f, 18000.0f, cutoff) / 70.0f)
                             / std::log (18000.0f / 70.0f);
            const float cx = r.getX() + norm * r.getWidth();
            juce::Path path;
            path.startNewSubPath (r.getX(), r.getY() + r.getHeight() * 0.25f);
            path.cubicTo (cx - 8.0f, r.getY() + r.getHeight() * 0.25f,
                          cx - 4.0f, r.getY() + r.getHeight() * (0.34f - resonance * 0.22f),
                          cx, r.getY() + r.getHeight() * (0.35f - resonance * 0.22f));
            path.cubicTo (cx + 8.0f, r.getY() + r.getHeight() * 0.42f,
                          cx + 24.0f, r.getBottom() - 4.0f, r.getRight(), r.getBottom() - 3.0f);
            g.setColour (juce::Colour (0xffff3158));
            g.strokePath (path, juce::PathStrokeType (2.0f));
        }
    private:
        float cutoff = 2000.0f, resonance = 0.2f;
    };

    class EnvelopeView final : public juce::Component
    {
    public:
        void setValues (float a, float d, float s, float r)
        {
            attack = a; decay = d; sustain = s; release = r; repaint();
        }
        void paint (juce::Graphics& g) override
        {
            auto b = getLocalBounds().toFloat().reduced (5.0f);
            g.setColour (juce::Colour (0xff090b0e));
            g.fillRoundedRectangle (b, 4.0f);
            const float total = attack + decay + release + 1.0f;
            const float ax = b.getX() + b.getWidth() * attack / total;
            const float dx = ax + b.getWidth() * decay / total;
            const float rx = b.getRight() - b.getWidth() * release / total;
            juce::Path p;
            p.startNewSubPath (b.getX(), b.getBottom());
            p.lineTo (ax, b.getY() + 3.0f);
            p.lineTo (dx, b.getBottom() - sustain * (b.getHeight() - 6.0f));
            p.lineTo (rx, b.getBottom() - sustain * (b.getHeight() - 6.0f));
            p.lineTo (b.getRight(), b.getBottom());
            g.setColour (juce::Colour (0xffff3158));
            g.strokePath (p, juce::PathStrokeType (1.6f));
        }
    private:
        float attack = 1.0f, decay = 1.0f, sustain = 0.8f, release = 4.0f;
    };

    class VelocityGraph final : public juce::Component
    {
    public:
        std::function<void(int)> onFixedChanged;

        void setValues (int newCurve, int newFixed)
        {
            curve = juce::jlimit (0, 3, newCurve);
            fixed = juce::jlimit (1, 127, newFixed);
            repaint();
        }

        void mouseDown (const juce::MouseEvent& e) override { updateFixed (e.position); }
        void mouseDrag (const juce::MouseEvent& e) override { updateFixed (e.position); }

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat().reduced (6.0f);
            g.setColour (juce::Colour (0xff090b0e));
            g.fillRoundedRectangle (r, 4.0f);
            g.setColour (juce::Colour (0xff303640));
            for (int i = 1; i < 4; ++i)
            {
                g.drawVerticalLine ((int) (r.getX() + r.getWidth() * i / 4.0f),
                                    r.getY(), r.getBottom());
                g.drawHorizontalLine ((int) (r.getY() + r.getHeight() * i / 4.0f),
                                      r.getX(), r.getRight());
            }

            juce::Path p;
            for (int i = 0; i <= 64; ++i)
            {
                const float x = (float) i / 64.0f;
                float y = x;
                if (curve == 0) y = std::sqrt (x);
                else if (curve == 2) y = x * x;
                else if (curve == 3) y = (float) fixed / 127.0f;
                const auto point = juce::Point<float> (
                    r.getX() + x * r.getWidth(), r.getBottom() - y * r.getHeight());
                if (i == 0) p.startNewSubPath (point); else p.lineTo (point);
            }
            g.setColour (juce::Colour (0xffff3158));
            g.strokePath (p, juce::PathStrokeType (1.8f));
            const float fy = r.getBottom() - (float) fixed / 127.0f * r.getHeight();
            if (curve == 3)
                g.fillEllipse (r.getRight() - 4.0f, fy - 3.0f, 6.0f, 6.0f);
        }

    private:
        void updateFixed (juce::Point<float> point)
        {
            if (curve != 3 || getHeight() <= 1)
                return;
            const float normalized = 1.0f - juce::jlimit (
                0.0f, 1.0f, point.y / (float) getHeight());
            fixed = juce::jlimit (1, 127, juce::roundToInt (normalized * 127.0f));
            if (onFixedChanged != nullptr) onFixedChanged (fixed);
            repaint();
        }

        int curve = 1;
        int fixed = 100;
    };

    class XYPad final : public juce::Component
    {
    public:
        std::function<void(float, float)> onChanged;
        void setPosition (float newX, float newY)
        {
            x = juce::jlimit (0.0f, 1.0f, newX);
            y = juce::jlimit (0.0f, 1.0f, newY);
            repaint();
        }
        void mouseDown (const juce::MouseEvent& e) override { update (e.position); }
        void mouseDrag (const juce::MouseEvent& e) override { update (e.position); }
        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat().reduced (5.0f);
            g.setColour (juce::Colour (0xff090b0e));
            g.fillRoundedRectangle (r, 4.0f);
            g.setColour (juce::Colour (0xff343a43));
            g.drawLine (r.getCentreX(), r.getY(), r.getCentreX(), r.getBottom());
            g.drawLine (r.getX(), r.getCentreY(), r.getRight(), r.getCentreY());
            const auto point = juce::Point<float> (r.getX() + x * r.getWidth(),
                                                   r.getBottom() - y * r.getHeight());
            g.setColour (juce::Colour (0xffff3158).withAlpha (0.25f));
            g.fillEllipse (juce::Rectangle<float> (20.0f, 20.0f).withCentre (point));
            g.setColour (juce::Colour (0xffff3158));
            g.fillEllipse (juce::Rectangle<float> (8.0f, 8.0f).withCentre (point));
        }
    private:
        void update (juce::Point<float> p)
        {
            auto r = getLocalBounds().toFloat().reduced (5.0f);
            if (r.isEmpty()) return;
            setPosition ((p.x - r.getX()) / r.getWidth(),
                         1.0f - (p.y - r.getY()) / r.getHeight());
            if (onChanged != nullptr) onChanged (x, y);
        }
        float x = 0.5f, y = 0.5f;
    };

    //==============================================================================
    static void configureButton (juce::TextButton& button,
                                 const juce::String& text, bool toggles)
    {
        button.setButtonText (text);
        button.setClickingTogglesState (toggles);
        button.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff181c22));
        button.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffbc1738));
        button.setColour (juce::TextButton::textColourOffId, juce::Colour (0xffd7d9de));
        button.setColour (juce::TextButton::textColourOnId, juce::Colours::white);
        button.setColour (juce::ComboBox::outlineColourId, juce::Colour (0xff343943));
    }

    static void configureCombo (juce::ComboBox& box)
    {
        box.setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff0b0e12));
        box.setColour (juce::ComboBox::textColourId, juce::Colour (0xffeceef2));
        box.setColour (juce::ComboBox::outlineColourId, juce::Colour (0xff353a43));
        box.setColour (juce::ComboBox::arrowColourId, juce::Colour (0xffff3158));
    }

    void configureSlider (juce::Slider& slider, const juce::String& name,
                          bool vertical = false)
    {
        slider.setName (name);
        slider.setSliderStyle (vertical ? juce::Slider::LinearVertical
                                        : juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false,
                                vertical ? 48 : 54, 13);
        slider.setColour (juce::Slider::rotarySliderFillColourId, juce::Colour (0xffff3158));
        slider.setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour (0xff303640));
        slider.setColour (juce::Slider::thumbColourId, juce::Colour (0xffe7e9ed));
        slider.setColour (juce::Slider::textBoxTextColourId, juce::Colour (0xffbdc1c9));
        slider.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        slider.setScrollWheelEnabled (false);
        slider.onDragEnd = [this] { stateWasEdited(); };
        addAndMakeVisible (slider);
    }

    void setupLayerSliders()
    {
        configureSlider (osc1Octave, "OITAVA");
        configureSlider (osc2Octave, "OITAVA");
        configureSlider (subOctave, "SUB OCT");
        configureSlider (osc2Tune, "TUNE");
        configureSlider (osc1Level, "OSC 1", true);
        configureSlider (osc2Level, "OSC 2", true);
        configureSlider (subLevel, "SUB", true);
        configureSlider (noiseLevel, "NOISE", true);
        configureSlider (cutoff, "CUTOFF");
        configureSlider (resonance, "RESO");
        configureSlider (filterEnv, "ENV AMT");
        configureSlider (drive, "DRIVE");
        configureSlider (attack, "ATTACK");
        configureSlider (decay, "DECAY");
        configureSlider (sustain, "SUSTAIN");
        configureSlider (release, "RELEASE");
        configureSlider (modAttack, "ATTACK");
        configureSlider (modDecay, "DECAY");
        configureSlider (modSustain, "SUSTAIN");
        configureSlider (modRelease, "RELEASE");
        configureSlider (pan, "PAN");
        configureSlider (lfoRate, "RATE");
        configureSlider (lfoDepth, "DEPTH");
        configureSlider (lfoFade, "FADE");
        configureSlider (lfoPhase, "PHASE");
    }

    void setupGlobalSliders()
    {
        configureSlider (master, "MASTER");
        configureSlider (transposeSlider, "TRANSPOSE");
        configureSlider (toneLow, "LOW");
        configureSlider (toneMid, "MID");
        configureSlider (toneHigh, "HIGH");
        configureSlider (chorus, "CHORUS");
        configureSlider (reverb, "REVERB");
        configureSlider (delay, "DELAY");
        configureSlider (delayTimeSlider, "TIME");
        configureSlider (delayFeedbackSlider, "FEEDBACK");
        configureSlider (macro1, "AMBIENCIA");
        configureSlider (macro2, "REVERB");
        configureSlider (macro3, "DELAY");
        configureSlider (macro4, "MASTER");
        configureSlider (fadeInSlider, "FADE IN");
        configureSlider (fadeOutSlider, "FADE OUT");
        configureSlider (utilityAttackSlider, "ATTACK");
        configureSlider (utilityReleaseSlider, "RELEASE");
        configureSlider (gateDepthSlider, "DEPTH");

        // Nos cards compactos o valor em caixa rouba quase toda a altura do
        // knob. O parametro continua editavel e o tooltip mostra o valor; a
        // serigrafia permanece legivel como no hardware de palco.
        for (auto* compact : { &cutoff, &resonance, &filterEnv, &pan,
                               &attack, &decay, &sustain, &release,
                               &modAttack, &modDecay, &modSustain, &modRelease,
                               &lfoRate, &lfoDepth, &lfoFade, &lfoPhase,
                               &toneLow, &toneMid, &toneHigh,
                               &reverb, &delay, &chorus,
                               &delayTimeSlider, &delayFeedbackSlider,
                               &macro1, &macro2, &macro3, &macro4,
                               &drive, &utilityAttackSlider,
                               &utilityReleaseSlider, &gateDepthSlider })
            compact->setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    }

    void createGlobalAttachments()
    {
        globalAttachments.clear();
        auto add = [this] (juce::RangedAudioParameter& p, juce::Slider& s)
        {
            globalAttachments.push_back (
                std::make_unique<juce::SliderParameterAttachment> (p, s));
        };
        add (*processor.masterDb, master);
        add (*processor.transpose, transposeSlider);
        add (*processor.toneLow, toneLow);
        add (*processor.toneMid, toneMid);
        add (*processor.toneHigh, toneHigh);
        add (*processor.chorusMix, chorus);
        add (*processor.reverbMix, reverb);
        add (*processor.delayMix, delay);
        add (*processor.delayTime, delayTimeSlider);
        add (*processor.delayFeedback, delayFeedbackSlider);
        add (*processor.macroAmbience, macro1);
        add (*processor.macroReverb, macro2);
        add (*processor.macroDelay, macro3);
        add (*processor.macroMaster, macro4);
        add (*processor.fadeIn, fadeInSlider);
        add (*processor.fadeOut, fadeOutSlider);
        add (*processor.utilityAttack, utilityAttackSlider);
        add (*processor.utilityRelease, utilityReleaseSlider);
        add (*processor.gateDepth, gateDepthSlider);

        globalButtonAttachments.clear();
        auto addButton = [this] (juce::RangedAudioParameter& p,
                                 juce::Button& b)
        {
            globalButtonAttachments.push_back (
                std::make_unique<juce::ButtonParameterAttachment> (p, b));
        };
        addButton (*processor.eqEnabled, effectButtons[0]);
        addButton (*processor.reverbEnabled, effectButtons[1]);
        addButton (*processor.delayEnabled, effectButtons[2]);
        addButton (*processor.chorusEnabled, effectButtons[3]);
        addButton (*processor.gateEnabled, gateButton);

        gateRateAttachment = std::make_unique<juce::ComboBoxParameterAttachment> (
            *processor.gateRate, gateRateBox);
    }

    void rebuildLayerAttachments()
    {
        layerSliderAttachments.clear();
        layerComboAttachments.clear();
        layerEnabledAttachment.reset();

        auto& p = processor.layer (selectedLayer);
        auto add = [this] (juce::RangedAudioParameter& param, juce::Slider& slider)
        {
            layerSliderAttachments.push_back (
                std::make_unique<juce::SliderParameterAttachment> (param, slider));
        };
        add (*p.osc1Octave, osc1Octave);
        add (*p.osc2Octave, osc2Octave);
        add (*p.subOctave, subOctave);
        add (*p.osc2Tune, osc2Tune);
        add (*p.osc1Level, osc1Level);
        add (*p.osc2Level, osc2Level);
        add (*p.subLevel, subLevel);
        add (*p.noiseLevel, noiseLevel);
        add (*p.cutoff, cutoff);
        add (*p.resonance, resonance);
        add (*p.filterEnv, filterEnv);
        add (*p.drive, drive);
        add (*p.attack, attack);
        add (*p.decay, decay);
        add (*p.sustain, sustain);
        add (*p.release, release);
        add (*p.modAttack, modAttack);
        add (*p.modDecay, modDecay);
        add (*p.modSustain, modSustain);
        add (*p.modRelease, modRelease);
        add (*p.pan, pan);
        add (*p.lfoRate, lfoRate);
        add (*p.lfoDepth, lfoDepth);
        add (*p.lfoFade, lfoFade);
        add (*p.lfoPhase, lfoPhase);

        layerComboAttachments.push_back (
            std::make_unique<juce::ComboBoxParameterAttachment> (*p.osc1Wave, osc1WaveBox));
        layerComboAttachments.push_back (
            std::make_unique<juce::ComboBoxParameterAttachment> (*p.osc2Wave, osc2WaveBox));
        layerComboAttachments.push_back (
            std::make_unique<juce::ComboBoxParameterAttachment> (*p.subWave, subWaveBox));
        layerComboAttachments.push_back (
            std::make_unique<juce::ComboBoxParameterAttachment> (*p.noiseType, noiseTypeBox));
        layerComboAttachments.push_back (
            std::make_unique<juce::ComboBoxParameterAttachment> (*p.filterType, filterTypeBox));
        layerComboAttachments.push_back (
            std::make_unique<juce::ComboBoxParameterAttachment> (*p.lfoWave, lfoWaveBox));
        layerEnabledAttachment = std::make_unique<juce::ButtonParameterAttachment> (
            *p.enabled, layerEnabledButton);

        osc1Wave.setWave (p.osc1Wave->getIndex());
        osc2Wave.setWave (p.osc2Wave->getIndex());
        refreshFromProcessor();
    }

    void selectLayer (int index)
    {
        selectedLayer = juce::jlimit (0, PadSynthProcessor::numLayers - 1, index);
        for (int i = 0; i < PadSynthProcessor::numLayers; ++i)
        {
            const bool selected = i == selectedLayer;
            layerButtons[(size_t) i].setColour (
                juce::TextButton::buttonColourId,
                selected ? juce::Colour (0xffa31531) : juce::Colour (0xff15191f));
            layerButtons[(size_t) i].setColour (
                juce::TextButton::textColourOffId,
                selected ? juce::Colours::white : juce::Colour (0xff9197a2));
        }
        rebuildLayerAttachments();
        repaint();
    }

    void syncSectionButtons()
    {
        for (int i = 0; i < (int) sectionButtons.size(); ++i)
        {
            const bool selected = i == selectedSection;
            sectionButtons[(size_t) i].setColour (
                juce::TextButton::buttonColourId,
                selected ? juce::Colour (0xffa31531)
                         : juce::Colour (0xff101318));
            sectionButtons[(size_t) i].setColour (
                juce::TextButton::textColourOffId,
                selected ? juce::Colours::white
                         : juce::Colour (0xff9ca1aa));
        }
    }

    void syncColourButtons()
    {
        static const juce::Colour colours[10] = {
            juce::Colour (0xffff3158), juce::Colour (0xffff8a2a),
            juce::Colour (0xffd9e33a), juce::Colour (0xff28cf63),
            juce::Colour (0xff31c5dc), juce::Colour (0xff347df2),
            juce::Colour (0xff8b43e8), juce::Colour (0xffe441b8),
            juce::Colour (0xffc7afd9), juce::Colour (0xffd8dadd)
        };
        const int selected = processor.padColour != nullptr
                               ? processor.padColour->getIndex() : 0;
        for (int i = 0; i < (int) colourButtons.size(); ++i)
        {
            auto colour = colours[i];
            colourButtons[(size_t) i].setColour (
                juce::TextButton::buttonColourId,
                i == selected ? colour.brighter (0.30f) : colour);
            colourButtons[(size_t) i].setColour (
                juce::TextButton::buttonOnColourId, colour.brighter (0.35f));
            colourButtons[(size_t) i].setTooltip (
                "Cor do Pad " + juce::String (i + 1));
        }
    }

    void syncProgramUi()
    {
        const juce::ScopedValueSetter<bool> guard (syncing, true);
        const int current = processor.getCurrentProgram();
        presetBox.setSelectedId (current + 1, juce::dontSendNotification);
        const bool favourite = processor.isProgramFavourite (current);
        favouriteButton.setToggleState (favourite, juce::dontSendNotification);
        favouriteButton.setButtonText (favourite
            ? juce::String::fromUTF8 ("\xe2\x98\x85")
            : juce::String::fromUTF8 ("\xe2\x98\x86"));
        for (int i = 0; i < PadSynthProcessor::numPrograms; ++i)
        {
            const auto programName = processor.getProgramName (i);
            presetBox.changeItemText (i + 1, programName);
            programButtons[(size_t) i].setButtonText (
                juce::String (i + 1) + "\n" + programName);
            programButtons[(size_t) i].setTooltip (programName);
            programButtons[(size_t) i].setColour (
                juce::TextButton::buttonColourId,
                i == current ? juce::Colour (0xffa91633) : juce::Colour (0xff11151a));
            programButtons[(size_t) i].setColour (
                juce::TextButton::textColourOffId,
                i == current ? juce::Colours::white : juce::Colour (0xffa2a7b0));
        }
        syncColourButtons();
    }

    void timerCallback() override
    {
        if (! isShowing() || workspaceMode != WorkspaceMode::synth)
        {
            stopTimer();
            return;
        }

        const float peak = processor.readAndResetOutputPeak();
        displayedPeak += (peak > displayedPeak ? 0.50f : 0.08f) * (peak - displayedPeak);
        osc1Wave.setWave (processor.layer (selectedLayer).osc1Wave->getIndex());
        osc2Wave.setWave (processor.layer (selectedLayer).osc2Wave->getIndex());
        refreshFromProcessor();
        if (statusText.isNotEmpty()
             && juce::Time::getMillisecondCounter() - statusSetAt > 2200)
        {
            statusText.clear();
            repaint (statusBounds);
        }
        repaint (meterBounds.expanded (2));
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override
    {
        if (! isShowing() || workspaceMode != WorkspaceMode::synth)
            return;

        syncProgramUi();
        pasteButton.setEnabled (processor.canPasteProgram());
        repaint();
    }

    void stateWasEdited()
    {
        if (onStateChanged != nullptr) onStateChanged();
    }

    void setStatus (const juce::String& text, bool warning)
    {
        statusText = text;
        statusWarning = warning;
        statusSetAt = juce::Time::getMillisecondCounter();
        repaint (statusBounds);
    }

    //==============================================================================
    static void drawCard (juce::Graphics& g, juce::Rectangle<int> bounds,
                          const juce::String& title, juce::Colour accent)
    {
        if (bounds.isEmpty()) return;
        g.setColour (juce::Colour (0xff0c0f13));
        g.fillRoundedRectangle (bounds.toFloat(), 8.0f);
        g.setColour (juce::Colour (0xff313640));
        g.drawRoundedRectangle (bounds.toFloat().reduced (0.5f), 8.0f, 1.0f);
        g.setColour (accent);
        g.fillRoundedRectangle ((float) bounds.getX() + 1.0f,
                                (float) bounds.getY() + 1.0f,
                                (float) bounds.getWidth() - 2.0f, 3.0f, 2.0f);
        g.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
        g.drawText (title.toUpperCase(), bounds.reduced (12, 7).removeFromTop (16),
                    juce::Justification::centredLeft);
    }

    static void drawSubCard (juce::Graphics& g, juce::Rectangle<int> bounds,
                             const juce::String& title)
    {
        if (bounds.isEmpty()) return;
        g.setColour (juce::Colour (0xff11151a));
        g.fillRoundedRectangle (bounds.toFloat(), 6.0f);
        g.setColour (juce::Colour (0xff303640));
        g.drawRoundedRectangle (bounds.toFloat().reduced (0.5f), 6.0f, 1.0f);
        g.setColour (juce::Colour (0xffe7e8eb));
        g.setFont (juce::Font (juce::FontOptions (9.5f, juce::Font::bold)));
        g.drawText (title, bounds.reduced (9, 5).removeFromTop (13),
                    juce::Justification::centredLeft);
    }

    void drawToolbar (juce::Graphics& g)
    {
        if (toolbarBounds.isEmpty())
            return;

        g.setColour (juce::Colour (0xff111419));
        g.fillRect (toolbarBounds);
        g.setColour (juce::Colour (0xffef214b));
        g.fillRect (toolbarBounds.getX(), toolbarBounds.getBottom() - 1,
                    toolbarBounds.getWidth(), 1);
        auto title = toolbarBounds.reduced (14, 0).removeFromLeft (245);
        g.setColour (juce::Colour (0xffff3158));
        g.setFont (juce::Font (juce::FontOptions (15.0f, juce::Font::bold)));
        g.drawText (workspaceMode == WorkspaceMode::samples
                        ? "PADS DE SAMPLES" : "PAD SYNTH",
                    title.removeFromTop (31),
                    juce::Justification::centredLeft);
        g.setColour (juce::Colour (0xff7f858f));
        g.setFont (juce::Font (juce::FontOptions (9.0f)));
        g.drawText (workspaceMode == WorkspaceMode::samples
                        ? juce::String::fromUTF8 (
                            "BIBLIOTECA INSTALADA  /  EDI\xc3\x87\xc3\x83O REAL DO CANAL")
                        : juce::String ("4 LAYERS  /  SYNTH ENGINE  /  LIVE READY"),
                    title,
                    juce::Justification::centredLeft);

        if (! padSelectorBounds.isEmpty())
        {
            g.setColour (juce::Colour (0xff0b0e12));
            g.fillRoundedRectangle (padSelectorBounds.toFloat(), 4.0f);
            g.setColour (juce::Colour (0xff353a43));
            g.drawRoundedRectangle (padSelectorBounds.toFloat().reduced (0.5f),
                                    4.0f, 1.0f);
            g.setColour (juce::Colour (0xffe7e8eb));
            g.setFont (juce::Font (juce::FontOptions (10.0f,
                                                       juce::Font::bold)));
            g.drawText ("PAD " + juce::String (processor.getCurrentProgram() + 1),
                        padSelectorBounds, juce::Justification::centred);
        }

        g.setColour (juce::Colour (0xff8f949d));
        g.setFont (juce::Font (juce::FontOptions (8.5f, juce::Font::bold)));
        g.drawText ("CAMADA", layerLabelBounds, juce::Justification::centred);
    }

    void drawComponentLabels (juce::Graphics& g)
    {
        g.setFont (juce::Font (juce::FontOptions (8.2f, juce::Font::bold)));
        g.setColour (juce::Colour (0xff888e98));
        juce::Slider* sliders[] = {
            &osc1Octave, &osc2Octave, &osc2Tune, &osc1Level, &osc2Level,
            &subOctave, &subLevel, &noiseLevel, &cutoff, &resonance, &filterEnv,
            &drive,
            &attack, &decay, &sustain, &release, &pan,
            &modAttack, &modDecay, &modSustain, &modRelease,
            &lfoRate, &lfoDepth, &lfoFade, &lfoPhase,
            &master, &transposeSlider,
            &toneLow, &toneMid, &toneHigh,
            &chorus, &reverb, &delay, &delayTimeSlider, &delayFeedbackSlider,
            &macro1, &macro2, &macro3, &macro4, &fadeInSlider, &fadeOutSlider,
            &utilityAttackSlider, &utilityReleaseSlider, &gateDepthSlider
        };
        for (auto* slider : sliders)
        {
            const auto b = slider->getBounds();
            if (! b.isEmpty())
            {
                // O +8 px antigo fazia os nomes de quatro knobs vizinhos se
                // sobreporem (SUSTAINRELEASE na captura). Cada serigrafia agora
                // fica estritamente dentro da propria celula e reduz a fonte se
                // necessario, sem escapar para o controle ao lado.
                const auto label = juce::Rectangle<int> (b.getX(), b.getY() - 12,
                                                         b.getWidth(), 11)
                                           .reduced (1, 0);
                g.drawFittedText (slider->getName(), label,
                                  juce::Justification::centred, 1, 0.72f);
            }
        }

        if (! meterBounds.isEmpty())
        {
            g.setColour (juce::Colour (0xff07090c));
            g.fillRoundedRectangle (meterBounds.toFloat(), 3.0f);
            const float fill = juce::jlimit (0.0f, 1.0f,
                juce::Decibels::gainToDecibels (displayedPeak, -60.0f) / 60.0f + 1.0f);
            auto bar = meterBounds.toFloat().reduced (2.0f);
            bar.setY (bar.getBottom() - bar.getHeight() * fill);
            g.setGradientFill (juce::ColourGradient (
                juce::Colour (0xffef214b), bar.getX(), bar.getBottom(),
                juce::Colour (0xffffb21e), bar.getX(), bar.getY(), false));
            g.fillRoundedRectangle (bar, 2.0f);
        }
    }

    //==============================================================================
    void layoutBank()
    {
        auto area = bankBounds.reduced (10, 25);
        const int gap = 4;
        const int cellW = juce::jmax (1, (area.getWidth() - gap * 3) / 4);
        const int cellH = juce::jmax (1, (area.getHeight() - gap * 3) / 4);
        for (int i = 0; i < PadSynthProcessor::numPrograms; ++i)
        {
            const int row = i / 4, col = i % 4;
            programButtons[(size_t) i].setBounds (
                area.getX() + col * (cellW + gap),
                area.getY() + row * (cellH + gap), cellW, cellH);
        }
    }

    void layoutSettings()
    {
        auto area = settingsBounds.reduced (10, 25);
        groupBox.setBounds (area.removeFromTop (25));
        area.removeFromTop (6);
        auto colours = area.removeFromTop (22);
        const int cw = juce::jmax (12, colours.getWidth() / (int) colourButtons.size());
        for (auto& button : colourButtons)
            button.setBounds (colours.removeFromLeft (cw).reduced (3));
        area.removeFromTop (5);

        auto transposeArea = area.removeFromTop (52);
        transposeSlider.setBounds (transposeArea.withSizeKeepingCentre (
            juce::jmax (70, transposeArea.getWidth() * 58 / 100), 52));
        master.setBounds ({});
        area.removeFromTop (4);
        auto modes = area.removeFromTop (28);
        holdButton.setBounds (modes.removeFromLeft ((modes.getWidth() - 5) / 2));
        modes.removeFromLeft (5);
        toggleButton.setBounds (modes);
        area.removeFromTop (5);
        const int meterH = juce::jmin (28, area.getHeight());
        meterBounds = area.removeFromBottom (meterH)
                          .withSizeKeepingCentre (10, meterH);
    }

    void layoutSynth()
    {
        auto area = synthBounds.reduced (8, 25);
        const int gap = 7;

        // Na pagina SINTESE, envelopes/LFO/XY pertencem as abas proprias. Antes
        // todos eram comprimidos numa quarta fileira de 50 px, produzindo cards
        // vazios e serigrafia sobreposta. A pagina principal agora dedica toda
        // a altura aos osciladores, mixer e filtro.
        if (selectedSection == 0)
        {
            lfoBounds = ampEnvBounds = modEnvBounds = xyBounds = {};
            juce::Component* hiddenSynthesisPageControls[] {
                &lfoWaveBox, &lfoRate, &lfoDepth, &lfoFade, &lfoPhase,
                &ampEnvelopeView, &attack, &decay, &sustain, &release,
                &modEnvelopeView, &modAttack, &modDecay, &modSustain,
                &modRelease, &xyPad
            };
            for (auto* component : hiddenSynthesisPageControls)
                component->setBounds ({});

            auto oscRow = area.removeFromTop (
                juce::jmax (160, area.getHeight() * 48 / 100));
            osc1Bounds = oscRow.removeFromLeft ((oscRow.getWidth() - gap) / 2);
            oscRow.removeFromLeft (gap);
            osc2Bounds = oscRow;
            layoutOscillator (osc1Bounds, osc1Wave, osc1WaveBox,
                              osc1Octave, nullptr);
            layoutOscillator (osc2Bounds, osc2Wave, osc2WaveBox,
                              osc2Octave, &osc2Tune);

            area.removeFromTop (gap);
            auto lower = area;
            mixerBounds = lower.removeFromLeft (
                juce::jmax (235, lower.getWidth() * 41 / 100));
            lower.removeFromLeft (gap);
            filterBounds = lower;
            layoutMixer();
            layoutFilter();
            return;
        }

        auto oscRow = area.removeFromTop (juce::jmax (138, area.getHeight() * 34 / 100));
        osc1Bounds = oscRow.removeFromLeft ((oscRow.getWidth() - gap) / 2);
        oscRow.removeFromLeft (gap);
        osc2Bounds = oscRow;
        layoutOscillator (osc1Bounds, osc1Wave, osc1WaveBox, osc1Octave, nullptr);
        layoutOscillator (osc2Bounds, osc2Wave, osc2WaveBox, osc2Octave, &osc2Tune);

        area.removeFromTop (gap);
        auto middle = area.removeFromTop (juce::jmax (118, area.getHeight() * 46 / 100));
        mixerBounds = middle.removeFromLeft (juce::jmax (205, middle.getWidth() * 39 / 100));
        middle.removeFromLeft (gap);
        filterBounds = middle;
        layoutMixer();
        layoutFilter();

        area.removeFromTop (gap);
        auto bottom = area;
        const int part = juce::jmax (80, (bottom.getWidth() - gap * 3) / 4);
        lfoBounds = bottom.removeFromLeft (part);
        bottom.removeFromLeft (gap);
        ampEnvBounds = bottom.removeFromLeft (part);
        bottom.removeFromLeft (gap);
        modEnvBounds = bottom.removeFromLeft (part);
        bottom.removeFromLeft (gap);
        xyBounds = bottom;
        layoutBottomCards();
    }

    static void layoutOscillator (juce::Rectangle<int> bounds,
                                  WaveDisplay& wave, juce::ComboBox& box,
                                  juce::Slider& octave, juce::Slider* tune)
    {
        auto area = bounds.reduced (8, 21);
        auto upper = area.removeFromTop (juce::jmax (48, area.getHeight() * 50 / 100));
        wave.setBounds (upper.removeFromLeft (upper.getWidth() * 56 / 100));
        upper.removeFromLeft (6);
        box.setBounds (upper.withSizeKeepingCentre (upper.getWidth(), 25));
        area.removeFromTop (4);
        const int knobW = juce::jmax (50, area.getWidth() / (tune != nullptr ? 2 : 1));
        octave.setBounds (area.removeFromLeft (knobW).reduced (5, 1));
        if (tune != nullptr) tune->setBounds (area.removeFromLeft (knobW).reduced (5, 1));
    }

    void layoutMixer()
    {
        auto area = mixerBounds.reduced (7, 24);
        auto types = area.removeFromBottom (34);
        area.removeFromBottom (3);
        const int w = juce::jmax (38, area.getWidth() / 4);
        osc1Level.setBounds (area.removeFromLeft (w).reduced (4, 1));
        osc2Level.setBounds (area.removeFromLeft (w).reduced (4, 1));
        subLevel.setBounds (area.removeFromLeft (w).reduced (4, 1));
        noiseLevel.setBounds (area.removeFromLeft (w).reduced (4, 1));

        const int subWaveW = juce::jmax (50, types.getWidth() * 38 / 100);
        subWaveBox.setBounds (types.removeFromLeft (subWaveW)
                                   .withSizeKeepingCentre (subWaveW, 24));
        const int subOctW = juce::jmax (42, types.getWidth() / 3);
        auto subOct = types.removeFromLeft (subOctW);
        subOctave.setBounds (subOct.reduced (2));
        noiseTypeBox.setBounds (types.withSizeKeepingCentre (types.getWidth(), 24));
    }

    void layoutFilter()
    {
        auto area = filterBounds.reduced (8, 22);
        filterTypeBox.setBounds (area.removeFromTop (24));
        area.removeFromTop (4);
        auto graph = area.removeFromTop (juce::jmax (42, area.getHeight() * 42 / 100));
        filterGraph.setBounds (graph);
        area.removeFromTop (4);
        const int w = juce::jmax (46, area.getWidth() / 5);
        cutoff.setBounds (area.removeFromLeft (w).reduced (3, 1));
        resonance.setBounds (area.removeFromLeft (w).reduced (3, 1));
        filterEnv.setBounds (area.removeFromLeft (w).reduced (3, 1));
        drive.setBounds (area.removeFromLeft (w).reduced (3, 1));
        pan.setBounds (area.reduced (3, 1));
    }

    void layoutBottomCards()
    {
        auto lfo = lfoBounds.reduced (7, 23);
        lfoWaveBox.setBounds (lfo.removeFromTop (24));
        lfo.removeFromTop (4);
        const int lw = juce::jmax (34, lfo.getWidth() / 4);
        lfoRate.setBounds (lfo.removeFromLeft (lw).reduced (2, 1));
        lfoDepth.setBounds (lfo.removeFromLeft (lw).reduced (2, 1));
        lfoFade.setBounds (lfo.removeFromLeft (lw).reduced (2, 1));
        lfoPhase.setBounds (lfo.reduced (2, 1));

        auto amp = ampEnvBounds.reduced (7, 23);
        ampEnvelopeView.setBounds (amp.removeFromTop (juce::jmax (34, amp.getHeight() / 3)));
        amp.removeFromTop (3);
        const int aw = juce::jmax (36, amp.getWidth() / 4);
        attack.setBounds (amp.removeFromLeft (aw).reduced (1));
        decay.setBounds (amp.removeFromLeft (aw).reduced (1));
        sustain.setBounds (amp.removeFromLeft (aw).reduced (1));
        release.setBounds (amp.removeFromLeft (aw).reduced (1));

        auto mod = modEnvBounds.reduced (7, 23);
        modEnvelopeView.setBounds (mod.removeFromTop (juce::jmax (34, mod.getHeight() / 3)));
        mod.removeFromTop (3);
        const int mw = juce::jmax (34, mod.getWidth() / 4);
        modAttack.setBounds (mod.removeFromLeft (mw).reduced (1));
        modDecay.setBounds (mod.removeFromLeft (mw).reduced (1));
        modSustain.setBounds (mod.removeFromLeft (mw).reduced (1));
        modRelease.setBounds (mod.reduced (1));
        xyPad.setBounds (xyBounds.reduced (7, 23));
    }

    void layoutEffects()
    {
        auto area = effectsBounds.reduced (8, 25);
        const int gap = 7;
        const int totalHeight = area.getHeight();
        // Reserva de baixo para cima: VELOCIDADE deixava de existir quando as
        // tres secoes acima consumiam seus minimos. Cada painel agora recebe um
        // piso real antes de FX usar a sobra.
        velocityBounds = area.removeFromBottom (
            juce::jmin (105, juce::jmax (76, totalHeight * 18 / 100)));
        area.removeFromBottom (gap);
        utilityBounds = area.removeFromBottom (
            juce::jmin (132, juce::jmax (96, totalHeight * 23 / 100)));
        area.removeFromBottom (gap);
        macroBounds = area.removeFromBottom (
            juce::jmin (92, juce::jmax (68, totalHeight * 15 / 100)));
        area.removeFromBottom (gap);
        fxSlotsBounds = area;

        auto slots = fxSlotsBounds;
        const int rowH = juce::jmax (30, slots.getHeight() / 4);
        for (int i = 0; i < 4; ++i)
        {
            auto row = slots.removeFromTop (rowH).reduced (1, 2);
            effectButtons[(size_t) i].setBounds (row.removeFromLeft (78));
            row.removeFromLeft (4);
            if (i == 0)
            {
                const int w = row.getWidth() / 3;
                toneLow.setBounds (row.removeFromLeft (w).reduced (2));
                toneMid.setBounds (row.removeFromLeft (w).reduced (2));
                toneHigh.setBounds (row.reduced (2));
            }
            else if (i == 1)
                reverb.setBounds (row.withSizeKeepingCentre (60, row.getHeight()));
            else if (i == 2)
            {
                const int w = row.getWidth() / 3;
                delay.setBounds (row.removeFromLeft (w).reduced (2));
                delayTimeSlider.setBounds (row.removeFromLeft (w).reduced (2));
                delayFeedbackSlider.setBounds (row.reduced (2));
            }
            else
                chorus.setBounds (row.withSizeKeepingCentre (60, row.getHeight()));
        }

        auto macros = macroBounds.reduced (7, 24);
        const int mw = juce::jmax (38, macros.getWidth() / 4);
        macro1.setBounds (macros.removeFromLeft (mw).reduced (2));
        macro2.setBounds (macros.removeFromLeft (mw).reduced (2));
        macro3.setBounds (macros.removeFromLeft (mw).reduced (2));
        macro4.setBounds (macros.removeFromLeft (mw).reduced (2));

        auto util = utilityBounds.reduced (7, 23);
        auto topUtil = util.removeFromTop (juce::jmax (42, util.getHeight() * 58 / 100));
        const int uw = juce::jmax (38, topUtil.getWidth() / 4);
        fadeInSlider.setBounds (topUtil.removeFromLeft (uw).reduced (2));
        fadeOutSlider.setBounds (topUtil.removeFromLeft (uw).reduced (2));
        utilityAttackSlider.setBounds (topUtil.removeFromLeft (uw).reduced (2));
        utilityReleaseSlider.setBounds (topUtil.reduced (2));
        util.removeFromTop (3);
        gateButton.setBounds (util.removeFromLeft (58).withSizeKeepingCentre (58, 25));
        util.removeFromLeft (4);
        const int gateRateW = juce::jmax (55, util.getWidth() / 2);
        gateRateBox.setBounds (util.removeFromLeft (gateRateW)
                                   .withSizeKeepingCentre (gateRateW, 25));
        gateDepthSlider.setBounds (util.reduced (2));

        auto velocityArea = velocityBounds.reduced (7, 22);
        velocityBox.setBounds (velocityArea.removeFromBottom (25));
        velocityArea.removeFromBottom (4);
        velocityGraph.setBounds (velocityArea);

        fxEqBounds = {};
        fxSpaceBounds = {};
    }

    PadSynthProcessor& processor;
    WorkspaceMode workspaceMode = WorkspaceMode::samples;
    int selectedLayer = 0;
    int selectedSection = 0;
    int sourceChannel = -1;
    juce::String sourceName;
    bool syncing = false;

    juce::TextButton closeButton, synthSourceButton, quickPadButton;
    juce::TextButton samplesSourceButton, engineButton;
    juce::TextButton favouriteButton, copyButton, pasteButton, saveButton;
    std::array<juce::TextButton, 7> sectionButtons;
    juce::ComboBox presetBox, midiChannelBox, velocityBox, groupBox;
    std::array<juce::TextButton, PadSynthProcessor::numLayers> layerButtons;
    std::array<ProgramButton, PadSynthProcessor::numPrograms> programButtons;
    std::array<juce::TextButton, 10> colourButtons;
    std::array<juce::TextButton, 4> effectButtons;
    juce::TextButton layerEnabledButton, holdButton, toggleButton, gateButton;
    juce::ComboBox osc1WaveBox, osc2WaveBox, subWaveBox, noiseTypeBox;
    juce::ComboBox filterTypeBox, lfoWaveBox, gateRateBox;
    WaveDisplay osc1Wave, osc2Wave;
    FilterGraph filterGraph;
    EnvelopeView ampEnvelopeView, modEnvelopeView;
    VelocityGraph velocityGraph;
    XYPad xyPad;
    SamplePadEditorPanel sampleWorkspace;

    juce::Slider osc1Octave, osc2Octave, subOctave, osc2Tune;
    juce::Slider osc1Level, osc2Level, subLevel, noiseLevel;
    juce::Slider cutoff, resonance, filterEnv, drive;
    juce::Slider attack, decay, sustain, release;
    juce::Slider modAttack, modDecay, modSustain, modRelease, pan;
    juce::Slider lfoRate, lfoDepth, lfoFade, lfoPhase;
    juce::Slider master, transposeSlider, toneLow, toneMid, toneHigh;
    juce::Slider chorus, reverb, delay;
    juce::Slider delayTimeSlider, delayFeedbackSlider;
    juce::Slider macro1, macro2, macro3, macro4, fadeInSlider, fadeOutSlider;
    juce::Slider utilityAttackSlider, utilityReleaseSlider, gateDepthSlider;

    std::unique_ptr<juce::ButtonParameterAttachment> engineAttachment;
    std::unique_ptr<juce::ComboBoxParameterAttachment> midiAttachment, velocityAttachment;
    std::unique_ptr<juce::ComboBoxParameterAttachment> groupAttachment, gateRateAttachment;
    std::unique_ptr<juce::ButtonParameterAttachment> layerEnabledAttachment;
    std::unique_ptr<juce::ButtonParameterAttachment> holdAttachment, toggleAttachment;
    std::vector<std::unique_ptr<juce::SliderParameterAttachment>> layerSliderAttachments;
    std::vector<std::unique_ptr<juce::ComboBoxParameterAttachment>> layerComboAttachments;
    std::vector<std::unique_ptr<juce::SliderParameterAttachment>> globalAttachments;
    std::vector<std::unique_ptr<juce::ButtonParameterAttachment>> globalButtonAttachments;

    juce::Rectangle<int> toolbarBounds, sourceNavigationBounds;
    juce::Rectangle<int> bankBounds, settingsBounds, synthBounds, effectsBounds;
    juce::Rectangle<int> osc1Bounds, osc2Bounds, mixerBounds, filterBounds;
    juce::Rectangle<int> lfoBounds, ampEnvBounds, modEnvBounds, xyBounds;
    juce::Rectangle<int> fxEqBounds, fxSpaceBounds, fxSlotsBounds;
    juce::Rectangle<int> macroBounds, utilityBounds, velocityBounds;
    juce::Rectangle<int> sourceBounds, statusBounds, meterBounds;
    juce::Rectangle<int> padSelectorBounds, layerLabelBounds;
    juce::String statusText;
    bool statusWarning = false;
    juce::uint32 statusSetAt = 0;
    float displayedPeak = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PadEditorPanel)
};
