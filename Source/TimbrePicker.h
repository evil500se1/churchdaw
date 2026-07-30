#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "NordLookAndFeel.h"

#include <functional>

//==============================================================================
// ComboBox que delega a abertura do menu. Serve pra trocar o popup padrao do
// JUCE (que nao tem busca) pelo seletor abaixo, sem perder nada do resto do
// comportamento do ComboBox.
//==============================================================================
class PickerComboBox : public juce::ComboBox
{
public:
    PickerComboBox() = default;

    std::function<void()> onShowPopup;

    // O ComboBox marca "menu ativo" ANTES de chamar showPopup() e so limpa quando
    // o popup DELE e dispensado — com o popup nativo desligado, quem tem de limpar
    // a flag e o dono do seletor, chamando hidePopup() quando o card fecha.
    //
    // Nao limpar aqui dentro e proposital: mouseDown E mouseUp chamam
    // showPopupIfNotActive(), entao zerar a flag no meio faria o clique abrir e
    // fechar o seletor no mesmo movimento.
    void showPopup() override
    {
        if (onShowPopup != nullptr)
            onShowPopup();
        else
            juce::ComboBox::showPopup();
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PickerComboBox)
};

//==============================================================================
// Seletor de timbre COM BUSCA — redesenho "biblioteca premium" (spec do dono,
// 30/07/2026): header com o contexto do canal, busca com lupa e limpar, chips
// de categoria, itens com subtitulo e cor por familia, timbre atual marcado
// como "Selecionado", acao de remover DISCRETA no rodape (a barra vermelha
// gigante "Canal vazio" dominava a tela como se fosse um alerta).
//==============================================================================
class TimbrePicker : public juce::Component,
                     private juce::ListBoxModel
{
public:
    struct Item
    {
        int id = 0;
        juce::String name;      // nome ja apresentavel
        juce::String section;   // biblioteca / grupo
        int kind = 0;           // TimbreGlyph::Kind (int pra nao criar dependencia circular)
        juce::String category;  // chave da familia ("PIANOS", "ORGAOS", ...)
        juce::String subtitle;  // "Piano acustico · Church Live"
    };

    std::function<void (int)> onPick;
    std::function<void()> onCancel;

    // quem cria o picker diz como desenhar o icone (o glyph vive em outro header)
    std::function<void (juce::Graphics&, juce::Rectangle<float>, int)> drawIcon;
    // cor da familia para o tile do icone e acentos (mesma paleta dos cards)
    std::function<juce::Colour (int)> colourForKind;

    TimbrePicker (juce::Array<Item> allItems, int currentId)
        : items (std::move (allItems)), currentSelectedId (currentId)
    {
        search.setTextToShowWhenEmpty (
            juce::String::fromUTF8 ("Buscar timbre, \xc3\xb3rg\xc3\xa3o, pad ou plugin..."),
            NordLookAndFeel::dimText().withAlpha (0.65f));
        search.setFont (juce::Font (juce::FontOptions (15.0f)));
        // campo premium: vidro escuro proprio, contorno sutil (nao preto chapado)
        search.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff14171c));
        search.setColour (juce::TextEditor::outlineColourId, juce::Colours::white.withAlpha (0.10f));
        search.setColour (juce::TextEditor::focusedOutlineColourId, NordLookAndFeel::nordRedBright().withAlpha (0.65f));
        search.setColour (juce::TextEditor::textColourId, NordLookAndFeel::textColour());
        search.setColour (juce::TextEditor::highlightColourId, NordLookAndFeel::nordRed().withAlpha (0.5f));
        search.setIndents (30, 4);   // abre espaco para a lupa desenhada no paint
        search.onTextChange = [this] { clearBtn.setVisible (search.getText().isNotEmpty()); rebuildRows(); };
        search.onEscapeKey  = [this] { if (onCancel != nullptr) onCancel(); };
        search.onReturnKey  = [this] { pickRow (list.getSelectedRow()); };
        search.onArrow      = [this] (int delta) { moveSelection (delta); };
        addAndMakeVisible (search);

        clearBtn.setButtonText (juce::String::fromUTF8 ("\xc3\x97"));
        clearBtn.setTooltip ("Limpar busca");
        clearBtn.setColour (juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
        clearBtn.setColour (juce::TextButton::textColourOffId, NordLookAndFeel::dimText());
        clearBtn.onClick = [this] { search.setText ({}, juce::sendNotification); search.grabKeyboardFocus(); };
        addChildComponent (clearBtn);   // so aparece quando ha texto

        // chips de categoria: filtro em um toque, reconhecimento por familia
        for (int i = 0; i < numCategories; ++i)
        {
            auto* chip = new juce::TextButton (categoryLabel (i));
            chip->setClickingTogglesState (false);
            chip->setColour (juce::TextButton::buttonColourId, juce::Colour (0xff20252d));
            chip->setColour (juce::TextButton::buttonOnColourId, NordLookAndFeel::nordRed().withAlpha (0.85f));
            chip->setColour (juce::TextButton::textColourOffId, NordLookAndFeel::txtSecondary());
            chip->setColour (juce::TextButton::textColourOnId, juce::Colours::white);
            chip->onClick = [this, i]
            {
                activeCategory = (activeCategory == i ? 0 : i);
                for (int c = 0; c < numCategories; ++c)
                    chips[c]->setToggleState (c == activeCategory, juce::dontSendNotification);
                rebuildRows();
            };
            chip->setToggleState (i == 0, juce::dontSendNotification);
            addAndMakeVisible (chip);
            chips.add (chip);
        }

        list.setModel (this);
        list.setRowHeight (26);
        list.setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
        list.setOutlineThickness (0);
        list.onMove = [this] (int delta) { moveSelection (delta); };
        list.onChoose = [this] { pickRow (list.getSelectedRow()); };
        list.onCancel = [this] { if (onCancel != nullptr) onCancel(); };
        list.setWantsKeyboardFocus (false);
        addAndMakeVisible (list);

        closeBtn.setButtonText (juce::String::fromUTF8 ("\xc3\x97"));   // ×
        closeBtn.setTooltip ("Fechar seletor de instrumento");
        closeBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2a2d34));
        closeBtn.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
        closeBtn.onClick = [this] { if (onCancel != nullptr) onCancel(); };
        addAndMakeVisible (closeBtn);

        // Acao destrutiva DISCRETA: substitui a antiga barra vermelha gigante
        // "Canal vazio / remover instrumento" que dominava a lista.
        removeBtn.setButtonText ("Remover do canal");
        removeBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1c2027));
        removeBtn.setColour (juce::TextButton::textColourOffId,
                             NordLookAndFeel::nordRedBright().withAlpha (0.85f));
        removeBtn.onClick = [this] { if (onPick != nullptr) onPick (1); };
        removeBtn.setVisible (currentSelectedId >= 2);
        addAndMakeVisible (removeBtn);

        setSize (440 + margin * 2, 486 + margin * 2);
        rebuildRows();
    }

    void useTouchLayout (bool shouldUseTouch)
    {
        touchLayout = shouldUseTouch;
        // 64 dp: nome e subtitulo em zonas proprias com respiro — com 58 os
        // descendentes do nome encostavam no subtitulo (apontado pelo dono).
        list.setRowHeight (touchLayout ? 64 : 26);
        list.setWantsKeyboardFocus (touchLayout);
        search.setFont (juce::Font (juce::FontOptions (touchLayout ? 17.0f : 15.0f)));
        resized();
        repaint();
    }

    // contexto do canal para o header ("CANAL 1" + timbre atual)
    void setContext (juce::String channelBadgeToUse, juce::String currentNameToUse)
    {
        channelBadge = std::move (channelBadgeToUse);
        currentName = std::move (currentNameToUse);
        repaint();
    }

    // grabKeyboardFocus dentro de visibilityChanged() nao pega: o componente
    // ainda nao esta na tela. O foco e pedido depois, ja montado.
    void focusSearch()
    {
        // No celular, o foco inicial vai para a LISTA: D-pad/setas/Enter passam
        // a funcionar sem abrir o teclado virtual e sem cobrir os timbres.
       #if JUCE_ANDROID || JUCE_IOS
        list.grabKeyboardFocus();
       #else
        search.grabKeyboardFocus();
       #endif
    }

    void paint (juce::Graphics& g) override
    {
        auto card = getLocalBounds().toFloat().reduced ((float) margin);
        const float rad = 14.0f;

        {
            juce::Path sp;
            sp.addRoundedRectangle (card, rad);
            juce::DropShadow (juce::Colours::black.withAlpha (0.6f), 28, { 0, 8 }).drawForPath (g, sp);
        }

        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff1a1f28), card.getX(), card.getY(),
                                                 juce::Colour (0xff11141b), card.getX(), card.getBottom(), false));
        g.fillRoundedRectangle (card, rad);
        g.setColour (juce::Colours::white.withAlpha (0.06f));
        g.drawLine (card.getX() + 14.0f, card.getY() + 1.5f, card.getRight() - 14.0f, card.getY() + 1.5f, 1.0f);
        // moldura NEUTRA: o vermelho fica para estados, nao para a moldura
        g.setColour (juce::Colours::white.withAlpha (0.10f));
        g.drawRoundedRectangle (card, rad, 1.2f);

        g.setColour (NordLookAndFeel::nordRed());
        g.fillRoundedRectangle (card.reduced (16.0f, 0.0f).removeFromTop (8.0f).withTrimmedTop (6.0f), 1.0f);

        // ---- header: titulo + badge do canal + timbre atual ----
        auto head = card.toNearestInt().reduced (20, 0)
                        .withTop (card.toNearestInt().getY() + 14)
                        .withHeight (touchLayout ? 26 : 22);
        g.setColour (juce::Colours::white);
        g.setFont (juce::Font (juce::FontOptions (touchLayout ? 15.0f : 14.0f))
                       .boldened().withExtraKerningFactor (0.10f));
        const juce::String title ("ESCOLHER INSTRUMENTO");
        g.drawText (title, head, juce::Justification::centredLeft);

        if (channelBadge.isNotEmpty())
        {
            const float titleW = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), title);
            auto badgeFont = NordLookAndFeel::stageLabel (touchLayout ? 11.0f : 10.0f);
            const float badgeW = juce::GlyphArrangement::getStringWidth (badgeFont, channelBadge) + 18.0f;
            auto badge = juce::Rectangle<float> ((float) head.getX() + titleW + 14.0f,
                                                 (float) head.getCentreY() - 10.0f,
                                                 badgeW, 20.0f);
            g.setColour (NordLookAndFeel::nordRed());
            g.fillRoundedRectangle (badge, 10.0f);
            g.setColour (juce::Colours::white);
            g.setFont (badgeFont);
            g.drawText (channelBadge, badge.toNearestInt(), juce::Justification::centred, false);

            if (currentName.isNotEmpty())
            {
                g.setColour (NordLookAndFeel::txtSecondary());
                g.setFont (juce::Font (juce::FontOptions (touchLayout ? 13.0f : 12.0f)));
                g.drawText (currentName,
                            juce::Rectangle<int> ((int) badge.getRight() + 10, head.getY(),
                                                  juce::jmax (10, (int) card.getRight() - 76
                                                                   - ((int) badge.getRight() + 10)),
                                                  head.getHeight()),
                            juce::Justification::centredLeft, true);
            }
        }

        // ---- estado vazio elegante ----
        if (rows.isEmpty() && ! list.getBounds().isEmpty())
        {
            auto empty = list.getBounds();
            g.setColour (NordLookAndFeel::dimText().withAlpha (0.8f));
            g.setFont (juce::Font (juce::FontOptions (15.0f)).boldened());
            g.drawText ("Nenhum timbre encontrado",
                        empty.removeFromTop (empty.getHeight() / 2).withTrimmedTop (empty.getHeight() / 3),
                        juce::Justification::centredBottom);
            g.setColour (NordLookAndFeel::dimText().withAlpha (0.55f));
            g.setFont (juce::Font (juce::FontOptions (12.5f)));
            g.drawText ("Tente outro nome ou escolha outra categoria",
                        empty.withTrimmedTop (6), juce::Justification::centredTop);
        }

        // ---- rodape ----
        auto foot = card.toNearestInt().reduced (20, 0).withHeight (16);
        foot.setY (card.toNearestInt().getBottom() - 26);
        g.setColour (NordLookAndFeel::dimText().withAlpha (0.55f));
        g.setFont (juce::Font (juce::FontOptions (11.0f)));

        if (! touchLayout)
            g.drawText (juce::String::fromUTF8 ("Enter escolhe  \xc2\xb7  setas navegam  \xc2\xb7  Esc fecha"),
                        foot, juce::Justification::centredLeft);

        g.drawText (juce::String (countPickable()) + " itens", foot, juce::Justification::centredRight);
    }

    void paintOverChildren (juce::Graphics& g) override
    {
        // lupa POR CIMA do campo (o fundo opaco do TextEditor cobria o paint)
        if (! search.getBounds().isEmpty())
        {
            auto sb = search.getBounds().toFloat();
            auto lens = juce::Rectangle<float> (sb.getX() + 10.0f, sb.getCentreY() - 8.0f, 11.0f, 11.0f);
            g.setColour (NordLookAndFeel::dimText());
            g.drawEllipse (lens, 1.6f);
            g.drawLine (lens.getRight() - 1.0f, lens.getBottom() - 1.0f,
                        lens.getRight() + 4.0f, lens.getBottom() + 4.0f, 1.6f);
        }
    }

    void resized() override
    {
        auto card = getLocalBounds().reduced (margin);
        closeBtn.setBounds (card.getRight() - (touchLayout ? 58 : 42),
                            card.getY() + (touchLayout ? 10 : 12),
                            touchLayout ? 48 : 26,
                            touchLayout ? 44 : 22);

        if (touchLayout)
        {
            // ---- BIBLIOTECA em duas colunas (spec do dono): categorias na
            //      lateral esquerda, busca + lista na direita. Estrutura de
            //      biblioteca premium e o fim da rolagem infinita num card
            //      baixo.
            auto area = card.reduced (14, 10);
            area.removeFromTop (44);                       // titulo + contexto

            auto rail = area.removeFromLeft (juce::jmin (172, area.getWidth() / 4));
            area.removeFromLeft (10);

            // remover discreto ANTES: reservar a base evita o trilho de
            // categorias estourar por cima dele (aconteceu no TANK 3 —
            // "Strings" e "Remover" sobrepostos, "Leads" invisivel).
            removeBtn.setBounds (rail.removeFromBottom (38).withTrimmedTop (4));
            rail.removeFromBottom (4);

            // chips empilhados dividindo o que sobrou; piso de 28 dp
            const int catH = juce::jlimit (28, 42,
                rail.getHeight() / numCategories - 3);
            for (int i = 0; i < numCategories; ++i)
            {
                chips[i]->setBounds (rail.removeFromTop (
                    juce::jmin (catH, juce::jmax (1, rail.getHeight()))));
                rail.removeFromTop (juce::jmin (3, rail.getHeight()));
            }

            auto searchRow = area.removeFromTop (46);
            search.setBounds (searchRow);
            clearBtn.setBounds (searchRow.removeFromRight (searchRow.getHeight()).reduced (6));
            area.removeFromTop (8);
            area.removeFromBottom (22);                    // rodape (contagem)
            list.setBounds (area);
            return;
        }

        auto area = card.reduced (20, 16);
        area.removeFromTop (24);                           // titulo + contexto
        auto searchRow = area.removeFromTop (34);
        search.setBounds (searchRow);
        clearBtn.setBounds (searchRow.removeFromRight (searchRow.getHeight()).reduced (5));
        area.removeFromTop (8);

        // desktop: chips horizontais de largura igual
        auto chipRow = area.removeFromTop (26);
        const int chipGap = 6;
        const int chipW = juce::jmax (30,
            (chipRow.getWidth() - chipGap * (numCategories - 1)) / numCategories);
        for (int i = 0; i < numCategories; ++i)
        {
            chips[i]->setBounds (i == numCategories - 1 ? chipRow
                                                        : chipRow.removeFromLeft (chipW));
            if (i < numCategories - 1)
                chipRow.removeFromLeft (juce::jmin (chipGap, chipRow.getWidth()));
        }
        area.removeFromTop (8);

        auto footArea = area.removeFromBottom (24);
        removeBtn.setBounds (footArea.removeFromLeft (juce::jmin (170, footArea.getWidth() / 2))
                                 .withTrimmedBottom (2));
        list.setBounds (area);
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::escapeKey)
        {
            if (onCancel != nullptr)
                onCancel();

            return true;
        }

        return false;
    }

