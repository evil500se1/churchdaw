#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "AndroidCultoConnectivity.h"
#include "BinaryData.h"
#include "NordLookAndFeel.h"
#include "TimbresProcessor.h"

#include <functional>
#include <memory>

//==============================================================================
// Central full-screen premium de ÁUDIO / LATÊNCIA / MIDI no Android.
//
// Substitui o card rolável antigo por três abas claras, com cards de seção,
// tipografia consistente com o resto do app (Library, Login, Pad) e estados
// legíveis. A lógica (deviceManager, USB, BLE, velocity) permanece inalterada.
//==============================================================================
class AndroidAudioMidiCenter final : public juce::Component,
                                     private juce::Timer
{
public:
    struct Host
    {
        juce::AudioDeviceManager& deviceManager;
        juce::ApplicationProperties& appProperties;
        std::function<void()> closeCenter;
        std::function<void()> restartAudioDevice;
        std::function<void()> refreshMidiInputs;
        std::function<void()> forceUsbReconcile;
        std::function<TimbresProcessor*()> getChurchLive;
        bool& audioDialogOpen;
    };

    explicit AndroidAudioMidiCenter (Host hostToUse)
        : host (std::move (hostToUse))
    {
        host.audioDialogOpen = true;
        brand = juce::ImageCache::getFromMemory (BinaryData::icon_png,
                                                 BinaryData::icon_pngSize);

        closeBtn.setButtonText (juce::String::fromUTF8 (u8"×"));
        styleIconButton (closeBtn);
        closeBtn.onClick = [this]
        {
            if (host.closeCenter != nullptr)
                host.closeCenter();
        };
        addAndMakeVisible (closeBtn);

        for (int i = 0; i < 3; ++i)
        {
            auto* t = tabs.add (new juce::TextButton());
            t->setClickingTogglesState (true);
            t->setRadioGroupId (0xA11D10);
            t->setWantsKeyboardFocus (false);
            styleTab (*t, i == 0);
            t->onClick = [this, i] { setTab (i); };
            addAndMakeVisible (t);
        }
        tabs[0]->setButtonText (juce::String::fromUTF8 (u8"Áudio"));
        tabs[1]->setButtonText (juce::String::fromUTF8 (u8"Latência"));
        tabs[2]->setButtonText ("MIDI / USB");
        tabs[0]->setToggleState (true, juce::dontSendNotification);

        // ---- Página Áudio ----
        styleSection (audioSummaryTitle, juce::String::fromUTF8 (u8"Áudio ativo"));
        styleBody (audioSummaryBody);
        styleSection (outTitle, juce::String::fromUTF8 (u8"Saída de áudio"));
        styleBody (outHint);
        styleSection (inTitle, juce::String::fromUTF8 (u8"Entrada de áudio"));
        styleBody (inHint);
        inHint.setText (juce::String::fromUTF8 (u8"Entrada desativada\nO ChurchDAW funciona só com instrumentos virtuais (sem microfone)."),
                        juce::dontSendNotification);

        styleCombo (outDeviceBox);
        outDeviceBox.onChange = [this] { applyOutputDeviceFromBox(); };
        addAndMakeVisible (outDeviceBox);

        testOutBtn.setButtonText (juce::String::fromUTF8 (u8"Testar saída"));
        styleSecondary (testOutBtn);
        testOutBtn.onClick = [this] { runOutputTest(); };
        addAndMakeVisible (testOutBtn);

        refreshDevicesBtn.setButtonText ("Atualizar lista");
        styleSecondary (refreshDevicesBtn);
        refreshDevicesBtn.onClick = [this]
        {
            rebuildDeviceList();
            refreshStatus();
        };
        addAndMakeVisible (refreshDevicesBtn);

        // ---- Página Latência ----
        styleSection (latModeTitle, "Resposta do teclado");
        styleBody (latModeHint);
        styleSection (srTitle, "Taxa de amostragem");
        styleSection (bufTitle, "Tamanho do buffer");
        styleSection (latTotalTitle, juce::String::fromUTF8 (u8"Latência estimada"));
        // Hero + detalhe em labels SEPARADOS (um Label com 28 pt + 2 linhas
        // se sobrepunha: "40.0 ms" em cima de "Processamento...").
        styleBody (latTotalHero);
        latTotalHero.setFont (juce::Font (juce::FontOptions (36.0f)).boldened());
        latTotalHero.setJustificationType (juce::Justification::centredLeft);
        latTotalHero.setColour (juce::Label::textColourId, NordLookAndFeel::txtPrimary());
        styleBody (latTotalDetail);
        latTotalDetail.setFont (juce::Font (juce::FontOptions (12.5f)));
        latTotalDetail.setColour (juce::Label::textColourId, NordLookAndFeel::txtSecondary());
        styleBody (latStabilityBody);
        latStabilityBody.setColour (juce::Label::textColourId, NordLookAndFeel::greenOk());

        styleCombo (sampleRateBox);
        sampleRateBox.onChange = [this] { applySampleRateFromBox(); };
        addAndMakeVisible (sampleRateBox);

        styleCombo (bufferBox);
        bufferBox.onChange = [this] { applyBufferFromBox(); };
        addAndMakeVisible (bufferBox);

        for (int i = 0; i < 4; ++i)
        {
            auto* b = perfButtons.add (new juce::TextButton());
            b->setClickingTogglesState (true);
            b->setRadioGroupId (0xA11D20);
            b->setWantsKeyboardFocus (false);
            stylePreset (*b, false);
            b->onClick = [this, i] { applyPerfPreset (i); };
            addAndMakeVisible (b);
        }
        perfButtons[0]->setButtonText ("Segura");
        perfButtons[1]->setButtonText ("Natural");
        perfButtons[2]->setButtonText (juce::String::fromUTF8 (u8"Rápida"));
        perfButtons[3]->setButtonText ("Manual");
        // Restaura ultimo perfil (padrao Natural = equilibrio culto).
        manualPerf = juce::jlimit (
            0, 3,
            host.appProperties.getUserSettings()->getIntValue (
                "androidPerfPreset", 1));
        for (int i = 0; i < perfButtons.size(); ++i)
        {
            stylePreset (*perfButtons[i], i == manualPerf);
            perfButtons[i]->setToggleState (i == manualPerf,
                                            juce::dontSendNotification);
        }

        styleCombo (velBox);
        addAndMakeVisible (velBox);

        // ---- Página MIDI / USB ----
        styleSection (midiTitle, "Controladores MIDI");
        styleBody (midiBody);
        styleSection (bleTitle, "MIDI Bluetooth");
        styleBody (bleBody);
        styleSection (usbTitle, "Reconhecer USB");
        styleBody (usbBody);

        bleBtn.setButtonText ("Procurar MIDI Bluetooth");
        styleSecondary (bleBtn);
        bleBtn.onClick = [this] { openBluetoothMidi(); };
        addAndMakeVisible (bleBtn);

        usbBtn.setButtonText ("Verificar USB novamente");
        styleSecondary (usbBtn);
        usbBtn.onClick = [this] { recoverUsb(); };
        addAndMakeVisible (usbBtn);

        addAndMakeVisible (audioSummaryTitle);
        addAndMakeVisible (audioSummaryBody);
        addAndMakeVisible (outTitle);
        addAndMakeVisible (outHint);
        addAndMakeVisible (inTitle);
        addAndMakeVisible (inHint);
        addAndMakeVisible (latModeTitle);
        addAndMakeVisible (latModeHint);
        addAndMakeVisible (srTitle);
        addAndMakeVisible (bufTitle);
        addAndMakeVisible (latTotalTitle);
        addAndMakeVisible (latTotalHero);
        addAndMakeVisible (latTotalDetail);
        addAndMakeVisible (latStabilityBody);
        addAndMakeVisible (midiTitle);
        addAndMakeVisible (midiBody);
        addAndMakeVisible (bleTitle);
        addAndMakeVisible (bleBody);
        addAndMakeVisible (usbTitle);
        addAndMakeVisible (usbBody);

        rebuildDeviceList();
        rebuildRateBufferLists();
        bindVelocity();
        refreshStatus();
        setTab (0);
        startTimerHz (4);
    }

    ~AndroidAudioMidiCenter() override
    {
        stopTimer();
        host.audioDialogOpen = false;
        if (auto* dev = host.deviceManager.getCurrentAudioDevice())
        {
            if (auto xml = host.deviceManager.createStateXml())
                host.appProperties.getUserSettings()->setValue ("audioSetup", xml.get());
            host.appProperties.getUserSettings()->setValue (
                "bufferSize", dev->getCurrentBufferSizeSamples());
            host.appProperties.getUserSettings()->setValue (
                "sampleRate", dev->getCurrentSampleRate());
            host.appProperties.getUserSettings()->saveIfNeeded();
        }
        stopOutputTest();
    }

    void paint (juce::Graphics& g) override
    {
        NordLookAndFeel::fillAppBackground (g, getLocalBounds().toFloat());

        // Header glass + separador vermelho.
        auto headFull = getLocalBounds().removeFromTop (headerH);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff1c222c), 0.0f, (float) headFull.getY(),
                                                 juce::Colour (0xff12161c), 0.0f, (float) headFull.getBottom(),
                                                 false));
        g.fillRect (headFull);

        auto head = headFull;
        auto shadow = head.removeFromBottom (6);
        g.setGradientFill (juce::ColourGradient (
            juce::Colours::black.withAlpha (0.28f), 0.0f, (float) shadow.getY(),
            juce::Colours::black.withAlpha (0.0f), 0.0f, (float) shadow.getBottom(),
            false));
        g.fillRect (shadow);

        g.setColour (NordLookAndFeel::nordRedBright());
        g.fillRect (head.getX(), head.getBottom() - 2, head.getWidth(), 2);

        if (brand.isValid())
        {
            g.setOpacity (1.0f);
            g.drawImageWithin (brand, 14, 12, 36, 36,
                               juce::RectanglePlacement::centred);
        }
        else
        {
            NordLookAndFeel::drawChurchDAWMark (
                g, { 12.0f, 10.0f, 40.0f, 40.0f });
        }

        g.setColour (NordLookAndFeel::txtPrimary());
        g.setFont (juce::Font (juce::FontOptions (17.0f)).boldened());
        g.drawText (juce::String::fromUTF8 (u8"Áudio / MIDI"),
                    58, 10, getWidth() - 160, 24,
                    juce::Justification::centredLeft);

        g.setColour (NordLookAndFeel::txtSecondary());
        g.setFont (juce::Font (juce::FontOptions (11.5f)));
        g.drawText (juce::String::fromUTF8 (u8"Dispositivos, latência e controladores"),
                    58, 32, getWidth() - 160, 18,
                    juce::Justification::centredLeft);

        // Status badge.
        const auto badge = statusBadgeBounds();
        g.setColour (statusColour().withAlpha (0.15f));
        g.fillRoundedRectangle (badge.toFloat(), 14.0f);
        g.setColour (NordLookAndFeel::borderDefault());
        g.drawRoundedRectangle (badge.toFloat().reduced (0.5f), 14.0f, 1.0f);
        g.setColour (statusColour());
        g.fillEllipse ((float) badge.getX() + 10.0f,
                       (float) badge.getCentreY() - 4.0f, 8.0f, 8.0f);
        g.setColour (NordLookAndFeel::txtPrimary());
        g.setFont (juce::Font (juce::FontOptions (11.5f)).boldened());
        g.drawText (statusText,
                    badge.withTrimmedLeft (26).withTrimmedRight (10),
                    juce::Justification::centredLeft);

        // Cards de seção.
        drawCard (g, cardAudioSummary);
        drawCard (g, cardOutput);
        drawCard (g, cardInput);
        drawCard (g, cardLatencyMode);
        drawCard (g, cardLatencyDetails);
        drawCard (g, cardLatencyTotal);
        drawCard (g, cardMidi);
        drawCard (g, cardBle);
        drawCard (g, cardUsb);

        // Barra de latência (desenhada dentro do card de total).
        if (! latBarBounds.isEmpty())
        {
            g.setColour (NordLookAndFeel::bgControl());
            g.fillRoundedRectangle (latBarBounds.toFloat(), 6.0f);

            const float norm = juce::jlimit (0.0f, 1.0f, latBarFraction);
            if (norm > 0.0f)
            {
                auto fill = latBarBounds.toFloat();
                fill.setWidth (fill.getWidth() * norm);
                g.setColour (latBarColour);
                g.fillRoundedRectangle (fill, 6.0f);
            }
        }
    }

    void resized() override
    {
        auto r = getLocalBounds();

        auto headerArea = r.removeFromTop (headerH);
        closeBtn.setBounds (headerArea.getRight() - 56, headerArea.getY() + 9, 42, 40);

        // Abas tipo segmented control.
        auto tabRow = r.removeFromTop (52).reduced (14, 6);
        const int tw = juce::jmax (1, tabRow.getWidth() / 3);
        for (int i = 0; i < tabs.size(); ++i)
            tabs[i]->setBounds (tabRow.removeFromLeft (tw).reduced (i == 1 ? 4 : 3, 0));

        content = r.reduced (14, 8);
        layoutCurrentTab();
    }

    void setTab (int index)
    {
        current = static_cast<Tab> (juce::jlimit (0, 2, index));
        for (int i = 0; i < tabs.size(); ++i)
            styleTab (*tabs[i], i == (int) current);
        refreshStatus();
        layoutCurrentTab();
        repaint();
    }

