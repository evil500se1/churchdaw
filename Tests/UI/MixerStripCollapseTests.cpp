// Regressao da fonte unica de recolhimento da strip.
//
// Motivacao real: esconderStrip e o Modo Palco (layoutFocusedMobile) mantinham
// CADA UM a sua copia da lista de controles a zerar. Um botao novo esquecido
// numa das duas voltava a aparecer "fantasma" sobre o card recolhido. A lista
// agora vive em Source/MixerStripCollapse.h e este teste trava o contrato.

#include "MixerStripCollapse.h"

#include <algorithm>
#include <iostream>
#include <vector>

namespace
{
int failures = 0;

void expect (bool condition, const char* message)
{
    if (condition)
        return;

    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

// Mock de Strip: churchDAWStripCollapseList so precisa de enderecos
// convertiveis para juce::Component*, entao basta que os 45 membros listados
// existam como juce::Component. Nenhum estado de UI e necessario.
struct StripMock
{
    juce::Component name;
    juce::Component timbreName;
    juce::Component transpLabel;
    juce::Component octaveLabel;
    juce::Component volDb;
    juce::Component glyph;
    juce::Component activity;
    juce::Component combo;
    juce::Component midiInBox;
    juce::Component midiChBox;
    juce::Component zoneBox;
    juce::Component vol;
    juce::Component transp;
    juce::Component octave;
    juce::Component muteBtn;
    juce::Component soloBtn;
    juce::Component onBtn;
    juce::Component autoEqBtn;
    juce::Component tonePageBtn;
    juce::Component pluginOpenBtn;
    juce::Component addButton;
    juce::Component clearBtn;
    juce::Component flushBtn;
    juce::Component zoneEditBtn;
    juce::Component pluginSlotBtn;
    juce::Component pluginMenuBtn;
    juce::Component rotaryModeBox;
    juce::Component compKnob;
    juce::Component dlyKnob;
    juce::Component echoKnob;
    juce::Component rotaryKnob;
    juce::Component attackKnob;
    juce::Component releaseKnob;
    juce::Component lowKnob;
    juce::Component midKnob;
    juce::Component highKnob;
    juce::Component compLabel;
    juce::Component dlyLabel;
    juce::Component echoLabel;
    juce::Component rotaryLabel;
    juce::Component attackLabel;
    juce::Component releaseLabel;
    juce::Component lowLabel;
    juce::Component midLabel;
    juce::Component highLabel;
};

// Contrato: 45 controles. Se um botao novo entrar na strip e na lista, este
// numero muda — e a intencao e que mude AQUI, num unico lugar, e nao em duas
// listas que saem de sincronia silenciosamente.
constexpr size_t expectedCollapseCount = 45;

bool contem (const std::vector<juce::Component*>& lista, juce::Component* alvo)
{
    return std::find (lista.begin(), lista.end(), alvo) != lista.end();
}
} // namespace

int runMidiLearnOverlayTests();
int runTimbrePickerLayoutTests();
int runLevelMeterSkinTests();
int runPluginGalleryLayoutTests();
int runFullUpgradeTests();
int runPluginPreviewLifecycleTests (bool legacy);
int runPluginCandidateSearchTests (bool legacy);

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceGui;
    if (argc > 1 && std::string (argv[1]) == "--plugin-candidate-search") return runPluginCandidateSearchTests (false);
    if (argc > 1 && std::string (argv[1]) == "--legacy-plugin-candidate-search") return runPluginCandidateSearchTests (true);
    if (argc > 1 && std::string (argv[1]) == "--plugin-preview") return runPluginPreviewLifecycleTests (false);
    if (argc > 1 && std::string (argv[1]) == "--legacy-plugin-preview") return runPluginPreviewLifecycleTests (true);
    if (argc > 1 && std::string (argv[1]) == "--full-upgrade") return runFullUpgradeTests();
    if (argc > 1 && std::string (argv[1]) == "--midi-learn")
        return runMidiLearnOverlayTests();
    if (argc > 1 && std::string (argv[1]) == "--plugin-gallery")
        return runPluginGalleryLayoutTests();
    if (argc > 1 && std::string (argv[1]) == "--timbre-picker")
        return runTimbrePickerLayoutTests();
    if (argc > 1 && std::string (argv[1]) == "--level-meter-skin")
        return runLevelMeterSkinTests();