private:
    // TextEditor que deixa as setas passarem pra lista em vez de comer o evento
    struct SearchBox final : public juce::TextEditor
    {
        std::function<void (int)> onArrow;

        bool keyPressed (const juce::KeyPress& key) override
        {
            if (key == juce::KeyPress::upKey || key == juce::KeyPress::downKey)
            {
                if (onArrow != nullptr)
                    onArrow (key == juce::KeyPress::downKey ? 1 : -1);

                return true;
            }

            return juce::TextEditor::keyPressed (key);
        }
    };

    // O popup touch abre com foco na lista, sem teclado virtual. Este adaptador
    // usa a mesma regra que pula cabecalhos e trata Enter/Esc para D-pad.
    struct PickerListBox final : public juce::ListBox
    {
        std::function<void (int)> onMove;
        std::function<void()> onChoose;
        std::function<void()> onCancel;

        bool keyPressed (const juce::KeyPress& key) override
        {
            if (key == juce::KeyPress::upKey
                 || key == juce::KeyPress::downKey)
            {
                if (onMove != nullptr)
                    onMove (key == juce::KeyPress::downKey ? 1 : -1);

                return true;
            }

            if (key == juce::KeyPress::returnKey)
            {
                if (onChoose != nullptr)
                    onChoose();

                return true;
            }

            if (key == juce::KeyPress::escapeKey)
            {
                if (onCancel != nullptr)
                    onCancel();

                return true;
            }

            return juce::ListBox::keyPressed (key);
        }
    };

    struct Row
    {
        bool header = false;
        juce::String text;
        int id = 0;
        int kind = 0;
        juce::String subtitle;
    };

    static constexpr int margin = 16;
    static constexpr int numCategories = 8;

    static juce::String categoryLabel (int index)
    {
        switch (index)
        {
            case 0: return "Todos";
            case 1: return "Pianos";
            case 2: return juce::String::fromUTF8 ("\xc3\x93rg\xc3\xa3os");
            case 3: return "Pads";
            case 4: return "Bells";
            case 5: return "Strings";
            case 6: return "Leads";
            case 7: return "Plugins";
        }
        return {};
    }

    bool matchesCategory (const Item& it) const
    {
        switch (activeCategory)
        {
            case 0: return true;
            case 1: return it.category == "PIANOS" || it.category == "EP & TECLAS";
            case 2: return it.category == "ORGAOS";
            case 3: return it.category == "PADS" || it.category == "AMBIENCIAS";
            case 4: return it.category == "BELLS";
            case 5: return it.category == "STRINGS";
            case 6: return it.category == "LEADS & SYNTHS";
            case 7: return it.category == "PLUGINS";
        }
        return true;
    }

    void rebuildRows()
    {
        rows.clearQuick();
        const auto q = search.getText().trim();
        const bool browsing = q.isEmpty() && activeCategory == 0;

        juce::String lastSection;

        for (const auto& it : items)
        {
            if (it.id == 1)
                continue;   // a remocao virou botao discreto no rodape

            if (! matchesCategory (it))
                continue;

            if (q.isNotEmpty()
                 && ! (it.name.containsIgnoreCase (q)
                        || it.section.containsIgnoreCase (q)
                        || it.category.containsIgnoreCase (q)
                        || it.subtitle.containsIgnoreCase (q)))
                continue;

            if (browsing && it.section != lastSection)
            {
                rows.add ({ true, it.section, 0, 0, {} });
                lastSection = it.section;
            }

            rows.add ({ false, it.name, it.id, it.kind,
                        it.subtitle.isNotEmpty() ? it.subtitle : it.section });
        }

        list.updateContent();
        list.selectRow (firstPickableRow());
        repaint();
    }

    int firstPickableRow() const
    {
        for (int i = 0; i < rows.size(); ++i)
            if (! rows.getReference (i).header)
                return i;

        return -1;
    }

    int countPickable() const
    {
        int n = 0;

        for (const auto& r : rows)
            if (! r.header)
                ++n;

        return n;
    }

    void moveSelection (int delta)
    {
        // sem clamp: o jlimit prendia a linha no header 0 e a selecao sumia.
        // Se sair dos limites ou so achar header, mantem a selecao original.
        int row = list.getSelectedRow();

        for (int step = 0; step < rows.size(); ++step)
        {
            row += delta;

            if (! juce::isPositiveAndBelow (row, rows.size()))
                return;

            if (! rows.getReference (row).header)
            {
                list.selectRow (row);
                list.scrollToEnsureRowIsOnscreen (row);
                return;
            }
        }
    }

    void pickRow (int row)
    {
        if (! juce::isPositiveAndBelow (row, rows.size()))
            return;

        const auto& r = rows.getReference (row);

        if (! r.header && onPick != nullptr)
            onPick (r.id);
    }

    //== ListBoxModel ==========================================================
    int getNumRows() override { return rows.size(); }

    void paintListBoxItem (int rowNumber, juce::Graphics& g, int width, int height, bool rowIsSelected) override
    {
        if (! juce::isPositiveAndBelow (rowNumber, rows.size()))
            return;

        const auto& r = rows.getReference (rowNumber);

        if (r.header)
        {
            // secao em cinza discreto: o vermelho fica para estados
            g.setColour (juce::Colours::white.withAlpha (0.50f));
            g.setFont (juce::Font (juce::FontOptions (touchLayout ? 11.5f : 10.0f))
                                   .boldened().withExtraKerningFactor (0.16f));
            g.drawText (r.text, 8, 0, width - 16, height, juce::Justification::centredLeft);
            g.setColour (juce::Colours::white.withAlpha (0.08f));
            g.fillRect (8, height - 1, width - 16, 1);
            return;
        }

        const bool isCurrent = (r.id == currentSelectedId);
        const auto accent = colourForKind != nullptr ? colourForKind (r.kind)
                                                     : NordLookAndFeel::txtSecondary();

        if (rowIsSelected)
        {
            auto rowArea = juce::Rectangle<float> (3.0f, 1.0f, (float) width - 6.0f, (float) height - 2.0f);

            g.setGradientFill (juce::ColourGradient (NordLookAndFeel::nordRed().withAlpha (0.55f),
                                                     rowArea.getX(), rowArea.getY(),
                                                     NordLookAndFeel::nordRedDark().withAlpha (0.45f),
                                                     rowArea.getX(), rowArea.getBottom(), false));
            g.fillRoundedRectangle (rowArea, 6.0f);
            g.setColour (NordLookAndFeel::nordRedBright());
            g.fillRoundedRectangle (rowArea.withWidth (3.0f), 1.5f);
            g.setColour (juce::Colours::white.withAlpha (0.14f));
            g.drawRoundedRectangle (rowArea, 6.0f, 1.0f);
        }
        else if (isCurrent)
        {
            // timbre JA CARREGADO: chapa sutil + borda na cor da familia
            auto rowArea = juce::Rectangle<float> (3.0f, 1.0f, (float) width - 6.0f, (float) height - 2.0f);
            g.setColour (juce::Colour (0xff222833).withAlpha (0.9f));
            g.fillRoundedRectangle (rowArea, 6.0f);
            g.setColour (accent.withAlpha (0.55f));
            g.drawRoundedRectangle (rowArea.reduced (0.5f), 6.0f, 1.2f);
            g.fillRoundedRectangle (rowArea.withWidth (3.0f), 1.5f);
        }

        // tile do icone com a cor da familia: reconhecimento num relance
        const float tile = (float) height - (touchLayout ? 14.0f : 8.0f);
        auto tileArea = juce::Rectangle<float> (10.0f, ((float) height - tile) * 0.5f, tile, tile);
        g.setColour (accent.withAlpha (0.16f));
        g.fillRoundedRectangle (tileArea, 6.0f);
        g.setColour (accent.withAlpha (0.35f));
        g.drawRoundedRectangle (tileArea.reduced (0.5f), 6.0f, 1.0f);

        if (drawIcon != nullptr && r.kind != 0)
            drawIcon (g, tileArea.reduced (3.0f), r.kind);

        const int textX = (int) tileArea.getRight() + 10;
        const int textW = width - textX - (isCurrent ? 96 : 12);

        g.setColour (rowIsSelected ? juce::Colours::white : NordLookAndFeel::textColour());

        if (touchLayout && r.subtitle.isNotEmpty())
        {
            // duas linhas em ZONAS FIXAS com folga real entre elas: nome
            // forte (sempre bold — biblioteca le pelo nome) e contexto
            // discreto embaixo, sem os descendentes de um tocarem o outro.
            g.setFont (juce::Font (juce::FontOptions (16.5f, juce::Font::bold)));
            g.drawText (r.text, textX, 9, textW, 24,
                        juce::Justification::centredLeft, true);
            g.setColour (rowIsSelected ? juce::Colours::white.withAlpha (0.72f)
                                       : NordLookAndFeel::txtSecondary());
            g.setFont (juce::Font (juce::FontOptions (12.0f)));
            g.drawText (r.subtitle, textX, height - 26, textW, 18,
                        juce::Justification::centredLeft, true);
        }
        else
        {
            g.setFont (juce::Font (juce::FontOptions (touchLayout ? 16.0f : 14.0f,
                                                       rowIsSelected ? juce::Font::bold
                                                                     : juce::Font::plain)));
            g.drawText (r.text, textX, 0, textW, height, juce::Justification::centredLeft, true);
        }

        if (isCurrent)
        {
            // tag "Selecionado" no lugar do check solto
            auto tagFont = NordLookAndFeel::stageLabel (9.0f);
            const juce::String tag ("SELECIONADO");
            const float tagW = juce::GlyphArrangement::getStringWidth (tagFont, tag) + 14.0f;
            auto tagArea = juce::Rectangle<float> ((float) width - tagW - 10.0f,
                                                   (float) height * 0.5f - 9.0f, tagW, 18.0f);
            g.setColour (accent.withAlpha (0.22f));
            g.fillRoundedRectangle (tagArea, 9.0f);
            g.setColour (accent.brighter (0.4f));
            g.setFont (tagFont);
            g.drawText (tag, tagArea.toNearestInt(), juce::Justification::centred, false);
        }
    }

    void listBoxItemClicked (int row, const juce::MouseEvent&) override { pickRow (row); }
    void returnKeyPressed (int row) override                            { pickRow (row); }

    juce::Array<Item> items;
    juce::Array<Row> rows;
    int currentSelectedId = 0;
    bool touchLayout = false;
    int activeCategory = 0;
    juce::String channelBadge, currentName;

    SearchBox search;
    PickerListBox list;
    juce::TextButton closeBtn, clearBtn, removeBtn;
    juce::OwnedArray<juce::TextButton> chips;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TimbrePicker)
};