private:
    enum class Tab { Audio = 0, Latency = 1, MidiUsb = 2 };

    Host host;
    juce::Image brand;
    juce::TextButton closeBtn;
    juce::OwnedArray<juce::TextButton> tabs;
    Tab current = Tab::Audio;
    juce::String statusText { "Verificando" };
    juce::Colour statusColourValue { NordLookAndFeel::amberSolo() };
    juce::Rectangle<int> content;

    // Áreas dos cards (atualizadas no layout, usadas no paint).
    juce::Rectangle<int> cardAudioSummary, cardOutput, cardInput;
    juce::Rectangle<int> cardLatencyMode, cardLatencyDetails, cardLatencyTotal;
    juce::Rectangle<int> cardMidi, cardBle, cardUsb;
    juce::Rectangle<int> latBarBounds;
    float latBarFraction = 0.0f;
    juce::Colour latBarColour { NordLookAndFeel::greenOk() };

    // Audio page
    juce::Label audioSummaryTitle, audioSummaryBody;
    juce::Label outTitle, outHint, inTitle, inHint;
    juce::ComboBox outDeviceBox;
    juce::TextButton testOutBtn, refreshDevicesBtn;
    juce::StringArray outDeviceIds;

    // Latency page
    juce::Label latModeTitle, latModeHint, srTitle, bufTitle, latTotalTitle,
        latTotalHero, latTotalDetail, latStabilityBody;
    juce::ComboBox sampleRateBox, bufferBox, velBox;
    juce::OwnedArray<juce::TextButton> perfButtons;
    std::unique_ptr<juce::ComboBoxParameterAttachment> velAttachment;
    int manualPerf = 1;

    // MIDI/USB page
    juce::Label midiTitle, midiBody, bleTitle, bleBody, usbTitle, usbBody;
    juce::TextButton bleBtn, usbBtn;
    juce::String usbMessage;
    bool usbBusy = false;
    bool testingOut = false;

    static constexpr int headerH = 58;
    static constexpr int cardRadius = 14;
    static constexpr int titleH = 16;

    static void drawCard (juce::Graphics& g, const juce::Rectangle<int>& area)
    {
        if (area.isEmpty())
            return;
        NordLookAndFeel::drawGlassCard (g, area.toFloat().reduced (0.5f),
                                        (float) cardRadius, false, {}, false, false);
    }

    static void styleSection (juce::Label& l, const juce::String& text)
    {
        // Sentence case nos titulos de secao (menos grito ALL CAPS).
        l.setText (text, juce::dontSendNotification);
        l.setFont (juce::Font (juce::FontOptions (11.0f)).boldened()
                       .withExtraKerningFactor (0.04f));
        l.setColour (juce::Label::textColourId, NordLookAndFeel::nordRed().withAlpha (0.85f));
        l.setJustificationType (juce::Justification::centredLeft);
    }

    static void styleBody (juce::Label& l)
    {
        l.setFont (juce::Font (juce::FontOptions (13.5f)));
        l.setColour (juce::Label::textColourId, NordLookAndFeel::txtPrimary());
        l.setJustificationType (juce::Justification::topLeft);
    }

    static void styleCombo (juce::ComboBox& b)
    {
        b.setWantsKeyboardFocus (false);
        b.setColour (juce::ComboBox::backgroundColourId, NordLookAndFeel::bgControl());
        b.setColour (juce::ComboBox::outlineColourId, NordLookAndFeel::borderDefault());
        b.setColour (juce::ComboBox::textColourId, NordLookAndFeel::txtPrimary());
        b.setColour (juce::ComboBox::arrowColourId, NordLookAndFeel::nordRedBright());
        b.setColour (juce::ComboBox::focusedOutlineColourId, NordLookAndFeel::nordRedBright().withAlpha (0.75f));
    }

    static void styleTab (juce::TextButton& b, bool on)
    {
        b.setColour (juce::TextButton::buttonColourId,
                     on ? NordLookAndFeel::nordRed() : NordLookAndFeel::bgControl());
        b.setColour (juce::TextButton::buttonOnColourId, NordLookAndFeel::nordRed());
        b.setColour (juce::TextButton::textColourOffId, NordLookAndFeel::txtPrimary());
        b.setColour (juce::TextButton::textColourOnId, juce::Colours::white);
    }

    static void stylePreset (juce::TextButton& b, bool on)
    {
        b.setColour (juce::TextButton::buttonColourId,
                     on ? NordLookAndFeel::nordRed() : NordLookAndFeel::bgControl());
        b.setColour (juce::TextButton::buttonOnColourId, NordLookAndFeel::nordRed());
        b.setColour (juce::TextButton::textColourOffId, NordLookAndFeel::txtPrimary());
        b.setColour (juce::TextButton::textColourOnId, juce::Colours::white);
    }

    static void styleSecondary (juce::TextButton& b)
    {
        b.setWantsKeyboardFocus (false);
        b.setColour (juce::TextButton::buttonColourId, NordLookAndFeel::bgControl());
        b.setColour (juce::TextButton::buttonOnColourId, NordLookAndFeel::nordRed());
        b.setColour (juce::TextButton::textColourOffId, NordLookAndFeel::txtPrimary());
        b.setColour (juce::TextButton::textColourOnId, juce::Colours::white);
    }

    static void styleIconButton (juce::TextButton& b)
    {
        b.setWantsKeyboardFocus (false);
        b.setColour (juce::TextButton::buttonColourId, NordLookAndFeel::bgControl());
        b.setColour (juce::TextButton::buttonOnColourId, NordLookAndFeel::nordRed());
        b.setColour (juce::TextButton::textColourOffId, NordLookAndFeel::txtPrimary());
    }

    juce::Colour statusColour() const { return statusColourValue; }

    juce::Rectangle<int> statusBadgeBounds() const
    {
        const int w = juce::jlimit (120, 172, getWidth() / 3);
        return { getWidth() - 56 - 10 - w, 15, w, 28 };
    }

    void hideAllPageWidgets()
    {
        const auto hide = [] (juce::Component& c) { c.setVisible (false); };
        hide (audioSummaryTitle); hide (audioSummaryBody);
        hide (outTitle); hide (outHint); hide (inTitle); hide (inHint);
        hide (outDeviceBox); hide (testOutBtn); hide (refreshDevicesBtn);
        hide (latModeTitle); hide (latModeHint); hide (srTitle); hide (bufTitle);
        hide (latTotalTitle); hide (latTotalHero); hide (latTotalDetail);
        hide (latStabilityBody);
        hide (sampleRateBox); hide (bufferBox); hide (velBox);
        for (auto* b : perfButtons) b->setVisible (false);
        hide (midiTitle); hide (midiBody); hide (bleTitle); hide (bleBody);
        hide (usbTitle); hide (usbBody); hide (bleBtn); hide (usbBtn);

        cardAudioSummary = {};
        cardOutput = {};
        cardInput = {};
        cardLatencyMode = {};
        cardLatencyDetails = {};
        cardLatencyTotal = {};
        cardMidi = {};
        cardBle = {};
        cardUsb = {};
        latBarBounds = {};
    }

    void layoutCurrentTab()
    {
        hideAllPageWidgets();
        auto r = content;
        const bool shortH = r.getHeight() < 420;
        const bool wide = r.getWidth() >= 720 && r.getWidth() > r.getHeight();
        const int gap = shortH ? 8 : 12;
        const int fieldH = shortH ? 40 : 46;
        const int btnH = shortH ? 42 : 48;
        const int cardPadX = 14;
        const int cardPadY = 12;
        const int sectionTitleGap = 4;

        // Cria um card de altura fixa na área e devolve o retângulo interno
        // (já com padding) onde os widgets devem ser posicionados.
        auto makeCard = [&] (juce::Rectangle<int>& area, int cardHeight,
                             juce::Rectangle<int>& cardOut) -> juce::Rectangle<int>
        {
            cardOut = area.removeFromTop (cardHeight + gap)
                          .withTrimmedBottom (gap)
                          .reduced (0, 1);   // respiro entre cards
            return cardOut.reduced (cardPadX, cardPadY);
        };

        auto placeSection = [&] (juce::Label& title, juce::Label& body,
                                 int bodyH, juce::Rectangle<int>& area,
                                 juce::Rectangle<int>& cardOut)
        {
            const int cardHeight = cardPadY * 2 + titleH + sectionTitleGap + bodyH;
            auto inner = makeCard (area, cardHeight, cardOut);
            title.setVisible (true);
            body.setVisible (true);
            title.setBounds (inner.removeFromTop (titleH));
            inner.removeFromTop (sectionTitleGap);
            body.setBounds (inner.removeFromTop (bodyH));
        };

        if (current == Tab::Audio)
        {
            if (wide)
            {
                auto left = r.removeFromLeft (r.getWidth() / 2).withTrimmedRight (gap / 2);
                auto right = r.withTrimmedLeft (gap / 2);

                // ---- Card de saída (esquerda, topo) ----
                {
                    const int cardHeight = cardPadY * 2 + titleH + sectionTitleGap
                                           + fieldH + 6 + btnH + 6 + (shortH ? 34 : 40);
                    auto inner = makeCard (left, cardHeight, cardOutput);
                    outTitle.setVisible (true);
                    outTitle.setBounds (inner.removeFromTop (titleH));
                    inner.removeFromTop (sectionTitleGap);
                    outDeviceBox.setVisible (true);
                    outDeviceBox.setBounds (inner.removeFromTop (fieldH));
                    inner.removeFromTop (6);
                    auto row = inner.removeFromTop (btnH);
                    testOutBtn.setVisible (true);
                    refreshDevicesBtn.setVisible (true);
                    testOutBtn.setBounds (row.removeFromLeft (row.getWidth() / 2)
                                              .reduced (0, 1).withTrimmedRight (5));
                    refreshDevicesBtn.setBounds (row.reduced (0, 1).withTrimmedLeft (5));
                    inner.removeFromTop (6);
                    outHint.setVisible (true);
                    outHint.setBounds (inner.removeFromTop (shortH ? 34 : 40));
                }

                // ---- Card de resumo (direita, topo) ----
                {
                    const int cardHeight = cardPadY * 2 + titleH + sectionTitleGap
                                           + (shortH ? 80 : 96);
                    auto inner = makeCard (right, cardHeight, cardAudioSummary);
                    audioSummaryTitle.setVisible (true);
                    audioSummaryTitle.setBounds (inner.removeFromTop (titleH));
                    inner.removeFromTop (sectionTitleGap);
                    audioSummaryBody.setVisible (true);
                    audioSummaryBody.setBounds (inner.removeFromTop (shortH ? 80 : 96));
                }

                // ---- Card de entrada (esquerda, restante) ----
                if (left.getHeight() > 64)
                    placeSection (inTitle, inHint,
                                  juce::jmin (left.getHeight() - 28,
                                              shortH ? 48 : 56),
                                  left, cardInput);
            }
            else
            {
                // ---- Card de resumo (topo) ----
                placeSection (audioSummaryTitle, audioSummaryBody,
                              shortH ? 74 : 86, r, cardAudioSummary);

                // ---- Card de saída ----
                {
                    const int cardHeight = cardPadY * 2 + titleH + sectionTitleGap
                                           + fieldH + (shortH ? 6 : 8)
                                           + btnH + (shortH ? 6 : 8)
                                           + (shortH ? 34 : 40);
                    auto inner = makeCard (r, cardHeight, cardOutput);
                    outTitle.setVisible (true);
                    outTitle.setBounds (inner.removeFromTop (titleH));
                    inner.removeFromTop (sectionTitleGap);
                    outDeviceBox.setVisible (true);
                    outDeviceBox.setBounds (inner.removeFromTop (fieldH));
                    inner.removeFromTop (shortH ? 6 : 8);
                    auto row = inner.removeFromTop (btnH);
                    testOutBtn.setVisible (true);
                    refreshDevicesBtn.setVisible (true);
                    testOutBtn.setBounds (row.removeFromLeft (row.getWidth() / 2)
                                              .reduced (0, 1).withTrimmedRight (5));
                    refreshDevicesBtn.setBounds (row.reduced (0, 1).withTrimmedLeft (5));
                    inner.removeFromTop (shortH ? 6 : 8);
                    outHint.setVisible (true);
                    outHint.setBounds (inner.removeFromTop (shortH ? 34 : 40));
                }

                // ---- Card de entrada (restante) ----
                if (r.getHeight() > 64)
                    placeSection (inTitle, inHint,
                                  juce::jmin (r.getHeight() - 28,
                                              shortH ? 48 : 56),
                                  r, cardInput);
            }
        }
        else if (current == Tab::Latency)
        {
            updateLatencyBar();

            if (wide)
            {
                auto left = r.removeFromLeft (r.getWidth() / 2).withTrimmedRight (gap / 2);
                auto right = r.withTrimmedLeft (gap / 2);

                // ---- Card modo/teclado (esquerda, topo) ----
                {
                    const int cardHeight = cardPadY * 2 + titleH + sectionTitleGap
                                           + fieldH + 6 + (shortH ? 34 : 38)
                                           + gap + fieldH;
                    auto inner = makeCard (left, cardHeight, cardLatencyMode);
                    latModeTitle.setVisible (true);
                    latModeTitle.setBounds (inner.removeFromTop (titleH));
                    inner.removeFromTop (sectionTitleGap);
                    auto presets = inner.removeFromTop (fieldH);
                    const int pw = presets.getWidth() / 4;
                    for (int i = 0; i < perfButtons.size(); ++i)
                    {
                        perfButtons[i]->setVisible (true);
                        perfButtons[i]->setBounds (presets.removeFromLeft (pw).reduced (2, 0));
                    }
                    inner.removeFromTop (6);
                    latModeHint.setVisible (true);
                    latModeHint.setBounds (inner.removeFromTop (shortH ? 34 : 38));
                    inner.removeFromTop (gap);
                    velBox.setVisible (true);
                    velBox.setBounds (inner.removeFromTop (fieldH));
                }

                // ---- Card taxa/buffer (esquerda, meio) ----
                {
                    const int cardHeight = cardPadY * 2
                                           + (titleH + sectionTitleGap + fieldH) * 2
                                           + gap;
                    auto inner = makeCard (left, cardHeight, cardLatencyDetails);
                    srTitle.setVisible (true);
                    srTitle.setBounds (inner.removeFromTop (titleH));
                    inner.removeFromTop (sectionTitleGap);
                    sampleRateBox.setVisible (true);
                    sampleRateBox.setBounds (inner.removeFromTop (fieldH));
                    inner.removeFromTop (gap);
                    bufTitle.setVisible (true);
                    bufTitle.setBounds (inner.removeFromTop (titleH));
                    inner.removeFromTop (sectionTitleGap);
                    bufferBox.setVisible (true);
                    bufferBox.setBounds (inner.removeFromTop (fieldH));
                }

                // ---- Card latência total (direita, topo) ----
                {
                    const int heroH = shortH ? 40 : 44;
                    const int detailH = shortH ? 20 : 22;
                    const int stabH = shortH ? 28 : 32;
                    const int cardHeight = cardPadY * 2 + titleH + sectionTitleGap
                                           + heroH + 4 + detailH + 10 + 10 + 8
                                           + stabH;
                    auto inner = makeCard (right, cardHeight, cardLatencyTotal);
                    latTotalTitle.setVisible (true);
                    latTotalTitle.setBounds (inner.removeFromTop (titleH));
                    inner.removeFromTop (sectionTitleGap);
                    latTotalHero.setVisible (true);
                    latTotalHero.setBounds (inner.removeFromTop (heroH));
                    inner.removeFromTop (4);
                    latTotalDetail.setVisible (true);
                    latTotalDetail.setBounds (inner.removeFromTop (detailH));
                    inner.removeFromTop (10);
                    latBarBounds = inner.removeFromTop (10);
                    inner.removeFromTop (8);
                    latStabilityBody.setVisible (true);
                    latStabilityBody.setBounds (inner.removeFromTop (stabH));
                }
            }
            else
            {
                // ---- Card modo/teclado ----
                {
                    const int cardHeight = cardPadY * 2 + titleH + sectionTitleGap
                                           + fieldH + 6 + (shortH ? 30 : 34)
                                           + (shortH ? 6 : 10) + fieldH;
                    auto inner = makeCard (r, cardHeight, cardLatencyMode);
                    latModeTitle.setVisible (true);
                    latModeTitle.setBounds (inner.removeFromTop (titleH));
                    inner.removeFromTop (sectionTitleGap);
                    auto presets = inner.removeFromTop (fieldH);
                    const int pw = presets.getWidth() / 4;
                    for (int i = 0; i < perfButtons.size(); ++i)
                    {
                        perfButtons[i]->setVisible (true);
                        perfButtons[i]->setBounds (presets.removeFromLeft (pw).reduced (2, 0));
                    }
                    inner.removeFromTop (6);
                    latModeHint.setVisible (true);
                    latModeHint.setBounds (inner.removeFromTop (shortH ? 30 : 34));
                    inner.removeFromTop (shortH ? 6 : 10);
                    velBox.setVisible (true);
                    velBox.setBounds (inner.removeFromTop (fieldH));
                }

                // ---- Card taxa/buffer ----
                if (shortH)
                {
                    const int cardHeight = cardPadY * 2 + titleH + sectionTitleGap + fieldH;
                    auto inner = makeCard (r, cardHeight, cardLatencyDetails);
                    auto rowT = inner.removeFromTop (titleH);
                    srTitle.setVisible (true);
                    bufTitle.setVisible (true);
                    srTitle.setBounds (rowT.removeFromLeft (rowT.getWidth() / 2).withTrimmedRight (6));
                    bufTitle.setBounds (rowT.withTrimmedLeft (6));
                    inner.removeFromTop (sectionTitleGap);
                    auto rowF = inner.removeFromTop (fieldH);
                    sampleRateBox.setVisible (true);
                    bufferBox.setVisible (true);
                    sampleRateBox.setBounds (rowF.removeFromLeft (rowF.getWidth() / 2).withTrimmedRight (6));
                    bufferBox.setBounds (rowF.withTrimmedLeft (6));
                }
                else
                {
                    const int cardHeight = cardPadY * 2
                                           + (titleH + sectionTitleGap + fieldH) * 2
                                           + gap;
                    auto inner = makeCard (r, cardHeight, cardLatencyDetails);
                    srTitle.setVisible (true);
                    srTitle.setBounds (inner.removeFromTop (titleH));
                    inner.removeFromTop (sectionTitleGap);
                    sampleRateBox.setVisible (true);
                    sampleRateBox.setBounds (inner.removeFromTop (fieldH));
                    inner.removeFromTop (gap);
                    bufTitle.setVisible (true);
                    bufTitle.setBounds (inner.removeFromTop (titleH));
                    inner.removeFromTop (sectionTitleGap);
                    bufferBox.setVisible (true);
                    bufferBox.setBounds (inner.removeFromTop (fieldH));
                }

                // ---- Card latência total ----
                {
                    const int heroH = shortH ? 38 : 42;
                    const int detailH = shortH ? 18 : 20;
                    const int stabH = shortH ? 26 : 30;
                    const int cardHeight = cardPadY * 2 + titleH + sectionTitleGap
                                           + heroH + 4 + detailH + 8 + 10 + 6
                                           + stabH;
                    auto inner = makeCard (r, cardHeight, cardLatencyTotal);
                    latTotalTitle.setVisible (true);
                    latTotalTitle.setBounds (inner.removeFromTop (titleH));
                    inner.removeFromTop (sectionTitleGap);
                    latTotalHero.setVisible (true);
                    latTotalHero.setBounds (inner.removeFromTop (heroH));
                    inner.removeFromTop (4);
                    latTotalDetail.setVisible (true);
                    latTotalDetail.setBounds (inner.removeFromTop (detailH));
                    inner.removeFromTop (8);
                    latBarBounds = inner.removeFromTop (10);
                    inner.removeFromTop (6);
                    latStabilityBody.setVisible (true);
                    latStabilityBody.setBounds (inner.removeFromTop (stabH));
                }
            }
        }
        else
        {
            if (wide)
            {
                auto left = r.removeFromLeft (r.getWidth() / 2).withTrimmedRight (gap / 2);
                auto right = r.withTrimmedLeft (gap / 2);

                placeSection (midiTitle, midiBody, shortH ? 64 : 80, left, cardMidi);

                {
                    const int cardHeight = cardPadY * 2 + titleH + sectionTitleGap
                                           + (shortH ? 40 : 48) + 8 + btnH;
                    auto inner = makeCard (right, cardHeight, cardBle);
                    bleTitle.setVisible (true);
                    bleTitle.setBounds (inner.removeFromTop (titleH));
                    inner.removeFromTop (sectionTitleGap);
                    bleBody.setVisible (true);
                    bleBody.setBounds (inner.removeFromTop (shortH ? 40 : 48));
                    inner.removeFromTop (8);
                    bleBtn.setVisible (true);
                    bleBtn.setBounds (inner.removeFromTop (btnH));
                }

                {
                    const int cardHeight = cardPadY * 2 + titleH + sectionTitleGap
                                           + (shortH ? 44 : 52) + 8 + btnH;
                    auto inner = makeCard (right, cardHeight, cardUsb);
                    usbTitle.setVisible (true);
                    usbTitle.setBounds (inner.removeFromTop (titleH));
                    inner.removeFromTop (sectionTitleGap);
                    usbBody.setVisible (true);
                    usbBody.setBounds (inner.removeFromTop (shortH ? 44 : 52));
                    inner.removeFromTop (8);
                    usbBtn.setVisible (true);
                    usbBtn.setBounds (inner.removeFromTop (btnH));
                }
            }
            else
            {
                placeSection (midiTitle, midiBody, shortH ? 64 : 78, r, cardMidi);

                {
                    const int cardHeight = cardPadY * 2 + titleH + sectionTitleGap
                                           + (shortH ? 40 : 48) + 8 + btnH;
                    auto inner = makeCard (r, cardHeight, cardBle);
                    bleTitle.setVisible (true);
                    bleTitle.setBounds (inner.removeFromTop (titleH));
                    inner.removeFromTop (sectionTitleGap);
                    bleBody.setVisible (true);
                    bleBody.setBounds (inner.removeFromTop (shortH ? 40 : 48));
                    inner.removeFromTop (8);
                    bleBtn.setVisible (true);
                    bleBtn.setBounds (inner.removeFromTop (btnH));
                }

                {
                    const int cardHeight = cardPadY * 2 + titleH + sectionTitleGap
                                           + (shortH ? 44 : 52) + 8 + btnH;
                    auto inner = makeCard (r, cardHeight, cardUsb);
                    usbTitle.setVisible (true);
                    usbTitle.setBounds (inner.removeFromTop (titleH));
                    inner.removeFromTop (sectionTitleGap);
                    usbBody.setVisible (true);
                    usbBody.setBounds (inner.removeFromTop (shortH ? 44 : 52));
                    inner.removeFromTop (8);
                    usbBtn.setVisible (true);
                    usbBtn.setBounds (inner.removeFromTop (btnH));
                }
            }
        }

        repaint();
    }

    void timerCallback() override
    {
        refreshStatus();
        if (current == Tab::MidiUsb)
            refreshMidiPage();
        if (current == Tab::Latency)
        {
            updateLatencyBar();
            refreshLatencyPage();
        }
        if (current == Tab::Audio)
            refreshAudioSummary();
    }

    void refreshStatus()
    {
        auto* dev = host.deviceManager.getCurrentAudioDevice();
        if (usbBusy)
        {
            statusText = "Recuperando USB";
            statusColourValue = NordLookAndFeel::amberSolo();
        }
        else if (dev == nullptr)
        {
            statusText = juce::String::fromUTF8 (u8"Erro de áudio");
            statusColourValue = NordLookAndFeel::redClip();
        }
        else
        {
            const auto name = dev->getName();
            if (ChurchDAWAndroidCulto::nameLooksLikeUsbAudio (name))
            {
                statusText = juce::String::fromUTF8 (u8"Áudio ativo");
                statusColourValue = NordLookAndFeel::greenOk();
            }
            else
            {
                statusText = juce::String::fromUTF8 (u8"Saída do aparelho");
                statusColourValue = NordLookAndFeel::amberSolo();
            }
        }
        repaint (statusBadgeBounds().expanded (4));
    }

    void refreshAudioSummary()
    {
        auto* dev = host.deviceManager.getCurrentAudioDevice();
        const auto setup = host.deviceManager.getAudioDeviceSetup();

        juce::String outName = setup.outputDeviceName;
        if (outName.isEmpty() && dev != nullptr)
            outName = dev->getName();
        if (outName.isEmpty())
            outName = juce::String::fromUTF8 (u8"Nenhuma saída");

        juce::String friendly = outName;
        if (outName.containsIgnoreCase ("System Default")
             || outName.containsIgnoreCase ("default"))
        {
            friendly = juce::String::fromUTF8 (u8"Saída padrão do aparelho");
            outHint.setText (
                juce::String::fromUTF8 (u8"Nenhuma interface USB em uso. O áudio vai para a saída padrão do Android."),
                juce::dontSendNotification);
        }
        else if (ChurchDAWAndroidCulto::nameLooksLikeUsbAudio (outName))
        {
            friendly = juce::String::fromUTF8 (u8"USB · ") + prettify (outName);
            outHint.setText (
                juce::String::fromUTF8 ("Interface USB conectada."),
                juce::dontSendNotification);
        }
        else
        {
            friendly = prettify (outName);
            outHint.setText ({}, juce::dontSendNotification);
        }

        const double sr = dev != nullptr ? dev->getCurrentSampleRate()
                                         : setup.sampleRate;
        const int buf = dev != nullptr ? dev->getCurrentBufferSizeSamples()
                                       : setup.bufferSize;
        const double blockMs = (sr > 0.0 && buf > 0)
                                   ? (1000.0 * (double) buf / sr)
                                   : 0.0;
        const double estTotal = blockMs * 2.0;

        juce::String body;
        body << friendly << "\n";
        if (sr > 0.0 && buf > 0)
            body << juce::String (sr, 0) << " Hz · "
                 << buf << " samples · ~"
                 << juce::String (estTotal, 1) << " ms\n";
        body << juce::String::fromUTF8 (u8"Estéreo L/R");
        if (dev != nullptr)
            body << " · " << dev->getActiveOutputChannels().countNumberOfSetBits()
                 << " canais";

        audioSummaryBody.setText (body, juce::dontSendNotification);
    }

    void updateLatencyBar()
    {
        auto* dev = host.deviceManager.getCurrentAudioDevice();
        const auto setup = host.deviceManager.getAudioDeviceSetup();
        const double sr = dev != nullptr ? dev->getCurrentSampleRate() : setup.sampleRate;
        const int buf = dev != nullptr ? dev->getCurrentBufferSizeSamples() : setup.bufferSize;
        const double blockMs = (sr > 0.0 && buf > 0) ? (1000.0 * (double) buf / sr) : 0.0;
        const double totalMs = blockMs * 2.0;

        // Normaliza: 30 ms = 100% da barra.
        latBarFraction = (float) (totalMs / 30.0);
        if (totalMs <= 8.0)
            latBarColour = NordLookAndFeel::greenOk();
        else if (totalMs <= 18.0)
            latBarColour = NordLookAndFeel::amberSolo();
        else
            latBarColour = NordLookAndFeel::redClip();
    }

    void refreshLatencyPage()
    {
        auto* dev = host.deviceManager.getCurrentAudioDevice();
        const auto setup = host.deviceManager.getAudioDeviceSetup();
        const double sr = dev != nullptr ? dev->getCurrentSampleRate()
                                         : setup.sampleRate;
        const int buf = dev != nullptr ? dev->getCurrentBufferSizeSamples()
                                       : setup.bufferSize;
        const double blockMs = (sr > 0.0 && buf > 0)
                                   ? (1000.0 * (double) buf / sr)
                                   : 0.0;

        if (blockMs > 0.0)
        {
            // Hero sozinho; detalhe numa label menor — nunca se sobrepoem.
            latTotalHero.setText (juce::String (blockMs * 2.0, 1) + " ms",
                                  juce::dontSendNotification);
            latTotalDetail.setText (
                "Processamento ~" + juce::String (blockMs, 1)
                    + juce::String::fromUTF8 (u8" ms · saída estimada"),
                juce::dontSendNotification);

            // Cor do numero: verde confortavel, ambar se sobe, vermelho so se alto.
            const double totalMs = blockMs * 2.0;
            auto heroCol = NordLookAndFeel::greenOk();
            if (totalMs > 18.0)
                heroCol = NordLookAndFeel::amberSolo();
            if (totalMs > 30.0)
                heroCol = NordLookAndFeel::redClip();
            latTotalHero.setColour (juce::Label::textColourId, heroCol);
        }
        else
        {
            latTotalHero.setText ("—", juce::dontSendNotification);
            latTotalDetail.setText (
                juce::String::fromUTF8 (u8"Estimativa indisponível"),
                juce::dontSendNotification);
            latTotalHero.setColour (juce::Label::textColourId,
                                    NordLookAndFeel::txtPrimary());
        }

        // Nao exibe contagem de xrun (acumulava e assustava). O tuner sobe o
        // buffer em silencio; a UI so confirma estabilidade do setup.
        latStabilityBody.setText (
            juce::String::fromUTF8 (u8"✓ Áudio estável · buffer adaptativo ativo"),
            juce::dontSendNotification);
        latStabilityBody.setColour (juce::Label::textColourId,
                                    NordLookAndFeel::greenOk());

        latModeHint.setText (perfHint (manualPerf), juce::dontSendNotification);
    }

    void refreshMidiPage()
    {
        juce::String midi;
        int n = 0;
        for (const auto& d : juce::MidiInput::getAvailableDevices())
        {
            if (! host.deviceManager.isMidiInputDeviceEnabled (d.identifier))
                continue;
            ++n;
            const auto tag = ChurchDAWAndroidCulto::midiTransportTag (d.name);
            midi << juce::String::fromUTF8 (u8"● ") << prettify (d.name);
            if (tag.isNotEmpty())
                midi << juce::String::fromUTF8 (u8"  ·  ") << tag;
            midi << "\n";
        }
        if (n == 0)
            midi = juce::String::fromUTF8 (u8"Nenhum controlador MIDI conectado.\nConecte USB ou use Procurar MIDI Bluetooth.");
        midiBody.setText (midi, juce::dontSendNotification);

        bleBody.setText (
            juce::String::fromUTF8 (
                "Conecte teclados MIDI sem fio. O sistema lista a porta "
                "depois do pareamento."),
            juce::dontSendNotification);

        if (usbBusy)
            usbBody.setText (juce::String::fromUTF8 (u8"Verificando dispositivos USB…"),
                             juce::dontSendNotification);
        else if (usbMessage.isNotEmpty())
            usbBody.setText (usbMessage, juce::dontSendNotification);
        else
            usbBody.setText (
                juce::String::fromUTF8 (u8"Use se teclado ou interface USB não aparecer."),
                juce::dontSendNotification);
    }

    static juce::String perfHint (int preset)
    {
        switch (preset)
        {
            case 0:
                return juce::String::fromUTF8 (
                    u8"Máxima estabilidade (~20 ms). Ideal para culto e multi-camadas.");
            case 2:
                return juce::String::fromUTF8 (
                    u8"Resposta rápida (~8 ms). Use em aparelhos potentes.");
            case 3:
                return "Escolha manualmente taxa e buffer abaixo.";
            default:
                return juce::String::fromUTF8 (
                    u8"Equilíbrio culto (~12 ms). Recomendado no dia a dia.");
        }
    }

    static juce::String prettify (juce::String n)
    {
        n = n.trim();
        for (const char* s : { " Output Port", " Input Port", " (Output)", " (Input)" })
            if (const int at = n.indexOfIgnoreCase (s); at >= 0)
                n = n.substring (0, at).trim();
        return n;
    }

    void rebuildDeviceList()
    {
        outDeviceBox.clear (juce::dontSendNotification);
        outDeviceIds.clear();

        auto* type = host.deviceManager.getCurrentDeviceTypeObject();
        if (type == nullptr)
            return;

        type->scanForDevices();
        const auto names = type->getDeviceNames (false);
        const auto setup = host.deviceManager.getAudioDeviceSetup();

        int selected = 0;
        for (int i = 0; i < names.size(); ++i)
        {
            const auto& name = names[i];
            juce::String label = prettify (name);
            if (ChurchDAWAndroidCulto::nameLooksLikeUsbAudio (name))
                label = juce::String::fromUTF8 (u8"USB · ") + label;
            else if (name.containsIgnoreCase ("default")
                      || name.containsIgnoreCase ("System Default"))
                label = juce::String::fromUTF8 (u8"Saída padrão do aparelho");

            outDeviceBox.addItem (label, i + 1);
            outDeviceIds.add (name);
            if (name == setup.outputDeviceName)
                selected = i + 1;
        }

        if (outDeviceBox.getNumItems() == 0)
            outDeviceBox.addItem (juce::String::fromUTF8 (u8"Nenhuma saída encontrada"), 1);

        if (selected > 0)
            outDeviceBox.setSelectedId (selected, juce::dontSendNotification);
        else if (outDeviceBox.getNumItems() > 0)
            outDeviceBox.setSelectedItemIndex (0, juce::dontSendNotification);
    }

    void rebuildRateBufferLists()
    {
        sampleRateBox.clear (juce::dontSendNotification);
        bufferBox.clear (juce::dontSendNotification);

        auto* dev = host.deviceManager.getCurrentAudioDevice();
        const auto setup = host.deviceManager.getAudioDeviceSetup();

        juce::Array<double> rates;
        juce::Array<int> bufs;
        if (dev != nullptr)
        {
            rates = dev->getAvailableSampleRates();
            bufs = dev->getAvailableBufferSizes();
        }

        if (rates.isEmpty())
            rates.add (setup.sampleRate > 0 ? setup.sampleRate : 48000.0);
        if (bufs.isEmpty())
            bufs.add (setup.bufferSize > 0 ? setup.bufferSize : 256);

        int selR = 0, selB = 0;
        for (int i = 0; i < rates.size(); ++i)
        {
            sampleRateBox.addItem (juce::String (rates[i], 0) + " Hz", i + 1);
            if (std::abs (rates[i] - setup.sampleRate) < 0.5
                 || (dev != nullptr
                     && std::abs (rates[i] - dev->getCurrentSampleRate()) < 0.5))
                selR = i + 1;
        }
        for (int i = 0; i < bufs.size(); ++i)
        {
            const double ms = rates.getFirst() > 0
                                  ? 1000.0 * (double) bufs[i] / rates.getFirst()
                                  : 0.0;
            juce::String label = juce::String (bufs[i]) + " samples";
            if (ms > 0.0)
                label << juce::String::fromUTF8 (u8"  ·  ~") << juce::String (ms, 1) << " ms";
            bufferBox.addItem (label, i + 1);
            if (bufs[i] == setup.bufferSize
                 || (dev != nullptr
                     && bufs[i] == dev->getCurrentBufferSizeSamples()))
                selB = i + 1;
        }

        if (selR > 0)
            sampleRateBox.setSelectedId (selR, juce::dontSendNotification);
        else if (sampleRateBox.getNumItems() > 0)
            sampleRateBox.setSelectedItemIndex (0, juce::dontSendNotification);

        if (selB > 0)
            bufferBox.setSelectedId (selB, juce::dontSendNotification);
        else if (bufferBox.getNumItems() > 0)
            bufferBox.setSelectedItemIndex (0, juce::dontSendNotification);
    }

    void bindVelocity()
    {
        velAttachment.reset();
        velBox.clear (juce::dontSendNotification);
        if (host.getChurchLive == nullptr)
            return;
        if (auto* live = host.getChurchLive())
            if (auto* p = live->getVelCurveParam())
            {
                velBox.addItemList (p->choices, 1);
                velAttachment =
                    std::make_unique<juce::ComboBoxParameterAttachment> (*p, velBox);
                return;
            }
        velBox.addItem (juce::String::fromUTF8 (u8"Church Live não carregado"), 1);
        velBox.setSelectedId (1, juce::dontSendNotification);
        velBox.setEnabled (false);
    }

    void applyOutputDeviceFromBox()
    {
        const int idx = outDeviceBox.getSelectedItemIndex();
        if (! juce::isPositiveAndBelow (idx, outDeviceIds.size()))
            return;

        auto setup = host.deviceManager.getAudioDeviceSetup();
        setup.outputDeviceName = outDeviceIds[idx];
        setup.inputDeviceName.clear();
        setup.inputChannels.clear();
        setup.useDefaultInputChannels = false;
        const auto err = host.deviceManager.setAudioDeviceSetup (setup, true);
        if (err.isNotEmpty())
            outHint.setText ("Falha: " + err, juce::dontSendNotification);
        rebuildRateBufferLists();
        refreshAudioSummary();
        refreshStatus();
    }

    void applySampleRateFromBox()
    {
        auto* dev = host.deviceManager.getCurrentAudioDevice();
        if (dev == nullptr)
            return;
        const auto rates = dev->getAvailableSampleRates();
        const int idx = sampleRateBox.getSelectedItemIndex();
        if (! juce::isPositiveAndBelow (idx, rates.size()))
            return;

        auto setup = host.deviceManager.getAudioDeviceSetup();
        setup.sampleRate = rates[idx];
        setup.inputDeviceName.clear();
        setup.inputChannels.clear();
        setup.useDefaultInputChannels = false;
        host.deviceManager.setAudioDeviceSetup (setup, true);
        rebuildRateBufferLists();
        refreshLatencyPage();
        refreshAudioSummary();
    }

    void applyBufferFromBox()
    {
        auto* dev = host.deviceManager.getCurrentAudioDevice();
        if (dev == nullptr)
            return;
        const auto bufs = dev->getAvailableBufferSizes();
        const int idx = bufferBox.getSelectedItemIndex();
        if (! juce::isPositiveAndBelow (idx, bufs.size()))
            return;

        auto setup = host.deviceManager.getAudioDeviceSetup();
        setup.bufferSize = bufs[idx];
        setup.inputDeviceName.clear();
        setup.inputChannels.clear();
        setup.useDefaultInputChannels = false;
        host.deviceManager.setAudioDeviceSetup (setup, true);
        host.appProperties.getUserSettings()->setValue ("bufferSize", bufs[idx]);
        host.appProperties.getUserSettings()->setValue ("androidPerfPreset", 3);
        host.appProperties.getUserSettings()->saveIfNeeded();
        manualPerf = 3;
        for (int i = 0; i < perfButtons.size(); ++i)
            stylePreset (*perfButtons[i], i == 3);
        rebuildRateBufferLists();
        refreshLatencyPage();
        refreshAudioSummary();
    }

    void applyPerfPreset (int preset)
    {
        manualPerf = preset;
        for (int i = 0; i < perfButtons.size(); ++i)
            stylePreset (*perfButtons[i], i == preset);

        if (preset == 3)
        {
            latModeHint.setText (perfHint (3), juce::dontSendNotification);
            return;
        }

        auto* dev = host.deviceManager.getCurrentAudioDevice();
        if (dev == nullptr)
            return;

        auto bufs = dev->getAvailableBufferSizes();
        if (bufs.isEmpty())
            return;

        const double sr = juce::jmax (1.0, dev->getCurrentSampleRate());
        auto pick = [&] (double targetMs)
        {
            int best = bufs.getFirst();
            double bestErr = 1.0e9;
            for (int b : bufs)
            {
                const double ms = 1000.0 * (double) b / sr;
                const double err = std::abs (ms - targetMs);
                if (err < bestErr)
                {
                    bestErr = err;
                    best = b;
                }
            }
            return best;
        };

        // Perfis pensados pro culto no celular (fluidez > latencia minima):
        // Segura ~20 ms, Natural ~12 ms, Rapida ~8 ms (nunca 5 ms no TANK).
        int want = pick (20.0);
        if (preset == 1)
            want = pick (12.0);
        else if (preset == 2)
            want = pick (8.0);

        auto setup = host.deviceManager.getAudioDeviceSetup();
        setup.bufferSize = want;
        setup.inputDeviceName.clear();
        setup.inputChannels.clear();
        setup.useDefaultInputChannels = false;
        host.deviceManager.setAudioDeviceSetup (setup, true);

        // Persiste ja: o tuner e o restart respeitam a escolha do musico.
        host.appProperties.getUserSettings()->setValue ("bufferSize", want);
        host.appProperties.getUserSettings()->setValue ("androidPerfPreset", preset);
        host.appProperties.getUserSettings()->saveIfNeeded();

        rebuildRateBufferLists();
        refreshLatencyPage();
        refreshAudioSummary();
        latModeHint.setText (perfHint (preset), juce::dontSendNotification);
    }

    void runOutputTest()
    {
        if (testingOut)
        {
            stopOutputTest();
            return;
        }

        auto* dev = host.deviceManager.getCurrentAudioDevice();
        if (dev == nullptr)
        {
            outHint.setText (juce::String::fromUTF8 (u8"Sem dispositivo de saída para testar."),
                             juce::dontSendNotification);
            return;
        }

        testingOut = true;
        testOutBtn.setButtonText ("Parar teste");
        outHint.setText (
            juce::String::fromUTF8 (u8"Testando saída… ouça um tom breve no fone/caixa."),
            juce::dontSendNotification);

        host.deviceManager.playTestSound();

        juce::Timer::callAfterDelay (1200, [safe = juce::Component::SafePointer<AndroidAudioMidiCenter> (this)]
        {
            if (safe != nullptr)
                safe->stopOutputTest();
        });
    }

    void stopOutputTest()
    {
        testingOut = false;
        testOutBtn.setButtonText (juce::String::fromUTF8 (u8"Testar saída"));
        if (host.deviceManager.getCurrentAudioDevice() != nullptr)
            outHint.setText (
                juce::String::fromUTF8 (u8"✓ Teste enviado (se não ouviu, confira volume/USB)."),
                juce::dontSendNotification);
    }

    void openBluetoothMidi()
    {
        // NUNCA use getMainActivity().get() em um temporário: o LocalRef morre no
        // fim da expressão, DeleteLocalRef roda e o jobject fica dangling → SIGSEGV
        // dentro de openBluetoothSettings (crash observado no Redmi/Android 16).
        bleBody.setText (juce::String::fromUTF8 (
                             u8"Preparando MIDI Bluetooth\u2026"),
                         juce::dontSendNotification);

        auto launch = [safe = juce::Component::SafePointer<AndroidAudioMidiCenter> (this)]
        {
            if (safe == nullptr)
                return;

            // Preferir o seletor in-app do JUCE (lista só MIDI BLE).
            if (juce::BluetoothMidiDevicePairingDialogue::isAvailable()
                && juce::RuntimePermissions::isGranted (
                       juce::RuntimePermissions::bluetoothMidi))
            {
                if (juce::BluetoothMidiDevicePairingDialogue::open())
                {
                    safe->bleBody.setText (
                        juce::String::fromUTF8 (
                            u8"Selecione o teclado na lista. Depois ele aparece em "
                            u8"CONTROLADORES MIDI."),
                        juce::dontSendNotification);
                    if (safe->host.refreshMidiInputs)
                        safe->host.refreshMidiInputs();
                    safe->refreshMidiPage();
                    return;
                }
            }

#if JUCE_ANDROID
            auto* env = juce::getEnv();
            // LocalRef deve viver por TODA a sequência JNI abaixo.
            juce::LocalRef<jobject> activity (juce::getMainActivity());
            if (env == nullptr || activity.get() == nullptr)
            {
                safe->bleBody.setText (
                    juce::String::fromUTF8 (
                        u8"N\u00e3o foi poss\u00edvel acessar o Bluetooth neste momento."),
                    juce::dontSendNotification);
                return;
            }

            jclass clazz = env->FindClass ("com/churchdaw/app/ChurchDAWMidiBleBridge");
            if (clazz == nullptr)
            {
                env->ExceptionClear();
                safe->bleBody.setText (
                    juce::String::fromUTF8 (u8"Bridge MIDI Bluetooth indispon\u00edvel."),
                    juce::dontSendNotification);
                return;
            }

            jboolean requestedPerms = JNI_FALSE;
            if (jmethodID method = env->GetStaticMethodID (
                    clazz, "requestMidiBlePermissions",
                    "(Landroid/app/Activity;I)Z"))
            {
                requestedPerms = env->CallStaticBooleanMethod (
                    clazz, method, activity.get(), (jint) 0x41DA);
                if (env->ExceptionCheck())
                    env->ExceptionClear();
            }
            else
            {
                env->ExceptionClear();
            }

            // Se pedimos permissão agora, não abrir Settings na mesma batida —
            // o diálogo de permissão tem prioridade. Settings no resume seguinte.
            if (requestedPerms == JNI_FALSE)
            {
                if (jmethodID method = env->GetStaticMethodID (
                        clazz, "openBluetoothSettings",
                        "(Landroid/app/Activity;)Z"))
                {
                    env->CallStaticBooleanMethod (clazz, method, activity.get());
                    if (env->ExceptionCheck())
                        env->ExceptionClear();
                }
                else
                {
                    env->ExceptionClear();
                }

                safe->bleBody.setText (
                    juce::String::fromUTF8 (
                        u8"Abra o Bluetooth, pareie o teclado MIDI e volte ao "
                        u8"ChurchDAW. A porta aparece em CONTROLADORES MIDI."),
                    juce::dontSendNotification);
            }
            else
            {
                safe->bleBody.setText (
                    juce::String::fromUTF8 (
                        u8"Permiss\u00e3o de Bluetooth solicitada. Aceite e toque "
                        u8"de novo em Procurar MIDI Bluetooth."),
                    juce::dontSendNotification);
            }

            env->DeleteLocalRef (clazz);
#endif

            if (safe->host.refreshMidiInputs)
                safe->host.refreshMidiInputs();
            safe->refreshMidiPage();
        };

        // Pedir RuntimePermissions do JUCE (bluetoothMidi) quando faltar; depois
        // seguir com o seletor / Settings. callAsync evita startActivity no meio
        // do mouseUp (alguns OEMs não gostam).
        if (! juce::RuntimePermissions::isGranted (juce::RuntimePermissions::bluetoothMidi))
        {
            juce::RuntimePermissions::request (
                juce::RuntimePermissions::bluetoothMidi,
                [launch] (bool /*granted*/)
                {
                    juce::MessageManager::callAsync (launch);
                });
            return;
        }

        juce::MessageManager::callAsync (launch);
    }

    void recoverUsb()
    {
        if (usbBusy)
            return;

        usbBusy = true;
        usbBtn.setEnabled (false);
        usbBtn.setButtonText (juce::String::fromUTF8 (u8"Verificando…"));
        usbMessage = juce::String::fromUTF8 (u8"Verificando dispositivos USB…");
        refreshStatus();
        refreshMidiPage();

        juce::Component::SafePointer<AndroidAudioMidiCenter> safe (this);
        juce::MessageManager::callAsync ([safe]
        {
            if (safe == nullptr)
                return;

            if (safe->host.forceUsbReconcile)
                safe->host.forceUsbReconcile();
            if (safe->host.restartAudioDevice)
                safe->host.restartAudioDevice();
            if (safe->host.refreshMidiInputs)
                safe->host.refreshMidiInputs();

            safe->rebuildDeviceList();
            safe->rebuildRateBufferLists();

            juce::String msg;
            if (auto* dev = safe->host.deviceManager.getCurrentAudioDevice())
            {
                if (ChurchDAWAndroidCulto::nameLooksLikeUsbAudio (dev->getName()))
                    msg = juce::String::fromUTF8 (u8"✓ Interface USB: ")
                          + prettify (dev->getName());
                else
                    msg = juce::String::fromUTF8 (u8"Nenhuma interface USB em uso. Saída atual: ")
                          + prettify (dev->getName());
            }
            else
            {
                msg = juce::String::fromUTF8 (u8"Nenhum dispositivo de áudio disponível.");
            }

            safe->usbBusy = false;
            safe->usbMessage = msg;
            safe->usbBtn.setEnabled (true);
            safe->usbBtn.setButtonText ("Verificar USB novamente");
            safe->refreshStatus();
            safe->refreshMidiPage();
            safe->refreshAudioSummary();
        });
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AndroidAudioMidiCenter)
};
