#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "NordLookAndFeel.h"

#include <array>
#include <cmath>
#include <functional>

//==============================================================================
// Pagina desktop de gerenciamento do MIDI Learn.
//
// A logica de captura/aplicacao continua em MainComponent. Este componente e a
// superficie de trabalho: apresenta os alvos reais, filtra, seleciona, arma o
// aprendizado e devolve as intencoes por callbacks. Assim o desenho premium nao
// duplica a infraestrutura de seguranca/persistencia que ja existe no motor.
//==============================================================================
class MidiMappingPanel final : public juce::Component,
                               private juce::Timer
{
public:
    enum class Status { neutral, working, learning, success, warning, error };

    enum class TargetKind
    {
        layerVolume,
        layerReverb,
        layerComp,
        padCutoff,
        padVolume,
        masterVolume,
        processorParameter,
        panic,
        nextScene,
        previousScene,
        scene,
        padProgram,
        tapTempo,
        sustainToggle,
        padEngineToggle,
        none
    };

    struct Command
    {
        juce::String name;
        juce::String category;
        juce::String type { "CC" };
        juce::String device;
        juce::String control;
        int midiChannel = 1;
        int number = -1;
        int mode = 1;
        TargetKind target = TargetKind::none;
        int argument = -1;
        int parameterSlot = -2;
        int parameterIndex = -1;
        int parameterInsert = -1;
        juce::String parameterId;
        juce::String parameterName;
        // Identity that was visibly loaded when this row was built. Explicit
        // relink and ordinary parameter rows recheck it at click time, before
        // any map/disk mutation.
        juce::String expectedCurrentPluginId;
        bool expectedParameterEntryPresent = false;
        int persistedParameterSlot = -2;
        int persistedParameterIndex = -1;
        int persistedParameterInsert = -1;
        juce::String persistedParameterId;
        juce::String persistedParameterName;
        juce::String persistedPluginId;
        juce::String mappedSourceId;
        juce::String mappedSourceName;
        int mappedChannel = 0;
        int mappedNumber = -1;
        bool mappedWasNote = false;
        bool mapped = false;
        bool conflict = false;
        bool needsRelink = false;
        // Ordinal do controle dentro do grupo que o construiu. Linhas
        // placeholder do Church Live (motor ausente => parameterId vazio)
        // ficariam identicas em todos os campos semanticos; este ordinal e a
        // unica coisa que as distingue sem recorrer ao name/label.
        int rowDisambiguator = -1;

        // Pure semantic identity for selection restoration. Display labels
        // (name/category/device/control) are deliberately absent: two Kontakt
        // instances may render the same text while occupying different slots.
        static bool sameSelectedParameterIdentity (
            const Command& first, const Command& second) noexcept
        {
            if (first.parameterSlot != second.parameterSlot
                 || first.parameterInsert != second.parameterInsert
                 || first.expectedCurrentPluginId
                        != second.expectedCurrentPluginId)
                return false;

            if (first.parameterId.isNotEmpty()
                 || second.parameterId.isNotEmpty())
                return first.parameterId.isNotEmpty()
                    && first.parameterId == second.parameterId;

            // Legacy parameters without a stable ID are resolvable only by a
            // unique name. Their numeric index is diagnostic and may reorder.
            return first.parameterName == second.parameterName;
        }

        static bool sameStableIdentity (const Command& first,
                                        const Command& second) noexcept
        {
            return first.type == second.type
                && first.midiChannel == second.midiChannel
                && first.number == second.number
                && first.mode == second.mode
                && first.target == second.target
                && first.argument == second.argument
                && sameSelectedParameterIdentity (first, second)
                && first.expectedParameterEntryPresent
                    == second.expectedParameterEntryPresent
                && first.persistedParameterSlot
                    == second.persistedParameterSlot
                && first.persistedParameterIndex
                    == second.persistedParameterIndex
                && first.persistedParameterInsert
                    == second.persistedParameterInsert
                && first.persistedParameterId
                    == second.persistedParameterId
                && first.persistedParameterName
                    == second.persistedParameterName
                && first.persistedPluginId == second.persistedPluginId
                && first.mappedSourceId == second.mappedSourceId
                && first.mappedSourceName == second.mappedSourceName
                && first.mappedChannel == second.mappedChannel
                && first.mappedNumber == second.mappedNumber
                && first.mappedWasNote == second.mappedWasNote
                && first.rowDisambiguator == second.rowDisambiguator;
        }
    };

    MidiMappingPanel()
    {
        setOpaque (true);
        setMouseClickGrabsKeyboardFocus (false);

        search.setTextToShowWhenEmpty (juce::String::fromUTF8 (
                                           "Buscar: Volume Canal, Pad Cont\xc3\xadnuo\xe2\x80\xa6"),
                                       muted().withAlpha (0.72f));
        search.setColour (juce::TextEditor::backgroundColourId, control());
        search.setColour (juce::TextEditor::outlineColourId, border());
        search.setColour (juce::TextEditor::focusedOutlineColourId, red().withAlpha (0.72f));
        search.setColour (juce::TextEditor::textColourId, text());
        search.setColour (juce::TextEditor::highlightColourId, red().withAlpha (0.30f));
        search.setFont (juce::Font (juce::FontOptions (12.5f)));
        search.onTextChange = [this]
        {
            firstVisibleRow = 0;
            rebuildVisibleRows();
        };
        addAndMakeVisible (search);

        configureButton (detectButton, "Detectar dispositivos", false);
        configureButton (quickButton, "Mapear na tela", false);
        configureButton (learnButton, "Modo de aprendizado", true);
        configureButton (importButton, "Importar", false);
        configureButton (exportButton, "Exportar", false);
        configureButton (clearButton, "Limpar mapas", false, true);
        configureButton (testButton, "Armar", false);
        configureButton (saveButton,
                         juce::String::fromUTF8 ("Salvar altera\xc3\xa7\xc3\xb5" "es"),
                         false, true);

        learnButton.setClickingTogglesState (true);
        learnButton.onClick = [this]
        {
            const bool enabled = learnButton.getToggleState();
            setLearning (enabled, false);

            if (onLearningChanged != nullptr)
                onLearningChanged (enabled);

            if (enabled && juce::isPositiveAndBelow (selectedCommand, commands.size())
                 && onCommandSelected != nullptr)
                onCommandSelected (commands.getReference (selectedCommand));
        };

        detectButton.onClick = [this]
        {
            setStatus ("Procurando controladores MIDI...", Status::working);
            if (onDetectDevices != nullptr)
                onDetectDevices();
        };
        quickButton.setTooltip (
            "Voltar ao ChurchDAW, tocar um knob ou fader na tela e mover o controle MIDI");
        quickButton.onClick = [this]
        {
            setStatus ("Modo rapido: toque um controle na tela e depois mova o MIDI",
                       Status::learning);
            if (onQuickMapping != nullptr)
                onQuickMapping();
        };
        importButton.onClick = [this] { if (onImport != nullptr) onImport(); };
        exportButton.onClick = [this] { if (onExport != nullptr) onExport(); };
        clearButton.onClick = [this]
        {
            if (! clearArmed)
            {
                clearArmed = true;
                clearButton.setButtonText ("Confirmar limpeza");
                setStatus ("Clique novamente para apagar todos os mapas", Status::warning);
                clearArmDeadline = juce::Time::getMillisecondCounter() + 4200u;
                startTimerHz (12);
                if (onClearRequested != nullptr)
                    onClearRequested (false);
                return;
            }

            clearArmed = false;
            clearButton.setButtonText ("Limpar mapas");
            if (onClearRequested != nullptr)
                onClearRequested (true);
        };

        testButton.onClick = [this]
        {
            if (selectedCommand >= 0 && onTestCommand != nullptr)
                onTestCommand (commands.getReference (selectedCommand));
        };
        saveButton.onClick = [this]
        {
            if (selectedCommand >= 0 && onSaveCommand != nullptr)
            {
                auto edited = commands.getReference (selectedCommand);
                edited.type = typeCombo.getSelectedId() == 2 ? "Nota" : "CC";
                edited.midiChannel = juce::jlimit (1, 16, channelCombo.getSelectedId());
                edited.number = juce::jlimit (0, 127, numberCombo.getSelectedId() - 1);
                edited.mode = juce::jlimit (1, 4, modeCombo.getSelectedId());
                edited.device = deviceCombo.getText();
                edited.control = (edited.type == "Nota" ? "Nota " : "CC ")
                                  + juce::String (edited.number);
                onSaveCommand (edited);
            }
        };

        setupCombo (typeCombo, { "CC (Control Change)", "Nota" });
        setupCombo (channelCombo, {});
        for (int c = 1; c <= 16; ++c)
            channelCombo.addItem ("Canal " + juce::String (c), c);
        setupCombo (numberCombo, {});
        for (int n = 0; n < 128; ++n)
            numberCombo.addItem (juce::String (n), n + 1);
        setupCombo (deviceCombo, { "Qualquer dispositivo" });
        setupCombo (controlCombo, { "Aguardando MIDI" });
        setupCombo (modeCombo, { "Absoluto", "Relativo", "Alternar", "Momentaneo" });

        for (auto* combo : { &typeCombo, &channelCombo, &numberCombo,
                             &deviceCombo, &controlCombo, &modeCombo })
            addAndMakeVisible (*combo);

        categories = {{
            { "Todos", 0 }, { "Transporte", 0 }, { "Mixer", 0 },
            { "Instrumentos", 0 }, { "Efeitos", 0 }, { "Pad / Trigger", 0 },
            { "Macros", 0 }, { juce::String::fromUTF8 ("Navega\xc3\xa7\xc3\xa3o"), 0 },
            { juce::String::fromUTF8 ("Live / Sess\xc3\xa3o"), 0 },
            { "Revalidar mapas", 0 }
        }};
    }

    std::function<void(bool)> onLearningChanged;
    std::function<void()> onDetectDevices;
    std::function<void()> onQuickMapping;
    std::function<void()> onImport;
    std::function<void()> onExport;
    std::function<void(bool confirmed)> onClearRequested;
    std::function<void(const Command&)> onCommandSelected;
    std::function<void(const Command&)> onDeleteCommand;
    std::function<void(const Command&)> onTestCommand;
    std::function<void(const Command&)> onSaveCommand;

    void setCommands (juce::Array<Command> next)
    {
        const bool hadSelection =
            juce::isPositiveAndBelow (selectedCommand, commands.size());
        const Command previous = hadSelection
            ? commands.getReference (selectedCommand) : Command {};
        commands = std::move (next);
        selectedCommand = -1;

        for (int i = 0; hadSelection && i < commands.size(); ++i)
            if (Command::sameStableIdentity (
                    commands.getReference (i), previous))
            {
                selectedCommand = i;
                break;
            }

        rebuildCategoryCounts();
        rebuildVisibleRows();
        updateDetails();
    }

    void setDevice (juce::String name, bool connected)
    {
        deviceName = std::move (name);
        deviceConnected = connected;

        deviceCombo.clear (juce::dontSendNotification);
        deviceCombo.addItem (deviceConnected && deviceName.isNotEmpty()
                               ? deviceName : juce::String ("Qualquer dispositivo"), 1);
        deviceCombo.setSelectedId (1, juce::dontSendNotification);
        repaint (toolbarBounds);
    }

    void setLearning (bool enabled, bool notify = false)
    {
        learning = enabled;
        learnButton.setToggleState (enabled, juce::dontSendNotification);
        learnPulseStarted = juce::Time::getMillisecondCounter();

        if (enabled)
            setStatus (selectedCommand >= 0
                         ? "Mova o controle que voce quer mapear"
                         : "Selecione um comando e mova um controle MIDI",
                       Status::learning);
        else if (status == Status::learning)
            setStatus ("Modo de aprendizado pausado", Status::neutral);

        if (enabled && ! isTimerRunning())
            startTimerHz (24);
        else if (! enabled && ! clearArmed)
            stopTimer();

        if (notify && onLearningChanged != nullptr)
            onLearningChanged (enabled);
    }

    bool isLearning() const noexcept { return learning; }

    void setMappingCount (int count)
    {
        mappingCount = juce::jmax (0, count);
        repaint (toolbarBounds);
    }

    void setStatus (juce::String message, Status next)
    {
        statusText = std::move (message);
        status = next;
        statusChangedAt = juce::Time::getMillisecondCounter();
        repaint (statusBounds.expanded (8));
    }

    void showMapped (juce::String target, int channel, int number, bool saved,
                     bool isNote = false)
    {
        setStatus ((saved ? juce::String::fromUTF8 ("Mapeamento salvo: ")
                          : juce::String::fromUTF8 ("Mapeado nesta sess\xc3\xa3o: "))
                     + target + "  \xe2\x80\xa2  Ch " + juce::String (channel)
                     + "  \xe2\x80\xa2  " + (isNote ? "Nota " : "CC ")
                     + juce::String (number),
                   saved ? Status::success : Status::warning);
    }

    // A pagina MAPEAR MIDI e a superficie que o cliente esta olhando. O fluxo
    // antigo atualizava somente o MidiLearnPanel legado (que fica escondido
    // nesta pagina), entao um mapa valido continuava mostrando "Mova agora..."
    // e o soft-takeover parecia um controle quebrado. Estes dois estados deixam
    // explicito se o CC ja assumiu o parametro ou ainda precisa fazer pickup.
    void showActivity (juce::String target, int channel, int cc, int value)
    {
        setStatus ("ATIVO: " + target
                     + "  \xe2\x80\xa2  Ch " + juce::String (channel)
                     + "  \xe2\x80\xa2  CC " + juce::String (cc)
                     + "  \xe2\x80\xa2  "
                     + juce::String (juce::roundToInt (
                           juce::jlimit (0, 127, value) * 100.0f / 127.0f)) + "%",
                   Status::success);
    }

    void showPickup (juce::String target, int channel, int cc)
    {
        setStatus ("PICKUP: aproxime CC " + juce::String (cc)
                     + " do valor atual  \xe2\x80\xa2  Ch " + juce::String (channel)
                     + "  \xe2\x80\xa2  " + target,
                   Status::warning);
    }

    void showConflict (juce::String message)
    {
        setStatus (std::move (message), Status::error);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (background());

        // halo vermelho ambiente, extremamente controlado
        g.setGradientFill (juce::ColourGradient (
            redDark().withAlpha (0.10f), 0.0f, 0.0f,
            juce::Colours::transparentBlack, (float) getWidth() * 0.52f,
            (float) getHeight() * 0.42f, true));
        g.fillRect (getLocalBounds());

        drawHero (g);
        drawToolbar (g);
        drawCategories (g);
        drawTable (g);
        drawDetails (g);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (10, 9);

        heroBounds = area.removeFromTop (juce::jlimit (96, 124,
                                                       juce::roundToInt (getHeight() * 0.18f)));
        area.removeFromTop (10);
        toolbarBounds = area.removeFromTop (48);
        area.removeFromTop (10);

        const int leftW = juce::jlimit (158, 196,
                                        juce::roundToInt (getWidth() * 0.165f));
        const int rightW = juce::jlimit (214, 270,
                                         juce::roundToInt (getWidth() * 0.225f));
        categoriesBounds = area.removeFromLeft (leftW);
        area.removeFromLeft (10);
        detailsBounds = area.removeFromRight (rightW);
        area.removeFromRight (10);
        tableBounds = area;

        auto toolbar = toolbarBounds.reduced (9, 6);
        auto connected = toolbar.removeFromLeft (juce::jlimit (190, 238, toolbar.getWidth() / 5));
        connectedDeviceBounds = connected;
        toolbar.removeFromLeft (8);
        detectButton.setBounds (toolbar.removeFromLeft (136));
        toolbar.removeFromLeft (juce::jmax (8, toolbar.getWidth() / 18));

        clearButton.setBounds (toolbar.removeFromRight (118));
        toolbar.removeFromRight (7);
        exportButton.setBounds (toolbar.removeFromRight (86));
        toolbar.removeFromRight (7);
        importButton.setBounds (toolbar.removeFromRight (86));
        toolbar.removeFromRight (9);
        learnButton.setBounds (toolbar.removeFromRight (154));
        toolbar.removeFromRight (7);
        quickButton.setBounds (toolbar.removeFromRight (132));

        auto table = tableBounds.reduced (9);
        search.setBounds (table.removeFromTop (34));
        table.removeFromTop (8);
        tableHeaderBounds = table.removeFromTop (28);
        tableRowsBounds = table;

        auto detail = detailsBounds.reduced (12);
        detail.removeFromTop (103);
        const int rowH = 25;
        const int labelH = 10;
        detailFields.clearQuick();
        std::array<juce::ComboBox*, 6> combos {{
            &typeCombo, &channelCombo, &numberCombo,
            &deviceCombo, &controlCombo, &modeCombo
        }};
        for (int row = 0; row < 3; ++row)
        {
            auto pair = detail.removeFromTop (labelH + rowH + 5);
            auto left = pair.removeFromLeft ((pair.getWidth() - 7) / 2);
            pair.removeFromLeft (7);
            auto right = pair;

            for (int column = 0; column < 2; ++column)
            {
                auto field = column == 0 ? left : right;
                const int index = row * 2 + column;
                detailFields.add (field);
                field.removeFromTop (labelH);
                combos[(size_t) index]->setBounds (field.removeFromTop (rowH));
            }
        }

        detail.removeFromTop (4);
        auto actions = detail.removeFromBottom (38);
        testButton.setBounds (actions.removeFromLeft (juce::jmin (82, actions.getWidth() / 3)));
        actions.removeFromLeft (7);
        saveButton.setBounds (actions);

        auto statusArea = heroBounds.reduced (30, 10);
        statusArea.removeFromTop (52);
        statusBounds = statusArea.removeFromTop (19).removeFromLeft (
            juce::jlimit (280, 470, heroBounds.getWidth() / 2));
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const int next = rowAt (e.position);
        if (next != hoveredVisibleRow)
        {
            hoveredVisibleRow = next;
            repaint (tableRowsBounds);
        }
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        hoveredVisibleRow = -1;
        repaint (tableRowsBounds);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (categoriesBounds.contains (e.getPosition()))
        {
            const int idx = categoryAt (e.position);
            if (juce::isPositiveAndBelow (idx, (int) categories.size()))
            {
                selectedCategory = idx;
                firstVisibleRow = 0;
                rebuildVisibleRows();
                repaint (categoriesBounds);
            }
            return;
        }

        if (scrollbarHitBounds().contains (e.getPosition())
             && visibleRows.size() > maxVisibleRows())
        {
            draggingScrollbar = true;
            setScrollFromMouseY (e.getPosition().y);
            return;
        }

        const int visibleIndex = rowAt (e.position);
        if (! juce::isPositiveAndBelow (visibleIndex, visibleRows.size()))
            return;

        const int listIndex = firstVisibleRow + visibleIndex;
        if (! juce::isPositiveAndBelow (listIndex, visibleRows.size()))
            return;

        const int commandIndex = visibleRows[listIndex];
        if (! juce::isPositiveAndBelow (commandIndex, commands.size()))
            return;

        auto row = rowBounds (visibleIndex);
        const auto deleteArea = row.removeFromRight (31);

        if (deleteArea.contains (e.getPosition()))
        {
            if (onDeleteCommand != nullptr)
                onDeleteCommand (commands.getReference (commandIndex));
            return;
        }

        selectedCommand = commandIndex;
        updateDetails();

        if (learning)
            setStatus ("Mova o controle que voce quer mapear para "
                         + commands.getReference (commandIndex).name,
                       Status::learning);

        if (onCommandSelected != nullptr)
            onCommandSelected (commands.getReference (commandIndex));

        repaint (tableRowsBounds);
        repaint (detailsBounds);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (draggingScrollbar)
            setScrollFromMouseY (e.getPosition().y);
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        draggingScrollbar = false;
    }

    void mouseWheelMove (const juce::MouseEvent& e,
                         const juce::MouseWheelDetails& wheel) override
    {
        if (visibleRows.isEmpty())
            return;

        const int lastStart = juce::jmax (0, visibleRows.size() - maxVisibleRows());
        float axis = std::abs (wheel.deltaY) >= std::abs (wheel.deltaX)
                       ? wheel.deltaY : wheel.deltaX;
        if (wheel.isReversed)
            axis = -axis;
        const int amount = axis < 0.0f ? 3 : axis > 0.0f ? -3 : 0;
        const int next = juce::jlimit (0, lastStart, firstVisibleRow + amount);
        if (next != firstVisibleRow)
        {
            firstVisibleRow = next;
            hoveredVisibleRow = -1;
            repaint (tableRowsBounds);
        }
    }

private:
    struct Category
    {
        juce::String name;
        int count = 0;
    };

    static juce::Colour background() { return juce::Colour (0xff080b10); }
    static juce::Colour surface()    { return juce::Colour (0xff11161c); }
    static juce::Colour surface2()   { return juce::Colour (0xff171b22); }
    static juce::Colour surface3()   { return juce::Colour (0xff1b2028); }
    static juce::Colour control()    { return juce::Colour (0xff0b0f14); }
    static juce::Colour border()     { return juce::Colours::white.withAlpha (0.085f); }
    static juce::Colour text()       { return juce::Colour (0xfff4f5f7); }
    static juce::Colour muted()      { return juce::Colour (0xff9da3ad); }
    static juce::Colour red()        { return juce::Colour (0xffff304f); }
    static juce::Colour redDark()    { return juce::Colour (0xff8d1728); }
    static juce::Colour green()      { return juce::Colour (0xff44d477); }
    static juce::Colour amber()      { return juce::Colour (0xfff1b648); }

    static void drawCard (juce::Graphics& g, juce::Rectangle<float> r,
                          float radius = 10.0f, bool accent = false)
    {
        juce::Path p;
        p.addRoundedRectangle (r, radius);
        juce::DropShadow (juce::Colours::black.withAlpha (0.52f), 12, { 0, 4 })
            .drawForPath (g, p);
        g.setGradientFill (juce::ColourGradient (surface2(), r.getX(), r.getY(),
                                                 surface().darker (0.20f), r.getX(),
                                                 r.getBottom(), false));
        g.fillRoundedRectangle (r, radius);
        g.setColour (juce::Colours::white.withAlpha (0.075f));
        g.drawRoundedRectangle (r.reduced (0.5f), radius, 1.0f);
        g.setColour ((accent ? red() : juce::Colours::white).withAlpha (accent ? 0.42f : 0.045f));
        g.drawLine (r.getX() + radius, r.getY() + 1.0f,
                    r.getRight() - radius, r.getY() + 1.0f, 1.0f);
    }

    void configureButton (juce::TextButton& button, const juce::String& label,
                          bool toggle, bool danger = false)
    {
        button.setButtonText (label);
        button.setWantsKeyboardFocus (false);
        button.setClickingTogglesState (toggle);
        button.setColour (juce::TextButton::buttonColourId,
                          danger ? redDark().darker (0.24f) : control());
        button.setColour (juce::TextButton::buttonOnColourId, redDark());
        button.setColour (juce::TextButton::textColourOffId,
                          danger ? red().brighter (0.12f) : text());
        button.setColour (juce::TextButton::textColourOnId, text());
        addAndMakeVisible (button);
    }

    static void setupCombo (juce::ComboBox& combo,
                            std::initializer_list<const char*> items)
    {
        combo.setColour (juce::ComboBox::backgroundColourId, control());
        combo.setColour (juce::ComboBox::outlineColourId, border());
        combo.setColour (juce::ComboBox::textColourId, text());
        combo.setColour (juce::ComboBox::arrowColourId, red());
        int id = 1;
        for (const auto* item : items)
            combo.addItem (item, id++);
        if (id > 1)
            combo.setSelectedId (1, juce::dontSendNotification);
    }

    void drawHero (juce::Graphics& g)
    {
        const auto r = heroBounds.toFloat();
        drawCard (g, r, 12.0f, true);

        g.setGradientFill (juce::ColourGradient (
            redDark().withAlpha (0.33f), r.getX(), r.getCentreY(),
            juce::Colours::transparentBlack, r.getX() + r.getWidth() * 0.54f,
            r.getCentreY(), false));
        g.fillRoundedRectangle (r.reduced (1.0f), 11.0f);

        auto content = heroBounds.reduced (30, 10);
        auto tip = content.removeFromRight (juce::jlimit (210, 268, content.getWidth() / 4));
        content.removeFromRight (18);
        auto copy = content.removeFromLeft (juce::jlimit (310, 410, content.getWidth() / 2));

        g.setColour (text());
        g.setFont (juce::Font (juce::FontOptions (27.0f, juce::Font::bold))
                       .withExtraKerningFactor (0.035f));
        g.drawText ("MAPEAR MIDI", copy.removeFromTop (32),
                    juce::Justification::centredLeft, false);
        g.setColour (text().withAlpha (0.78f));
        g.setFont (juce::Font (juce::FontOptions (13.0f)));
        g.drawText ("Personalize o controle do seu setup", copy.removeFromTop (18),
                    juce::Justification::centredLeft, false);
        copy.removeFromTop (19);

        // Controlador MIDI vetorial: teclas, pads e knobs integrados ao banner.
        auto controller = content.toFloat().reduced (6.0f, 8.0f);
        controller = controller.withTrimmedRight (8.0f);
        g.setColour (juce::Colours::black.withAlpha (0.44f));
        g.fillRoundedRectangle (controller.translated (0.0f, 4.0f), 9.0f);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff242932), controller.getX(), controller.getY(),
                                                 juce::Colour (0xff0a0d12), controller.getX(), controller.getBottom(), false));
        g.fillRoundedRectangle (controller, 9.0f);
        g.setColour (red().withAlpha (0.18f));
        g.drawRoundedRectangle (controller.reduced (0.5f), 9.0f, 1.0f);

        auto keys = controller.reduced (12.0f, 13.0f).removeFromBottom (controller.getHeight() * 0.54f);
        const float keyW = keys.getWidth() / 12.0f;
        for (int i = 0; i < 12; ++i)
        {
            auto key = keys.withX (keys.getX() + keyW * (float) i).withWidth (keyW - 1.0f);
            g.setColour (juce::Colour (0xffd9dce1).withAlpha (0.78f));
            g.fillRoundedRectangle (key, 1.5f);
            if (i < 11 && (i % 7 != 2 && i % 7 != 6))
            {
                g.setColour (juce::Colour (0xff11141a));
                g.fillRoundedRectangle (key.withX (key.getRight() - keyW * 0.34f)
                                           .withWidth (keyW * 0.62f)
                                           .withHeight (keys.getHeight() * 0.62f), 1.5f);
            }
        }

        auto pads = controller.reduced (14.0f, 10.0f).removeFromTop (controller.getHeight() * 0.31f)
                              .removeFromRight (controller.getWidth() * 0.47f);
        for (int i = 0; i < 4; ++i)
        {
            const auto pad = juce::Rectangle<float> (pads.getX() + (float) i * (pads.getWidth() / 4.0f),
                                                      pads.getY(), pads.getWidth() / 4.0f - 4.0f,
                                                      pads.getHeight());
            g.setGradientFill (juce::ColourGradient (red().withAlpha (0.92f), pad.getX(), pad.getY(),
                                                     redDark(), pad.getX(), pad.getBottom(), false));
            g.fillRoundedRectangle (pad, 3.0f);
        }

        auto tipF = tip.toFloat();
        g.setColour (juce::Colour (0xff0d1016).withAlpha (0.90f));
        g.fillRoundedRectangle (tipF, 9.0f);
        g.setColour (juce::Colours::white.withAlpha (0.08f));
        g.drawRoundedRectangle (tipF.reduced (0.5f), 9.0f, 1.0f);
        g.setColour (amber());
        g.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
        g.drawText (juce::String::fromUTF8 ("\xe2\x97\x89  Dica"), tip.removeFromTop (28),
                    juce::Justification::centredLeft, false);
        g.setColour (muted());
        g.setFont (juce::Font (juce::FontOptions (10.5f)));
        g.drawFittedText ("1. Selecione o alvo  2. Mova o fader/knob fisico  3. Faca o pickup se solicitado.",
                          tip.reduced (0, 2), juce::Justification::centredLeft, 3, 0.82f);

        drawStatus (g);
    }

    void drawStatus (juce::Graphics& g)
    {
        if (statusBounds.isEmpty() || statusText.isEmpty())
            return;

        auto colour = muted();
        switch (status)
        {
            case Status::working:  colour = amber(); break;
            case Status::learning: colour = red(); break;
            case Status::success:  colour = green(); break;
            case Status::warning:  colour = amber(); break;
            case Status::error:    colour = red(); break;
            case Status::neutral:  break;
        }

        auto r = statusBounds.toFloat();
        g.setColour (juce::Colours::black.withAlpha (0.38f));
        g.fillRoundedRectangle (r, 5.0f);
        float alpha = 0.82f;
        if (status == Status::learning)
        {
            const auto phase = (float) ((juce::Time::getMillisecondCounter() - learnPulseStarted) % 1300u) / 1300.0f;
            alpha = 0.66f + 0.28f * std::sin (phase * juce::MathConstants<float>::twoPi) * 0.5f + 0.14f;
        }
        g.setColour (colour.withAlpha (juce::jlimit (0.55f, 0.96f, alpha)));
        g.fillEllipse (r.getX() + 8.0f, r.getCentreY() - 3.5f, 7.0f, 7.0f);
        g.setFont (juce::Font (juce::FontOptions (10.2f, juce::Font::bold)));
        g.drawFittedText (statusText, statusBounds.withTrimmedLeft (22),
                          juce::Justification::centredLeft, 1, 0.78f);
    }

    void drawToolbar (juce::Graphics& g)
    {
        drawCard (g, toolbarBounds.toFloat(), 9.0f);
        auto r = connectedDeviceBounds.toFloat().reduced (2.0f);
        g.setColour (deviceConnected ? green().withAlpha (0.16f)
                                     : red().withAlpha (0.12f));
        g.fillRoundedRectangle (r, 7.0f);
        g.setColour (deviceConnected ? green() : red());
        g.fillEllipse (r.getX() + 10.0f, r.getCentreY() - 4.0f, 8.0f, 8.0f);
        g.setColour (muted());
        g.setFont (juce::Font (juce::FontOptions (8.6f, juce::Font::bold)));
        g.drawText (deviceConnected ? "CONTROLADOR CONECTADO" : "SEM CONTROLADOR",
                    connectedDeviceBounds.withTrimmedLeft (28).removeFromTop (18),
                    juce::Justification::centredLeft, false);
        g.setColour (text());
        g.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
        g.drawFittedText (deviceConnected && deviceName.isNotEmpty() ? deviceName : "Detectar dispositivo MIDI",
                          connectedDeviceBounds.withTrimmedLeft (28).withTrimmedTop (16),
                          juce::Justification::centredLeft, 1, 0.78f);

        auto badge = learnButton.getBounds().translated (-48, 0).withWidth (43).reduced (3, 9);
        g.setColour ((mappingCount > 0 ? green() : muted()).withAlpha (0.15f));
        g.fillRoundedRectangle (badge.toFloat(), 5.0f);
        g.setColour (mappingCount > 0 ? green() : muted());
        g.setFont (juce::Font (juce::FontOptions (9.5f, juce::Font::bold)));
        g.drawText (juce::String (mappingCount), badge, juce::Justification::centred);
    }

    void drawCategories (juce::Graphics& g)
    {
        drawCard (g, categoriesBounds.toFloat(), 10.0f);
        auto area = categoriesBounds.reduced (11);
        g.setColour (text());
        g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
        g.drawText ("Categorias", area.removeFromTop (25), juce::Justification::centredLeft);
        area.removeFromTop (4);

        categoryRows.clearQuick();
        const int rowH = juce::jmax (31, juce::jmin (40, area.getHeight() / juce::jmax (1, (int) categories.size())));
        for (int i = 0; i < (int) categories.size(); ++i)
        {
            auto row = area.removeFromTop (rowH).reduced (1, 2);
            categoryRows.add (row);
            const bool selected = i == selectedCategory;
            if (selected)
            {
                g.setGradientFill (juce::ColourGradient (redDark().brighter (0.15f), row.getX(), row.getY(),
                                                         redDark().darker (0.28f), row.getRight(), row.getBottom(), false));
                g.fillRoundedRectangle (row.toFloat(), 7.0f);
                g.setColour (red().withAlpha (0.72f));
                g.drawRoundedRectangle (row.toFloat().reduced (0.5f), 7.0f, 1.0f);
            }
            else
            {
                g.setColour (control().withAlpha (0.72f));
                g.fillRoundedRectangle (row.toFloat(), 7.0f);
                g.setColour (border());
                g.drawRoundedRectangle (row.toFloat().reduced (0.5f), 7.0f, 1.0f);
            }

            g.setColour (selected ? text() : text().withAlpha (0.84f));
            g.setFont (juce::Font (juce::FontOptions (10.6f, selected ? juce::Font::bold : juce::Font::plain)));
            g.drawText (categories[(size_t) i].name, row.withTrimmedLeft (12).withTrimmedRight (36),
                        juce::Justification::centredLeft, true);
            auto count = row.removeFromRight (31).reduced (4, 7);
            g.setColour ((selected ? red() : muted()).withAlpha (selected ? 0.45f : 0.20f));
            g.fillRoundedRectangle (count.toFloat(), 4.0f);
            g.setColour (selected ? text() : muted());
            g.setFont (juce::Font (juce::FontOptions (8.8f, juce::Font::bold)));
            g.drawText (juce::String (categories[(size_t) i].count), count,
                        juce::Justification::centred);
        }
    }

    void drawTable (juce::Graphics& g)
    {
        drawCard (g, tableBounds.toFloat(), 10.0f);
        g.setColour (juce::Colours::white.withAlpha (0.035f));
        g.fillRect (tableHeaderBounds);

        const std::array<const char*, 7> headers {{
            "COMANDO", "TIPO", "CANAL", "CC / NOTA", "DISPOSITIVO", "CONTROLE", "ACAO"
        }};
        const auto cols = tableColumns();
        g.setColour (muted().withAlpha (0.90f));
        g.setFont (juce::Font (juce::FontOptions (8.4f, juce::Font::bold))
                       .withExtraKerningFactor (0.08f));
        for (int i = 0; i < (int) headers.size(); ++i)
            g.drawText (headers[(size_t) i], cols[(size_t) i].withY (tableHeaderBounds.getY())
                                                     .withHeight (tableHeaderBounds.getHeight()),
                        i == 6 ? juce::Justification::centred : juce::Justification::centredLeft, false);

        const int available = juce::jmax (0, visibleRows.size() - firstVisibleRow);
        const int visibleCount = juce::jmin (available, maxVisibleRows());
        for (int i = 0; i < visibleCount; ++i)
        {
            const int commandIndex = visibleRows[firstVisibleRow + i];
            if (! juce::isPositiveAndBelow (commandIndex, commands.size()))
                continue;
            const auto& command = commands.getReference (commandIndex);
            auto row = rowBounds (i);
            const bool selected = commandIndex == selectedCommand;
            const bool hovered = i == hoveredVisibleRow;

            if (selected)
            {
                g.setGradientFill (juce::ColourGradient (redDark().withAlpha (0.56f), row.getX(), row.getY(),
                                                         redDark().withAlpha (0.18f), row.getRight(), row.getBottom(), false));
                g.fillRoundedRectangle (row.toFloat().reduced (1.0f, 1.5f), 6.0f);
                g.setColour (red().withAlpha (0.52f));
                g.fillRect (row.getX(), row.getY() + 4, 2, row.getHeight() - 8);
            }
            else if (hovered)
            {
                g.setColour (juce::Colours::white.withAlpha (0.045f));
                g.fillRoundedRectangle (row.toFloat().reduced (1.0f, 1.5f), 6.0f);
            }

            g.setColour (juce::Colours::white.withAlpha (0.045f));
            g.fillRect (row.getX() + 5, row.getBottom() - 1, row.getWidth() - 10, 1);

            auto rowCols = tableColumnsForRow (row);
            drawCommandIcon (g, rowCols[0].removeFromLeft (25), command.target, selected);
            g.setColour (selected ? text() : text().withAlpha (0.88f));
            g.setFont (juce::Font (juce::FontOptions (10.3f, selected ? juce::Font::bold : juce::Font::plain)));
            g.drawFittedText (command.name, rowCols[0], juce::Justification::centredLeft, 1, 0.76f);

            drawPill (g, rowCols[1], command.type, command.type == "Nota" ? amber() : muted());
            drawCell (g, rowCols[2], juce::String (command.midiChannel));
            drawCell (g, rowCols[3], command.number >= 0
                                        ? (command.type == "Nota" ? "Nota " : "CC ") + juce::String (command.number)
                                        : juce::String::fromUTF8 ("\xe2\x80\x94"));
            drawCell (g, rowCols[4], command.device.isNotEmpty() ? command.device : "Qualquer");
            drawPill (g, rowCols[5],
                      command.needsRelink ? "Revalidar"
                        : command.control.isNotEmpty() ? command.control
                                                       : "Nao mapeado",
                      command.needsRelink ? amber()
                        : command.mapped ? green() : muted());

            auto action = rowCols[6];
            auto del = action.removeFromRight (29).reduced (5, 5);
            auto edit = action.removeFromRight (29).reduced (5, 5);
            g.setColour (juce::Colours::white.withAlpha (0.055f));
            g.fillRoundedRectangle (edit.toFloat(), 5.0f);
            g.fillRoundedRectangle (del.toFloat(), 5.0f);
            g.setColour (text().withAlpha (0.78f));
            g.drawLine ((float) edit.getX() + 5.0f, (float) edit.getBottom() - 5.0f,
                        (float) edit.getRight() - 4.0f, (float) edit.getY() + 4.0f, 1.4f);
            g.setColour (red().withAlpha (
                command.mapped || command.needsRelink ? 0.90f : 0.34f));
            g.drawRect (del.reduced (5, 4), 1);
        }

        if (visibleCount == 0)
        {
            g.setColour (muted());
            g.setFont (juce::Font (juce::FontOptions (13.0f)));
            g.drawText ("Nenhum comando encontrado", tableRowsBounds,
                        juce::Justification::centred);
        }

        if (visibleRows.size() > visibleCount && visibleCount > 0)
        {
            auto trackArea = tableRowsBounds;
            auto track = trackArea.removeFromRight (4).reduced (0, 3).toFloat();
            g.setColour (juce::Colours::white.withAlpha (0.05f));
            g.fillRoundedRectangle (track, 2.0f);
            const float ratio = (float) visibleCount / (float) visibleRows.size();
            const float thumbH = juce::jmax (22.0f, track.getHeight() * ratio);
            const int maxStart = juce::jmax (1, visibleRows.size() - visibleCount);
            const float travel = track.getHeight() - thumbH;
            const float y = track.getY() + travel * (float) firstVisibleRow / (float) maxStart;
            g.setColour (red().withAlpha (0.72f));
            g.fillRoundedRectangle ({ track.getX(), y, track.getWidth(), thumbH }, 2.0f);
        }
    }

    void drawDetails (juce::Graphics& g)
    {
        drawCard (g, detailsBounds.toFloat(), 10.0f, selectedCommand >= 0);
        auto area = detailsBounds.reduced (12);
        g.setColour (text());
        g.setFont (juce::Font (juce::FontOptions (11.8f, juce::Font::bold)));
        g.drawText ("Detalhes do Comando", area.removeFromTop (23),
                    juce::Justification::centredLeft);
        area.removeFromTop (4);

        auto summary = area.removeFromTop (72);
        g.setColour (control());
        g.fillRoundedRectangle (summary.toFloat(), 8.0f);
        g.setColour (border());
        g.drawRoundedRectangle (summary.toFloat().reduced (0.5f), 8.0f, 1.0f);

        const bool hasSelection = juce::isPositiveAndBelow (selectedCommand, commands.size());
        if (hasSelection)
        {
            const auto& command = commands.getReference (selectedCommand);
            drawCommandIcon (g, summary.removeFromLeft (38).reduced (7), command.target, true);
            g.setColour (text());
            g.setFont (juce::Font (juce::FontOptions (11.8f, juce::Font::bold)));
            g.drawFittedText (command.name, summary.removeFromTop (31).withTrimmedRight (5),
                              juce::Justification::centredLeft, 1, 0.76f);
            g.setColour (muted());
            g.setFont (juce::Font (juce::FontOptions (9.3f)));
            g.drawFittedText (command.needsRelink
                                ? "Mapa antigo pendente: confira o alvo e clique Revalidar"
                                : command.mapped ? "Mapeamento ativo e persistido"
                                                 : "Selecione e mova um controle MIDI",
                              summary.withTrimmedRight (5), juce::Justification::centredLeft, 2, 0.78f);
        }
        else
        {
            g.setColour (muted());
            g.setFont (juce::Font (juce::FontOptions (10.5f)));
            g.drawFittedText ("Selecione um comando na tabela para editar ou aprender.",
                              summary.reduced (12), juce::Justification::centredLeft, 2, 0.78f);
        }

        static const std::array<const char*, 6> labels {{
            "Tipo", "Canal MIDI", "CC / Nota", "Dispositivo", "Controle fisico", "Modo"
        }};
        g.setColour (muted());
        g.setFont (juce::Font (juce::FontOptions (8.8f, juce::Font::bold)));
        for (int i = 0; i < detailFields.size() && i < (int) labels.size(); ++i)
        {
            auto field = detailFields.getReference (i);
            g.drawText (labels[(size_t) i], field.removeFromTop (10),
                        juce::Justification::centredLeft, false);
        }
    }

    static void drawCommandIcon (juce::Graphics& g, juce::Rectangle<int> area,
                                 TargetKind target, bool selected)
    {
        auto r = area.toFloat().withSizeKeepingCentre (18.0f, 18.0f);
        g.setColour ((selected ? red() : muted()).withAlpha (0.16f));
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (selected ? text() : muted().brighter (0.15f));
        const auto cx = r.getCentreX(), cy = r.getCentreY();
        if (target == TargetKind::panic)
        {
            juce::Path p;
            p.addTriangle (cx, r.getY() + 3.0f, r.getRight() - 3.0f, r.getBottom() - 3.0f,
                           r.getX() + 3.0f, r.getBottom() - 3.0f);
            g.strokePath (p, juce::PathStrokeType (1.3f));
            g.fillEllipse (cx - 0.8f, cy + 2.5f, 1.6f, 1.6f);
        }
        else if (target == TargetKind::nextScene || target == TargetKind::previousScene
                  || target == TargetKind::scene)
        {
            g.drawRoundedRectangle (r.reduced (4.0f), 2.0f, 1.2f);
            g.drawLine (cx - 2.0f, cy, cx + 3.0f, cy, 1.2f);
        }
        else
        {
            for (int i = -1; i <= 1; ++i)
            {
                const float x = cx + (float) i * 4.0f;
                g.drawLine (x, r.getY() + 4.0f, x, r.getBottom() - 4.0f, 1.1f);
                g.fillEllipse (x - 1.7f, cy + (float) i * 2.0f - 1.7f, 3.4f, 3.4f);
            }
        }
    }

    static void drawCell (juce::Graphics& g, juce::Rectangle<int> area,
                          const juce::String& value)
    {
        g.setColour (text().withAlpha (0.78f));
        g.setFont (juce::Font (juce::FontOptions (9.4f)));
        g.drawFittedText (value, area, juce::Justification::centredLeft, 1, 0.72f);
    }

    static void drawPill (juce::Graphics& g, juce::Rectangle<int> area,
                          const juce::String& value, juce::Colour accent)
    {
        auto pill = area.reduced (2, 7);
        pill.setWidth (juce::jmin (pill.getWidth(), juce::jmax (30, value.length() * 6 + 14)));
        g.setColour (accent.withAlpha (0.12f));
        g.fillRoundedRectangle (pill.toFloat(), 5.0f);
        g.setColour (accent.withAlpha (0.34f));
        g.drawRoundedRectangle (pill.toFloat().reduced (0.5f), 5.0f, 1.0f);
        g.setColour (accent.brighter (0.25f));
        g.setFont (juce::Font (juce::FontOptions (8.6f, juce::Font::bold)));
        g.drawFittedText (value, pill.reduced (5, 0), juce::Justification::centred, 1, 0.72f);
    }

    std::array<juce::Rectangle<int>, 7> tableColumns() const
    {
        return tableColumnsForRow (tableHeaderBounds);
    }

    std::array<juce::Rectangle<int>, 7> tableColumnsForRow (juce::Rectangle<int> row) const
    {
        std::array<juce::Rectangle<int>, 7> result;
        const int usable = row.getWidth();
        const std::array<float, 7> ratios {{ 0.285f, 0.085f, 0.080f, 0.115f, 0.170f, 0.155f, 0.110f }};
        int used = 0;
        for (int i = 0; i < 7; ++i)
        {
            const int w = i == 6 ? usable - used
                                 : juce::roundToInt ((float) usable * ratios[(size_t) i]);
            result[(size_t) i] = row.withX (row.getX() + used).withWidth (w).reduced (5, 0);
            used += w;
        }
        return result;
    }

    int maxVisibleRows() const
    {
        return juce::jmax (1, tableRowsBounds.getHeight() / rowHeight());
    }

    int rowHeight() const
    {
        return juce::jlimit (29, 38, tableRowsBounds.getHeight() / juce::jmax (1, juce::jmin (12, visibleRows.size())));
    }

    juce::Rectangle<int> rowBounds (int visibleIndex) const
    {
        return tableRowsBounds.withY (tableRowsBounds.getY() + visibleIndex * rowHeight())
                              .withHeight (rowHeight());
    }

    int rowAt (juce::Point<float> point) const
    {
        if (! tableRowsBounds.contains (point.toInt()))
            return -1;
        const int row = ((int) point.y - tableRowsBounds.getY()) / juce::jmax (1, rowHeight());
        return row < juce::jmin (visibleRows.size(), maxVisibleRows()) ? row : -1;
    }

    juce::Rectangle<int> scrollbarHitBounds() const
    {
        auto area = tableRowsBounds;
        return area.removeFromRight (12);
    }

    void setScrollFromMouseY (int mouseY)
    {
        const int visible = maxVisibleRows();
        const int lastStart = juce::jmax (0, visibleRows.size() - visible);
        const auto track = scrollbarHitBounds().reduced (0, 3);
        if (lastStart <= 0 || track.getHeight() <= 1)
            return;

        const float position = juce::jlimit (0.0f, 1.0f,
            (float) (mouseY - track.getY()) / (float) track.getHeight());
        firstVisibleRow = juce::jlimit (0, lastStart,
                                       juce::roundToInt (position * (float) lastStart));
        hoveredVisibleRow = -1;
        repaint (tableRowsBounds);
    }

    int categoryAt (juce::Point<float> point) const
    {
        for (int i = 0; i < categoryRows.size(); ++i)
            if (categoryRows.getReference (i).contains (point.toInt()))
                return i;
        return -1;
    }

    void rebuildCategoryCounts()
    {
        for (auto& c : categories)
            c.count = 0;
        categories[0].count = commands.size();
        for (const auto& command : commands)
            for (int i = 1; i < (int) categories.size(); ++i)
                if (command.category == categories[(size_t) i].name)
                    ++categories[(size_t) i].count;
    }

    void rebuildVisibleRows()
    {
        visibleRows.clearQuick();
        const auto query = search.getText().trim();
        const auto category = categories[(size_t) juce::jlimit (0, (int) categories.size() - 1,
                                                               selectedCategory)].name;
        for (int i = 0; i < commands.size(); ++i)
        {
            const auto& command = commands.getReference (i);
            const bool categoryMatches = selectedCategory == 0 || command.category == category;
            const bool queryMatches = query.isEmpty()
                                      || command.name.containsIgnoreCase (query)
                                      || command.control.containsIgnoreCase (query)
                                      || command.device.containsIgnoreCase (query);
            if (categoryMatches && queryMatches)
                visibleRows.add (i);
        }
        if (selectedCommand >= 0 && ! visibleRows.contains (selectedCommand))
        {
            selectedCommand = -1;
            updateDetails();
        }
        firstVisibleRow = juce::jlimit (0, juce::jmax (0, visibleRows.size() - maxVisibleRows()),
                                        firstVisibleRow);
        hoveredVisibleRow = -1;
        repaint (tableBounds);
    }

    void updateDetails()
    {
        const bool enabled = juce::isPositiveAndBelow (selectedCommand, commands.size());
        for (auto* combo : { &typeCombo, &channelCombo, &numberCombo,
                             &deviceCombo, &controlCombo, &modeCombo })
            combo->setEnabled (enabled);
        testButton.setEnabled (enabled);
        saveButton.setEnabled (enabled);

        if (! enabled)
        {
            saveButton.setButtonText (juce::String::fromUTF8 (
                "Salvar altera\xc3\xa7\xc3\xb5" "es"));
            return;
        }

        const auto& command = commands.getReference (selectedCommand);
        saveButton.setButtonText (command.needsRelink
                                    ? "Confirmar e revalidar"
                                    : juce::String::fromUTF8 (
                                        "Salvar altera\xc3\xa7\xc3\xb5" "es"));
        typeCombo.setSelectedId (command.type == "Nota" ? 2 : 1, juce::dontSendNotification);
        channelCombo.setSelectedId (juce::jlimit (1, 16, command.midiChannel), juce::dontSendNotification);
        numberCombo.setSelectedId (juce::jlimit (0, 127, juce::jmax (0, command.number)) + 1,
                                   juce::dontSendNotification);
        deviceCombo.setText (command.device.isNotEmpty() ? command.device
                              : deviceConnected && deviceName.isNotEmpty() ? deviceName
                                                                           : "Qualquer dispositivo",
                             juce::dontSendNotification);
        controlCombo.setText (command.control.isNotEmpty() ? command.control : "Aguardando MIDI",
                              juce::dontSendNotification);
        modeCombo.setSelectedId (juce::jlimit (1, 4, command.mode),
                                 juce::dontSendNotification);
    }

    void timerCallback() override
    {
        const auto now = juce::Time::getMillisecondCounter();
        // Comparacao modular: getMillisecondCounter volta a zero a cada ~49
        // dias. `now >= deadline` cancelava imediatamente quando os 4,2 s
        // atravessavam essa virada.
        if (clearArmed
             && static_cast<juce::int32> (now - clearArmDeadline) >= 0)
        {
            clearArmed = false;
            clearButton.setButtonText ("Limpar mapas");
            setStatus ("Limpeza cancelada", Status::neutral);
        }

        if (learning)
            repaint (statusBounds.expanded (8));
        else if (! clearArmed)
            stopTimer();
    }

    juce::TextEditor search;
    juce::TextButton detectButton, quickButton, learnButton, importButton, exportButton,
                     clearButton, testButton, saveButton;
    juce::ComboBox typeCombo, channelCombo, numberCombo, deviceCombo,
                   controlCombo, modeCombo;

    juce::Array<Command> commands;
    juce::Array<int> visibleRows;
    std::array<Category, 10> categories;
    juce::Array<juce::Rectangle<int>> categoryRows, detailFields;

    juce::Rectangle<int> heroBounds, toolbarBounds, categoriesBounds,
                         tableBounds, detailsBounds, connectedDeviceBounds,
                         tableHeaderBounds, tableRowsBounds, statusBounds;
    juce::String deviceName, statusText;
    bool deviceConnected = false;
    bool learning = false;
    bool clearArmed = false;
    bool draggingScrollbar = false;
    int mappingCount = 0;
    int selectedCategory = 0;
    int selectedCommand = -1;
    int hoveredVisibleRow = -1;
    int firstVisibleRow = 0;
    Status status = Status::neutral;
    juce::uint32 clearArmDeadline = 0;
    juce::uint32 statusChangedAt = 0;
    juce::uint32 learnPulseStarted = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiMappingPanel)
};
