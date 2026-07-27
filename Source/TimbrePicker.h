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
// Seletor de timbre COM BUSCA. A biblioteca tem centenas de itens e o menu do
// ComboBox nao tem campo de busca: achar um timbre exigia rolar a lista inteira.
// Aqui e so digitar; Enter escolhe o primeiro resultado, Esc fecha.
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
    };

    std::function<void (int)> onPick;
    std::function<void()> onCancel;

    // quem cria o picker diz como desenhar o icone (o glyph vive em outro header)
    std::function<void (juce::Graphics&, juce::Rectangle<float>, int)> drawIcon;

    TimbrePicker (juce::Array<Item> allItems, int currentId)
        : items (std::move (allItems)), currentSelectedId (currentId)
    {
        search.setTextToShowWhenEmpty (juce::String::fromUTF8 ("Buscar timbre ou plugin..."),
                                       NordLookAndFeel::dimText().withAlpha (0.65f));
        search.setFont (juce::Font (juce::FontOptions (15.0f)));
        // mesmo vidro escuro embutido das combos e do visor de programa
        search.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff0c0d10));
        search.setColour (juce::TextEditor::outlineColourId, juce::Colours::black);
        search.setColour (juce::TextEditor::focusedOutlineColourId, NordLookAndFeel::nordRedBright().withAlpha (0.65f));
        search.setColour (juce::TextEditor::textColourId, NordLookAndFeel::textColour());
        search.setColour (juce::TextEditor::highlightColourId, NordLookAndFeel::nordRed().withAlpha (0.5f));
        search.onTextChange = [this] { rebuildRows(); };
        search.onEscapeKey  = [this] { if (onCancel != nullptr) onCancel(); };
        search.onReturnKey  = [this] { pickRow (list.getSelectedRow()); };
        search.onArrow      = [this] (int delta) { moveSelection (delta); };
        addAndMakeVisible (search);

        list.setModel (this);
        list.setRowHeight (26);
        list.setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
        list.setOutlineThickness (0);
        list.setWantsKeyboardFocus (false);   // o teclado e sempre da busca
        addAndMakeVisible (list);

        closeBtn.setButtonText (juce::String::fromUTF8 ("\xc3\x97"));   // ×
        closeBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2a2d34));
        closeBtn.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
        closeBtn.onClick = [this] { if (onCancel != nullptr) onCancel(); };
        addAndMakeVisible (closeBtn);

        setSize (440 + margin * 2, 486 + margin * 2);
        rebuildRows();
    }

    // grabKeyboardFocus dentro de visibilityChanged() nao pega: o componente
    // ainda nao esta na tela. O foco e pedido depois, ja montado.
    void focusSearch() { search.grabKeyboardFocus(); }

    void paint (juce::Graphics& g) override
    {
        auto card = getLocalBounds().toFloat().reduced ((float) margin);
        const float rad = 14.0f;

        {
            juce::Path sp;
            sp.addRoundedRectangle (card, rad);
            juce::DropShadow (juce::Colours::black.withAlpha (0.6f), 28, { 0, 8 }).drawForPath (g, sp);
        }

        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff1b1c22), card.getX(), card.getY(),
                                                 juce::Colour (0xff101013), card.getX(), card.getBottom(), false));
        g.fillRoundedRectangle (card, rad);
        g.setColour (juce::Colours::white.withAlpha (0.06f));
        g.drawLine (card.getX() + 14.0f, card.getY() + 1.5f, card.getRight() - 14.0f, card.getY() + 1.5f, 1.0f);
        g.setColour (NordLookAndFeel::nordRed().withAlpha (0.45f));
        g.drawRoundedRectangle (card, rad, 1.2f);

        g.setColour (NordLookAndFeel::nordRed());
        g.fillRoundedRectangle (card.reduced (16.0f, 0.0f).removeFromTop (9.0f).withTrimmedTop (6.0f), 1.5f);

        auto head = card.toNearestInt().reduced (20, 0).withTop (card.toNearestInt().getY() + 16).withHeight (22);
        g.setColour (juce::Colours::white);
        g.setFont (juce::Font (juce::FontOptions (14.0f)).boldened().withExtraKerningFactor (0.10f));
        g.drawText ("ESCOLHER INSTRUMENTO", head, juce::Justification::centredLeft);

        // rodape com as teclas de atalho
        auto foot = card.toNearestInt().reduced (20, 0).withBottom (card.toNearestInt().getBottom() - 12).withHeight (16);
        foot.setY (card.toNearestInt().getBottom() - 26);
        g.setColour (NordLookAndFeel::dimText().withAlpha (0.55f));
        g.setFont (juce::Font (juce::FontOptions (11.0f)));
        g.drawText (juce::String::fromUTF8 ("Enter escolhe  \xc2\xb7  setas navegam  \xc2\xb7  Esc fecha"),
                    foot, juce::Justification::centredLeft);

        g.setColour (NordLookAndFeel::dimText().withAlpha (0.55f));
        g.drawText (juce::String (countPickable()) + " itens", foot, juce::Justification::centredRight);
    }

    void resized() override
    {
        auto card = getLocalBounds().reduced (margin);
        closeBtn.setBounds (card.getRight() - 42, card.getY() + 14, 26, 22);

        auto area = card.reduced (20, 16);
        area.removeFromTop (24);            // titulo
        search.setBounds (area.removeFromTop (34));
        area.removeFromTop (10);
        area.removeFromBottom (24);         // rodape
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

    struct Row
    {
        bool header = false;
        juce::String text;
        int id = 0;
        int kind = 0;
    };

    static constexpr int margin = 16;

    void rebuildRows()
    {
        rows.clearQuick();
        const auto q = search.getText().trim();

        if (q.isEmpty())
        {
            juce::String lastSection;

            for (const auto& it : items)
            {
                if (it.section != lastSection)
                {
                    rows.add ({ true, it.section, 0, 0 });
                    lastSection = it.section;
                }

                rows.add ({ false, it.name, it.id, it.kind });
            }
        }
        else
        {
            // buscando: lista plana, com a biblioteca ao lado pra dar contexto
            for (const auto& it : items)
                if (it.name.containsIgnoreCase (q) || it.section.containsIgnoreCase (q))
                    rows.add ({ false, it.name + juce::String::fromUTF8 ("   \xc2\xb7   ") + it.section,
                                it.id, it.kind });
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
            g.setColour (NordLookAndFeel::nordRedBright().withAlpha (0.85f));
            g.setFont (juce::Font (juce::FontOptions (10.0f)).boldened().withExtraKerningFactor (0.16f));
            g.drawText (r.text, 8, 0, width - 16, height, juce::Justification::centredLeft);
            g.setColour (juce::Colours::white.withAlpha (0.08f));
            g.fillRect (8, height - 1, width - 16, 1);
            return;
        }

        const bool isCurrent = (r.id == currentSelectedId);

        // ITEM ATIVO com muito mais contraste: o vinho de 0xff4a1622 mal separava
        // a linha do fundo do card, e navegando de seta voce perdia onde estava.
        if (rowIsSelected)
        {
            auto rowArea = juce::Rectangle<float> (3.0f, 1.0f, (float) width - 6.0f, (float) height - 2.0f);

            g.setGradientFill (juce::ColourGradient (NordLookAndFeel::nordRed().withAlpha (0.55f),
                                                     rowArea.getX(), rowArea.getY(),
                                                     NordLookAndFeel::nordRedDark().withAlpha (0.45f),
                                                     rowArea.getX(), rowArea.getBottom(), false));
            g.fillRoundedRectangle (rowArea, 5.0f);

            // barra vermelha na borda esquerda: marca a linha sem depender da cor
            // de fundo, que e o que se le num relance
            g.setColour (NordLookAndFeel::nordRedBright());
            g.fillRoundedRectangle (rowArea.withWidth (3.0f), 1.5f);

            g.setColour (juce::Colours::white.withAlpha (0.14f));
            g.drawRoundedRectangle (rowArea, 5.0f, 1.0f);
        }
        else if (isCurrent)
        {
            // timbre JA CARREGADO: chapa vinho sutil por baixo -- so o texto
            // vermelho + check na direita nao guiava o olho ao abrir a lista
            auto rowArea = juce::Rectangle<float> (3.0f, 1.0f, (float) width - 6.0f, (float) height - 2.0f);
            g.setColour (juce::Colour (0xff4a1622).withAlpha (0.75f));
            g.fillRoundedRectangle (rowArea, 5.0f);
        }

        // icone da categoria/marca: da pra reconhecer o tipo num relance, sem ler
        const float iconSize = (float) height - 8.0f;
        auto iconArea = juce::Rectangle<float> (10.0f, 4.0f, iconSize, iconSize);

        if (drawIcon != nullptr && r.kind != 0)
            drawIcon (g, iconArea, r.kind);

        const int textX = (int) iconArea.getRight() + 10;

        // linha selecionada sempre em BRANCO puro: em cima da chapa vermelha o
        // texto vermelho de "item atual" ficava ilegivel
        g.setColour (rowIsSelected ? juce::Colours::white
                     : isCurrent   ? NordLookAndFeel::nordRedBright()
                                   : NordLookAndFeel::textColour());
        g.setFont (juce::Font (juce::FontOptions (14.0f, rowIsSelected ? juce::Font::bold
                                                                       : juce::Font::plain)));
        g.drawText (r.text, textX, 0, width - textX - 26, height, juce::Justification::centredLeft, true);

        if (isCurrent)
        {
            g.setFont (juce::Font (juce::FontOptions (13.0f)).boldened());
            g.drawText (juce::String::fromUTF8 ("\xe2\x9c\x93"), width - 24, 0, 16, height,
                        juce::Justification::centred);
        }
    }

    void listBoxItemClicked (int row, const juce::MouseEvent&) override { pickRow (row); }
    void returnKeyPressed (int row) override                            { pickRow (row); }

    juce::Array<Item> items;
    juce::Array<Row> rows;
    int currentSelectedId = 0;

    SearchBox search;
    juce::ListBox list;
    juce::TextButton closeBtn;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TimbrePicker)
};
