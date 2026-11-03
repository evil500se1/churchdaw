#pragma once

#include "TimbresProcessor.h"
#include "NordLookAndFeel.h"
#include "MidiLearnTargetTag.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <memory>
#include <vector>

//==============================================================================
// Editor dos PADS SAMPLEADOS que ja pertencem ao Church Live. Ele nao cria um
// segundo motor: lista o catalogo real do TimbresProcessor, carrega a entrada no
// canal escolhido e prende cada controle diretamente aos parametros desse canal.
//==============================================================================
class SamplePadEditorPanel final : public juce::Component,
                                   private juce::ListBoxModel,
                                   private juce::ChangeListener,
                                   private juce::Timer
{
public:
    enum EditorPage
    {
        samplePage = 0,
        filterPage,
        ampEqPage
    };

    SamplePadEditorPanel()
    {
        setOpaque (true);

        padList.setModel (this);
        padList.setRowHeight (46);
        padList.setColour (juce::ListBox::backgroundColourId,
                           juce::Colours::transparentBlack);
        padList.setColour (juce::ListBox::outlineColourId,
                           juce::Colour (0xff303640));
        padList.setOutlineThickness (1);
        addAndMakeVisible (padList);

        configureCombo (channelBox);
        channelBox.onChange = [this]
        {
            if (syncing || channelBox.getSelectedId() <= 0)
                return;

            targetLayer = channelBox.getSelectedId() - 1;
            rebuildAttachments();
            repaint();
        };
        addAndMakeVisible (channelBox);

        configureButton (loadButton, "CARREGAR NO CANAL", false);
        loadButton.onClick = [this] { loadSelectedPad(); };
        addAndMakeVisible (loadButton);

        configureButton (refreshButton, "ATUALIZAR LISTA", false);
        refreshButton.onClick = [this]
        {
            knownEntryCount = -1;
            refreshPadEntries (true);
        };
        addAndMakeVisible (refreshButton);

        configureButton (onButton, "CANAL ON", true);
        onButton.onClick = [this]
        {
            if (onStateChanged != nullptr) onStateChanged();
        };
        addAndMakeVisible (onButton);

        configureSlider (volume, "VOLUME", true);
        configureSlider (attack, "ATTACK");
        configureSlider (release, "RELEASE");
        configureSlider (lowCut, "LOW CUT");
        configureSlider (highCut, "HIGH CUT");
        configureSlider (reverb, "REVERB");
        configureSlider (delay, "DELAY");
        configureSlider (transpose, "TRANSPOSE");

        for (auto* tab : { &samplePageButton, &filterPageButton,
                           &ampEqPageButton })
        {
            configureButton (*tab, {}, true);
            tab->setRadioGroupId (0x504144);
            addAndMakeVisible (*tab);
        }
        samplePageButton.setButtonText ("SAMPLE");
        filterPageButton.setButtonText ("FILTRO NORD");
        ampEqPageButton.setButtonText ("AMP / EQ");
        samplePageButton.onClick = [this] { setEditorPage (samplePage); };
        filterPageButton.onClick = [this] { setEditorPage (filterPage); };
        ampEqPageButton.onClick = [this] { setEditorPage (ampEqPage); };
        samplePageButton.setToggleState (true, juce::dontSendNotification);

        configureButton (filterOnButton, "FILTRO ON", true);
        configureButton (brightButton, "BRIGHT", true);
        configureButton (filterVelocityButton, "VELOCITY", true);
        configureButton (ampEqOnButton, "AMP EQ ON", true);
        for (auto* button : { &filterOnButton, &brightButton,
                              &filterVelocityButton, &ampEqOnButton })
            addAndMakeVisible (*button);

        configureCombo (filterTypeBox);
        filterTypeBox.addItemList (
            { "LP12", "LP24", "LP M", "LP+HP", "BP", "HP" }, 1);
        configureCombo (keyTrackBox);
        keyTrackBox.addItemList ({ "Off", "1/3", "2/3", "1:1" }, 1);
        configureCombo (driveBox);
        driveBox.addItemList ({ "Off", "Drive 1", "Drive 2", "Drive 3" }, 1);
        for (auto* box : { &filterTypeBox, &keyTrackBox, &driveBox })
            addAndMakeVisible (*box);

        configureSlider (filterCutoff, "CUTOFF");
        configureSlider (filterResonance, "RES / HP");
        configureSlider (filterEnvAmount, "ENV AMT");
        configureSlider (filterEnvAttack, "FILTER ATTACK");
        configureSlider (filterEnvDecay, "FILTER DECAY");
        configureSlider (filterEnvRelease, "FILTER RELEASE");
        configureSlider (ampBass, "BASS");
        configureSlider (ampMid, "MID");
        configureSlider (ampMidHz, "MID FREQ");
        configureSlider (ampTreble, "TREBLE");
        configureSlider (compressor, "COMPRESSOR");
        filterCutoff.setTextValueSuffix (" Hz");
        filterEnvAttack.setTextValueSuffix (" s");
        filterEnvDecay.setTextValueSuffix (" s");
        filterEnvRelease.setTextValueSuffix (" s");
        ampBass.setTextValueSuffix (" dB");
        ampMid.setTextValueSuffix (" dB");
        ampMidHz.setTextValueSuffix (" Hz");
        ampTreble.setTextValueSuffix (" dB");

        addAndMakeVisible (waveform);
        updateEditorPageVisibility();
    }

    ~SamplePadEditorPanel() override
    {
        stopTimer();
        if (processor != nullptr)
            processor->removeChangeListener (this);
    }

    std::function<void()> onStateChanged;

    void setEditorActive (bool shouldBeActive)
    {
        if (editorActive == shouldBeActive)
            return;

        editorActive = shouldBeActive;
        if (editorActive)
        {
            refreshFromProcessor();
            startTimerHz (12);
        }
        else
        {
            stopTimer();
        }
    }

    void setProcessor (TimbresProcessor* newProcessor)
    {
        if (processor == newProcessor)
        {
            refreshFromProcessor();
            return;
        }

        if (processor != nullptr)
            processor->removeChangeListener (this);

        processor = newProcessor;
        attachments.clear();
        buttonAttachments.clear();
        comboAttachments.clear();
        onAttachment.reset();

        if (processor != nullptr)
        {
            processor->addChangeListener (this);
            int selected = processor->getSelectedLayer();
            if (! juce::isPositiveAndBelow (selected, TimbresProcessor::numLayers))
                selected = firstLoadedPadLayer();
            targetLayer = juce::jlimit (0, TimbresProcessor::numLayers - 1,
                                       selected < 0 ? 0 : selected);
        }

        knownEntryCount = -1;
        refreshChannelBox();
        refreshPadEntries (true);
        rebuildAttachments();
        repaint();
    }

    void setTargetLayer (int layer)
    {
        targetLayer = juce::jlimit (0, TimbresProcessor::numLayers - 1, layer);
        refreshChannelBox();
        rebuildAttachments();

        if (processor != nullptr)
        {
            const auto current = processor->getLayerTimbre (targetLayer);
            for (int i = 0; i < padEntries.size(); ++i)
                if (padEntries.getReference (i).label == current)
                {
                    padList.selectRow (i);
                    selectPadRow (i);
                    break;
                }
        }
    }

    void refreshFromProcessor()
    {
        if (! editorActive)
            return;

        refreshChannelBox();
        refreshPadEntries (false);
        rebuildAttachments();
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff090b0e));

        drawPanel (g, libraryBounds,
                   juce::String::fromUTF8 ("PADS DISPON\xc3\x8dVEIS"),
                   juce::Colour (0xffef214b));
        const juce::String editorTitle =
            editorPage == samplePage ? "EDITOR DE SAMPLE"
          : editorPage == filterPage ? "FILTRO / ENVELOPE NORD"
                                     : "AMP / EQ NORD";
        drawPanel (g, editorBounds, editorTitle,
                   juce::Colour (0xffef214b));
        drawPanel (g, liveBounds, "PAD NO CANAL",
                   juce::Colour (0xff55d789));

        g.setColour (juce::Colour (0xff8e949e));
        g.setFont (juce::Font (juce::FontOptions (10.0f)));
        g.drawText (juce::String (padEntries.size()) + " pads sampleados instalados",
                    libraryFooterBounds, juce::Justification::centredLeft);

        const auto selectedName = selectedEntryIndex >= 0
                                && selectedEntryIndex < padEntries.size()
            ? TimbresProcessor::prettyLabel (
                  padEntries.getReference (selectedEntryIndex).label)
            : juce::String ("Selecione um pad");

        g.setColour (juce::Colour (0xfff1f2f5));
        g.setFont (juce::Font (juce::FontOptions (22.0f,
                                                  juce::Font::bold)));
        g.drawText (selectedName, sampleTitleBounds,
                    juce::Justification::centredLeft, true);

        g.setColour (juce::Colour (0xffef214b));
        g.setFont (juce::Font (juce::FontOptions (9.5f,
                                                  juce::Font::bold)));
        g.drawText ("SAMPLE", sampleBadgeBounds,
                    juce::Justification::centred);

        juce::String detail;
        if (selectedEntryIndex >= 0 && selectedEntryIndex < padEntries.size())
        {
            const auto& entry = padEntries.getReference (selectedEntryIndex);
            detail = entry.library;
            if (entry.files.size() > 0)
            {
                const auto file = entry.files.getFirst();
                if (detail.isNotEmpty())
                    detail << juce::String::fromUTF8 ("  \xc2\xb7  ");
                detail << file.getFileName()
                       << juce::String::fromUTF8 ("  \xc2\xb7  ")
                       << juce::File::descriptionOfSizeInBytes (file.getSize());
            }
        }
        g.setColour (juce::Colour (0xff9ba1ab));
        g.setFont (juce::Font (juce::FontOptions (10.0f)));
        g.drawFittedText (detail, sampleDetailBounds,
                          juce::Justification::centredLeft, 1, 0.9f);

        g.setColour (juce::Colour (0xff8f96a1));
        g.setFont (juce::Font (juce::FontOptions (9.0f,
                                                  juce::Font::bold)));
        for (size_t i = 0; i < controlLabelBounds.size(); ++i)
            if (! controlLabelBounds[i].isEmpty()
                 && controlNames[i].isNotEmpty())
                g.drawText (controlNames[i], controlLabelBounds[i],
                            juce::Justification::centred);

        if (! meterBounds.isEmpty())
        {
            auto meter = meterBounds.toFloat();
            g.setColour (juce::Colour (0xff05070a));
            g.fillRoundedRectangle (meter, 4.0f);
            g.setColour (juce::Colour (0xff2a3038));
            g.drawRoundedRectangle (meter.reduced (0.5f), 4.0f, 1.0f);

            auto fill = meter.reduced (3.0f);
            fill.setY (fill.getBottom() - fill.getHeight() * displayedLevel);
            g.setGradientFill (juce::ColourGradient (
                juce::Colour (0xff7f0d20), fill.getX(), fill.getBottom(),
                juce::Colour (0xffff3158), fill.getX(), fill.getY(), false));
            g.fillRoundedRectangle (fill, 2.0f);
        }

        const auto loaded = processor != nullptr
                          ? TimbresProcessor::prettyLabel (
                                processor->getLayerTimbre (targetLayer))
                          : juce::String();
        g.setColour (juce::Colour (0xff8f96a1));
        g.setFont (juce::Font (juce::FontOptions (9.0f,
                                                  juce::Font::bold)));
        g.drawText ("PAD ATUAL", currentLabelBounds,
                    juce::Justification::centredLeft);
        g.setColour (loaded.isNotEmpty() ? juce::Colour (0xfff0f1f4)
                                         : juce::Colour (0xff777d87));
        g.setFont (juce::Font (juce::FontOptions (15.0f,
                                                  juce::Font::bold)));
        g.drawFittedText (loaded.isNotEmpty() ? loaded : "CANAL VAZIO",
                          currentNameBounds, juce::Justification::centredLeft,
                          1, 0.86f);

        g.setColour (statusWarning ? juce::Colour (0xffe7a62f)
                                   : juce::Colour (0xff55d789));
        g.setFont (juce::Font (juce::FontOptions (10.0f,
                                                  juce::Font::bold)));
        g.drawFittedText (statusText, statusBounds,
                          juce::Justification::centred, 2, 0.86f);
    }

    void resized() override
    {
        const bool compactMobile = getWidth() < 900 || getHeight() < 420;
        auto area = getLocalBounds().reduced (compactMobile ? 8 : 12, compactMobile ? 6 : 10);
        const int gap = compactMobile ? 6 : 10;
        const int leftW = compactMobile
            ? juce::jlimit (148, 200, area.getWidth() * 23 / 100)
            : juce::jlimit (225, 310, area.getWidth() / 5);
        const int rightW = compactMobile
            ? juce::jlimit (152, 196, area.getWidth() * 23 / 100)
            : juce::jlimit (215, 270, area.getWidth() / 6);

        libraryBounds = area.removeFromLeft (leftW);
        area.removeFromLeft (gap);
        liveBounds = area.removeFromRight (rightW);
        area.removeFromRight (gap);
        editorBounds = area;

        auto library = libraryBounds.reduced (compactMobile ? 8 : 12, compactMobile ? 22 : 28);
        libraryFooterBounds = library.removeFromBottom (compactMobile ? 20 : 28);
        library.removeFromBottom (compactMobile ? 4 : 6);
        refreshButton.setBounds (library.removeFromBottom (compactMobile ? 32 : 30));
        library.removeFromBottom (compactMobile ? 5 : 7);
        padList.setBounds (library);

        auto editor = editorBounds.reduced (compactMobile ? 8 : 13, compactMobile ? 22 : 29);
        auto title = editor.removeFromTop (compactMobile ? 28 : 48);
        sampleBadgeBounds = title.removeFromRight (compactMobile ? 58 : 66)
                                 .withSizeKeepingCentre (compactMobile ? 54 : 62, 20);
        sampleTitleBounds = title.removeFromLeft (
            juce::jmax (compactMobile ? 96 : 140, title.getWidth() * 44 / 100));
        sampleDetailBounds = title;
        editor.removeFromTop (compactMobile ? 4 : 7);
        auto tabs = editor.removeFromTop (compactMobile ? 32 : 31);
        const int tabW = juce::jmax (64, (tabs.getWidth() - 12) / 3);
        samplePageButton.setBounds (tabs.removeFromLeft (tabW));
        tabs.removeFromLeft (6);
        filterPageButton.setBounds (tabs.removeFromLeft (tabW));
        tabs.removeFromLeft (6);
        ampEqPageButton.setBounds (tabs);
        editor.removeFromTop (compactMobile ? 5 : 8);

        for (auto& bounds : controlLabelBounds) bounds = {};
        for (auto& name : controlNames) name.clear();

        const auto layoutGrid = [&] (juce::Rectangle<int> controls,
                                     juce::Component* const* components,
                                     const juce::String* names,
                                     int count, int columns, int rows)
        {
            const int cellW = juce::jmax (
                compactMobile ? 44 : 58,
                (controls.getWidth() - gap * (columns - 1)) / columns);
            const int cellH = juce::jmax (
                compactMobile ? 38 : 54,
                (controls.getHeight() - gap * (rows - 1)) / rows);

            for (int i = 0; i < count; ++i)
            {
                const int row = i / columns;
                const int col = i % columns;
                auto cell = juce::Rectangle<int> (
                    controls.getX() + col * (cellW + gap),
                    controls.getY() + row * (cellH + gap), cellW, cellH);
                controlLabelBounds[(size_t) i] = cell.removeFromTop (compactMobile ? 13 : 17);
                controlNames[(size_t) i] = names[i];

                if (dynamic_cast<juce::Slider*> (components[i]) != nullptr)
                    components[i]->setBounds (cell.reduced (compactMobile ? 2 : 4, 0));
                else
                    components[i]->setBounds (
                        cell.reduced (compactMobile ? 2 : 4, 0).withSizeKeepingCentre (
                            juce::jmax (20, cell.getWidth() - (compactMobile ? 4 : 8)),
                            juce::jmin (32, cell.getHeight())));
            }
        };

        if (editorPage == samplePage)
        {
            waveform.setBounds (editor.removeFromTop (compactMobile
                ? juce::jlimit (40, 68, editor.getHeight() * 26 / 100)
                : juce::jlimit (120, 220, editor.getHeight() * 38 / 100)));
            editor.removeFromTop (compactMobile ? 5 : 8);
            juce::Component* components[] = {
                &attack, &release, &lowCut, &highCut,
                &reverb, &delay, &transpose, &volume
            };
            const juce::String names[] = {
                "ATTACK", "RELEASE", "HPF LEGADO", "LPF LEGADO",
                "REVERB", "DELAY", "TRANSPOSE", "VOLUME"
            };
            layoutGrid (editor, components, names, 8, 4, 2);
        }
        else if (editorPage == filterPage)
        {
            waveform.setBounds ({});
            juce::Component* components[] = {
                &filterOnButton, &filterTypeBox,
                &filterCutoff, &filterResonance,
                &keyTrackBox, &driveBox,
                &brightButton, &filterVelocityButton,
                &filterEnvAmount, &filterEnvAttack,
                &filterEnvDecay, &filterEnvRelease
            };
            const juce::String names[] = {
                "FILTRO", "TIPO", "CUTOFF", "RES / FREQ HP",
                "KEY TRACK", "DRIVE", "BRIGHT", "VELOCITY",
                "ENV AMOUNT", "ENV ATTACK", "ENV DECAY", "ENV RELEASE"
            };
            layoutGrid (editor, components, names, 12, 4, 3);
        }
        else
        {
            waveform.setBounds ({});
            juce::Component* components[] = {
                &ampEqOnButton, &ampBass, &ampMid, &ampMidHz,
                &ampTreble, &compressor, &reverb, &delay
            };
            const juce::String names[] = {
                "AMP EQ", "BASS 100 HZ", "MID", "MID FREQ",
                "TREBLE 4 KHZ", "COMPRESSOR", "REVERB", "DELAY"
            };
            layoutGrid (editor, components, names, 8, 4, 2);
        }

        auto live = liveBounds.reduced (compactMobile ? 8 : 13, compactMobile ? 22 : 30);
        currentLabelBounds = live.removeFromTop (compactMobile ? 14 : 16);
        currentNameBounds = live.removeFromTop (compactMobile ? 22 : 30);
        live.removeFromTop (compactMobile ? 4 : 8);
        channelBox.setBounds (live.removeFromTop (compactMobile ? 32 : 32));
        live.removeFromTop (compactMobile ? 5 : 8);
        onButton.setBounds (live.removeFromTop (compactMobile ? 32 : 32));
        live.removeFromTop (compactMobile ? 6 : 12);
        loadButton.setBounds (live.removeFromBottom (compactMobile ? 40 : 44));
        live.removeFromBottom (compactMobile ? 4 : 8);
        statusBounds = live.removeFromBottom (
            juce::jmin (compactMobile ? 34 : 50, juce::jmax (18, live.getHeight() / 2)));
        live.removeFromBottom (compactMobile ? 4 : 10);
        auto meterRow = live.removeFromTop (juce::jmax (0, juce::jmin (210, live.getHeight())));
        meterBounds = meterRow.withSizeKeepingCentre (26, meterRow.getHeight());
    }

