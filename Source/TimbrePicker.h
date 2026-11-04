#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "NordLookAndFeel.h"
#include "TimbrePickerInteraction.h"

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
                     private juce::ListBoxModel,
                     private juce::Timer
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

        // Pack online ainda nao instalado: aparece na lista com acao Baixar.
        // O download e por biblioteca (unidade real do catalogo); downloads
        // diferentes podem acontecer em paralelo.
        bool downloadable = false;
        juce::String packId;
        juce::String actionLabel;   // "Baixar · 235 MB", "Instalando 42%", "Em breve"
        bool actionEnabled = true;
        float progress = -1.0f;     // -1 = sem barra; 0..1 = progresso
        juce::String key;           // identidade estavel para favoritos/recentes
        bool importAction = false; // opens a file chooser; not an installed instrument
    };

    // Botao proprio do browser: as tres primeiras opcoes sao tabs largas e as
    // demais sao filtros de familia com icones vetoriais. Usar TextButton cru
    // deixava dez retangulos genericos sem a hierarquia vista na referencia.
    class FilterButton final : public juce::TextButton
    {
    public:
        FilterButton (juce::String text, int filterIndex)
            : juce::TextButton (std::move (text)), index (filterIndex)
        {
            setWantsKeyboardFocus (false);
        }

        void paintButton (juce::Graphics& g, bool highlighted, bool down) override
        {
            const auto bounds = getLocalBounds().toFloat().reduced (1.0f);
            const bool active = getToggleState();
            const bool desktop = NordLookAndFeel::usesDesktopControls (*this);
            const float alpha = isEnabled() ? 1.0f : 0.42f;
            g.setColour ((active ? (index < 3 ? NordLookAndFeel::bgControlHi()
                                               : NordLookAndFeel::nordRed().withAlpha (0.14f))
                                : highlighted || down ? NordLookAndFeel::bgControlHi()
                                                      : juce::Colours::transparentBlack).withMultipliedAlpha (alpha));
            g.fillRoundedRectangle (bounds, 7.0f);
            if (active)
            {
                g.setColour ((index < 3 ? NordLookAndFeel::txtSecondary() : NordLookAndFeel::nordRedBright())
                                 .withMultipliedAlpha (alpha));
                const float cueHeight = desktop ? juce::jmin (14.0f, bounds.getHeight() - 8.0f)
                                                : juce::jmax (2.0f, bounds.getHeight() - 16);
                g.fillRoundedRectangle (bounds.getX(), bounds.getCentreY() - cueHeight * 0.5f,
                                        desktop ? 2.0f : 3.0f, cueHeight, 1.0f);
            }
            auto text = bounds.toNearestInt().reduced (14, 3);
            auto icon = text.removeFromLeft (24).toFloat().withSizeKeepingCentre (18, 18);
            auto ink = (active ? NordLookAndFeel::txtPrimary() : NordLookAndFeel::txtSecondary())
                           .withMultipliedAlpha (alpha);
            if (index >= 3) paintCategoryIcon (g, icon, index, ink);
            else
            {
                g.setColour (active ? NordLookAndFeel::nordRedBright().withMultipliedAlpha (alpha) : ink);
                g.setFont (juce::Font (juce::FontOptions (15.0f)));
                g.drawText (index == 0 ? juce::String::fromUTF8 ("≡")
                           : index == 1 ? juce::String::fromUTF8 ("☆")
                                        : juce::String::fromUTF8 ("↶"),
                            icon, juce::Justification::centred);
            }
            text.removeFromLeft (8);
            g.setColour (ink);
            g.setFont (juce::Font (juce::FontOptions (juce::jlimit (11.5f, 14.0f, bounds.getHeight() * 0.37f),
                                                    active ? juce::Font::bold : juce::Font::plain)));
            g.drawFittedText (getButtonText(), text, juce::Justification::centredLeft, 1, 0.9f);
    }

    private:
        static void paintCategoryIcon (juce::Graphics& g,
                                       juce::Rectangle<float> r,
                                       int category,
                                       juce::Colour colour)
        {
            g.setColour (colour.withMultipliedAlpha (0.92f));
            r = r.reduced (1.0f);

            if (category == 3 || category == 4) // piano / orgao
            {
                const int rows = category == 4 ? 2 : 1;
                for (int row = 0; row < rows; ++row)
                {
                    auto keys = r.withHeight ((r.getHeight() - (rows - 1) * 3.0f)
                                              / (float) rows)
                                 .translated (0.0f, row * (r.getHeight() + 3.0f)
                                                     / (float) rows);
                    g.drawRoundedRectangle (keys, 1.5f, 1.1f);
                    for (int key = 1; key < 4; ++key)
                    {
                        const float x = keys.getX() + keys.getWidth() * key / 4.0f;
                        g.drawVerticalLine (juce::roundToInt (x),
                                            keys.getY() + 1.0f,
                                            keys.getBottom() - 1.0f);
                    }
                }
                return;
            }

            if (category == 5 || category == 8) // pad / lead
            {
                const float mid = r.getCentreY();
                const float heights[] { 0.30f, 0.72f, 0.46f, 0.86f, 0.38f };
                for (int i = 0; i < 5; ++i)
                {
                    const float h = r.getHeight() * heights[i];
                    g.fillRoundedRectangle (r.getX() + i * r.getWidth() / 5.0f,
                                            mid - h * 0.5f, 1.6f, h, 0.8f);
                }
                return;
            }

            if (category == 6) // bell
            {
                juce::Path bell;
                bell.startNewSubPath (r.getCentreX(), r.getY() + 2.0f);
                bell.cubicTo (r.getX() + 3.0f, r.getY() + 5.0f,
                              r.getX() + 4.0f, r.getBottom() - 6.0f,
                              r.getX() + 1.5f, r.getBottom() - 4.0f);
                bell.lineTo (r.getRight() - 1.5f, r.getBottom() - 4.0f);
                bell.cubicTo (r.getRight() - 4.0f, r.getBottom() - 6.0f,
                              r.getRight() - 3.0f, r.getY() + 5.0f,
                              r.getCentreX(), r.getY() + 2.0f);
                g.strokePath (bell, juce::PathStrokeType (1.4f));
                g.fillEllipse (r.getCentreX() - 1.5f, r.getBottom() - 3.5f,
                               3.0f, 3.0f);
                return;
            }

            if (category == 7) // strings
            {
                for (int i = 0; i < 4; ++i)
                {
                    const float x = r.getX() + 3.0f + i * (r.getWidth() - 6.0f) / 3.0f;
                    g.drawLine (x - 3.0f, r.getBottom() - 2.0f,
                                x + 3.0f, r.getY() + 2.0f, 1.1f);
                }
                return;
            }

            // plugins: corpo do plug + dois pinos, inteiramente vetorial.
            auto plug = r.reduced (4.0f, 3.0f);
            g.drawRoundedRectangle (plug, 2.0f, 1.2f);
            g.drawLine (plug.getX() - 3.0f, plug.getCentreY() - 3.0f,
                        plug.getX(), plug.getCentreY() - 3.0f, 1.2f);
            g.drawLine (plug.getX() - 3.0f, plug.getCentreY() + 3.0f,
                        plug.getX(), plug.getCentreY() + 3.0f, 1.2f);
            g.drawLine (plug.getRight(), plug.getCentreY(),
                        plug.getRight() + 3.0f, plug.getCentreY(), 1.2f);
        }

        int index = 0;
    };

    std::function<void (int)> onPick;
    std::function<bool (int)> canPreview;
    std::function<void (int)> onPreview;
    std::function<void()> onStopPreview;
    std::function<void (float)> onPreviewLevel;
    std::function<bool()> isPreviewing;
    std::function<juce::String()> previewStatus;
    std::function<void (const juce::String& packId)> onDownload;
    std::function<void()> onCancel;
    std::function<void (const juce::String& key, bool isFavorite)> onFavoriteChanged;

    // quem cria o picker diz como desenhar o icone (o glyph vive em outro header)
    std::function<void (juce::Graphics&, juce::Rectangle<float>, int)> drawIcon;
    std::function<void (juce::Graphics&, juce::Rectangle<float>, int, const juce::String&)> drawArtwork;
    // cor da familia para o tile do icone e acentos (mesma paleta dos cards)
    std::function<juce::Colour (int)> colourForKind;

    static juce::Rectangle<int> safeTouchBoundsFor (juce::Component& parent)
    {
        auto safe = parent.getLocalBounds();
       #if CHURCHDAW_MOBILE_SHELL
        if (const auto* display = juce::Desktop::getInstance().getDisplays()
                                      .getDisplayForRect (parent.getScreenBounds()))
        {
            const auto screenSafe = display->safeAreaInsets.subtractedFrom (parent.getScreenBounds());
            const auto localSafe = parent.getLocalArea (nullptr, screenSafe).getIntersection (safe);
            if (! localSafe.isEmpty())
                safe = localSafe;
        }
       #endif
        const int w = juce::jmin (safe.getWidth(), juce::jmax (300, safe.getWidth() - 12));
        const int h = juce::jmin (safe.getHeight(), juce::jmax (220, safe.getHeight() - 8));
        return safe.withSizeKeepingCentre (w, h);
    }

    TimbrePicker (juce::Array<Item> allItems, int currentId,
                  juce::StringArray favoriteKeysToUse = {},
                  juce::StringArray recentKeysToUse = {})
        : items (std::move (allItems)), currentSelectedId (currentId),
          favoriteKeys (std::move (favoriteKeysToUse)),
          recentKeys (std::move (recentKeysToUse))
    {
        normalizeItemKeys();
        search.setTextToShowWhenEmpty (
            "Buscar instrumento ou biblioteca...",
            NordLookAndFeel::dimText().withAlpha (0.65f));
        search.setTitle ("Buscar instrumentos");
        search.setFont (juce::Font (juce::FontOptions (15.0f)));
        // campo premium: vidro escuro proprio, contorno sutil (nao preto chapado)
        search.setColour (juce::TextEditor::backgroundColourId, juce::Colours::transparentBlack);
        search.setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
        search.setColour (juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
        search.setColour (juce::TextEditor::textColourId, NordLookAndFeel::textColour());
        search.setColour (juce::TextEditor::highlightColourId, NordLookAndFeel::nordRed().withAlpha (0.5f));
        search.setIndents (38, 6);   // abre espaco para a lupa desenhada no paint
        search.setOpaque (false);
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
            auto* chip = new FilterButton (categoryLabel (i), i);
            chip->setClickingTogglesState (false);
            chip->setColour (juce::TextButton::buttonColourId, juce::Colour (0xff20252d));
            chip->setColour (juce::TextButton::buttonOnColourId, NordLookAndFeel::nordRed().withAlpha (0.85f));
            chip->setColour (juce::TextButton::textColourOffId, NordLookAndFeel::txtSecondary());
            chip->setColour (juce::TextButton::textColourOnId, juce::Colours::white);
            chip->onClick = [this, i]
            {
                if (i < 3)
                    activeTab = i;
                else
                    activeFamily = (activeFamily == i ? -1 : i);

                syncFilterButtons();
                rebuildRows();
            };
            chip->setToggleState (i == 0, juce::dontSendNotification);
            addAndMakeVisible (chip);
            chips.add (chip);
        }

        list.setModel (this);
        list.setRowHeight (72);
        list.setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
        list.setOutlineThickness (0);
        list.onMove = [this] (int delta) { moveSelection (delta); };
        list.onChoose = [this] { pickRow (list.getSelectedRow()); };
        list.onCancel = [this] { if (onCancel != nullptr) onCancel(); };
        list.onType = [this] (juce::juce_wchar character)
        {
            search.grabKeyboardFocus();
            search.insertTextAtCaret (juce::String::charToString (character));
        };
        list.setWantsKeyboardFocus (true);
        addAndMakeVisible (list);

        closeBtn.setButtonText (juce::String::fromUTF8 ("\xc3\x97"));   // ×
        closeBtn.setComponentID ("timbrePickerClose");
        closeBtn.setTooltip ("Fechar seletor de instrumento");
        closeBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2a2d34));
        closeBtn.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
        closeBtn.onClick = [this] { if (onCancel != nullptr) onCancel(); };
        addAndMakeVisible (closeBtn);

        // Acao destrutiva DISCRETA: substitui a antiga barra vermelha gigante
        // "Canal vazio / remover instrumento" que dominava a lista.
        removeBtn.setButtonText ("Remover instrumento");
        removeBtn.setComponentID ("timbrePickerRemove");
        removeBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1c2027));
        removeBtn.setColour (juce::TextButton::textColourOffId,
                             NordLookAndFeel::nordRedBright().withAlpha (0.85f));
        removeBtn.onClick = [this] { if (onPick != nullptr) onPick (1); };
        addAndMakeVisible (removeBtn);
        removeBtn.setVisible (currentSelectedId >= 2);

        previewButton.setButtonText ("Ouvir");
        previewButton.setTitle ("Ouvir amostra do timbre");
        previewButton.onClick = [this]
        {
            if (isPreviewing != nullptr && isPreviewing())
            { if (onStopPreview != nullptr) onStopPreview(); }
            else if (const int id = selectedItemId(); id > 1 && canPreview != nullptr && canPreview (id))
            {
                if (onPreviewLevel != nullptr) onPreviewLevel ((float) previewLevel.getValue());
                if (onPreview != nullptr) onPreview (id);
            }
            syncSelectionControls();
        };
        addAndMakeVisible (previewButton);
        loadButton.setButtonText ("Carregar timbre");
        loadButton.onClick = [this] { pickRow (list.getSelectedRow()); };
        addAndMakeVisible (loadButton);
        previewLevel.setRange (-48.0, -12.0, 1.0);
        previewLevel.setValue (-24.0, juce::dontSendNotification);
        previewLevel.setSliderStyle (juce::Slider::LinearHorizontal);
        previewLevel.setTextBoxStyle (juce::Slider::TextBoxRight, false, 55, 24);
        previewLevel.setTextValueSuffix (" dB");
        previewLevel.setTitle ("Volume da pre-escuta");
        previewLevel.setTooltip ("Volume independente da amostra. Nao altera o canal.");
        previewLevel.onValueChange = [this]
        { if (onPreviewLevel != nullptr) onPreviewLevel ((float) previewLevel.getValue()); };
        addAndMakeVisible (previewLevel);
        previewLabel.setFont (juce::Font (juce::FontOptions (11.5f)));
        previewLabel.setColour (juce::Label::textColourId, NordLookAndFeel::txtSecondary());
        addAndMakeVisible (previewLabel);
        startTimerHz (5);
        setSize (1320, 860);
        rebuildRows();
    }

    void useTouchLayout (bool shouldUseTouch)
    {
        touchLayout = shouldUseTouch;
        // 64 dp: nome e subtitulo em zonas proprias com respiro — com 58 os
        // descendentes do nome encostavam no subtitulo (apontado pelo dono).
        list.setRowHeight (touchLayout ? 64 : 72);
        list.setWantsKeyboardFocus (true);
        search.setFont (juce::Font (juce::FontOptions (touchLayout ? 17.0f : 15.0f)));
        resized();
        repaint();
    }

    int getVisibleInstrumentCount() const { return countPickable(); }

    // contexto do canal para o header ("CANAL 1" + timbre atual)
    void setContext (juce::String channelBadgeToUse, juce::String currentNameToUse)
    {
        channelBadge = std::move (channelBadgeToUse);
        currentName = std::move (currentNameToUse);
        repaint();
    }

    // Abre o seletor vindo de um atalho externo (por exemplo, a barra lateral)
    // ja na familia correta, usando exatamente o mesmo filtro dos chips.
    void selectCategory (int categoryIndex)
    {
        const int safeIndex = juce::jlimit (0, numCategories - 1, categoryIndex);
        if (safeIndex < 3)
            activeTab = safeIndex;
        else
        {
            activeTab = 0;
            activeFamily = safeIndex;
        }
        search.setText ({}, false);
        clearBtn.setVisible (false);
        syncFilterButtons();
        rebuildRows();
    }

    // Atualiza progresso/rotulo de um pack em download sem fechar o seletor.
    void updateDownloadState (const juce::String& packId, float progressValue,
                              juce::String label, bool enabled)
    {
        bool changed = false;

        for (auto& it : items)
        {
            if (! it.downloadable || it.packId != packId)
                continue;

            it.progress = progressValue;
            it.actionLabel = label;
            it.actionEnabled = enabled;
            changed = true;
        }

        if (! changed)
            return;

        for (auto& r : rows)
        {
            if (! r.downloadable || r.packId != packId)
                continue;

            r.progress = progressValue;
            r.actionLabel = label;
            r.actionEnabled = enabled;
        }

        list.repaint();
        repaint();
    }

    // Um instalador continua vivo quando o seletor e fechado. Ao reabrir,
    // reaplica o progresso e deixa claro que os demais downloads aguardam,
    // em vez de exibir botoes habilitados que nao fazem nada.
    void setDownloadBusy (const juce::String& activePackId,
                          float progressValue,
                          const juce::String& activeLabel)
    {
        // Atualiza apenas o item correspondente. Reconstruir rows aqui fazia
        // o ListBox recalcular o viewport a cada pacote de progresso, jogando
        // o usuario para o topo e escondendo o download em andamento.
        updateDownloadState (activePackId, progressValue, activeLabel, false);
    }

    // Substitui a lista (ex.: apos instalar um pack e rescanear).
    void replaceItems (juce::Array<Item> newItems, int currentId)
    {
        const auto viewPosition = list.getVerticalPosition();
        items = std::move (newItems);
        normalizeItemKeys();
        currentSelectedId = currentId;
        removeBtn.setVisible (currentSelectedId >= 2);
        rebuildRows();
        list.setVerticalPosition (viewPosition);
    }

    // grabKeyboardFocus dentro de visibilityChanged() nao pega: o componente
    // ainda nao esta na tela. O foco e pedido depois, ja montado.
    void focusSearch()
    {
        // No celular, o foco inicial vai para a LISTA: D-pad/setas/Enter passam
        // a funcionar sem abrir o teclado virtual e sem cobrir os timbres.
        list.grabKeyboardFocus();
        // Mounting/resizing the modal may move the viewport after selectRow().
        // Reveal the complete current instrument once the final layout exists.
        if (list.getSelectedRow() >= 0)
            list.scrollToEnsureRowIsOnscreen (list.getSelectedRow());
    }

    void paint (juce::Graphics& g) override
    {
        // Overlay real: cobre a tela anfitria inteira e deixa o browser como
        // unica superficie ativa, em vez de parecer um popup pequeno do combo.
        const bool desktop = ! touchLayout && NordLookAndFeel::usesDesktopControls (*this);
        g.setColour (juce::Colours::black.withAlpha (desktop ? 0.58f : 0.72f));
        g.fillRect (getLocalBounds());

        auto card = modalCardBounds().toFloat();
        const float rad = 14.0f;

        g.setColour (desktop ? juce::Colour (0xff1b1f26) : NordLookAndFeel::bgCard());
        g.fillRoundedRectangle (card, rad);
        g.setColour (desktop ? juce::Colour (0xff3b414b) : NordLookAndFeel::borderDefault());
        g.drawRoundedRectangle (card.reduced (0.5f), rad, 1.0f);
        if (! touchLayout)
        {
            auto rail = card.reduced (16.0f, 16.0f).withTrimmedTop (68.0f)
                            .withWidth (juce::jmin (196.0f, card.getWidth() * 0.23f));
            if (desktop)
            {
                g.setColour (juce::Colours::white.withAlpha (0.055f));
                g.drawLine (rail.getRight() + 8.0f, rail.getY(),
                            rail.getRight() + 8.0f, rail.getBottom() - 10.0f, 1.0f);
            }
            else
            {
                g.setColour (NordLookAndFeel::bgApp().withAlpha (0.55f));
                g.fillRoundedRectangle (rail, 9.0f);
            }
        }

        // ---- header: titulo + badge do canal + timbre atual ----
        auto head = card.toNearestInt().reduced (touchLayout ? 20 : 22, 0)
                        .withTop (card.toNearestInt().getY() + (touchLayout ? 14 : 14))
                        .withHeight (touchLayout ? 26 : 36);
        g.setColour (juce::Colours::white);
        g.setFont (juce::Font (juce::FontOptions (touchLayout ? 16.0f : 22.0f,
                                                   juce::Font::bold)));
        const juce::String title ("Instrumentos");
        g.drawText (title, head, juce::Justification::centredLeft, false);

        if (channelBadge.isNotEmpty())
        {
            const float titleW = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), title);
            auto badgeFont = juce::Font (juce::FontOptions (
                touchLayout ? 11.0f : 12.0f, juce::Font::bold));
            const float badgeH = touchLayout ? 20.0f : 30.0f;
            const float badgeW = juce::GlyphArrangement::getStringWidth (badgeFont, channelBadge)
                                 + (touchLayout ? 18.0f : 22.0f);
            auto badge = juce::Rectangle<float> ((float) head.getX() + titleW
                                                     + (touchLayout ? 14.0f : 14.0f),
                                                 (float) head.getCentreY() - badgeH * 0.5f,
                                                 badgeW, badgeH);
            g.setColour (desktop ? juce::Colour (0xff2a2e37) : NordLookAndFeel::bgControlHi());
            g.fillRoundedRectangle (badge, desktop ? 6.0f : badgeH * 0.5f);
            if (! desktop)
            {
                g.setColour (juce::Colours::white.withAlpha (0.16f));
                g.drawRoundedRectangle (badge.reduced (0.5f), badgeH * 0.5f, 1.0f);
            }
            g.setColour (juce::Colours::white);
            g.setFont (badgeFont);
            g.drawText (channelBadge, badge.toNearestInt(), juce::Justification::centred, false);

            const juce::String shownName = currentName.isNotEmpty()
                                               ? currentName : juce::String ("Canal vazio");
            auto currentFont = juce::Font (juce::FontOptions (
                touchLayout ? 13.0f : 13.5f, juce::Font::plain));
            const juce::String prefix ("Atual: ");
            const float prefixW = juce::GlyphArrangement::getStringWidth (currentFont, prefix);
            auto currentArea = juce::Rectangle<int> (
                (int) badge.getRight() + (touchLayout ? 10 : 12), head.getY(),
                juce::jmax (10, (int) card.getRight() - (touchLayout ? 76 : 212)
                                 - ((int) badge.getRight() + (touchLayout ? 10 : 12))),
                head.getHeight());
            g.setColour (NordLookAndFeel::txtSecondary());
            g.setFont (currentFont);
            g.drawText (prefix, currentArea, juce::Justification::centredLeft, false);
            currentArea.removeFromLeft (juce::roundToInt (prefixW));
            g.setColour (NordLookAndFeel::txtPrimary());
            g.setFont (currentFont.boldened());
            g.drawFittedText (shownName, currentArea,
                              juce::Justification::centredLeft, 1, 0.75f);
        }

        // A busca transparente recebe daqui o vidro, o contorno e o glow.
        if (! search.getBounds().isEmpty())
        {
            auto sb = search.getBounds().toFloat();
            g.setColour (desktop ? juce::Colour (0xff151920) : NordLookAndFeel::bgControl());
            g.fillRoundedRectangle (sb, 8.0f);
            g.setColour (search.hasKeyboardFocus (true) ? (desktop ? NordLookAndFeel::focusOutline()
                                                                    : NordLookAndFeel::nordRedBright())
                                                       : NordLookAndFeel::borderDefault());
            g.drawRoundedRectangle (sb.reduced (0.5f), 8.0f, 1.0f);
        }

        if (! desktop && ! list.getBounds().isEmpty())
        {
            auto lb = list.getBounds().toFloat();
            g.setColour (NordLookAndFeel::bgCard());
            g.fillRoundedRectangle (lb, 6.0f);
            g.setColour (juce::Colours::white.withAlpha (0.065f));
            g.drawRoundedRectangle (lb.reduced (0.5f), 6.0f, 0.9f);
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
            juce::String emptyHint;
            if (activeTab == 1)
                emptyHint = juce::String::fromUTF8 (
                    "Marque a estrela de um timbre para adicion\xc3\xa1-lo aos favoritos");
            else if (activeTab == 2)
                emptyHint = juce::String::fromUTF8 (
                    "Os timbres escolhidos recentemente aparecer\xc3\xa3o aqui");
            else if (search.getText().isNotEmpty())
                emptyHint = juce::String::fromUTF8 (
                    "Tente outro termo de busca ou remova o filtro de fam\xc3\xadlia");
            else
                emptyHint = juce::String::fromUTF8 (
                    "N\xc3\xa3o h\xc3\xa1 timbres dispon\xc3\xadveis neste filtro");

            g.drawText (emptyHint, empty.withTrimmedTop (6),
                        juce::Justification::centredTop);
        }

        // ---- rodape ----
        auto foot = card.toNearestInt().reduced (touchLayout ? 20 : 22, 0)
                        .withHeight (touchLayout ? 18 : 34);
        foot.setY (card.toNearestInt().getBottom() - (touchLayout ? 28 : 42));
        g.setColour (NordLookAndFeel::dimText().withAlpha (0.86f));
        g.setFont (juce::Font (juce::FontOptions (touchLayout ? 11.0f : 11.5f)));

        if (! touchLayout)
            foot = juce::Rectangle<int> (card.getRight() - 204, card.getY() + 22, 128, 26);

        const auto countLabel = activeTab == 1 ? " favoritos"
                              : activeTab == 2 ? " recentes"
                                               : " timbres";
        g.setColour (NordLookAndFeel::txtSecondary());
        const int shownCount = getVisibleInstrumentCount();
        g.drawText (juce::String (shownCount) + countLabel,
                    foot, juce::Justification::centredRight);
    }

    void paintOverChildren (juce::Graphics& g) override
    {
        const bool desktop = ! touchLayout && NordLookAndFeel::usesDesktopControls (*this);
        // lupa POR CIMA do campo (o fundo opaco do TextEditor cobria o paint)
        if (! search.getBounds().isEmpty())
        {
            auto sb = search.getBounds().toFloat();
            const float side = touchLayout ? 11.0f : 16.0f;
            auto lens = juce::Rectangle<float> (
                sb.getX() + (touchLayout ? 10.0f : 16.0f),
                sb.getCentreY() - side * 0.58f, side, side);
            g.setColour (NordLookAndFeel::txtSecondary());
            g.drawEllipse (lens, desktop ? 1.4f : touchLayout ? 1.6f : 2.2f);
            g.drawLine (lens.getRight() - 1.0f, lens.getBottom() - 1.0f,
                        lens.getRight() + (touchLayout ? 4.0f : 5.0f),
                        lens.getBottom() + (touchLayout ? 4.0f : 5.0f),
                        desktop ? 1.4f : touchLayout ? 1.6f : 1.8f);
        }
    }

    void resized() override
    {
        auto card = modalCardBounds();
        closeBtn.setBounds (card.getRight() - (touchLayout ? 58 : 54),
                            card.getY() + (touchLayout ? 10 : 18),
                            touchLayout ? 48 : 40,
                            touchLayout ? 44 : 40);

        if (touchLayout)
        {
            // ---- BIBLIOTECA em duas colunas (spec do dono): categorias na
            //      lateral esquerda, busca + lista na direita. Estrutura de
            //      biblioteca premium e o fim da rolagem infinita num card
            //      baixo.
            auto area = card.reduced (14, 10);
            area.removeFromTop (44);                       // titulo + contexto

            const bool twoColRail = (area.getHeight() - 42) < 310 && area.getWidth() >= 480;
            auto rail = area.removeFromLeft (twoColRail
                ? juce::jmin (216, area.getWidth() * 36 / 100)
                : juce::jmin (172, area.getWidth() / 4));
            area.removeFromLeft (10);

            // remover discreto ANTES: reservar a base evita o trilho de
            // categorias estourar por cima dele (aconteceu no TANK 3 —
            // "Strings" e "Remover" sobrepostos, "Leads" invisivel).
            removeBtn.setBounds (rail.removeFromBottom (38).withTrimmedTop (4));
            rail.removeFromBottom (4);

            if (twoColRail)
            {
                constexpr int cols = 2;
                const int rows = (numCategories + cols - 1) / cols;
                const int colGap = 4;
                const int rowGap = 4;
                const int colW = juce::jmax (48, (rail.getWidth() - colGap) / cols);
                const int chipH = juce::jlimit (24, 44,
                    (rail.getHeight() - rowGap * (rows - 1)) / rows);

                for (int i = 0; i < numCategories; ++i)
                {
                    const int r = i / cols;
                    const int c = i % cols;
                    const int cellY = rail.getY() + r * (chipH + rowGap);
                    chips[i]->setBounds (rail.getX() + c * (colW + colGap),
                                         cellY,
                                         colW,
                                         juce::jmax (1, juce::jmin (chipH, rail.getBottom() - cellY)));
                }
            }
            else
            {
                // chips empilhados dividindo o que sobrou; piso de 28 dp
                const int catH = juce::jlimit (28, 42,
                    rail.getHeight() / numCategories - 3);
                for (int i = 0; i < numCategories; ++i)
                {
                    chips[i]->setBounds (rail.removeFromTop (
                        juce::jmin (catH, juce::jmax (1, rail.getHeight()))));
                    rail.removeFromTop (juce::jmin (3, rail.getHeight()));
                }
            }

            auto searchRow = area.removeFromTop (46);
            search.setBounds (searchRow);
            clearBtn.setBounds (searchRow.removeFromRight (searchRow.getHeight()).reduced (6));
            area.removeFromTop (8);
            area.removeFromBottom (22);                    // rodape (contagem)
            auto auditionRow = area.removeFromBottom (44);
            loadButton.setBounds (auditionRow.removeFromRight (116));
            auditionRow.removeFromRight (6);
            previewButton.setBounds (auditionRow.removeFromLeft (80));
            previewLevel.setBounds (auditionRow);
            previewLabel.setBounds ({});
            area.removeFromBottom (6);
            list.setBounds (area);
            return;
        }

        auto area = card.reduced (24, 18);
        area.removeFromTop (62);
        auto footer = area.removeFromBottom (64);
        auto hint = footer.removeFromTop (24);
        hint.removeFromLeft (212);
        previewLabel.setBounds (hint);
        auto controls = footer.withTrimmedLeft (212);
        loadButton.setBounds (controls.removeFromRight (132)); controls.removeFromRight (12);
        previewButton.setBounds (controls.removeFromLeft (88)); controls.removeFromLeft (8);
        previewLevel.setBounds (controls);
        area.removeFromBottom (12);
        auto rail = area.removeFromLeft (juce::jmin (188, area.getWidth() / 4));
        area.removeFromLeft (24);
        const int gap = 4;
        const int groupGap = 16;
        const int rowHeight = juce::jlimit (23, 40, (rail.getHeight() - groupGap - 9 * gap) / numCategories);
        for (int i = 0; i < numCategories; ++i)
        {
            if (i == 3) rail.removeFromTop (groupGap);
            chips[i]->setBounds (rail.removeFromTop (rowHeight));
            rail.removeFromTop (gap);
        }
        auto searchRow = area.removeFromTop (44);
        search.setBounds (searchRow);
        clearBtn.setBounds (searchRow.removeFromRight (40).reduced (6));
        search.setIndents (42, 10);
        area.removeFromTop (16);
        list.setBounds (area);
        removeBtn.setBounds (footer.removeFromLeft (188));

        if (auto* viewport = list.getViewport())
        {
            viewport->setScrollBarThickness (9);

            auto& scrollBar = viewport->getVerticalScrollBar();
            scrollBar.setColour (juce::ScrollBar::backgroundColourId,
                                 juce::Colours::transparentBlack);
            scrollBar.setColour (juce::ScrollBar::trackColourId,
                                 juce::Colour (0xff171b22));
            scrollBar.setColour (juce::ScrollBar::thumbColourId,
                                 NordLookAndFeel::txtDisabled().withAlpha (0.80f));
        }
    }

    void parentSizeChanged() override
    {
        if (auto* parent = getParentComponent())
        {
            if (touchLayout)
            {
                setBounds (safeTouchBoundsFor (*parent));
            }
            else
            {
                setBounds (0, 0,
                           juce::jmax (640, parent->getWidth()),
                           juce::jmax (520, parent->getHeight()));
            }
        }
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
        std::function<void (juce::juce_wchar)> onType;

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

            const auto character = key.getTextCharacter();
            if (character >= 0x20 && character != 0x7f
                 && ! key.getModifiers().isCommandDown()
                 && ! key.getModifiers().isCtrlDown())
            {
                if (onType != nullptr)
                    onType (character);
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
        bool downloadable = false;
        juce::String packId;
        juce::String actionLabel;
        bool actionEnabled = true;
        float progress = -1.0f;
        juce::String key;
        bool importAction = false;
    };

    static constexpr int margin = 16;
    static constexpr int numCategories = 10;

    juce::Rectangle<int> modalCardBounds() const
    {
        auto available = getLocalBounds();
        if (touchLayout)
            return available.reduced (margin);

        const int cardH = juce::jmin (900, juce::jmax (280, available.getHeight() - 28));
        const int cardW = juce::jmin (
            juce::jmin (1380, juce::jmax (320, available.getWidth() - 36)),
            juce::roundToInt ((float) cardH * 1.50f));
        return juce::Rectangle<int> (cardW, cardH).withCentre (available.getCentre());
    }

    void syncFilterButtons()
    {
        for (int i = 0; i < numCategories; ++i)
        {
            const bool selected = i < 3 ? i == activeTab
                                        : i == activeFamily;
            chips[i]->setToggleState (selected, juce::dontSendNotification);
        }
    }

    static juce::String categoryLabel (int index)
    {
        switch (index)
        {
            case 0: return "Todos";
            case 1: return "Favoritos";
            case 2: return "Recentes";
            case 3: return "Pianos";
            case 4: return juce::String::fromUTF8 ("\xc3\x93rg\xc3\xa3os");
            case 5: return "Pads";
            case 6: return "Bells";
            case 7: return "Strings";
            case 8: return "Leads";
            case 9: return "Plugins";
        }
        return {};
    }

    bool matchesCategory (const Item& it) const
    {
        // Old preferences may contain the import command. It must not become
        // an instrument in Favorites or Recent merely because a key was saved.
        if (it.importAction && activeTab != 0)
            return false;

        if (activeTab == 1 && ! favoriteKeys.contains (itemKey (it)))
            return false;

        if (activeTab == 2 && ! recentKeys.contains (itemKey (it)))
            return false;

        switch (activeFamily)
        {
            case -1: return true;
            case 3: return it.category == "PIANOS" || it.category == "EP & TECLAS";
            case 4: return it.category == "ORGAOS";
            case 5: return it.category == "PADS" || it.category == "AMBIENCIAS";
            case 6: return it.category == "BELLS";
            case 7: return it.category == "STRINGS";
            case 8: return it.category == "LEADS & SYNTHS";
            case 9: return it.category == "PLUGINS";
        }
        return true;
    }

    static juce::String itemKey (const Item& it)
    {
        return it.key.isNotEmpty() ? it.key : it.section + "|" + it.name;
    }

    static juce::String displaySectionName (const juce::String& section)
    {
        if (section == "CHURCH ORGAN ENGINE")
            return juce::String::fromUTF8 ("Órgãos de igreja");

        if (section == "CHURCH LIVE")
            return "Biblioteca Church Live";

        if (section == "SOUNDFONTS")
            return "SoundFonts (SF2)";

        if (section.startsWithIgnoreCase ("PLUGINS"))
            return "Plugins externos";

        return section;
    }

    void normalizeItemKeys()
    {
        for (auto& it : items)
            if (it.key.isEmpty())
                it.key = itemKey (it);
    }

    void rebuildRows()
    {
        const auto previousViewPosition = list.getVerticalPosition();
        const bool preserveViewPosition = ! rows.isEmpty();
        rows.clearQuick();
        const auto q = search.getText().trim();
        const bool browsing = q.isEmpty() && activeTab == 0 && activeFamily < 0;

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
                rows.add ({ true, displaySectionName (it.section), 0, 0, {} });
                lastSection = it.section;
            }

            rows.add ({ false, it.name, it.id, it.kind,
                        it.subtitle.isNotEmpty() ? it.subtitle : it.section,
                        it.downloadable, it.packId, it.actionLabel,
                        it.actionEnabled, it.progress, itemKey (it), it.importAction });
        }

        list.updateContent();
        int rowToSelect = firstPickableRow();
        for (int i = 0; i < rows.size(); ++i)
        {
            const auto& row = rows.getReference (i);
            if (! row.header && row.id == currentSelectedId)
            {
                rowToSelect = i;
                break;
            }
        }

        list.selectRow (rowToSelect);
        if (! preserveViewPosition && rowToSelect >= 0)
            list.scrollToEnsureRowIsOnscreen (rowToSelect);
        else if (preserveViewPosition)
            list.setVerticalPosition (previousViewPosition);
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
            if (! r.header && ! r.importAction)
                ++n;

        return n;
    }

    int countAllPickable() const
    {
        int n = 0;
        for (const auto& item : items)
            if (item.id != 1 && ! item.importAction)
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

        if (r.header)
            return;

        if (r.downloadable)
        {
            if (r.actionEnabled && onDownload != nullptr && r.packId.isNotEmpty())
                onDownload (r.packId);

            return;
        }

        if (onPick != nullptr)
            onPick (r.id);
    }

    //== ListBoxModel ==========================================================
    int getNumRows() override { return rows.size(); }

    void paintListBoxItem (int rowNumber, juce::Graphics& g, int width, int height, bool rowIsSelected) override
    {
        if (! juce::isPositiveAndBelow (rowNumber, rows.size()))
            return;

        const auto& r = rows.getReference (rowNumber);
        const bool desktop = ! touchLayout && NordLookAndFeel::usesDesktopControls (*this);

        if (r.header)
        {
            // secao em cinza discreto: o vermelho fica para estados
            g.setColour (desktop ? NordLookAndFeel::txtSecondary() : juce::Colours::white.withAlpha (0.50f));
            g.setFont (juce::Font (juce::FontOptions (touchLayout ? 11.5f : desktop ? 11.0f : 10.0f))
                                   .boldened().withExtraKerningFactor (desktop ? 0.025f : 0.16f));
            g.drawText (r.text, 8, 0, width - 16, height, juce::Justification::centredLeft);
            g.setColour (juce::Colours::white.withAlpha (0.08f));
            g.fillRect (8, height - 1, width - 16, 1);
            return;
        }

        const bool isCurrent = (r.id == currentSelectedId);
        const auto accent = colourForKind != nullptr ? colourForKind (r.kind)
                                                     : NordLookAndFeel::txtSecondary();

        auto rowArea = juce::Rectangle<float> (3.0f, 2.0f, (float) width - 6.0f, (float) height - 4.0f);
        if (rowIsSelected || isCurrent)
        {
            g.setColour (rowIsSelected ? (desktop ? juce::Colour (0xff332730)
                                                  : NordLookAndFeel::nordRed().withAlpha (0.12f))
                                      : NordLookAndFeel::bgControl());
            g.fillRoundedRectangle (rowArea, 7.0f);
            g.setColour (rowIsSelected ? NordLookAndFeel::nordRedBright()
                                      : NordLookAndFeel::greenOk());
            const float cueHeight = desktop ? juce::jmin (20.0f, rowArea.getHeight() - 8.0f)
                                            : juce::jmax (1.0f, rowArea.getHeight() - 20);
            g.fillRoundedRectangle (rowArea.getX(), rowArea.getCentreY() - cueHeight * 0.5f,
                                    2.0f, cueHeight, 1.0f);
        }
        else
        {
            g.setColour (desktop ? juce::Colours::white.withAlpha (0.035f)
                                  : NordLookAndFeel::borderSoft().withAlpha (0.55f));
            g.fillRect (14, height - 1, juce::jmax (1, width - 28), 1);
        }
        const float tile = juce::jmin (56.0f, height - 14.0f);
        auto tileArea = juce::Rectangle<float> (12.0f, (height - tile) * 0.5f, tile, tile);
        if (! desktop || drawArtwork == nullptr)
        {
            g.setColour (accent.withAlpha (desktop ? 0.075f : 0.12f));
            g.fillRoundedRectangle (tileArea, 8.0f);
        }

        if (r.importAction)
        {
            // File action, not keyboard artwork: this row opens the chooser.
            auto glyph = tileArea.withSizeKeepingCentre (24.0f, 28.0f);
            g.setColour (NordLookAndFeel::txtSecondary());
            g.drawRoundedRectangle (glyph.reduced (2.0f), 3.0f, 1.3f);
            const auto centre = glyph.getCentre();
            juce::Path arrow;
            arrow.startNewSubPath (centre.x, centre.y - 6.0f);
            arrow.lineTo (centre.x, centre.y + 5.0f);
            arrow.startNewSubPath (centre.x - 4.0f, centre.y + 1.0f);
            arrow.lineTo (centre.x, centre.y + 5.0f);
            arrow.lineTo (centre.x + 4.0f, centre.y + 1.0f);
            g.strokePath (arrow, juce::PathStrokeType (1.3f, juce::PathStrokeType::curved,
                                                            juce::PathStrokeType::rounded));
        }
        else if (drawArtwork != nullptr && r.kind != 0)
            drawArtwork (g, tileArea.reduced (2.0f), r.kind, r.text);
        else if (drawIcon != nullptr && r.kind != 0)
            drawIcon (g, tileArea.reduced (3.0f), r.kind);

        // Reserva a direita para estrela e badge de acao (Baixar / Pronto).
        float rightReserve = 12.0f;

        if (r.downloadable && r.actionLabel.isNotEmpty())
        {
            auto actionFont = NordLookAndFeel::stageLabel (touchLayout ? 10.0f : 9.0f);
            rightReserve = juce::jmax (
                88.0f,
                juce::GlyphArrangement::getStringWidth (actionFont, r.actionLabel) + 28.0f);
        }
        else if (isCurrent)
        {
            rightReserve = touchLayout ? 126.0f : 128.0f;
        }
        else if (! r.downloadable)
        {
            rightReserve = 128.0f;
        }

        const int textX = (int) tileArea.getRight() + 10;
        const int textW = juce::jmax (40, width - textX - (int) rightReserve);

        g.setColour (rowIsSelected ? juce::Colours::white : NordLookAndFeel::textColour());

        if (height >= 50 && r.subtitle.isNotEmpty())
        {
            // duas linhas em ZONAS FIXAS com folga real entre elas: nome
            // forte (sempre bold — biblioteca le pelo nome) e contexto
            // discreto embaixo, sem os descendentes de um tocarem o outro.
            g.setFont (juce::Font (juce::FontOptions (15.0f, juce::Font::bold)));
            g.drawText (r.text, textX, (height - 44) / 2, textW, 24,
                        juce::Justification::centredLeft, true);
            g.setColour (rowIsSelected ? juce::Colours::white.withAlpha (0.72f)
                                       : NordLookAndFeel::txtSecondary());
            g.setFont (juce::Font (juce::FontOptions (12.0f)));
            g.drawText (r.subtitle, textX, (height - 44) / 2 + 26, textW, 18,
                        juce::Justification::centredLeft, true);
        }
        else
        {
            g.setFont (juce::Font (juce::FontOptions (touchLayout ? 16.0f : 14.5f,
                                                       rowIsSelected ? juce::Font::bold
                                                                     : juce::Font::plain)));
            g.drawText (r.text, textX, 0, textW, height, juce::Justification::centredLeft, true);
        }

        if (r.downloadable && r.actionLabel.isNotEmpty())
        {
            auto tagFont = NordLookAndFeel::stageLabel (touchLayout ? 10.0f : 9.0f);
            const float tagW = juce::jmin (
                (float) width - 20.0f,
                juce::GlyphArrangement::getStringWidth (tagFont, r.actionLabel) + 16.0f);
            auto tagArea = juce::Rectangle<float> ((float) width - tagW - 10.0f,
                                                   (float) height * 0.5f - 11.0f, tagW, 22.0f);

            if (r.progress >= 0.0f)
            {
                // Barra sob o badge enquanto instala.
                auto bar = juce::Rectangle<float> (tagArea.getX(), tagArea.getBottom() + 2.0f,
                                                   tagArea.getWidth(), 3.0f);
                g.setColour (juce::Colours::white.withAlpha (0.12f));
                g.fillRoundedRectangle (bar, 1.5f);
                g.setColour (NordLookAndFeel::nordRedBright());
                g.fillRoundedRectangle (
                    bar.withWidth (bar.getWidth()
                                   * juce::jlimit (0.0f, 1.0f, r.progress)),
                    1.5f);
                tagArea.translate (0.0f, -2.0f);
            }

            const auto fill = r.actionEnabled
                                  ? NordLookAndFeel::nordRed().withAlpha (0.88f)
                                  : juce::Colour (0xff2a2f38);
            g.setColour (fill);
            g.fillRoundedRectangle (tagArea, 11.0f);
            g.setColour (juce::Colours::white.withAlpha (r.actionEnabled ? 1.0f : 0.55f));
            g.setFont (tagFont);
            g.drawText (r.actionLabel, tagArea.toNearestInt(),
                        juce::Justification::centred, false);
        }
        if (isCurrent)
        {
            // Tag "Pronto" verde: feedback claro de timbre ja no canal.
            auto tagFont = juce::Font (juce::FontOptions (
                touchLayout ? 9.0f : 10.0f, juce::Font::bold));
            const juce::String tag (juce::String::fromUTF8 ("\xe2\x9c\x93 Atual"));
            const float tagH = touchLayout ? 18.0f : 24.0f;
            const float tagW = juce::GlyphArrangement::getStringWidth (tagFont, tag)
                               + (touchLayout ? 16.0f : 18.0f);
            auto tagArea = juce::Rectangle<float> ((float) width - tagW
                                                       - (touchLayout ? 38.0f : 42.0f),
                                                   (float) height * 0.5f - tagH * 0.5f,
                                                   tagW, tagH);
            if (! desktop)
            {
                g.setColour (NordLookAndFeel::greenOk().withAlpha (0.20f));
                g.fillRoundedRectangle (tagArea, tagH * 0.5f);
                g.setColour (NordLookAndFeel::greenOk().withAlpha (0.42f));
                g.drawRoundedRectangle (tagArea.reduced (0.5f), tagH * 0.5f, 0.9f);
            }
            g.setColour (NordLookAndFeel::greenOk().brighter (0.15f));
            g.setFont (tagFont);
            g.drawText (tag, tagArea.toNearestInt(), juce::Justification::centred, false);
        }

        if (! r.downloadable && ! isCurrent)
        {
            g.setColour (NordLookAndFeel::txtSecondary());
            g.setFont (juce::Font (juce::FontOptions (10.5f)));
            g.drawText (r.importAction ? "Importar" : "Instalado", width - 124, 0, 80, height,
                        juce::Justification::centredRight);
        }

        if (! r.downloadable && ! r.importAction)
        {
            const auto star = favoriteKeys.contains (r.key)
                                   ? juce::String::fromUTF8 ("\xe2\x98\x85")
                                   : juce::String::fromUTF8 ("\xe2\x98\x86");

            const auto starArea = juce::Rectangle<float> (
                (float) width - (touchLayout ? 31.0f : 38.0f),
                (float) height * 0.5f - (touchLayout ? 11.0f : 13.0f),
                touchLayout ? 22.0f : 28.0f,
                touchLayout ? 22.0f : 26.0f);

            if (! desktop && isCurrent)
            {
                g.setGradientFill (juce::ColourGradient (
                    juce::Colour (0xff20252d), starArea.getX(), starArea.getY(),
                    juce::Colour (0xff0c0f14), starArea.getX(), starArea.getBottom(), false));
                g.fillEllipse (starArea);
                g.setColour (juce::Colours::white.withAlpha (0.13f));
                g.drawEllipse (starArea.reduced (0.5f), 1.0f);
            }

            if (desktop)
            {
                const bool favourite = favoriteKeys.contains (r.key);
                const auto centre = starArea.getCentre();
                juce::Path outline;
                for (int point = 0; point < 10; ++point)
                {
                    const float angle = juce::MathConstants<float>::twoPi * (float) point / 10.0f
                                          - juce::MathConstants<float>::halfPi;
                    const float radius = point % 2 == 0 ? 7.5f : 3.5f;
                    const auto vertex = centre + juce::Point<float> (std::cos (angle) * radius, std::sin (angle) * radius);
                    if (point == 0) outline.startNewSubPath (vertex); else outline.lineTo (vertex);
                }
                outline.closeSubPath();
                g.setColour (favourite ? juce::Colour (0xffe5bc75) : NordLookAndFeel::txtSecondary());
                if (favourite) g.fillPath (outline);
                else g.strokePath (outline, juce::PathStrokeType (1.15f, juce::PathStrokeType::curved,
                                                                        juce::PathStrokeType::rounded));
                return;
            }
            g.setColour (favoriteKeys.contains (r.key)
                             ? juce::Colour (0xffffc857)
                             : NordLookAndFeel::dimText().withAlpha (0.72f));
            g.setFont (juce::Font (juce::FontOptions (touchLayout ? 20.0f : 20.0f)));
            g.drawText (star, starArea.toNearestInt(),
                        juce::Justification::centred, false);
        }
    }

    void selectedRowsChanged (int) override
    {
        if (onStopPreview != nullptr) onStopPreview();
        syncSelectionControls();
    }
    int selectedItemId() const
    {
        const int row = list.getSelectedRow();
        return juce::isPositiveAndBelow (row, rows.size()) && ! rows.getReference (row).header
            ? rows.getReference (row).id : 0;
    }
    void visibilityChanged() override
    {
        if (isShowing())
            syncSelectionControls();
    }

    void timerCallback() override
    {
        if (isShowing())
            syncSelectionControls();
    }

    void syncSelectionControls()
    {

        const int id = selectedItemId();
        const auto row = list.getSelectedRow();
        const bool valid = juce::isPositiveAndBelow (row, rows.size()) && id > 1;
        const bool importing = valid && rows.getReference (row).importAction;
        const bool canHear = valid && ! importing && canPreview != nullptr && canPreview (id);
        const bool playing = isPreviewing != nullptr && isPreviewing();
        previewButton.setEnabled (canHear || playing);
        previewButton.setButtonText (playing ? "Parar" : "Ouvir");
        previewButton.setTitle (playing ? juce::String::fromUTF8 ("Parar pr\xc3\xa9-escuta") : "Ouvir amostra do timbre");
        previewLevel.setEnabled (canHear || playing);
        const bool download = valid && rows.getReference (row).downloadable;
        loadButton.setEnabled (valid && id != currentSelectedId && (! download || rows.getReference (row).actionEnabled));
        loadButton.setButtonText (importing ? "Importar arquivo" : download ? "Baixar"
                                 : id == currentSelectedId ? "No canal" : "Carregar timbre");
        previewLabel.setText (importing ? "Escolha um arquivo SoundFont (.sf2)."
            : canHear && previewStatus != nullptr ? previewStatus()
            : download ? "Instale a biblioteca para ouvir suas amostras"
                       : juce::String::fromUTF8 ("Pr\xc3\xa9-escuta dispon\xc3\xadvel para timbres com amostras locais"), juce::dontSendNotification);
    }
    juce::TextButton previewButton, loadButton;
    juce::Slider previewLevel;
    juce::Label previewLabel;

    void listBoxItemClicked (int row, const juce::MouseEvent& e) override
    {
        const bool valid = juce::isPositiveAndBelow (row, rows.size());
        const bool header = valid && rows.getReference (row).header;
        const bool downloadable = valid && rows.getReference (row).downloadable;
        const bool favoriteHit = valid && ! downloadable && ! rows.getReference (row).importAction
                              && e.position.x >= (float) list.getWidth() - 58.0f;

        switch (ChurchDAWTimbrePickerInteraction::singleClickAction (
                    valid, header, downloadable, favoriteHit))
        {
            case ChurchDAWTimbrePickerInteraction::SingleClickAction::toggleFavorite:
                toggleFavorite (row);
                return;

            case ChurchDAWTimbrePickerInteraction::SingleClickAction::startDownload:
                list.selectRow (row);
                pickRow (row);
                return;

            case ChurchDAWTimbrePickerInteraction::SingleClickAction::select:
                list.selectRow (row);
                return;

            case ChurchDAWTimbrePickerInteraction::SingleClickAction::none:
                return;
        }
    }
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override
    {
        const bool valid = juce::isPositiveAndBelow (row, rows.size());
        const bool header = valid && rows.getReference (row).header;
        const bool downloadable = valid && rows.getReference (row).downloadable;

        // O primeiro clique de um double-click ja inicia o download. Ignorar o
        // segundo evita duas confirmacoes para o mesmo timbre.
        if (ChurchDAWTimbrePickerInteraction::activateOnDoubleClick (
                valid, header, downloadable))
            pickRow (row);
    }
    void returnKeyPressed (int row) override                            { pickRow (row); }

    void toggleFavorite (int row)
    {
        if (! juce::isPositiveAndBelow (row, rows.size()))
            return;

        const auto& r = rows.getReference (row);
        if (r.header || r.downloadable || r.importAction || r.key.isEmpty())
            return;

        const bool wasFavorite = favoriteKeys.contains (r.key);
        if (wasFavorite)
            favoriteKeys.removeString (r.key);
        else
            favoriteKeys.addIfNotAlreadyThere (r.key);

        if (onFavoriteChanged != nullptr)
            onFavoriteChanged (r.key, ! wasFavorite);

        if (activeTab == 1)
            rebuildRows();
        else
        {
            list.repaint();
            repaint();
        }
    }

    juce::Array<Item> items;
    juce::Array<Row> rows;
    juce::StringArray favoriteKeys;
    juce::StringArray recentKeys;
    int currentSelectedId = 0;
    bool touchLayout = false;
    int activeTab = 0;
    int activeFamily = -1;
    juce::String channelBadge, currentName;

    SearchBox search;
    PickerListBox list;
    juce::TextButton closeBtn, clearBtn, removeBtn;
    juce::OwnedArray<juce::TextButton> chips;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TimbrePicker)
};