    StripMock strip;
    const auto collapse = churchDAWStripCollapseList (strip);

    // (a) a lista tem exatamente os 45 controles.
    expect (collapse.size() == expectedCollapseCount,
            "collapse list does not have the expected number of controls");

    // (b) os quatro botoes do redesign de console estao la — e exatamente a
    // regressao que motivou a fonte unica (controle fantasma sobre o card
    // recolhido). clearBtn entrou na mesma leva.
    expect (contem (collapse, &strip.flushBtn),
            "flushBtn is missing from the collapse list");
    expect (contem (collapse, &strip.zoneEditBtn),
            "zoneEditBtn is missing from the collapse list");
    expect (contem (collapse, &strip.pluginSlotBtn),
            "pluginSlotBtn is missing from the collapse list");
    expect (contem (collapse, &strip.pluginMenuBtn),
            "pluginMenuBtn is missing from the collapse list");
    expect (contem (collapse, &strip.clearBtn),
            "clearBtn is missing from the collapse list");

    // (c) nenhum nullptr: um membro que nao existe mais viraria ponteiro nulo
    // e o laço de setBounds({}) explodiria em runtime.
    expect (std::find (collapse.begin(), collapse.end(), nullptr)
                == collapse.end(),
            "collapse list contains a null pointer");

    // (d) sem duplicatas: duplicata significa membro errado na lista (copia e
    // cola) e, pior, um controle real ficou de fora.
    std::vector<juce::Component*> ordenada = collapse;
    std::sort (ordenada.begin(), ordenada.end());
    expect (std::adjacent_find (ordenada.begin(), ordenada.end()) == ordenada.end(),
            "collapse list contains duplicated controls");

    // Fecho: todo controle do mock tem que estar na lista. E o caso que pega
    // quem adicionar botao novo na strip e esquecer a lista.
    const std::vector<juce::Component*> esperados {
        &strip.name,         &strip.timbreName,    &strip.transpLabel,
        &strip.octaveLabel,  &strip.volDb,         &strip.glyph,
        &strip.activity,     &strip.combo,         &strip.midiInBox,
        &strip.midiChBox,    &strip.zoneBox,       &strip.vol,
        &strip.transp,       &strip.octave,        &strip.muteBtn,
        &strip.soloBtn,      &strip.onBtn,         &strip.autoEqBtn,
        &strip.tonePageBtn,  &strip.pluginOpenBtn, &strip.addButton,
        &strip.clearBtn,     &strip.flushBtn,      &strip.zoneEditBtn,
        &strip.pluginSlotBtn,&strip.pluginMenuBtn, &strip.rotaryModeBox,
        &strip.compKnob,     &strip.dlyKnob,       &strip.echoKnob,
        &strip.rotaryKnob,   &strip.attackKnob,    &strip.releaseKnob,
        &strip.lowKnob,      &strip.midKnob,       &strip.highKnob,
        &strip.compLabel,    &strip.dlyLabel,      &strip.echoLabel,
        &strip.rotaryLabel,  &strip.attackLabel,   &strip.releaseLabel,
        &strip.lowLabel,     &strip.midLabel,      &strip.highLabel
    };

    expect (esperados.size() == expectedCollapseCount,
            "mock and expected count drifted apart");

    for (auto* c : esperados)
        expect (contem (collapse, c),
                "a strip control is not covered by the collapse list");

    if (failures != 0)
        return 1;

    std::cout << "MIXER_STRIP_COLLAPSE_OK\n";
    return 0;
}