private:
    //==============================================================================
    class Waveform final : public juce::Component
    {
    public:
        Waveform() : thumbnail (256, formats, cache)
        {
            formats.registerBasicFormats();
        }

        void setFile (const juce::File& file)
        {
            currentFile = file;
            thumbnail.clear();
            if (file.existsAsFile())
                thumbnail.setSource (new juce::FileInputSource (file));
            repaint();
        }

        void paint (juce::Graphics& g) override
        {
            auto b = getLocalBounds().toFloat();
            g.setColour (juce::Colour (0xff07090c));
            g.fillRoundedRectangle (b, 6.0f);
            g.setColour (juce::Colour (0xff303640));
            g.drawRoundedRectangle (b.reduced (0.5f), 6.0f, 1.0f);

            auto content = getLocalBounds().reduced (12, 12);
            if (thumbnail.getTotalLength() > 0.0)
            {
                g.setColour (juce::Colour (0xffef3153).withAlpha (0.88f));
                thumbnail.drawChannels (g, content, 0.0,
                                        thumbnail.getTotalLength(), 1.0f);
                g.setColour (juce::Colours::white.withAlpha (0.10f));
                g.drawVerticalLine (content.getCentreX(),
                                    (float) content.getY(),
                                    (float) content.getBottom());
            }
            else
            {
                g.setColour (juce::Colour (0xff777e89));
                g.setFont (juce::Font (juce::FontOptions (12.0f)));
                g.drawText (juce::String::fromUTF8 (
                                "Selecione um pad sampleado para ver o \xc3\xa1udio real"),
                            content, juce::Justification::centred);
            }
        }

    private:
        juce::AudioFormatManager formats;
        juce::AudioThumbnailCache cache { 8 };
        juce::AudioThumbnail thumbnail;
        juce::File currentFile;
    };

    int getNumRows() override { return padEntries.size(); }

    void paintListBoxItem (int row, juce::Graphics& g, int width, int height,
                           bool selected) override
    {
        if (! juce::isPositiveAndBelow (row, padEntries.size()))
            return;

        const auto& entry = padEntries.getReference (row);
        auto b = juce::Rectangle<float> (2.0f, 2.0f,
                                         (float) width - 4.0f,
                                         (float) height - 4.0f);
        g.setColour (selected ? juce::Colour (0xff5a1322)
                              : juce::Colour (0xff12161b));
        g.fillRoundedRectangle (b, 5.0f);
        g.setColour (selected ? juce::Colour (0xffff3158).withAlpha (0.72f)
                              : juce::Colour (0xff2c323a));
        g.drawRoundedRectangle (b.reduced (0.5f), 5.0f, 1.0f);

        auto text = juce::Rectangle<int> (12, 3, width - 78, height - 6);
        g.setColour (juce::Colour (0xffeef0f3));
        g.setFont (juce::Font (juce::FontOptions (12.0f,
                                                  selected ? juce::Font::bold
                                                           : juce::Font::plain)));
        g.drawFittedText (TimbresProcessor::prettyLabel (entry.label),
                          text.removeFromTop (23),
                          juce::Justification::centredLeft, 1, 0.88f);
        g.setColour (juce::Colour (0xff8a919c));
        g.setFont (juce::Font (juce::FontOptions (8.5f)));
        g.drawFittedText (entry.library, text,
                          juce::Justification::centredLeft, 1, 0.82f);

        auto badge = juce::Rectangle<int> (width - 64, 13, 52, 20);
        g.setColour (juce::Colour (0xffef214b).withAlpha (selected ? 0.24f : 0.10f));
        g.fillRoundedRectangle (badge.toFloat(), 4.0f);
        g.setColour (juce::Colour (0xffd9dce2));
        g.setFont (juce::Font (juce::FontOptions (8.0f,
                                                  juce::Font::bold)));
        g.drawText ("SAMPLE", badge, juce::Justification::centred);
    }

    void selectedRowsChanged (int row) override { selectPadRow (row); }

    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override
    {
        selectPadRow (row);
        loadSelectedPad();
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override
    {
        if (! editorActive)
            return;

        juce::Component::SafePointer<SamplePadEditorPanel> safe (this);
        juce::MessageManager::callAsync ([safe]
        {
            if (auto* panel = safe.getComponent();
                panel != nullptr && panel->editorActive)
                panel->refreshFromProcessor();
        });
    }

    void timerCallback() override
    {
        if (! editorActive || ! isShowing())
        {
            setEditorActive (false);
            return;
        }

        if (processor == nullptr)
        {
            displayedLevel *= 0.82f;
            repaint (meterBounds);
            return;
        }

        const auto currentCount = processor->getEntriesNonBlocking().size();
        if (currentCount != knownEntryCount)
            refreshPadEntries (false);

        const float level = juce::jlimit (0.0f, 1.0f,
            processor->getLayerLevelShown (targetLayer));
        displayedLevel = juce::jmax (level, displayedLevel * 0.82f);
        repaint (meterBounds);
    }

    static void configureButton (juce::TextButton& button,
                                 const juce::String& text, bool toggles)
    {
        button.setButtonText (text);
        button.setClickingTogglesState (toggles);
        button.setColour (juce::TextButton::buttonColourId,
                          juce::Colour (0xff181c22));
        button.setColour (juce::TextButton::buttonOnColourId,
                          juce::Colour (0xffa81831));
        button.setColour (juce::TextButton::textColourOffId,
                          juce::Colour (0xffe0e2e6));
        button.setColour (juce::TextButton::textColourOnId,
                          juce::Colours::white);
    }

    static void configureCombo (juce::ComboBox& box)
    {
        box.setColour (juce::ComboBox::backgroundColourId,
                       juce::Colour (0xff0b0e12));
        box.setColour (juce::ComboBox::textColourId,
                       juce::Colour (0xffeceef2));
        box.setColour (juce::ComboBox::outlineColourId,
                       juce::Colour (0xff353a43));
        box.setColour (juce::ComboBox::arrowColourId,
                       juce::Colour (0xffff3158));
    }

    void configureSlider (juce::Slider& slider, const juce::String& name,
                          bool vertical = false)
    {
        slider.setName (name);
        slider.setSliderStyle (vertical ? juce::Slider::LinearVertical
                                        : juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false,
                                vertical ? 62 : 66, 18);
        slider.setColour (juce::Slider::rotarySliderFillColourId,
                          juce::Colour (0xffff3158));
        slider.setColour (juce::Slider::textBoxTextColourId,
                          juce::Colour (0xffd5d8de));
        slider.setColour (juce::Slider::textBoxBackgroundColourId,
                          juce::Colours::transparentBlack);
        slider.setColour (juce::Slider::textBoxOutlineColourId,
                          juce::Colours::transparentBlack);
        slider.setScrollWheelEnabled (false);
        slider.setViewportIgnoreDragFlag (true);
        slider.setPopupDisplayEnabled (true, false, this);
       #if JUCE_IOS || JUCE_ANDROID
        if (! vertical)
            slider.setMouseDragSensitivity (135);
       #endif
        slider.onDragEnd = [this]
        {
            if (onStateChanged != nullptr) onStateChanged();
        };
        addAndMakeVisible (slider);
    }

    void setEditorPage (int newPage)
    {
        editorPage = juce::jlimit ((int) samplePage, (int) ampEqPage,
                                   newPage);
        samplePageButton.setToggleState (
            editorPage == samplePage, juce::dontSendNotification);
        filterPageButton.setToggleState (
            editorPage == filterPage, juce::dontSendNotification);
        ampEqPageButton.setToggleState (
            editorPage == ampEqPage, juce::dontSendNotification);
        updateEditorPageVisibility();
        resized();
        repaint (editorBounds);
    }

    void updateEditorPageVisibility()
    {
        const bool sample = editorPage == samplePage;
        const bool filter = editorPage == filterPage;
        const bool amp = editorPage == ampEqPage;

        waveform.setVisible (sample);
        const std::array<juce::Component*, 6> sampleComponents {
            &volume, &attack, &release, &lowCut, &highCut, &transpose
        };
        for (auto* component : sampleComponents)
            component->setVisible (sample);

        const std::array<juce::Component*, 12> filterComponents {
            &filterOnButton, &filterTypeBox, &filterCutoff, &filterResonance,
            &keyTrackBox, &driveBox, &brightButton, &filterVelocityButton,
            &filterEnvAmount, &filterEnvAttack, &filterEnvDecay,
            &filterEnvRelease
        };
        for (auto* component : filterComponents)
            component->setVisible (filter);

        const std::array<juce::Component*, 6> ampComponents {
            &ampEqOnButton, &ampBass, &ampMid, &ampMidHz, &ampTreble,
            &compressor
        };
        for (auto* component : ampComponents)
            component->setVisible (amp);

        // Reverb e delay pertencem ao sample e tambem ao fechamento da cadeia
        // AMP/FX. Uma unica instancia/attachment aparece nas duas paginas.
        reverb.setVisible (sample || amp);
        delay.setVisible (sample || amp);
    }

    static bool looksLikePad (const TimbresProcessor::TimbreEntry& entry)
    {
        if (entry.category.equalsIgnoreCase ("PADS")
             || entry.category.equalsIgnoreCase ("AMBIENCIAS"))
            return true;

        const auto identity = (entry.label + " " + entry.library).toLowerCase();
        for (const char* token : { "pad", "ambien", "atmos", "shimmer",
                                   "cloud", "worship", "synth", "warm" })
            if (identity.contains (token))
                return true;
        return false;
    }

    void refreshPadEntries (bool preserveSelection)
    {
        juce::String previousLabel;
        if (preserveSelection && selectedEntryIndex >= 0
             && selectedEntryIndex < padEntries.size())
            previousLabel = padEntries.getReference (selectedEntryIndex).label;

        padEntries.clearQuick();
        selectedEntryIndex = -1;
        knownEntryCount = processor != nullptr
                        ? processor->getEntriesNonBlocking().size() : 0;

        if (processor != nullptr)
        {
            juce::StringArray identities;
            for (const auto& entry : processor->getEntriesNonBlocking())
            {
                if (! looksLikePad (entry) || entry.files.isEmpty())
                    continue;
                const auto identity = entry.library + "\n" + entry.label;
                if (! identities.contains (identity))
                {
                    identities.add (identity);
                    padEntries.add (entry);
                }
            }
        }

        padList.updateContent();
        int rowToSelect = -1;
        const auto loaded = processor != nullptr
                          ? processor->getLayerTimbre (targetLayer)
                          : juce::String();
        for (int i = 0; i < padEntries.size(); ++i)
            if ((! previousLabel.isEmpty()
                    && padEntries.getReference (i).label == previousLabel)
                 || (previousLabel.isEmpty() && ! loaded.isEmpty()
                    && padEntries.getReference (i).label == loaded))
            {
                rowToSelect = i;
                break;
            }
        if (rowToSelect < 0 && ! padEntries.isEmpty()) rowToSelect = 0;

        if (rowToSelect >= 0)
        {
            padList.selectRow (rowToSelect);
            selectPadRow (rowToSelect);
        }
        else
        {
            waveform.setFile ({});
            statusText = "Nenhum pad sampleado instalado";
            statusWarning = true;
        }
        repaint();
    }

    void refreshChannelBox()
    {
        syncing = true;
        channelBox.clear (juce::dontSendNotification);
        for (int layer = 0; layer < TimbresProcessor::numLayers; ++layer)
        {
            auto name = processor != nullptr
                      ? TimbresProcessor::prettyLabel (
                            processor->getLayerTimbre (layer))
                      : juce::String();
            if (name.isEmpty()) name = "Vazio";
            channelBox.addItem ("Canal " + juce::String (layer + 1)
                                + juce::String::fromUTF8 ("  \xc2\xb7  ")
                                + name, layer + 1);
        }
        channelBox.setSelectedId (targetLayer + 1,
                                  juce::dontSendNotification);
        syncing = false;
    }

    int firstLoadedPadLayer() const
    {
        if (processor == nullptr) return 0;
        for (int layer = 0; layer < TimbresProcessor::numLayers; ++layer)
            if (processor->isLayerPadOrAmbience (layer))
                return layer;
        return 0;
    }

    void selectPadRow (int row)
    {
        if (! juce::isPositiveAndBelow (row, padEntries.size()))
            return;

        selectedEntryIndex = row;
        const auto& entry = padEntries.getReference (row);
        waveform.setFile (entry.files.isEmpty() ? juce::File()
                                                 : entry.files.getFirst());
        statusText = "Pronto para carregar no canal "
                   + juce::String (targetLayer + 1);
        statusWarning = false;
        repaint();
    }

    void loadSelectedPad()
    {
        if (processor == nullptr
             || ! juce::isPositiveAndBelow (selectedEntryIndex,
                                             padEntries.size()))
        {
            statusText = "Selecione um pad instalado";
            statusWarning = true;
            repaint();
            return;
        }

        const auto& entry = padEntries.getReference (selectedEntryIndex);
        processor->setSelectedLayer (targetLayer);
        processor->loadTimbreIntoLayer (targetLayer, entry, true);
        statusText = "Carregando " + TimbresProcessor::prettyLabel (entry.label)
                   + " no canal " + juce::String (targetLayer + 1)
                   + juce::String::fromUTF8 ("\xe2\x80\xa6");
        statusWarning = false;
        if (onStateChanged != nullptr) onStateChanged();
        repaint();
    }

    void rebuildAttachments()
    {
        attachments.clear();
        buttonAttachments.clear();
        comboAttachments.clear();
        onAttachment.reset();
        for (auto* slider : { &volume, &attack, &release, &lowCut,
                              &highCut, &reverb, &delay, &transpose,
                              &filterCutoff, &filterResonance,
                              &filterEnvAmount, &filterEnvAttack,
                              &filterEnvDecay, &filterEnvRelease,
                              &ampBass, &ampMid, &ampMidHz, &ampTreble,
                              &compressor })
            ChurchDAWMidiLearnTarget::clear (*slider);
        const std::array<juce::Component*, 8> learnComponents {
            &onButton, &filterOnButton, &brightButton,
            &filterVelocityButton, &ampEqOnButton, &filterTypeBox,
            &keyTrackBox, &driveBox
        };
        for (auto* component : learnComponents)
            ChurchDAWMidiLearnTarget::clear (*component);

        if (processor == nullptr)
        {
            setControlsEnabled (false);
            return;
        }

        setControlsEnabled (true);
        auto attach = [this] (juce::RangedAudioParameter* parameter,
                              juce::Slider& slider)
        {
            if (parameter != nullptr)
            {
                attachments.push_back (
                    std::make_unique<juce::SliderParameterAttachment> (
                        *parameter, slider));
                ChurchDAWMidiLearnTarget::tag (
                    slider, *processor, *parameter);
            }
        };

        attach (dynamic_cast<juce::RangedAudioParameter*> (
                    processor->getLayerVolParam (targetLayer)), volume);
        attach (processor->getLayerAttackParam (targetLayer), attack);
        attach (processor->getLayerReleaseParam (targetLayer), release);
        attach (processor->getLayerLowCutParam (targetLayer), lowCut);
        attach (processor->getLayerHighCutParam (targetLayer), highCut);
        attach (processor->getLayerReverbSendParam (targetLayer), reverb);
        attach (processor->getLayerDelayParam (targetLayer), delay);
        attach (processor->getLayerTransposeParam (targetLayer), transpose);
        attach (processor->getLayerNordFilterCutoffParam (targetLayer),
                filterCutoff);
        attach (processor->getLayerNordFilterResonanceParam (targetLayer),
                filterResonance);
        attach (processor->getLayerNordFilterEnvAmountParam (targetLayer),
                filterEnvAmount);
        attach (processor->getLayerNordFilterEnvAttackParam (targetLayer),
                filterEnvAttack);
        attach (processor->getLayerNordFilterEnvDecayParam (targetLayer),
                filterEnvDecay);
        attach (processor->getLayerNordFilterEnvReleaseParam (targetLayer),
                filterEnvRelease);
        attach (processor->getLayerNordAmpEqBassParam (targetLayer), ampBass);
        attach (processor->getLayerNordAmpEqMidParam (targetLayer), ampMid);
        attach (processor->getLayerNordAmpEqMidHzParam (targetLayer), ampMidHz);
        attach (processor->getLayerNordAmpEqTrebleParam (targetLayer),
                ampTreble);
        attach (dynamic_cast<juce::RangedAudioParameter*> (
                    processor->getLayerCompParam (targetLayer)), compressor);

        const auto attachButton = [this] (
            juce::RangedAudioParameter* parameter, juce::Button& button)
        {
            if (parameter == nullptr)
                return;
            buttonAttachments.push_back (
                std::make_unique<juce::ButtonParameterAttachment> (
                    *parameter, button));
            ChurchDAWMidiLearnTarget::tag (
                button, *processor, *parameter);
        };
        const auto attachCombo = [this] (
            juce::RangedAudioParameter* parameter, juce::ComboBox& box)
        {
            if (parameter == nullptr)
                return;
            comboAttachments.push_back (
                std::make_unique<juce::ComboBoxParameterAttachment> (
                    *parameter, box));
            ChurchDAWMidiLearnTarget::tag (
                box, *processor, *parameter);
        };

        if (auto* on = dynamic_cast<juce::RangedAudioParameter*> (
                processor->getLayerOnParam (targetLayer)))
        {
            onAttachment = std::make_unique<juce::ButtonParameterAttachment> (
                *on, onButton);
            ChurchDAWMidiLearnTarget::tag (
                onButton, *processor, *on);
        }

        attachButton (processor->getLayerNordFilterOnParam (targetLayer),
                      filterOnButton);
        attachButton (processor->getLayerNordFilterBrightParam (targetLayer),
                      brightButton);
        attachButton (processor->getLayerNordFilterVelocityParam (targetLayer),
                      filterVelocityButton);
        attachButton (processor->getLayerNordAmpEqOnParam (targetLayer),
                      ampEqOnButton);
        attachCombo (processor->getLayerNordFilterTypeParam (targetLayer),
                     filterTypeBox);
        attachCombo (processor->getLayerNordFilterKeyTrackParam (targetLayer),
                     keyTrackBox);
        attachCombo (processor->getLayerNordFilterDriveParam (targetLayer),
                     driveBox);

        onButton.setButtonText (processor->getLayerOn (targetLayer)
                                ? "CANAL ON" : "CANAL OFF");
    }

    void setControlsEnabled (bool enabled)
    {
        for (auto* slider : { &volume, &attack, &release, &lowCut, &highCut,
                              &reverb, &delay, &transpose,
                              &filterCutoff, &filterResonance,
                              &filterEnvAmount, &filterEnvAttack,
                              &filterEnvDecay, &filterEnvRelease,
                              &ampBass, &ampMid, &ampMidHz, &ampTreble,
                              &compressor })
            slider->setEnabled (enabled);
        const std::array<juce::Component*, 7> extraComponents {
            &filterOnButton, &brightButton, &filterVelocityButton,
            &ampEqOnButton, &filterTypeBox, &keyTrackBox, &driveBox
        };
        for (auto* component : extraComponents)
            component->setEnabled (enabled);
        onButton.setEnabled (enabled);
        loadButton.setEnabled (enabled && ! padEntries.isEmpty());
    }

    static void drawPanel (juce::Graphics& g, juce::Rectangle<int> bounds,
                           const juce::String& title, juce::Colour accent)
    {
        if (bounds.isEmpty()) return;
        g.setColour (juce::Colour (0xff11151a));
        g.fillRoundedRectangle (bounds.toFloat(), 8.0f);
        g.setColour (juce::Colour (0xff303640));
        g.drawRoundedRectangle (bounds.toFloat().reduced (0.5f), 8.0f, 1.0f);
        g.setColour (accent);
        g.fillRoundedRectangle ((float) bounds.getX() + 1.0f,
                                (float) bounds.getY() + 1.0f,
                                (float) bounds.getWidth() - 2.0f, 3.0f, 2.0f);
        g.setFont (juce::Font (juce::FontOptions (10.5f,
                                                  juce::Font::bold)));
        g.drawText (title, bounds.reduced (12, 7).removeFromTop (16),
                    juce::Justification::centredLeft);
    }

    TimbresProcessor* processor = nullptr;
    juce::Array<TimbresProcessor::TimbreEntry> padEntries;
    int targetLayer = 0;
    int selectedEntryIndex = -1;
    int knownEntryCount = -1;
    bool syncing = false;
    bool editorActive = false;

    juce::ListBox padList {
        juce::String::fromUTF8 ("Pads dispon\xc3\xadveis"), this };
    juce::ComboBox channelBox;
    juce::TextButton loadButton, refreshButton, onButton;
    juce::TextButton samplePageButton, filterPageButton, ampEqPageButton;
    juce::TextButton filterOnButton, brightButton, filterVelocityButton;
    juce::TextButton ampEqOnButton;
    juce::ComboBox filterTypeBox, keyTrackBox, driveBox;
    Waveform waveform;
    juce::Slider volume, attack, release, lowCut, highCut;
    juce::Slider reverb, delay, transpose;
    juce::Slider filterCutoff, filterResonance, filterEnvAmount;
    juce::Slider filterEnvAttack, filterEnvDecay, filterEnvRelease;
    juce::Slider ampBass, ampMid, ampMidHz, ampTreble, compressor;
    std::vector<std::unique_ptr<juce::SliderParameterAttachment>> attachments;
    std::vector<std::unique_ptr<juce::ButtonParameterAttachment>> buttonAttachments;
    std::vector<std::unique_ptr<juce::ComboBoxParameterAttachment>> comboAttachments;
    std::unique_ptr<juce::ButtonParameterAttachment> onAttachment;
    int editorPage = samplePage;

    juce::Rectangle<int> libraryBounds, editorBounds, liveBounds;
    juce::Rectangle<int> libraryFooterBounds, sampleTitleBounds;
    juce::Rectangle<int> sampleBadgeBounds, sampleDetailBounds;
    juce::Rectangle<int> currentLabelBounds, currentNameBounds;
    juce::Rectangle<int> meterBounds, statusBounds;
    std::array<juce::Rectangle<int>, 12> controlLabelBounds;
    std::array<juce::String, 12> controlNames;
    juce::String statusText { "Selecione um pad instalado" };
    bool statusWarning = false;
    float displayedLevel = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SamplePadEditorPanel)
};
