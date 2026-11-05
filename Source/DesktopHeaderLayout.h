#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// Trilho de navegacao a esquerda (como no Android) em vez das abas no topo, na
// janela de referencia (>= 900x580). Defina CHURCHDAW_DESKTOP_RAIL=0 para voltar
// as abas.
#ifndef CHURCHDAW_DESKTOP_RAIL
 #define CHURCHDAW_DESKTOP_RAIL 1
#endif

// The desktop header allocates every region before placing components. This
// keeps device names readable without borrowing space from navigation.
struct DesktopHeaderLayout
{
    juce::Rectangle<int> brand, devices, engine, save, navigation;
    static constexpr int height = 100;
    static constexpr int railHeaderHeight = 56;   // uma fileira so: a navegacao sai para o trilho
    static int railWidth (int) noexcept { return 84; }
    // O mixer so mantem o layout de referencia com >= 860 px de largura propria;
    // por isso o trilho so entra quando a janela comporta os 900 px de antes MAIS o
    // trilho. Abaixo disso as abas do topo continuam como eram.
    static bool supportsRail (juce::Rectangle<int> bounds) noexcept
    {
        return supports (bounds) && bounds.getWidth() >= 900 + railWidth (bounds.getWidth());
    }

    static bool supports (juce::Rectangle<int> bounds) noexcept
    {
        return bounds.getWidth() >= 900 && bounds.getHeight() >= 580;
    }

    static DesktopHeaderLayout calculate (juce::Rectangle<int> bounds)
    {
        DesktopHeaderLayout result;
        auto top = bounds.withHeight (56).reduced (18, 4);
        const bool compact = bounds.getWidth() < 1100;
        result.brand = top.removeFromLeft (compact ? 164 : 184);
        top.removeFromLeft (compact ? 12 : 18);
        result.save = top.removeFromRight (compact ? 86 : 92).reduced (0, 5);
        top.removeFromRight (compact ? 10 : 12);
        result.engine = top.removeFromRight (compact ? 208 : 230);
        top.removeFromRight (compact ? 8 : 10);
        result.devices = top;
        result.navigation = { bounds.getX() + 18, bounds.getY() + 58,
                              juce::jmax (0, bounds.getWidth() - 36), 38 };
        return result;
    }
    // Mesma fileira de marca/dispositivos/engine/salvar, sem a fileira de abas.
    static DesktopHeaderLayout calculateRail (juce::Rectangle<int> bounds)
    {
        auto result = calculate (bounds);
        result.navigation = {};
        return result;
    }
};
