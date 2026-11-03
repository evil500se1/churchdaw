#pragma once

// Fonte unica do que precisa ser recolhido quando uma strip sai de cena.
// Antes eram duas listas identicas (esconderStrip e Modo Palco) que saiam de
// sincronia a cada botao novo.
//
// Dependencia minima: so juce_gui_basics. Todo membro listado deriva de
// juce::Component, entao o alvo de teste nao precisa linkar o app inteiro.

#include <juce_gui_basics/juce_gui_basics.h>

#include <vector>

// flushBtn, zoneEditBtn, pluginSlotBtn e pluginMenuBtn entraram junto com o
// clearBtn por causa do redesign de console: sao os controles novos que a
// strip ganhou e que nenhuma das duas listas conhecia. Sem eles aqui, o bounds
// antigo sobrevivia ao recolhimento e o botao voltava a aparecer "fantasma"
// desenhado sobre o card ja recolhido — e ainda roubava o clique de quem
// estava embaixo. Quem adicionar um controle novo na strip TEM que adicionar
// aqui; e o unico lugar.
template <typename StripT>
std::vector<juce::Component*> churchDAWStripCollapseList (StripT& s)
{
    return { &s.name,         &s.timbreName,    &s.transpLabel,
             &s.octaveLabel,  &s.volDb,         &s.glyph,
             &s.activity,     &s.combo,         &s.midiInBox,
             &s.midiChBox,    &s.zoneBox,       &s.vol,
             &s.transp,       &s.octave,        &s.muteBtn,
             &s.soloBtn,      &s.onBtn,         &s.autoEqBtn,
             &s.tonePageBtn,  &s.pluginOpenBtn, &s.addButton,
             &s.clearBtn,     &s.flushBtn,      &s.zoneEditBtn,
             &s.pluginSlotBtn,&s.pluginMenuBtn, &s.rotaryModeBox,
             &s.compKnob,     &s.dlyKnob,       &s.echoKnob,
             &s.rotaryKnob,   &s.attackKnob,    &s.releaseKnob,
             &s.lowKnob,      &s.midKnob,       &s.highKnob,
             &s.compLabel,    &s.dlyLabel,      &s.echoLabel,
             &s.rotaryLabel,  &s.attackLabel,   &s.releaseLabel,
             &s.lowLabel,     &s.midLabel,      &s.highLabel };
}
