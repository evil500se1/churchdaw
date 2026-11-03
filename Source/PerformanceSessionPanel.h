#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "LiveScenes.h"
#include "NordLookAndFeel.h"
#include "PadSynthProcessor.h"
#include "MidiLearnTargetTag.h"

#include <array>
#include <algorithm>
#include <functional>
#include <memory>

//==============================================================================
// Workspace de palco do ChurchDAW.
//
// PERFORMANCE e uma superficie de operacao: troca cenas reais, controla os
// macros reais do Pad Engine e chama PANIC/Sustain/Transpose do motor.
// SESSAO edita os metadados das mesmas cenas e a ordem real do Setlist.
//==============================================================================
class PerformanceSessionPanel final : public juce::Component
{
public:
    enum class Mode { performance, session };

    explicit PerformanceSessionPanel (PadSynthProcessor& synthToUse)
        : synth (synthToUse)
    {
        setOpaque (true);

        configureButton (previousButton, "ANTERIOR", false);
        configureButton (nextButton, utf8 ("PR\xc3\x93XIMA"), false);
        configureButton (panicButton, "PANIC", true);
        configureButton (emergencyPianoButton, "PIANO SEGURO", true);
        configureButton (stageTestButton, "TESTAR PALCO", false);
        configureButton (autoPatchButton, "AUTO PATCH", false);
        configureButton (playbackButton, "PLAYBACK", false);
        configureButton (tapButton, "TAP TEMPO", false);
        configureButton (quickPadButton, utf8 ("PAD CONT\xc3\x8dNUO"), false);
        configureButton (transposeDownButton, "-", false);
        configureButton (transposeUpButton, "+", false);
        // Sustain e estado, nao acao destrutiva: apagado fica grafite e so
        // acende vermelho quando realmente esta ligado.
        configureButton (sustainButton, "SUSTAIN", false);
        sustainButton.setClickingTogglesState (true);

        configureButton (loadButton, "CARREGAR CENA", false);
        configureButton (captureButton, "GRAVAR SOM ATUAL", true);
        configureButton (deleteButton, "APAGAR", false);
        configureButton (saveMetadataButton, "SALVAR DETALHES", true);
        configureButton (exportSessionButton, "EXPORTAR CULTO", false);
        configureButton (importSessionButton, "IMPORTAR CULTO", false);
        configureButton (undoButton, "DESFAZER", false);
        configureButton (redoButton, "REFAZER", false);
        configureButton (addSetlistButton, "+ SETLIST", false);
        configureButton (removeSetlistButton, "REMOVER", false);
        configureButton (moveUpButton,
                         juce::String::charToString ((juce::juce_wchar) 0x2191),
                         false);
            configureButton (moveDownButton,
                         juce::String::charToString ((juce::juce_wchar) 0x2193),
                         false);

        for (auto* button : { &previousButton, &nextButton, &panicButton,
                              &tapButton, &quickPadButton, &emergencyPianoButton,
                              &stageTestButton,
                              &autoPatchButton,
                              &playbackButton,
                              &transposeDownButton,
                              &transposeUpButton, &sustainButton, &loadButton,
                              &captureButton, &deleteButton, &saveMetadataButton,
                              &exportSessionButton, &importSessionButton,
                              &undoButton, &redoButton,
                              &addSetlistButton, &removeSetlistButton,
                              &moveUpButton, &moveDownButton })
            addAndMakeVisible (*button);

        configureMacro (macroSliders[0], "AMBIENCIA");
        configureMacro (macroSliders[1], "REVERB");
        configureMacro (macroSliders[2], "DELAY");
        configureMacro (macroSliders[3], "MASTER");
        const std::array<juce::AudioParameterFloat*, 4> parameters {
            synth.macroAmbience, synth.macroReverb,
            synth.macroDelay, synth.macroMaster
        };
        for (int i = 0; i < 4; ++i)
        {
            addAndMakeVisible (macroSliders[(size_t) i]);
            if (parameters[(size_t) i] != nullptr)
                macroAttachments[(size_t) i] =
                    std::make_unique<juce::SliderParameterAttachment> (
                        *parameters[(size_t) i], macroSliders[(size_t) i]);
            if (parameters[(size_t) i] != nullptr)
                ChurchDAWMidiLearnTarget::tag (
                    macroSliders[(size_t) i], synth, *parameters[(size_t) i]);
        }

        configureMacro (padVolumeSlider, "PAD VOLUME");
        padVolumeSlider.setSliderStyle (juce::Slider::LinearVertical);
        padVolumeSlider.setRange (0.0, 1.5, 0.01);
        padVolumeSlider.onValueChange = [this]
        {
            if (! updatingControls && onPadVolumeChanged != nullptr)
                onPadVolumeChanged ((float) padVolumeSlider.getValue());
        };
        addAndMakeVisible (padVolumeSlider);

        for (auto* editor : { &songEditor, &nameEditor, &keyEditor, &bpmEditor,
                              &meterEditor, &sectionEditor,
                              &transitionEditor, &durationEditor,
                              &setlistNameEditor })
        {
            configureEditor (*editor);
            addAndMakeVisible (*editor);
        }
        setlistNameEditor.setTextToShowWhenEmpty ("Nome do culto",
                                                   dim.withAlpha (0.72f));
        setlistNameEditor.onReturnKey = [this] { commitSetlistName(); };
        setlistNameEditor.onFocusLost = [this] { commitSetlistName(); };

        previousButton.onClick = [this] { loadRelativeScene (-1); };
        nextButton.onClick = [this] { loadRelativeScene (+1); };
        panicButton.onClick = [this]
        {
            synth.requestPanic();
            if (onPanic != nullptr) onPanic();
        };
        emergencyPianoButton.onClick = [this]
        {
            if (onEmergencyPiano != nullptr) onEmergencyPiano();
        };
        stageTestButton.onClick = [this]
        {
            if (onStageTest != nullptr) onStageTest();
        };
        autoPatchButton.onClick = [this]
        {
            if (onAutoPatch != nullptr) onAutoPatch();
        };
        playbackButton.onClick = [this]
        {
            if (onPlayback != nullptr) onPlayback();
        };
        quickPadButton.onClick = [this]
        {
            if (onQuickPad != nullptr) onQuickPad();
        };
        tapButton.onClick = [this] { registerTap(); };
        sustainButton.onClick = [this]
        {
            sustain = sustainButton.getToggleState();
            if (onSustainChanged != nullptr) onSustainChanged (sustain);
            repaint (performanceActionsBounds);
        };
        transposeDownButton.onClick = [this] { changeTranspose (-1); };
        transposeUpButton.onClick = [this] { changeTranspose (+1); };

        loadButton.onClick = [this]
        {
            if (onLoadScene != nullptr && validScene (selectedScene))
                onLoadScene (selectedScene);
        };
        captureButton.onClick = [this]
        {
            if (onCaptureScene != nullptr && validScene (selectedScene))
                onCaptureScene (selectedScene);
        };
        deleteButton.onClick = [this]
        {
            if (onDeleteScene != nullptr && validScene (selectedScene))
                onDeleteScene (selectedScene);
        };
        saveMetadataButton.onClick = [this] { commitMetadata(); };
        exportSessionButton.onClick = [this]
        {
            if (onExportSession != nullptr) onExportSession();
        };
        importSessionButton.onClick = [this]
        {
            if (onImportSession != nullptr) onImportSession();
        };
        undoButton.onClick = [this]
        {
            if (onUndo != nullptr) onUndo();
        };
        redoButton.onClick = [this]
        {
            if (onRedo != nullptr) onRedo();
        };
        addSetlistButton.onClick = [this] { addSelectedToSetlist(); };
        removeSetlistButton.onClick = [this] { removeSelectedFromSetlist(); };
        moveUpButton.onClick = [this] { moveSetlistSelection (-1); };
        moveDownButton.onClick = [this] { moveSetlistSelection (+1); };

        setMode (Mode::performance);
    }

    std::function<void(int)> onLoadScene;
    std::function<void(int)> onCaptureScene;
    std::function<void(int)> onDeleteScene;
    std::function<void(int, const ChurchDAWLive::Cena&)> onSceneMetadataChanged;
    std::function<void(const ChurchDAWLive::Setlist&)> onSetlistChanged;
    std::function<void()> onPanic;
    std::function<void()> onEmergencyPiano;
    std::function<void()> onStageTest;
    std::function<void()> onAutoPatch;
    std::function<void()> onPlayback;
    std::function<void()> onQuickPad;
    std::function<void(bool)> onSustainChanged;
    std::function<void(int)> onTransposeChanged;
    std::function<void(float)> onPadVolumeChanged;
    std::function<void(int)> onChannelSelected;
    std::function<void()> onExportSession;
    std::function<void()> onImportSession;
    std::function<void()> onUndo;
    std::function<void()> onRedo;

    void setPadVolumeMidiLearnTarget (juce::AudioProcessor* processor,
                                      juce::AudioProcessorParameter* parameter)
    {
        ChurchDAWMidiLearnTarget::clear (padVolumeSlider);
        if (processor != nullptr && parameter != nullptr)
            ChurchDAWMidiLearnTarget::tag (
                padVolumeSlider, *processor, *parameter);
    }

    void setHistoryAvailability (bool canUndo, bool canRedo)
    {
        undoButton.setEnabled (canUndo);
        redoButton.setEnabled (canRedo);
    }

    // Header e MIDI Map executam o mesmo TAP da pagina Performance. Assim o
    // calculo de BPM e a atualizacao da cena continuam em uma unica rotina.
    void triggerTapTempo() { registerTap(); }

    void setMode (Mode newMode)
    {
        mode = newMode;
        if (mode == Mode::session && ! validScene (selectedScene))
            selectedScene = validScene (activeScene) ? activeScene : firstFilledScene();
        updateControlVisibility();
        populateEditors (true);
        resized();
        repaint();
    }

    Mode getMode() const noexcept { return mode; }

    void updateState (
        const ChurchDAWLive::Cena (&source)[ChurchDAWLive::numeroDeCenas],
        const ChurchDAWLive::Setlist& sourceSetlist,
        int activeSceneToUse, int preparedSceneToUse,
        int padKeyToUse, float padVolumeToUse,
        bool sustainToUse, int transposeToUse)
    {
        for (int i = 0; i < ChurchDAWLive::numeroDeCenas; ++i)
            scenes[(size_t) i] = source[i];

        setlist = sourceSetlist;
        activeScene = validScene (activeSceneToUse) ? activeSceneToUse : -1;
        preparedScene = validScene (preparedSceneToUse)
                         && ! scenes[(size_t) preparedSceneToUse].preenchida
                            ? preparedSceneToUse : -1;
        padKey = juce::jlimit (-1, 11, padKeyToUse);
        sustain = sustainToUse;
        globalTranspose = juce::jlimit (-24, 24, transposeToUse);

        if (validScene (preparedScene))
            selectedScene = preparedScene;
        else if (! validScene (selectedScene))
            selectedScene = validScene (activeScene) ? activeScene : firstFilledScene();

        if (validScene (activeScene))
            setlist.reposicionarPara (activeScene);

        updatingControls = true;
        padVolumeSlider.setValue (padVolumeToUse, juce::dontSendNotification);
        sustainButton.setToggleState (sustain, juce::dontSendNotification);
        updatingControls = false;

        if (selectedSetlistPosition >= setlist.tamanho)
            selectedSetlistPosition = setlist.tamanho - 1;

        populateEditors (false);
        if (! setlistNameEditor.hasKeyboardFocus (true))
            setlistNameEditor.setText (setlist.nome, false);
        refreshActionEnablement();
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff080b10));

        g.setGradientFill (juce::ColourGradient (
            juce::Colour (0xff12161c), 0.0f, 0.0f,
            juce::Colour (0xff080a0e), 0.0f, (float) getHeight(), false));
        g.fillRect (getLocalBounds());

        if (mode == Mode::performance)
            paintPerformance (g);
        else
            paintSession (g);
    }

    void resized() override
    {
        if (mode == Mode::performance)
            layoutPerformance();
        else
            layoutSession();
    }

    void mouseDown (const juce::MouseEvent& event) override
    {
        if (mode == Mode::performance)
        {
            if (previousSceneBounds.contains (event.getPosition()))
            {
                loadRelativeScene (-1);
                return;
            }
            if (nextSceneBounds.contains (event.getPosition()))
            {
                loadRelativeScene (+1);
                return;
            }
            for (int i = 0; i < (int) channelBounds.size(); ++i)
                if (channelBounds[(size_t) i].contains (event.getPosition()))
                {
                    if (onChannelSelected != nullptr) onChannelSelected (i);
                    return;
                }
            return;
        }

        for (int i = 0; i < ChurchDAWLive::numeroDeCenas; ++i)
            if (sceneTileBounds[(size_t) i].contains (event.getPosition()))
            {
                selectedScene = i;
                selectedSetlistPosition = findSceneInSetlist (i);
                populateEditors (true);
                refreshActionEnablement();
                repaint();
                return;
            }

        for (int i = 0; i < setlist.tamanho; ++i)
            if (setlistRowBounds[(size_t) i].contains (event.getPosition()))
            {
                selectedSetlistPosition = i;
                selectedScene = setlist.ordem[(size_t) i];
                populateEditors (true);
                refreshActionEnablement();
                repaint();
                return;
            }
    }

private:
    inline static const juce::Colour red { 0xffef244b };
    inline static const juce::Colour panel { 0xff11151b };
    inline static const juce::Colour panelRaised { 0xff171c23 };
    inline static const juce::Colour line { 0xff303641 };
    inline static const juce::Colour text { 0xfff3f4f6 };
    inline static const juce::Colour dim { 0xff9aa1ac };

    static bool validScene (int index) noexcept
    {
        return juce::isPositiveAndBelow (index, ChurchDAWLive::numeroDeCenas);
    }

    static juce::String utf8 (const char* value)
    {
        return juce::String::fromUTF8 (value);
    }

    static void configureButton (juce::TextButton& button,
                                 const juce::String& label, bool primary)
    {
        button.setButtonText (label);
        button.setColour (juce::TextButton::buttonColourId,
                          primary ? juce::Colour (0xff781426) : panelRaised);
        button.setColour (juce::TextButton::buttonOnColourId,
                          juce::Colour (0xffa71934));
        button.setColour (juce::TextButton::textColourOffId,
                          primary ? text : juce::Colour (0xffd8dbe1));
        button.setColour (juce::TextButton::textColourOnId, text);
    }

    static void configureMacro (juce::Slider& slider, const char* name)
    {
        slider.setName (name);
        slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        slider.setDoubleClickReturnValue (true, 0.5);
        slider.setColour (juce::Slider::rotarySliderFillColourId, red);
        slider.setColour (juce::Slider::rotarySliderOutlineColourId,
                          juce::Colour (0xff343a44));
    }

    static void configureEditor (juce::TextEditor& editor)
    {
        editor.setColour (juce::TextEditor::backgroundColourId,
                          juce::Colour (0xff0c0f14));
        editor.setColour (juce::TextEditor::textColourId, text);
        editor.setColour (juce::TextEditor::outlineColourId, line);
        editor.setColour (juce::TextEditor::focusedOutlineColourId,
                          red.withAlpha (0.78f));
        editor.setColour (juce::TextEditor::highlightColourId,
                          red.withAlpha (0.32f));
        editor.setFont (juce::Font (juce::FontOptions (11.0f)));
        editor.setJustification (juce::Justification::centredLeft);
    }

    static juce::String sceneLabel (const ChurchDAWLive::Cena& scene,
                                    int index)
    {
        if (scene.nome.trim().isNotEmpty()) return scene.nome.trim();
        return "Cena " + juce::String (index + 1);
    }

    static juce::String songLabel (const ChurchDAWLive::Cena& scene,
                                   int index)
    {
        if (scene.musica.trim().isNotEmpty()) return scene.musica.trim();
        if (scene.nome.trim().isNotEmpty()) return scene.nome.trim();
        return juce::String::fromUTF8 ("M\xc3\xba" "sica ")
             + juce::String (index + 1);
    }

    static juce::String stageSceneLabel (const ChurchDAWLive::Cena& scene,
                                         int index)
    {
        const auto section = scene.secao.trim();
        if (section.isNotEmpty() && ! section.equalsIgnoreCase ("Cena"))
            return section;
        return sceneLabel (scene, index);
    }

    juce::String songGroupKeyForScene (int sceneIndex) const
    {
        if (! validScene (sceneIndex)) return {};
        const auto song = scenes[(size_t) sceneIndex].musica.trim();
        // Cena legada sem campo musica e um grupo independente. Usar o nome
        // como chave juntaria snapshots antigos com nomes repetidos por acaso.
        return song.isNotEmpty() ? song.toLowerCase()
                                 : "#" + juce::String (sceneIndex);
    }

    bool sameSongAtPositions (int first, int second) const
    {
        return juce::isPositiveAndBelow (first, setlist.tamanho)
            && juce::isPositiveAndBelow (second, setlist.tamanho)
            && songGroupKeyForScene (setlist.ordem[(size_t) first])
                == songGroupKeyForScene (setlist.ordem[(size_t) second]);
    }

    std::pair<int, int> songRangeAtPosition (int position) const
    {
        if (! juce::isPositiveAndBelow (position, setlist.tamanho))
            return { -1, -1 };
        int first = position, last = position;
        while (first > 0 && sameSongAtPositions (first - 1, position)) --first;
        while (last + 1 < setlist.tamanho
               && sameSongAtPositions (last + 1, position)) ++last;
        return { first, last };
    }

    int songCount() const
    {
        int count = 0;
        for (int i = 0; i < setlist.tamanho; ++i)
            if (i == 0 || ! sameSongAtPositions (i - 1, i)) ++count;
        return count;
    }

    int firstFilledScene() const noexcept
    {
        for (int i = 0; i < ChurchDAWLive::numeroDeCenas; ++i)
            if (scenes[(size_t) i].preenchida) return i;
        return 0;
    }

    int findSceneInSetlist (int scene) const noexcept
    {
        for (int i = 0; i < setlist.tamanho; ++i)
            if (setlist.ordem[(size_t) i] == scene) return i;
        return -1;
    }

    int relativeScene (int direction) const noexcept
    {
        if (! setlist.vazio())
        {
            int position = findSceneInSetlist (activeScene);
            if (position < 0) position = direction > 0 ? -1 : setlist.tamanho;
            const int target = position + direction;
            return juce::isPositiveAndBelow (target, setlist.tamanho)
                     ? setlist.ordem[(size_t) target] : -1;
        }

        int start = validScene (activeScene)
                      ? activeScene : direction > 0 ? -1
                                                   : ChurchDAWLive::numeroDeCenas;
        for (int i = start + direction;
             juce::isPositiveAndBelow (i, ChurchDAWLive::numeroDeCenas);
             i += direction)
            if (scenes[(size_t) i].preenchida) return i;
        return -1;
    }

    void loadRelativeScene (int direction)
    {
        const int target = relativeScene (direction);
        if (target >= 0 && onLoadScene != nullptr) onLoadScene (target);
    }

    void changeTranspose (int amount)
    {
        globalTranspose = juce::jlimit (-24, 24, globalTranspose + amount);
        if (onTransposeChanged != nullptr) onTransposeChanged (globalTranspose);
        repaint (performanceActionsBounds);
    }

    void registerTap()
    {
        const auto now = juce::Time::getMillisecondCounter();
        if (lastTapMs != 0)
        {
            const auto elapsed = now - lastTapMs;
            if (elapsed >= 200 && elapsed <= 2000 && validScene (activeScene))
            {
                auto& scene = scenes[(size_t) activeScene];
                scene.bpm = juce::jlimit (30.0, 300.0, 60000.0 / (double) elapsed);
                if (onSceneMetadataChanged != nullptr)
                    onSceneMetadataChanged (activeScene, scene);
            }
        }
        lastTapMs = now;
        repaint();
    }

    void populateEditors (bool force)
    {
        if (mode != Mode::session || ! validScene (selectedScene)) return;
        if (! force && editorScene == selectedScene) return;

        const auto& scene = scenes[(size_t) selectedScene];
        songEditor.setText (scene.musica, false);
        nameEditor.setText (sceneLabel (scene, selectedScene), false);
        keyEditor.setText (scene.tonalidade, false);
        bpmEditor.setText (juce::String (scene.bpm, 1), false);
        meterEditor.setText (scene.compasso, false);
        sectionEditor.setText (scene.secao, false);
        transitionEditor.setText (juce::String (scene.transicaoSegundos, 2), false);
        durationEditor.setText (juce::String (scene.duracaoSegundos, 0), false);
        setlistNameEditor.setText (setlist.nome, false);
        editorScene = selectedScene;
    }

    void commitMetadata()
    {
        if (! validScene (selectedScene)) return;
        auto& scene = scenes[(size_t) selectedScene];
        scene.musica = songEditor.getText().trim().substring (0, 56);
        scene.nome = nameEditor.getText().trim().substring (0, 42);
        scene.tonalidade = keyEditor.getText().trim().substring (0, 8);
        scene.bpm = juce::jlimit (30.0, 300.0, bpmEditor.getText().getDoubleValue());
        scene.compasso = meterEditor.getText().trim().substring (0, 8);
        scene.secao = sectionEditor.getText().trim().substring (0, 28);
        scene.transicaoSegundos = juce::jlimit (
            0.0, 15.0, transitionEditor.getText().getDoubleValue());
        scene.duracaoSegundos = juce::jlimit (
            0.0, 86400.0, durationEditor.getText().getDoubleValue());
        if (scene.tonalidade.isEmpty()) scene.tonalidade = "C";
        if (scene.compasso.isEmpty()) scene.compasso = "4/4";
        if (scene.secao.isEmpty()) scene.secao = "Cena";

        if (onSceneMetadataChanged != nullptr)
            onSceneMetadataChanged (selectedScene, scene);
        commitSetlistName();
        repaint();
    }

    void commitSetlistName()
    {
        auto name = setlistNameEditor.getText().trim().substring (0, 64);
        if (name.isEmpty()) name = "Culto atual";
        if (setlist.nome == name) return;
        setlist.nome = name;
        if (onSetlistChanged != nullptr) onSetlistChanged (setlist);
        repaint (setlistBounds);
    }

    void addSelectedToSetlist()
    {
        if (! validScene (selectedScene)) return;
        setlist.adicionar (selectedScene);
        // Mantem todas as cenas da mesma musica juntas. O Setlist continua
        // armazenando snapshots reais, mas a ordem visual e operacional passa
        // a ser culto -> musica -> cenas.
        int appended = findSceneInSetlist (selectedScene);
        const auto key = songGroupKeyForScene (selectedScene);
        int lastSibling = -1;
        for (int i = 0; i < appended; ++i)
            if (songGroupKeyForScene (setlist.ordem[(size_t) i]) == key)
                lastSibling = i;
        if (lastSibling >= 0 && appended > lastSibling + 1)
            setlist.mover (appended, lastSibling + 1);
        selectedSetlistPosition = findSceneInSetlist (selectedScene);
        publishSetlist();
    }

    void removeSelectedFromSetlist()
    {
        if (! juce::isPositiveAndBelow (selectedSetlistPosition, setlist.tamanho))
            return;
        setlist.removerNaPosicao (selectedSetlistPosition);
        selectedSetlistPosition = juce::jmin (selectedSetlistPosition,
                                               setlist.tamanho - 1);
        publishSetlist();
    }

    void moveSetlistSelection (int direction)
    {
        const auto range = songRangeAtPosition (selectedSetlistPosition);
        if (range.first < 0 || direction == 0) return;

        const int selectedSceneBeforeMove =
            setlist.ordem[(size_t) selectedSetlistPosition];
        auto begin = setlist.ordem.begin();
        bool moved = false;
        if (direction < 0 && range.first > 0)
        {
            const auto previous = songRangeAtPosition (range.first - 1);
            std::rotate (begin + previous.first, begin + range.first,
                         begin + range.second + 1);
            moved = true;
        }
        else if (direction > 0 && range.second + 1 < setlist.tamanho)
        {
            const auto next = songRangeAtPosition (range.second + 1);
            std::rotate (begin + range.first, begin + range.second + 1,
                         begin + next.second + 1);
            moved = true;
        }

        if (! moved) return;
        setlist.reposicionarPara (activeScene);
        selectedSetlistPosition = findSceneInSetlist (selectedSceneBeforeMove);
        publishSetlist();
    }

    void publishSetlist()
    {
        if (onSetlistChanged != nullptr) onSetlistChanged (setlist);
        refreshActionEnablement();
        resized();
        repaint();
    }

    void refreshActionEnablement()
    {
        const bool selected = validScene (selectedScene);
        const bool filled = selected && scenes[(size_t) selectedScene].preenchida;
        const bool inSetlist = juce::isPositiveAndBelow (
            selectedSetlistPosition, setlist.tamanho);
        const auto selectedSongRange = songRangeAtPosition (
            selectedSetlistPosition);

        // Carregar uma cena vazia e justamente o gesto PREPARAR NOVA: o motor
        // abre zerado e so vira cena salva quando o usuario gravar depois.
        loadButton.setEnabled (selected);
        loadButton.setButtonText (selected && ! filled
                                    ? "PREPARAR NOVA" : "CARREGAR CENA");
        deleteButton.setEnabled (filled);
        addSetlistButton.setEnabled (filled && findSceneInSetlist (selectedScene) < 0);
        removeSetlistButton.setEnabled (inSetlist);
        moveUpButton.setEnabled (inSetlist && selectedSongRange.first > 0);
        moveDownButton.setEnabled (inSetlist && selectedSongRange.second + 1
                                                  < setlist.tamanho);
        saveMetadataButton.setEnabled (selected);
        captureButton.setEnabled (selected);
    }

    void updateControlVisibility()
    {
        const bool performance = mode == Mode::performance;
        const std::array<juce::Component*, 11> performanceControls {
            &panicButton, &tapButton, &quickPadButton, &emergencyPianoButton,
            &stageTestButton,
            &autoPatchButton,
            &playbackButton,
            &transposeDownButton, &transposeUpButton, &sustainButton,
            &padVolumeSlider
        };
        for (auto* component : performanceControls)
            component->setVisible (performance);
        for (auto& slider : macroSliders) slider.setVisible (performance);
        previousButton.setVisible (false);
        nextButton.setVisible (false);

        const std::array<juce::Component*, 21> sessionControls {
            &loadButton, &captureButton, &deleteButton,
            &saveMetadataButton, &addSetlistButton, &removeSetlistButton,
            &exportSessionButton, &importSessionButton,
            &undoButton, &redoButton,
            &moveUpButton, &moveDownButton, &songEditor, &nameEditor, &keyEditor,
            &bpmEditor, &meterEditor, &sectionEditor,
            &transitionEditor, &durationEditor, &setlistNameEditor
        };
        for (auto* component : sessionControls)
            component->setVisible (! performance);
    }

    void drawCard (juce::Graphics& g, juce::Rectangle<int> bounds,
                   bool selected = false, float radius = 8.0f) const
    {
        g.setGradientFill (juce::ColourGradient (
            selected ? juce::Colour (0xff32131a) : panelRaised,
            (float) bounds.getX(), (float) bounds.getY(),
            selected ? juce::Colour (0xff160e12) : panel,
            (float) bounds.getX(), (float) bounds.getBottom(), false));
        g.fillRoundedRectangle (bounds.toFloat(), radius);
        g.setColour (selected ? red.withAlpha (0.72f) : line);
        g.drawRoundedRectangle (bounds.toFloat().reduced (0.5f), radius,
                                selected ? 1.2f : 0.8f);
    }

    void paintPerformance (juce::Graphics& g)
    {
        const int current = validScene (activeScene) ? activeScene : firstFilledScene();
        const auto& scene = scenes[(size_t) current];
        const int previous = relativeScene (-1);
        const int next = relativeScene (+1);

        drawCard (g, heroBounds, true, 10.0f);
        auto hero = heroBounds.reduced (18, 12);
        hero.removeFromBottom (50);
        g.setColour (red);
        g.setFont (juce::Font (juce::FontOptions (9.0f)).boldened()
                       .withExtraKerningFactor (0.16f));
        g.drawText ("AGORA NO PALCO", hero.removeFromTop (18),
                    juce::Justification::centredLeft);
        g.setColour (text);
        g.setFont (juce::Font (juce::FontOptions (27.0f)).boldened());
        g.drawFittedText (songLabel (scene, current).toUpperCase(),
                          hero.removeFromTop (43),
                          juce::Justification::centredLeft, 1);
        g.setColour (dim);
        g.setFont (juce::Font (juce::FontOptions (11.5f)));
        const auto bullet = juce::String::fromUTF8 ("  \xe2\x80\xa2  ");
        g.drawText ("Tom  " + scene.tonalidade + bullet
                        + juce::String (scene.bpm, 1) + " BPM" + bullet
                        + scene.compasso,
                    hero.removeFromTop (24), juce::Justification::centredLeft);
        g.setColour (red);
        g.setFont (juce::Font (juce::FontOptions (15.0f)).boldened());
        g.drawFittedText (stageSceneLabel (scene, current).toUpperCase(), hero,
                          juce::Justification::centredLeft, 1);

        auto drawNavLabel = [&] (juce::Rectangle<int> bounds,
                                 const juce::String& kicker,
                                 const juce::String& value, bool active)
        {
            drawCard (g, bounds, active);
            auto b = bounds.reduced (10, 5);
            g.setColour (active ? red : dim);
            g.setFont (juce::Font (juce::FontOptions (8.0f)).boldened());
            g.drawText (kicker, b.removeFromTop (15),
                        juce::Justification::centred);
            g.setColour (text);
            g.setFont (juce::Font (juce::FontOptions (14.0f)).boldened());
            g.drawFittedText (value, b, juce::Justification::centred, 1);
        };
        const auto navValue = [&] (int index)
        {
            if (! validScene (index)) return juce::String();
            const auto& target = scenes[(size_t) index];
            const auto stage = stageSceneLabel (target, index);
            return songGroupKeyForScene (index) == songGroupKeyForScene (current)
                     ? stage
                     : songLabel (target, index) + " / " + stage;
        };
        drawNavLabel (previousSceneBounds, "ANTERIOR",
                      previous >= 0 ? navValue (previous)
                                    : utf8 ("In\xc3\xad" "cio"), false);
        drawNavLabel (currentSceneBounds, "CENA ATUAL",
                      stageSceneLabel (scene, current), true);
        drawNavLabel (nextSceneBounds, utf8 ("PR\xc3\x93XIMA"),
                      next >= 0 ? navValue (next)
                                : "Final", false);

        drawCard (g, macroCardBounds);
        g.setColour (red);
        g.setFont (juce::Font (juce::FontOptions (8.5f)).boldened());
        g.drawText (utf8 ("CONTROLES R\xc3\x81PIDOS / MACROS"),
                    macroCardBounds.reduced (12, 5).removeFromTop (18),
                    juce::Justification::centredLeft);
        for (int i = 0; i < 4; ++i)
        {
            const auto b = macroSliders[(size_t) i].getBounds();
            g.setColour (dim);
            g.setFont (juce::Font (juce::FontOptions (8.2f)).boldened());
            g.drawText (macroSliders[(size_t) i].getName(),
                        b.withHeight (16).translated (0, -7),
                        juce::Justification::centred);
            g.setColour (text);
            g.setFont (juce::Font (juce::FontOptions (10.0f)));
            g.drawText (juce::String (juce::roundToInt (
                            macroSliders[(size_t) i].getValue() * 100.0)) + "%",
                        b.withHeight (16).withY (b.getBottom() - 5),
                        juce::Justification::centred);
        }
        g.setColour (dim);
        g.setFont (juce::Font (juce::FontOptions (8.2f)).boldened());
        g.drawText ("PAD VOLUME", padVolumeSlider.getBounds().translated (0, -12)
                                      .withHeight (14),
                    juce::Justification::centred);

        drawCard (g, channelsCardBounds);
        g.setColour (red);
        g.setFont (juce::Font (juce::FontOptions (8.5f)).boldened());
        g.drawText ("CAMADAS ATIVAS",
                    channelsCardBounds.reduced (12, 4).removeFromTop (16),
                    juce::Justification::centredLeft);
        const std::array<juce::Colour, 4> accents {
            juce::Colour (0xff56c96d), juce::Colour (0xffa46adb),
            juce::Colour (0xffe2a43b), juce::Colour (0xff54bfd0)
        };
        for (int i = 0; i < 4; ++i)
        {
            auto card = channelBounds[(size_t) i];
            if (card.isEmpty()) continue;
            drawCard (g, card);
            const auto& channel = scene.canais[i];
            const auto accent = accents[(size_t) i];
            g.setColour (accent.withAlpha (0.75f));
            g.fillRoundedRectangle (card.toFloat().reduced (1.0f)
                                     .withHeight (2.0f), 2.0f);
            auto c = card.reduced (9, 5);
            g.setColour (text);
            g.setFont (juce::Font (juce::FontOptions (10.5f)).boldened());
            g.drawFittedText (channel.rotuloDoTimbre.isNotEmpty()
                                  ? channel.rotuloDoTimbre
                                  : "Canal " + juce::String (i + 1),
                              c.removeFromTop (18),
                              juce::Justification::centredLeft, 1);
            g.setColour (channel.ligado ? juce::Colour (0xff52d47b) : dim);
            g.setFont (juce::Font (juce::FontOptions (8.0f)).boldened());
            g.drawText (channel.ligado ? "ON" : "OFF",
                        card.getRight() - 36, card.getY() + 7, 27, 13,
                        juce::Justification::centredRight);
            const float normalized = juce::jlimit (0.0f, 1.0f,
                                                    channel.volume / 1.5f);
            auto level = juce::Rectangle<float> ((float) card.getX() + 9.0f,
                (float) card.getBottom() - 11.0f,
                (float) card.getWidth() - 18.0f, 4.0f);
            g.setColour (juce::Colour (0xff252b32));
            g.fillRoundedRectangle (level, 2.0f);
            g.setColour (accent);
            g.fillRoundedRectangle (level.withWidth (level.getWidth() * normalized),
                                    2.0f);
        }

        drawCard (g, performancePadBounds, true);
        auto pad = performancePadBounds.reduced (14, 8);
        g.setColour (red);
        g.setFont (juce::Font (juce::FontOptions (8.5f)).boldened());
        g.drawText (utf8 ("PAD CONT\xc3\x8dNUO"), pad.removeFromTop (18),
                    juce::Justification::centredLeft);
        g.setColour (text);
        g.setFont (juce::Font (juce::FontOptions (16.0f)).boldened());
        static const char* names[12] = { "C", "C#", "D", "D#", "E", "F",
                                         "F#", "G", "G#", "A", "A#", "B" };
        g.drawText (padKey >= 0 ? juce::String (names[padKey]) : "DESLIGADO",
                    pad.removeFromTop (29), juce::Justification::centredLeft);
        g.setColour (dim);
        g.setFont (juce::Font (juce::FontOptions (9.5f)));
        pad.removeFromBottom (38);
        g.drawFittedText (synth.getProgramName (synth.getCurrentProgram()), pad,
                          juce::Justification::centredLeft, 1);

        drawCard (g, performanceActionsBounds);
        g.setColour (red);
        g.setFont (juce::Font (juce::FontOptions (8.5f)).boldened());
        g.drawText (utf8 ("A\xc3\x87\xc3\x95" "ES DE PERFORMANCE"),
                    performanceActionsBounds.reduced (12, 5).removeFromTop (18),
                    juce::Justification::centredLeft);
        g.setColour (dim);
        g.setFont (juce::Font (juce::FontOptions (8.2f)).boldened());
        g.drawText ("TRANSPOSE  " + juce::String (globalTranspose),
                    transposeLabelBounds,
                    juce::Justification::centred);
    }

    void paintSession (juce::Graphics& g)
    {
        g.setColour (text);
        g.setFont (juce::Font (juce::FontOptions (21.0f)).boldened());
        g.drawText (utf8 ("SESS\xc3\x83O / SETLIST"), sessionTitleBounds,
                    juce::Justification::centredLeft);
        g.setColour (dim);
        g.setFont (juce::Font (juce::FontOptions (10.0f)));
        g.drawText (utf8 ("Organize o culto por m\xc3\xba" "sicas e cenas; cada cena guarda um snapshot completo."),
                     sessionSubtitleBounds, juce::Justification::centredLeft);

        drawCard (g, sceneGridBounds);
        g.setColour (red);
        g.setFont (juce::Font (juce::FontOptions (8.5f)).boldened());
        g.drawText ("CENAS DO PROJETO",
                    sceneGridBounds.reduced (12, 5).removeFromTop (17),
                    juce::Justification::centredLeft);
        for (int i = 0; i < ChurchDAWLive::numeroDeCenas; ++i)
        {
            const auto b = sceneTileBounds[(size_t) i];
            drawCard (g, b, i == selectedScene, 6.0f);
            const auto& scene = scenes[(size_t) i];
            g.setColour (i == activeScene ? juce::Colour (0xff52d47b)
                         : i == preparedScene ? juce::Colour (0xffffc247)
                         : scene.preenchida ? red : dim);
            g.fillEllipse ((float) b.getX() + 8.0f,
                           (float) b.getCentreY() - 2.5f, 5.0f, 5.0f);
            auto content = b.reduced (18, 3);
            g.setColour (text);
            g.setFont (juce::Font (juce::FontOptions (9.4f)).boldened());
            const auto tileSong = ! scene.preenchida
                                      && scene.musica.trim().isEmpty()
                                    ? juce::String ("SLOT VAZIO")
                                    : songLabel (scene, i);
            g.drawFittedText (juce::String (i + 1) + "  " + tileSong,
                              content.removeFromTop (juce::jmax (13, content.getHeight() / 2)),
                              juce::Justification::centredLeft, 1);
            g.setColour (dim);
            g.setFont (juce::Font (juce::FontOptions (8.1f)));
            g.drawFittedText (scene.preenchida
                                  ? stageSceneLabel (scene, i)
                                  : "Cena " + juce::String (i + 1),
                              content,
                              juce::Justification::centredLeft, 1);
        }

        drawCard (g, inspectorBounds);
        g.setColour (red);
        g.setFont (juce::Font (juce::FontOptions (8.5f)).boldened());
        g.drawText (validScene (selectedScene)
                        ? "DETALHES DA CENA " + juce::String (selectedScene + 1)
                        : "DETALHES DA CENA",
                    inspectorBounds.reduced (12, 5).removeFromTop (17),
                    juce::Justification::centredLeft);
        const std::array<std::pair<juce::TextEditor*, juce::String>, 8> editors {{
            { &songEditor, utf8 ("M\xc3\x9aSICA") },
            { &nameEditor, "NOME DA CENA" }, { &keyEditor, "TOM" },
            { &bpmEditor, "BPM" }, { &meterEditor, "COMPASSO" },
            { &sectionEditor, utf8 ("SE\xc3\x87\xc3\x83O") },
            { &transitionEditor, utf8 ("TRANSI\xc3\x87\xc3\x83O (S)") },
            { &durationEditor, utf8 ("DURA\xc3\x87\xc3\x83O (S)") }
        }};
        g.setColour (dim);
        g.setFont (juce::Font (juce::FontOptions (7.8f)).boldened());
        for (const auto& item : editors)
            g.drawText (item.second,
                        item.first->getBounds().translated (0, -13).withHeight (12),
                        juce::Justification::centredLeft);

        drawCard (g, setlistBounds, false);
        g.setColour (red.withAlpha (0.78f));
        g.drawRoundedRectangle (setlistBounds.toFloat().reduced (0.5f),
                                8.0f, 1.0f);
        auto heading = setlistBounds.reduced (12, 5).removeFromTop (22);
        g.setColour (red);
        g.setFont (juce::Font (juce::FontOptions (8.5f)).boldened());
        g.drawText ("CULTO / SETLIST", heading,
                    juce::Justification::centredLeft);
        g.setColour (dim);
        g.setFont (juce::Font (juce::FontOptions (8.0f)));
        g.drawText (juce::String (songCount())
                        + utf8 (songCount() == 1 ? " m\xc3\xbasica  \xe2\x80\xa2  "
                                                : " m\xc3\xbasicas  \xe2\x80\xa2  ")
                        + juce::String (setlist.tamanho) + " cenas",
                     heading, juce::Justification::centredRight);

        int songNumber = 0;
        for (int i = 0; i < setlist.tamanho; ++i)
        {
            auto b = setlistRowBounds[(size_t) i];
            if (b.isEmpty()) continue;
            const bool startsSong = i == 0 || ! sameSongAtPositions (i - 1, i);
            if (startsSong) ++songNumber;
            if (i == selectedSetlistPosition)
            {
                g.setColour (juce::Colour (0xff451520));
                g.fillRoundedRectangle (b.toFloat(), 5.0f);
            }
            const int sceneIndex = setlist.ordem[(size_t) i];
            g.setColour (sceneIndex == activeScene ? juce::Colour (0xff52d47b)
                                                   : red);
            g.setFont (juce::Font (juce::FontOptions (9.0f)).boldened());
            g.drawText (startsSong
                            ? juce::String (songNumber).paddedLeft ('0', 2)
                            : juce::String::fromUTF8 ("\xe2\x86\xb3"),
                        b.removeFromLeft (29), juce::Justification::centred);
            auto copy = b.reduced (4, 3);
            if (startsSong)
            {
                g.setColour (red);
                g.setFont (juce::Font (juce::FontOptions (8.1f)).boldened());
                g.drawFittedText (songLabel (scenes[(size_t) sceneIndex], sceneIndex)
                                      .toUpperCase(),
                                  copy.removeFromTop (13),
                                  juce::Justification::centredLeft, 1);
            }
            g.setColour (text);
            g.setFont (juce::Font (juce::FontOptions (9.8f)).boldened());
            g.drawFittedText (stageSceneLabel (scenes[(size_t) sceneIndex], sceneIndex),
                              copy, juce::Justification::centredLeft, 1);
        }

        if (setlist.vazio())
        {
            auto empty = setlistBounds.reduced (24, 64);
            empty.removeFromBottom (72);
            g.setColour (juce::Colours::white.withAlpha (0.11f));
            g.drawRoundedRectangle (empty.toFloat(), 7.0f, 1.0f);
            g.setColour (text.withAlpha (0.74f));
            g.setFont (juce::Font (juce::FontOptions (12.0f)).boldened());
            g.drawText ("SETLIST VAZIA", empty.removeFromTop (28),
                        juce::Justification::centredBottom);
            g.setColour (dim);
            g.setFont (juce::Font (juce::FontOptions (9.5f)));
            g.drawFittedText (
                utf8 ("Defina a m\xc3\xba" "sica da cena e use + SETLIST para montar o culto."),
                empty.reduced (14, 6), juce::Justification::centred, 3);
        }

        double duration = 0.0;
        for (int i = 0; i < setlist.tamanho; ++i)
            duration += scenes[(size_t) setlist.ordem[(size_t) i]].duracaoSegundos;
        const int minutes = juce::roundToInt (duration) / 60;
        const int seconds = juce::roundToInt (duration) % 60;
        g.setColour (dim);
        g.setFont (juce::Font (juce::FontOptions (8.5f)));
        g.drawText (utf8 ("DURA\xc3\x87\xc3\x83O TOTAL  ")
                        + juce::String (minutes) + ":"
                        + juce::String (seconds).paddedLeft ('0', 2),
                    setlistFooterTextBounds,
                    juce::Justification::centredLeft);
    }

    void layoutPerformance()
    {
        auto area = getLocalBounds().reduced (10, 9);
        const int gap = 9;
        auto right = area.removeFromRight (juce::jlimit (205, 250,
            juce::roundToInt ((float) area.getWidth() * 0.23f)));
        area.removeFromRight (gap);

        const int heroHeight = juce::jlimit (110, 142,
                                             area.getHeight() * 30 / 100);
        heroBounds = area.removeFromTop (heroHeight);
        area.removeFromTop (gap);

        auto heroLayout = heroBounds;
        auto nav = heroLayout.removeFromBottom (45).reduced (9, 2);
        const int navGap = 7;
        const int navW = (nav.getWidth() - navGap * 2) / 3;
        previousSceneBounds = nav.removeFromLeft (navW);
        nav.removeFromLeft (navGap);
        currentSceneBounds = nav.removeFromLeft (navW);
        nav.removeFromLeft (navGap);
        nextSceneBounds = nav;
        // Os tres botoes transparentes ocupam os cards desenhados acima.
        previousButton.setBounds ({});
        nextButton.setBounds ({});

        const int macroHeight = juce::jlimit (118, 158,
                                              area.getHeight() * 56 / 100);
        macroCardBounds = area.removeFromTop (juce::jmin (macroHeight,
                                                          area.getHeight()));
        area.removeFromTop (gap);
        channelsCardBounds = area;

        auto macros = macroCardBounds.reduced (10, 22);
        const int sliderW = macros.getWidth() / 5;
        for (int i = 0; i < 4; ++i)
            macroSliders[(size_t) i].setBounds (
                macros.removeFromLeft (sliderW).reduced (4, 2));
        padVolumeSlider.setBounds (macros.reduced (7, 3));

        auto channels = channelsCardBounds.reduced (8, 19);
        const int channelGap = 6;
        const int channelW = (channels.getWidth() - channelGap * 3) / 4;
        for (int i = 0; i < 4; ++i)
        {
            channelBounds[(size_t) i] = channels.removeFromLeft (channelW);
            if (i < 3) channels.removeFromLeft (channelGap);
        }

        performancePadBounds = right.removeFromTop (
            juce::jlimit (92, 128, right.getHeight() * 29 / 100));
        right.removeFromTop (gap);
        performanceActionsBounds = right;
        auto padActions = performancePadBounds.reduced (12, 7);
        playbackButton.setBounds (padActions.removeFromTop (27));
        padActions.removeFromTop (4);
        autoPatchButton.setBounds (padActions.removeFromTop (27));
        padActions.removeFromTop (4);
        quickPadButton.setBounds (padActions.removeFromTop (27));

        auto actions = performanceActionsBounds.reduced (10, 24);
        const int actionGap = 7;
        const int actionH = juce::jmax (28,
            (actions.getHeight() - actionGap * 2) / 3);
        auto row1 = actions.removeFromTop (actionH);
        actions.removeFromTop (actionGap);
        auto row2 = actions.removeFromTop (actionH);
        actions.removeFromTop (actionGap);
        auto row3 = actions;
        const int half = (row1.getWidth() - actionGap) / 2;
        panicButton.setBounds (row1.removeFromLeft (half));
        row1.removeFromLeft (actionGap);
        tapButton.setBounds (row1);
        transposeLabelBounds = row2.removeFromTop (18);
        row2.removeFromTop (3);
        transposeDownButton.setBounds (row2.removeFromLeft (half));
        row2.removeFromLeft (actionGap);
        transposeUpButton.setBounds (row2);
        const int third = (row3.getWidth() - actionGap * 2) / 3;
        sustainButton.setBounds (row3.removeFromLeft (third));
        row3.removeFromLeft (actionGap);
        emergencyPianoButton.setBounds (row3.removeFromLeft (third));
        row3.removeFromLeft (actionGap);
        stageTestButton.setBounds (row3);
    }

    void layoutSession()
    {
        auto area = getLocalBounds().reduced (12, 8);
        sessionTitleBounds = area.removeFromTop (28);
        sessionSubtitleBounds = area.removeFromTop (20);
        area.removeFromTop (5);
        const int gap = 10;
        setlistBounds = area.removeFromRight (juce::jlimit (
            230, 300, juce::roundToInt ((float) area.getWidth() * 0.28f)));
        area.removeFromRight (gap);

        sceneGridBounds = area.removeFromTop (
            juce::jlimit (100, 130, area.getHeight() * 30 / 100));
        area.removeFromTop (gap);
        inspectorBounds = area;

        auto grid = sceneGridBounds.reduced (9, 23);
        const int tileGap = 5;
        const int tileW = (grid.getWidth() - tileGap * 4) / 5;
        const int tileH = (grid.getHeight() - tileGap) / 2;
        for (int i = 0; i < ChurchDAWLive::numeroDeCenas; ++i)
        {
            const int row = i / 5;
            const int col = i % 5;
            sceneTileBounds[(size_t) i] = {
                grid.getX() + col * (tileW + tileGap),
                grid.getY() + row * (tileH + tileGap), tileW, tileH };
        }

        auto inspector = inspectorBounds.reduced (10, 24);
        auto buttonRow = inspector.removeFromBottom (34);
        inspector.removeFromBottom (7);
        const int rowGap = 7;
        const int editorH = juce::jmax (24,
            (inspector.getHeight() - rowGap * 2) / 3);
        auto row1 = inspector.removeFromTop (editorH);
        inspector.removeFromTop (rowGap);
        auto row2 = inspector.removeFromTop (editorH);
        inspector.removeFromTop (rowGap);
        auto row3 = inspector;

        auto split = [] (juce::Rectangle<int> row, int firstPercent)
        {
            const int first = row.getWidth() * firstPercent / 100;
            auto left = row.removeFromLeft (first - 4);
            row.removeFromLeft (8);
            return std::pair { left, row };
        };
        auto r1 = split (row1, 56);
        songEditor.setBounds (r1.first.withTrimmedTop (11));
        nameEditor.setBounds (r1.second.withTrimmedTop (11));

        auto section = row2.removeFromLeft (row2.getWidth() * 34 / 100);
        row2.removeFromLeft (7);
        sectionEditor.setBounds (section.withTrimmedTop (11));
        const int compactGap = 7;
        const int compactWidth = (row2.getWidth() - compactGap * 2) / 3;
        keyEditor.setBounds (row2.removeFromLeft (compactWidth).withTrimmedTop (11));
        row2.removeFromLeft (compactGap);
        bpmEditor.setBounds (row2.removeFromLeft (compactWidth).withTrimmedTop (11));
        row2.removeFromLeft (compactGap);
        meterEditor.setBounds (row2.withTrimmedTop (11));
        auto r3 = split (row3, 50);
        transitionEditor.setBounds (r3.first.withTrimmedTop (11));
        durationEditor.setBounds (r3.second.withTrimmedTop (11));

        const int bgap = 6;
        saveMetadataButton.setBounds (buttonRow.removeFromRight (135));
        buttonRow.removeFromRight (bgap);
        importSessionButton.setBounds (buttonRow.removeFromRight (112));
        buttonRow.removeFromRight (bgap);
        exportSessionButton.setBounds (buttonRow.removeFromRight (112));
        buttonRow.removeFromRight (bgap);
        redoButton.setBounds (buttonRow.removeFromRight (78));
        buttonRow.removeFromRight (bgap);
        undoButton.setBounds (buttonRow.removeFromRight (82));
        buttonRow.removeFromRight (bgap);
        deleteButton.setBounds (buttonRow.removeFromRight (76));
        buttonRow.removeFromRight (bgap);
        captureButton.setBounds (buttonRow.removeFromRight (142));
        buttonRow.removeFromRight (bgap);
        loadButton.setBounds (buttonRow.removeFromRight (122));
        addSetlistButton.setBounds (buttonRow.removeFromLeft (
            juce::jmin (102, buttonRow.getWidth())));

        auto list = setlistBounds.reduced (10, 31);
        setlistNameEditor.setBounds (list.removeFromTop (29));
        list.removeFromTop (6);
        auto controls = list.removeFromBottom (34);
        setlistFooterTextBounds = list.removeFromBottom (27);
        list.removeFromBottom (5);
        const int rowHeight = juce::jmax (28, juce::jmin (46,
            list.getHeight() / juce::jmax (1, setlist.tamanho)));
        setlistRowBounds.fill (juce::Rectangle<int>{});
        for (int i = 0; i < setlist.tamanho; ++i)
            setlistRowBounds[(size_t) i] = list.removeFromTop (rowHeight);

        moveDownButton.setBounds (controls.removeFromRight (34));
        controls.removeFromRight (5);
        moveUpButton.setBounds (controls.removeFromRight (34));
        controls.removeFromRight (7);
        removeSetlistButton.setBounds (controls);
    }

    PadSynthProcessor& synth;
    Mode mode = Mode::performance;
    std::array<ChurchDAWLive::Cena, ChurchDAWLive::numeroDeCenas> scenes;
    ChurchDAWLive::Setlist setlist;
    int activeScene = -1;
    int preparedScene = -1;
    int selectedScene = -1;
    int selectedSetlistPosition = -1;
    int editorScene = -1;
    int padKey = -1;
    bool sustain = false;
    int globalTranspose = 0;
    bool updatingControls = false;
    juce::uint32 lastTapMs = 0;

    juce::TextButton previousButton, nextButton, panicButton, tapButton;
    juce::TextButton quickPadButton, transposeDownButton, transposeUpButton;
    juce::TextButton sustainButton;
    juce::TextButton loadButton, captureButton, deleteButton, emergencyPianoButton,
        stageTestButton, autoPatchButton, playbackButton;
    juce::TextButton saveMetadataButton, exportSessionButton, importSessionButton,
        undoButton, redoButton, addSetlistButton;
    juce::TextButton removeSetlistButton, moveUpButton, moveDownButton;
    std::array<juce::Slider, 4> macroSliders;
    std::array<std::unique_ptr<juce::SliderParameterAttachment>, 4>
        macroAttachments;
    juce::Slider padVolumeSlider;
    juce::TextEditor songEditor, nameEditor, keyEditor, bpmEditor, meterEditor;
    juce::TextEditor sectionEditor, transitionEditor, durationEditor;
    juce::TextEditor setlistNameEditor;

    juce::Rectangle<int> heroBounds, previousSceneBounds, currentSceneBounds;
    juce::Rectangle<int> nextSceneBounds, macroCardBounds, channelsCardBounds;
    juce::Rectangle<int> performancePadBounds, performanceActionsBounds;
    juce::Rectangle<int> transposeLabelBounds;
    std::array<juce::Rectangle<int>, 4> channelBounds;
    juce::Rectangle<int> sessionTitleBounds, sessionSubtitleBounds;
    juce::Rectangle<int> sceneGridBounds, inspectorBounds, setlistBounds;
    juce::Rectangle<int> setlistFooterTextBounds;
    std::array<juce::Rectangle<int>, ChurchDAWLive::numeroDeCenas> sceneTileBounds;
    std::array<juce::Rectangle<int>, ChurchDAWLive::Setlist::capacidadeMaxima>
        setlistRowBounds;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PerformanceSessionPanel)
};
