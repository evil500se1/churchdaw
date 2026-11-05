#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "MobileShellConfig.h"
#if CHURCHDAW_CHURCHLIVE_LOOK
 #include "ChurchLiveFonts.h"
#endif
#include <cmath>

//==============================================================================
// Visual escuro com o vermelho classico dos teclados Nord: fundo quase preto,
// paineis grafite e acentos vermelhos.
//==============================================================================
class NordLookAndFeel : public juce::LookAndFeel_V4
{
public:
    //==============================================================================
    // DESIGN TOKENS. Fonte unica de cor, espacamento e tamanho -- nada de valor
    // solto espalhado pelos paint(). Trocar um token aqui muda o app inteiro.
    //==============================================================================

    // ---- superficies (grafite quente, OLED premium) ----
    static juce::Colour bgApp()          { return juce::Colour (CHURCHDAW_CHURCHLIVE_LOOK ? 0xff0a0a0d : 0xff101216); }   // preto grafite neutro
    static juce::Colour bgAppLift()      { return juce::Colour (CHURCHDAW_CHURCHLIVE_LOOK ? 0xff14141a : 0xff191c22); }   // luz ambiente da console
    static juce::Colour bgCard()         { return juce::Colour (CHURCHDAW_CHURCHLIVE_LOOK ? 0xff14141a : 0xff191c22); }   // painel/card
    static juce::Colour bgCardHi()       { return juce::Colour (CHURCHDAW_CHURCHLIVE_LOOK ? 0xff1c1c24 : 0xff22262d); }   // topo do glass
    static juce::Colour bgCardSel()      { return juce::Colour (CHURCHDAW_CHURCHLIVE_LOOK ? 0xff1c1c24 : 0xff252a32); }   // card selecionado
    static juce::Colour bgControl()      { return juce::Colour (CHURCHDAW_CHURCHLIVE_LOOK ? 0xff1c1c24 : 0xff22262e); }   // campos/controles
    static juce::Colour bgControlHi()    { return juce::Colour (CHURCHDAW_CHURCHLIVE_LOOK ? 0xff23232c : 0xff2b3039); }
    static juce::Colour borderDefault()  { return juce::Colour (CHURCHDAW_CHURCHLIVE_LOOK ? 0xff2b2b36 : 0xff383e48); }
    static juce::Colour borderSoft()     { return juce::Colour (CHURCHDAW_CHURCHLIVE_LOOK ? 0xff22222b : 0xff292e36); }
    static juce::Colour hairline()       { return juce::Colours::white.withAlpha (0.050f); }
    static juce::Colour bgFloating()     { return juce::Colour (CHURCHDAW_CHURCHLIVE_LOOK ? 0xff1c1c24 : 0xff22262d); }
    static juce::Colour bgFloatingHover() { return juce::Colour (CHURCHDAW_CHURCHLIVE_LOOK ? 0xff23232c : 0xff36303a); }
    static juce::Colour focusOutline()   { return juce::Colour (CHURCHDAW_CHURCHLIVE_LOOK ? 0xffff6b7d : 0xffeb7184); }

    // ---- texto ----
    static juce::Colour txtPrimary()     { return juce::Colour (CHURCHDAW_CHURCHLIVE_LOOK ? 0xfff5f3f4 : 0xfff2f2f3); }
    static juce::Colour txtSecondary()   { return juce::Colour (CHURCHDAW_CHURCHLIVE_LOOK ? 0xffa3a1ad : 0xffb2bac7); }
    static juce::Colour txtDisabled()    { return juce::Colour (CHURCHDAW_CHURCHLIVE_LOOK ? 0xff7d7b88 : 0xff7f8998); }

    // ---- acento e status ----
    static juce::Colour nordRed()        { return juce::Colour (CHURCHDAW_CHURCHLIVE_LOOK ? 0xffd92b40 : 0xffdc3f57); }   // acento global
    static juce::Colour nordRedBright()  { return juce::Colour (CHURCHDAW_CHURCHLIVE_LOOK ? 0xffff5a6e : 0xfff15b70); }   // hover/destaque
    static juce::Colour nordRedDark()    { return juce::Colour (CHURCHDAW_CHURCHLIVE_LOOK ? 0xff5a1420 : 0xff6d1220); }
    static juce::Colour amberSolo()      { return juce::Colour (CHURCHDAW_CHURCHLIVE_LOOK ? 0xfff4b73b : 0xffffb020); }   // solo/atencao
    static juce::Colour greenOk()        { return juce::Colour (CHURCHDAW_CHURCHLIVE_LOOK ? 0xff3ddc84 : 0xff46d37c); }   // status normal
    static juce::Colour redClip()        { return juce::Colour (0xffff1744); }   // erro/clip

    // Fundo do app com estetica matte dark minimalista:
    // sem halos vermelhos difusos, sem vinhetas escuras artificiais.
    static void fillAppBackground (juce::Graphics& g, juce::Rectangle<float> area)
    {
        g.setColour (bgApp());
        g.fillRect (area);
    }

    // Card minimalista — acabamento mate elegante, linhas finas e sem excessos.
    static void drawGlassCard (juce::Graphics& g, juce::Rectangle<float> card,
                               float radius = radiusCard,
                               bool selected = false,
                               juce::Colour accent = {},
                               bool empty = false,
                               bool master = false,
                               bool integratedStrip = false)
    {
        if (card.isEmpty())
            return;

       #if ! CHURCHDAW_MOBILE_SHELL
        // Matte surfaces keep the hierarchy in spacing and luminance rather
        // than stacking shiny frames around every section of the console.
        const float corner = integratedStrip ? 9.0f : radius;
        const auto cue = accent.isTransparent() ? nordRed() : accent;
       #if CHURCHDAW_CHURCHLIVE_LOOK
        // Church Live no desktop: superficie neutra (sem o azul do grafite
        // antigo), borda de 1 px e selecao pela borda vermelha -- sem barra de
        // topo nem brilho. O codigo anterior segue no #else.
        const auto surface = empty ? juce::Colour (0xff0d0d11)
                           : selected ? bgCardSel()
                           : master ? juce::Colour (0xff1b1318) : bgCard();
        g.setColour (surface);
        g.fillRoundedRectangle (card, corner);
        g.setColour (selected ? nordRedBright().withAlpha (0.85f)
                              : (empty ? borderSoft() : borderDefault()));
        g.drawRoundedRectangle (card.reduced (0.5f), corner, 1.0f);
        juce::ignoreUnused (cue);
        return;
       #else
        const auto surface = empty ? juce::Colour (0xff171a20)
                           : selected ? juce::Colour (0xff242830)
                           : master ? juce::Colour (0xff1e2229) : juce::Colour (0xff20242b);
        g.setColour (surface);
        g.fillRoundedRectangle (card, corner);
        g.setColour (selected ? cue.withAlpha (0.30f)
                              : juce::Colours::white.withAlpha (empty ? 0.04f : integratedStrip ? 0.065f : 0.075f));
        g.drawRoundedRectangle (card.reduced (0.5f), corner, 1.0f);
        if (! empty && (selected || master))
        {
            const float cueWidth = juce::jmax (0.0f, juce::jmin (card.getWidth() - 24.0f, 28.0f));
            if (cueWidth > 0.0f)
            {
                g.setColour (cue.withAlpha (0.72f));
                g.fillRoundedRectangle (card.getCentreX() - cueWidth * 0.5f, card.getY() + 1.0f,
                                        cueWidth, 1.5f, 0.75f);
            }
        }
        return;
       #endif
       #else
        const float corner = integratedStrip ? 8.0f : radius;
        const auto cue = accent.isTransparent() ? nordRed() : accent;
        const auto topCol = empty ? juce::Colour (0xff15181e)
                          : selected ? juce::Colour (0xff272d38)
                          : master ? juce::Colour (0xff20252f)
                                   : juce::Colour (0xff1e232c);
        const auto botCol = empty ? juce::Colour (0xff101318)
                          : selected ? juce::Colour (0xff181c24)
                          : master ? juce::Colour (0xff13171e)
                                   : juce::Colour (0xff13161d);

        if (! empty && card.getHeight() >= 48.0f)
        {
            g.setColour (juce::Colours::black.withAlpha (selected ? 0.34f : 0.22f));
            g.fillRoundedRectangle (card.translated (0.0f, 1.5f), corner);
        }

        g.setGradientFill (juce::ColourGradient (
            topCol, card.getX(), card.getY(),
            botCol, card.getX(), card.getBottom(), false));
        g.fillRoundedRectangle (card, corner);

        if (! empty)
        {
            auto topRim = card.reduced (corner * 0.45f, 1.0f).removeFromTop (1.0f);
            if (topRim.getWidth() > 4.0f)
            {
                g.setColour (juce::Colours::white.withAlpha (selected ? 0.10f : 0.06f));
                g.fillRect (topRim);
            }
        }

        g.setColour (selected ? cue.withAlpha (0.50f)
                              : juce::Colours::white.withAlpha (empty ? 0.045f : master ? 0.095f : 0.075f));
        g.drawRoundedRectangle (card.reduced (0.5f), corner, selected ? 1.2f : 1.0f);

        if (! empty && (selected || master || ! accent.isTransparent()))
        {
            const float crownW = selected
                ? juce::jmax (20.0f, juce::jmin (card.getWidth() - 24.0f, 52.0f))
                : master ? juce::jmin (card.getWidth() - 24.0f, 32.0f)
                         : 22.0f;
            if (crownW > 4.0f && card.getWidth() >= 36.0f)
            {
                auto crown = juce::Rectangle<float> (
                    card.getCentreX() - crownW * 0.5f,
                    card.getY() + 2.5f, crownW, 2.2f);
                if (selected || master)
                {
                    g.setColour (cue.withAlpha (0.22f));
                    g.fillRoundedRectangle (crown.expanded (2.0f, 1.2f), 2.0f);
                }
                g.setColour (cue.withAlpha (selected || master ? 0.92f : 0.68f));
                g.fillRoundedRectangle (crown, 1.1f);
            }
        }
       #endif
    }

    // ---- cor por categoria de instrumento (apoio, nunca protagonista) ----
    static juce::Colour catPiano()       { return juce::Colour (0xff66818a); }
    static juce::Colour catPad()         { return juce::Colour (0xff78678b); }
    static juce::Colour catBell()        { return juce::Colour (0xffa38a54); }
    static juce::Colour catStrings()     { return juce::Colour (0xff557f76); }
    static juce::Colour catOrgan()       { return juce::Colour (0xff94643f); }
    static juce::Colour catLead()        { return juce::Colour (0xff55728f); }
    static juce::Colour catPlugin()      { return juce::Colour (0xff96643f); }

    // Marca vetorial para superficies aceleradas. Alguns drivers OpenGL Android
    // renderizam PNGs do BinaryData como textura preta, embora a mesma arte
    // apareca corretamente na splash. Cruz + igreja + teclas preservam a
    // identidade sem depender de upload de bitmap no contexto do mixer.
    static void drawChurchDAWMark (juce::Graphics& g, juce::Rectangle<float> bounds)
    {
        if (bounds.isEmpty())
            return;

        juce::Graphics::ScopedSaveState state (g);
        const float side = juce::jmin (bounds.getWidth(), bounds.getHeight());
        auto box = bounds.withSizeKeepingCentre (side, side).reduced (side * 0.035f);
        const float x = box.getX();
        const float y = box.getY();
        const float w = box.getWidth();
        const float h = box.getHeight();
        const float cx = box.getCentreX();
        const float radius = side * 0.21f;

        juce::Path plate;
        plate.addRoundedRectangle (box, radius);
        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.fillRoundedRectangle (box.translated (0.0f, 2.0f), radius);

        // Cor solida aqui e deliberada: o mesmo driver que recusou a textura
        // raster tambem escurecia o gradiente do plate. O brilho e criado por
        // contornos vetoriais, que permanecem identicos em qualquer GPU.
        g.setColour (juce::Colour (0xff790018));
        g.fillPath (plate);

        g.setColour (juce::Colours::white.withAlpha (0.45f));
        g.drawRoundedRectangle (box.reduced (side * 0.025f), radius * 0.88f,
                                juce::jmax (1.0f, side * 0.018f));
        g.setColour (juce::Colour (0xffff6a7e).withAlpha (0.9f));
        g.drawRoundedRectangle (box, radius, juce::jmax (1.0f, side * 0.025f));

        // Ondas de audio discretas dos dois lados da igreja.
        for (int i = 0; i < 4; ++i)
        {
            const float barH = h * (0.10f + 0.035f * (float) i);
            const float leftX = x + w * (0.10f + 0.045f * (float) i);
            const float rightX = x + w - (leftX - x);
            const float barW = juce::jmax (1.0f, side * 0.024f);
            g.setColour (juce::Colours::white.withAlpha (0.28f + 0.08f * (float) i));
            g.fillRoundedRectangle (leftX, y + h * 0.51f - barH * 0.5f,
                                    barW, barH, side * 0.012f);
            g.fillRoundedRectangle (rightX - barW, y + h * 0.51f - barH * 0.5f,
                                    barW, barH, side * 0.012f);
        }

        // Silhueta clara da igreja.
        juce::Path church;
        church.startNewSubPath (cx, y + h * 0.27f);
        church.lineTo (x + w * 0.28f, y + h * 0.47f);
        church.lineTo (x + w * 0.33f, y + h * 0.47f);
        church.lineTo (x + w * 0.33f, y + h * 0.83f);
        church.lineTo (x + w * 0.67f, y + h * 0.83f);
        church.lineTo (x + w * 0.67f, y + h * 0.47f);
        church.lineTo (x + w * 0.72f, y + h * 0.47f);
        church.closeSubPath();
        g.setColour (juce::Colour (0xfff8f5f1));
        g.fillPath (church);
        g.setColour (juce::Colour (0xff4a050f));
        g.strokePath (church, juce::PathStrokeType (juce::jmax (1.0f, side * 0.025f),
                                                    juce::PathStrokeType::curved,
                                                    juce::PathStrokeType::rounded));

        // Vitral central.
        auto window = juce::Rectangle<float> (cx - w * 0.055f, y + h * 0.39f,
                                              w * 0.11f, h * 0.18f);
        g.setColour (juce::Colour (0xff6c0717));
        g.fillRoundedRectangle (window, w * 0.045f);
        g.setColour (juce::Colour (0xffff3655));
        g.drawRoundedRectangle (window, w * 0.045f, juce::jmax (1.0f, side * 0.014f));

        // Teclado integrado a fachada.
        auto keys = juce::Rectangle<float> (x + w * 0.31f, y + h * 0.57f,
                                            w * 0.38f, h * 0.27f);
        g.setColour (juce::Colour (0xfff7f5f2));
        g.fillRoundedRectangle (keys, side * 0.025f);
        g.setColour (juce::Colour (0xff4a4545));
        g.drawRoundedRectangle (keys, side * 0.025f, juce::jmax (1.0f, side * 0.014f));

        const float whiteW = keys.getWidth() / 5.0f;
        for (int i = 1; i < 5; ++i)
            g.drawVerticalLine (juce::roundToInt (keys.getX() + whiteW * (float) i),
                                keys.getY(), keys.getBottom());

        g.setColour (juce::Colour (0xff171315));
        for (int i : { 1, 2, 4 })
        {
            const float keyX = keys.getX() + whiteW * (float) i - whiteW * 0.20f;
            g.fillRoundedRectangle (keyX, keys.getY(), whiteW * 0.40f,
                                    keys.getHeight() * 0.61f, side * 0.012f);
        }

        // Cruz acima do telhado.
        g.setColour (juce::Colours::white);
        const float crossW = juce::jmax (1.5f, side * 0.045f);
        g.fillRoundedRectangle (cx - crossW * 0.5f, y + h * 0.075f,
                                crossW, h * 0.19f, crossW * 0.45f);
        g.fillRoundedRectangle (cx - w * 0.075f, y + h * 0.12f,
                                w * 0.15f, crossW, crossW * 0.45f);
    }

    // ---- medidores: faixas de nivel em dBFS ----
    static juce::Colour meterOff()       { return juce::Colour (0xff252a31); }   // segmento apagado
    static juce::Colour meterVeryLow()   { return juce::Colour (0xff234a2d); }   // -60 a -30
    static juce::Colour meterLow()       { return juce::Colour (0xff2e7d32); }   // -30 a -12
    static juce::Colour meterMid()       { return juce::Colour (0xff43a047); }   // -12 a -6
    static juce::Colour meterWarn()      { return juce::Colour (0xfff9a825); }   //  -6 a -3
    static juce::Colour meterHot()       { return juce::Colour (0xffe53935); }   //  -3 a  0

    // ---- espacamento e tamanho (unidades logicas) ----
    static constexpr int spaceXs = 4;
    static constexpr int spaceSm = 8;
    static constexpr int spaceMd = 12;
    static constexpr int spaceLg = 16;
    static constexpr int knobVisual = 32;    // diametro desenhado
    static constexpr int hitMouse = 32;      // alvo minimo de mouse
    static constexpr int hitTouch = 44;      // alvo minimo de toque
    static constexpr int meterChanW = 21;    // medidor de canal (L+R)
    static constexpr int meterMasterW = 23;  // medidor do master

    // ---- raio de canto: TRES escalas, nao cinco ----
    // Premium: um pouco mais generoso no card/painel (vidro, nao tijolo).
    static constexpr float radiusControl = 6.0f;   // botoes, combos, chips
    static constexpr float radiusCard    = CHURCHDAW_CHURCHLIVE_LOOK ? 18.0f : 10.0f;  // cards de canal
    static constexpr float radiusPanel   = 14.0f;  // paineis e visores

    // LEDs do painel. Nas fotos: VERDE nas escadas de nivel dos faders, VERMELHO
    // nas drawbars do orgao, AMBAR nos ON/OFF de camada.
    static juce::Colour ledGreen()       { return juce::Colour (0xff5ede3a); }
    static juce::Colour ledGreenDim()    { return juce::Colour (0xff2f8f22); }
    static juce::Colour ledRed()         { return juce::Colour (0xffff2a18); }
    static juce::Colour ledAmber()       { return juce::Colour (0xffffa41f); }
    static juce::Colour ledOff()         { return juce::Colour (0xff1b2a19); }   // segmento apagado

    // Paineis: a Nord NAO usa preto. Seccao escura = azul-ardosia; seccao clara =
    // cinza-prata. E o contraste entre os dois que organiza o painel deles.
    static juce::Colour slate()          { return juce::Colour (0xff3d4655); }
    static juce::Colour slateDark()      { return juce::Colour (0xff2b3240); }   // barra de titulo da seccao
    static juce::Colour slateLight()     { return juce::Colour (0xff4a5466); }

    static juce::Colour oledAmber()      { return juce::Colour (0xffffb648); }   // texto do visor
    static juce::Colour oledBack()       { return juce::Colour (0xff0a0906); }   // vidro do visor

    // Rotulo de palco: caixa alta, pesado, bem espacado. E o que a Nord faz em
    // silkscreen -- legivel de longe, no escuro, sem precisar chegar perto.
    static juce::Font stageLabel (float height)
    {
        return juce::Font (juce::FontOptions (height, juce::Font::bold)).withExtraKerningFactor (0.14f);
    }
    // nomes antigos mantidos como apelidos dos tokens, para nao reescrever as
    // centenas de chamadas espalhadas de uma vez
    static juce::Colour background()     { return bgApp(); }
    static juce::Colour panel()          { return bgCard(); }
    static juce::Colour panelOutline()   { return borderDefault(); }
    static juce::Colour widget()         { return bgControl(); }
    static juce::Colour textColour()     { return txtPrimary(); }
    static juce::Colour dimText()        { return txtSecondary(); }

    NordLookAndFeel()
    {
        auto scheme = getDarkColourScheme();
        using UI = juce::LookAndFeel_V4::ColourScheme::UIColour;
        scheme.setUIColour (UI::windowBackground, background());
        scheme.setUIColour (UI::widgetBackground, widget());
        scheme.setUIColour (UI::menuBackground, panel());
        scheme.setUIColour (UI::outline, panelOutline());
        scheme.setUIColour (UI::defaultText, textColour());
        scheme.setUIColour (UI::defaultFill, nordRed());
        scheme.setUIColour (UI::highlightedText, juce::Colours::white);
        scheme.setUIColour (UI::highlightedFill, nordRed());
        scheme.setUIColour (UI::menuText, textColour());
        setColourScheme (scheme);

        setColour (juce::TextButton::buttonColourId, widget());
        setColour (juce::TextButton::buttonOnColourId, nordRed());
        setColour (juce::TextButton::textColourOffId, textColour());
        setColour (juce::TextButton::textColourOnId, juce::Colours::white);

        setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff0c0d10));
        setColour (juce::ComboBox::outlineColourId, juce::Colours::black);
        setColour (juce::ComboBox::arrowColourId, nordRedBright());
        // texto do mini-visor: branco levemente quente, como fosforo de tela
        setColour (juce::ComboBox::textColourId, juce::Colour (0xfff2efe6));

        setColour (juce::PopupMenu::backgroundColourId, panel());
        // vermelho chapado de ponta a ponta era pesado demais numa lista longa:
        // vinho escuro mantem a familia da marca sem gritar
        setColour (juce::PopupMenu::highlightedBackgroundColourId, juce::Colour (0xff4a1622));
        setColour (juce::PopupMenu::textColourId, textColour());
        setColour (juce::PopupMenu::highlightedTextColourId, juce::Colours::white);

        // cabecalho da tabela de plugins: vinha cinza-claro com texto preto,
        // de outro tema
        setColour (juce::TableHeaderComponent::backgroundColourId, juce::Colour (0xff23262d));
        setColour (juce::TableHeaderComponent::textColourId, dimText());
        setColour (juce::TableHeaderComponent::outlineColourId, panelOutline());
        setColour (juce::TableHeaderComponent::highlightColourId, juce::Colour (0xff2e323a));

        setColour (juce::Slider::rotarySliderFillColourId, nordRedBright());   // padrao do arco
        setColour (juce::Slider::thumbColourId, nordRedBright());
        setColour (juce::Slider::trackColourId, juce::Colour (0xff3c3f46));
        setColour (juce::Slider::backgroundColourId, widget());
        setColour (juce::Slider::textBoxTextColourId, textColour());
        setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);

        setColour (juce::ToggleButton::textColourId, juce::Colours::white);
        setColour (juce::ToggleButton::tickColourId, juce::Colours::white);
        setColour (juce::ToggleButton::tickDisabledColourId, juce::Colours::white.withAlpha (0.6f));

        setColour (juce::Label::textColourId, textColour());
        // barra de rolagem NEUTRA: em vermelho ela parecia uma barra de progresso
        setColour (juce::ScrollBar::thumbColourId, juce::Colour (0xff4a4e57));
        setColour (juce::ScrollBar::trackColourId, juce::Colour (0xff1a1c21));
        setColour (juce::AlertWindow::backgroundColourId, panel());
        setColour (juce::ListBox::backgroundColourId, panel());
        setColour (juce::ResizableWindow::backgroundColourId, background());
        setColour (juce::DocumentWindow::textColourId, textColour());

       #if ! CHURCHDAW_MOBILE_SHELL
        // Detached menus and dialogs use the application's shared look and feel,
        // but have no shell ancestor. Keep their palette in this same layer.
        // A translucent window flag allows genuinely rounded menu corners.
        setColour (juce::PopupMenu::backgroundColourId, bgFloating().withAlpha (0.99f));
        setColour (juce::PopupMenu::highlightedBackgroundColourId, bgFloatingHover());
        setColour (juce::PopupMenu::headerTextColourId, txtSecondary());
        setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff191d24));
        setColour (juce::TextEditor::textColourId, txtPrimary());
        setColour (juce::TextEditor::outlineColourId, borderDefault().withAlpha (0.75f));
        setColour (juce::TextEditor::focusedOutlineColourId, focusOutline());
        setColour (juce::TextEditor::highlightColourId, nordRed().withAlpha (0.32f));
        setColour (juce::TextEditor::highlightedTextColourId, txtPrimary());
        setColour (juce::CaretComponent::caretColourId, txtPrimary());
        setColour (juce::TooltipWindow::backgroundColourId, bgFloating());
        setColour (juce::TooltipWindow::outlineColourId, juce::Colour (0xff414752));
        setColour (juce::TooltipWindow::textColourId, txtPrimary());
        setColour (juce::ComboBox::textColourId, txtPrimary());
        setColour (juce::ComboBox::arrowColourId, txtSecondary());
       #else
        setColour (juce::PopupMenu::backgroundColourId, juce::Colour (0xff181c24));
        setColour (juce::PopupMenu::highlightedBackgroundColourId, juce::Colour (0xff4d1826));
        setColour (juce::PopupMenu::headerTextColourId, txtSecondary());
        setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff141820));
        setColour (juce::TextEditor::textColourId, txtPrimary());
        setColour (juce::TextEditor::outlineColourId, juce::Colour (0xff2f3643));
        setColour (juce::TextEditor::focusedOutlineColourId, nordRedBright());
        setColour (juce::TextEditor::highlightColourId, nordRed().withAlpha (0.34f));
        setColour (juce::TextEditor::highlightedTextColourId, juce::Colours::white);
        setColour (juce::CaretComponent::caretColourId, nordRedBright());
       #endif
    }

    //==============================================================================
    // Titlebar propria do ChurchDAW. A moldura nativa branca era o ultimo bloco
    // claro fora da linguagem do app; estes botoes continuam sendo os controles
    // padrao da DocumentWindow, apenas recebem desenho de console escuro.
    class PremiumTitleBarButton final : public juce::Button
    {
    public:
        explicit PremiumTitleBarButton (int buttonType)
            : juce::Button (buttonType == juce::DocumentWindow::closeButton ? "close"
                            : buttonType == juce::DocumentWindow::maximiseButton ? "maximise"
                                                                                 : "minimise"),
              type (buttonType)
        {
            setWantsKeyboardFocus (false);
        }

        void paintButton (juce::Graphics& g, bool highlighted, bool down) override
        {
            const auto bounds = getLocalBounds().toFloat();
            const bool close = type == juce::DocumentWindow::closeButton;

            if (highlighted || down)
            {
                const auto hover = close ? juce::Colour (0xffb51f38)
                                         : juce::Colours::white;
                g.setColour (hover.withAlpha (down ? (close ? 0.92f : 0.105f)
                                                   : (close ? 0.74f : 0.060f)));
                g.fillRect (bounds);
            }

            g.setColour (juce::Colours::white.withAlpha (0.026f));
            g.fillRect (bounds.getX(), bounds.getY() + 7.0f,
                        1.0f, juce::jmax (1.0f, bounds.getHeight() - 14.0f));

            const float alpha = isEnabled() ? (highlighted ? 0.94f : 0.70f) : 0.28f;
            g.setColour (juce::Colours::white.withAlpha (alpha));
            const auto c = bounds.getCentre();

            if (type == juce::DocumentWindow::closeButton)
            {
                g.drawLine (c.x - 4.3f, c.y - 4.3f,
                            c.x + 4.3f, c.y + 4.3f, 1.25f);
                g.drawLine (c.x + 4.3f, c.y - 4.3f,
                            c.x - 4.3f, c.y + 4.3f, 1.25f);
            }
            else if (type == juce::DocumentWindow::minimiseButton)
            {
                g.drawLine (c.x - 5.0f, c.y + 3.5f,
                            c.x + 5.0f, c.y + 3.5f, 1.15f);
            }
            else
            {
                if (getToggleState())
                {
                    g.drawRect (juce::Rectangle<float> (c.x - 3.0f, c.y - 4.5f,
                                                        8.0f, 7.0f), 1.0f);
                    g.drawRect (juce::Rectangle<float> (c.x - 5.0f, c.y - 2.5f,
                                                        8.0f, 7.0f), 1.0f);
                }
                else
                {
                    g.drawRect (juce::Rectangle<float> (c.x - 5.0f, c.y - 4.0f,
                                                        10.0f, 8.0f), 1.0f);
                }
            }
        }

    private:
        int type = 0;
    };

    juce::Button* createDocumentWindowButton (int buttonType) override
    {
        return new PremiumTitleBarButton (buttonType);
    }

    void positionDocumentWindowButtons (juce::DocumentWindow&,
                                        int titleBarX, int titleBarY,
                                        int titleBarW, int titleBarH,
                                        juce::Button* minimiseButton,
                                        juce::Button* maximiseButton,
                                        juce::Button* closeButton,
                                        bool positionOnLeft) override
    {
        constexpr int buttonW = 46;
        int x = positionOnLeft ? titleBarX : titleBarX + titleBarW;

        auto place = [&] (juce::Button* button)
        {
            if (button == nullptr)
                return;

            if (positionOnLeft)
            {
                button->setBounds (x, titleBarY, buttonW, titleBarH);
                x += buttonW;
            }
            else
            {
                x -= buttonW;
                button->setBounds (x, titleBarY, buttonW, titleBarH);
            }
        };

        place (closeButton);
        place (maximiseButton);
        place (minimiseButton);
    }

    void drawDocumentWindowTitleBar (juce::DocumentWindow& window,
                                     juce::Graphics& g, int w, int h,
                                     int titleSpaceX, int titleSpaceW,
                                     const juce::Image* icon,
                                     bool drawTitleTextOnLeft) override
    {
        juce::ignoreUnused (drawTitleTextOnLeft);
        if (w <= 0 || h <= 0)
            return;

        const auto bounds = juce::Rectangle<float> (0.0f, 0.0f,
                                                     (float) w, (float) h);
        g.setColour (bgApp());
        g.fillRect (bounds);
        g.setColour (borderSoft());
        g.fillRect (0.0f, (float) h - 1.0f, (float) w, 1.0f);

        int textX = juce::jmax (10, titleSpaceX);
        if (icon != nullptr && icon->isValid())
        {
            constexpr int iconSide = 18;
            const int iconY = (h - iconSide) / 2;
            g.setOpacity (window.isActiveWindow() ? 0.92f : 0.62f);
            g.drawImageWithin (*icon, textX, iconY, iconSide, iconSide,
                               juce::RectanglePlacement::centred, false);
            g.setOpacity (1.0f);
            textX += iconSide + 8;
        }
        else
        {
            g.setColour (nordRedBright().withAlpha (0.88f));
            g.fillRoundedRectangle ((float) textX, (float) h * 0.5f - 1.5f,
                                    11.0f, 3.0f, 1.5f);
            textX += 18;
        }

        const int textRight = juce::jmin (w - 8, titleSpaceX + titleSpaceW);
        g.setColour (txtPrimary().withAlpha (window.isActiveWindow() ? 0.92f : 0.54f));
        g.setFont (juce::Font (juce::FontOptions (11.5f, juce::Font::bold))
                       .withExtraKerningFactor (0.045f));
        g.drawFittedText (window.getName(), textX, 0,
                          juce::jmax (1, textRight - textX), h,
                          juce::Justification::centredLeft, 1, 0.82f);
    }

    // The desktop shell opts in at its root. Compact/touch surfaces keep their
    // existing measurements and rendering, including dialogs outside the shell.
    static bool usesDesktopShell (const juce::Component& component)
    {
       #if CHURCHDAW_MOBILE_SHELL
        juce::ignoreUnused (component);
        return false;
       #else
        for (auto* current = &component; current != nullptr; current = current->getParentComponent())
            if (current->getProperties().contains ("churchDesktopShell"))
                return (bool) current->getProperties()["churchDesktopShell"];
        return false;
       #endif
    }

    // Layout opt-in remains separate from control styling: detached desktop
    // dialogs must not fall back to the older controls merely because they
    // have no MainComponent ancestor. An explicit compact-shell opt-out wins.
    static bool usesDesktopControls (const juce::Component& component)
    {
       #if CHURCHDAW_MOBILE_SHELL
        juce::ignoreUnused (component);
        return false;
       #else
        for (auto* current = &component; current != nullptr; current = current->getParentComponent())
            if (current->getProperties().contains ("churchDesktopShell"))
                return (bool) current->getProperties()["churchDesktopShell"];
        return true;
       #endif
    }

    static void drawDesktopNavigationIcon (juce::Graphics& g, const juce::String& id,
                                            juce::Rectangle<float> icon, bool recording)
    {
        const float x = icon.getX(), y = icon.getY(), side = icon.getWidth();
        const auto point = [=] (float px, float py)
        { return juce::Point<float> (x + side * px, y + side * py); };
        const auto stroke = juce::PathStrokeType (1.35f, juce::PathStrokeType::curved,
                                                         juce::PathStrokeType::rounded);
        juce::Path path;
        if (id == "navInstruments")
        {
            path.addRoundedRectangle (icon.reduced (0.8f, 1.5f), 2.0f);
            for (int key = 1; key < 4; ++key)
            {
                path.startNewSubPath (point ((float) key * 0.25f, 0.12f));
                path.lineTo (point ((float) key * 0.25f, 0.88f));
            }
        }
        else if (id == "navLive")
        {
            for (int bar = 0; bar < 5; ++bar)
            {
                const float extent = 0.16f + 0.12f * (float) (2 - std::abs (2 - bar));
                path.startNewSubPath (point (0.12f + bar * 0.19f, 0.5f - extent));
                path.lineTo (point (0.12f + bar * 0.19f, 0.5f + extent));
            }
        }
        else if (id == "navMidi")
        {
            path.addEllipse (icon.reduced (1.0f));
            for (const auto pin : { point (0.28f, 0.55f), point (0.38f, 0.34f),
                                    point (0.62f, 0.34f), point (0.72f, 0.55f) })
                g.fillEllipse (pin.x - 0.8f, pin.y - 0.8f, 1.6f, 1.6f);
            path.startNewSubPath (point (0.39f, 0.77f));
            path.lineTo (point (0.61f, 0.77f));
        }
        else if (id == "navSave")
        {
            path.startNewSubPath (point (0.18f, 0.51f));
            path.lineTo (point (0.41f, 0.73f));
            path.lineTo (point (0.83f, 0.26f));
        }
        else if (id == "navSaveFailed")
        {
            path.startNewSubPath (point (0.5f, 0.09f));
            path.lineTo (point (0.93f, 0.88f));
            path.lineTo (point (0.07f, 0.88f));
            path.closeSubPath();
            path.startNewSubPath (point (0.5f, 0.35f));
            path.lineTo (point (0.5f, 0.57f));
            g.fillEllipse (x + side * 0.5f - 0.8f, y + side * 0.73f - 0.8f, 1.6f, 1.6f);
        }
        else if (id == "navPad")
        {
            const float cell = side * 0.34f;
            for (int row = 0; row < 2; ++row)
                for (int col = 0; col < 2; ++col)
                    path.addRoundedRectangle (x + side * (0.08f + col * 0.5f),
                                              y + side * (0.08f + row * 0.5f), cell, cell, 2.0f);
        }
        else if (id == "navPlugins")
        {
            path.startNewSubPath (point (0.35f, 0.08f));
            path.lineTo (point (0.35f, 0.3f));
            path.startNewSubPath (point (0.66f, 0.08f));
            path.lineTo (point (0.66f, 0.3f));
            path.startNewSubPath (point (0.21f, 0.32f));
            path.lineTo (point (0.79f, 0.32f));
            path.lineTo (point (0.79f, 0.49f));
            path.quadraticTo (point (0.79f, 0.76f), point (0.5f, 0.76f));
            path.quadraticTo (point (0.21f, 0.76f), point (0.21f, 0.49f));
            path.closeSubPath();
            path.startNewSubPath (point (0.5f, 0.76f));
            path.lineTo (point (0.5f, 0.94f));
        }
        else if (id == "navLiveCam")
        {
            if (recording)
            {
                g.fillEllipse (icon.reduced (4.0f));
                return;
            }
            path.addRoundedRectangle (x + side * 0.06f, y + side * 0.25f,
                                      side * 0.60f, side * 0.5f, 2.0f);
            path.startNewSubPath (point (0.67f, 0.39f));
            path.lineTo (point (0.93f, 0.26f));
            path.lineTo (point (0.93f, 0.74f));
            path.lineTo (point (0.67f, 0.61f));
        }
        else if (id == "navLibraries")
        {
            path.addRoundedRectangle (x + side * 0.08f, y + side * 0.15f,
                                      side * 0.55f, side * 0.72f, 1.5f);
            path.startNewSubPath (point (0.29f, 0.17f));
            path.lineTo (point (0.29f, 0.86f));
            path.startNewSubPath (point (0.78f, 0.15f));
            path.lineTo (point (0.95f, 0.84f));
        }
        else if (id == "navClear")
        {
            // Lixeira: tampa com alca, corpo afunilado e duas ranhuras.
            path.startNewSubPath (point (0.14f, 0.24f));
            path.lineTo (point (0.86f, 0.24f));
            path.startNewSubPath (point (0.38f, 0.24f));
            path.lineTo (point (0.38f, 0.12f));
            path.lineTo (point (0.62f, 0.12f));
            path.lineTo (point (0.62f, 0.24f));
            path.startNewSubPath (point (0.24f, 0.24f));
            path.lineTo (point (0.29f, 0.88f));
            path.lineTo (point (0.71f, 0.88f));
            path.lineTo (point (0.76f, 0.24f));
            path.startNewSubPath (point (0.42f, 0.42f));
            path.lineTo (point (0.42f, 0.7f));
            path.startNewSubPath (point (0.58f, 0.42f));
            path.lineTo (point (0.58f, 0.7f));
        }
        else if (id == "headerNav" || id == "navAudio")
        {
            for (int lane = 0; lane < 3; ++lane)
            {
                const float px = 0.18f + lane * 0.32f;
                const float py = lane == 1 ? 0.63f : 0.35f;
                path.startNewSubPath (point (px, 0.12f));
                path.lineTo (point (px, py - 0.1f));
                path.startNewSubPath (point (px, py + 0.1f));
                path.lineTo (point (px, 0.88f));
                path.addRoundedRectangle (x + side * (px - 0.11f), y + side * (py - 0.09f),
                                          side * 0.22f, side * 0.18f, 1.0f);
            }
        }
        g.strokePath (path, stroke);
    }

    static juce::String saveStatusLabel (const juce::TextButton& button)
    {
        // MainComponent owns persistence state. Remove only its legacy prefix
        // glyphs because this look and feel draws the equivalent vector icon.
        return button.getButtonText().trimCharactersAtStart (
            juce::String::fromUTF8 ("\xe2\x9c\x93\xe2\x9a\xa0 "));
    }

    static void drawDesktopNavigationContents (juce::Graphics& g, juce::TextButton& button,
                                                bool highlighted, bool selected)
    {
        const auto id = button.getComponentID();
        const bool saved = id == "navSave";
        const bool recording = (bool) button.getProperties()["recording"];
        const float alpha = button.isEnabled() ? 1.0f : 0.40f;
        auto area = button.getLocalBounds().reduced (10, 3);
        if (area.isEmpty()) return;
        const float iconSide = saved ? 15.0f : 17.0f;
        const float gap = saved ? 5.0f : 8.0f;
        const auto text = saved ? saveStatusLabel (button) : button.getButtonText();
        const bool saveFailed = saved && text.containsIgnoreCase ("falhou");
        const auto statusColour = button.findColour (juce::TextButton::textColourOffId);
        const auto font = button.getLookAndFeel().getTextButtonFont (button, button.getHeight());
        const float textWidth = juce::GlyphArrangement::getStringWidth (font, text);
        const float groupWidth = juce::jmin ((float) area.getWidth(), iconSide + gap + textWidth);
        const float startX = area.getCentreX() - groupWidth * 0.5f;
        const auto icon = juce::Rectangle<float> (startX, area.getCentreY() - iconSide * 0.5f,
                                                 iconSide, iconSide);
        g.setColour ((recording ? nordRedBright() : saved ? statusColour
                          : selected ? juce::Colour (0xffffbec8) : txtSecondary()).withMultipliedAlpha (alpha));
        drawDesktopNavigationIcon (g, saveFailed ? juce::String ("navSaveFailed") : id, icon, recording);
        g.setColour ((saveFailed ? statusColour : saved ? txtSecondary()
                                  : selected || highlighted ? juce::Colour (0xfff6f2ef)
                                                            : juce::Colour (0xffb7bdc7)).withMultipliedAlpha (alpha));
        g.setFont (font);
        auto textArea = area.withLeft (juce::roundToInt (startX + iconSide + gap));
        g.drawFittedText (text, textArea, juce::Justification::centredLeft, 1, 0.84f);
    }

    // Pilula unica da barra de navegacao mobile (tambem usada pelo botao Audio).
    static void drawTouchNavPill (juce::Graphics& g, juce::Rectangle<float> bounds, bool selected,
                                  bool accentRim, bool hover, bool down, float alpha)
    {
        auto pill = bounds.reduced (0.0f, 2.0f);
        if (pill.isEmpty()) return;
        const float radius = juce::jmin (8.0f, pill.getHeight() * 0.22f);

        if (selected)
        {
            const auto top = juce::Colour (0xff4a1b28), bottom = juce::Colour (0xff2a0f17);
            g.setGradientFill (juce::ColourGradient (
                (down ? top.darker (0.12f) : top).withMultipliedAlpha (alpha), pill.getX(), pill.getY(),
                (down ? bottom.darker (0.12f) : bottom).withMultipliedAlpha (alpha), pill.getX(), pill.getBottom(), false));
            g.fillRoundedRectangle (pill, radius);
            g.setColour (nordRedBright().withAlpha (0.55f * alpha));
            g.drawRoundedRectangle (pill.reduced (0.5f), radius, 1.0f);
            const float barW = juce::jlimit (16.0f, 26.0f, pill.getWidth() * 0.34f);
            g.setColour (nordRedBright().withAlpha (0.95f * alpha));
            g.fillRoundedRectangle (pill.getCentreX() - barW * 0.5f, pill.getBottom() - 3.0f, barW, 2.0f, 1.0f);
            return;
        }

        g.setColour ((accentRim ? nordRed().withAlpha (down ? 0.20f : hover ? 0.16f : 0.11f)
                                : juce::Colours::white.withAlpha (down ? 0.085f : hover ? 0.060f : 0.035f))
                         .withMultipliedAlpha (alpha));
        g.fillRoundedRectangle (pill, radius);
        g.setColour ((accentRim ? nordRedBright().withAlpha (0.50f)
                                : juce::Colours::white.withAlpha (hover ? 0.12f : 0.065f))
                         .withMultipliedAlpha (alpha));
        g.drawRoundedRectangle (pill.reduced (0.5f), radius, 1.0f);
    }

   #if CHURCHDAW_CHURCHLIVE_LOOK
    // Fonte padrao do app mobile: Barlow empacotada (nao a do sistema). Troca
    // so as fontes "padrao" (sans e monoespacada); nomes explicitos seguem.
    juce::Typeface::Ptr getTypefaceForFont (const juce::Font& font) override
    {
        if (font.getTypefaceName() == juce::Font::getDefaultSansSerifFontName())
        {
            if (auto face = ChurchLiveSkin::typefaceFor (font.isBold() ? ChurchLiveSkin::Face::barlowBold
                                                                       : ChurchLiveSkin::Face::barlow))
                return face;
        }
        else if (font.getTypefaceName() == juce::Font::getDefaultMonospacedFontName())
        {
            if (auto face = ChurchLiveSkin::typefaceFor (ChurchLiveSkin::Face::mono))
                return face;
        }

        return juce::LookAndFeel_V4::getTypefaceForFont (font);
    }
   #endif

    void drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour& backgroundColour,
                               bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override
    {
        const auto id = button.getComponentID();
        const bool nav = id.startsWith ("nav") || id == "headerNav";
        const bool desktopNavigation = nav && usesDesktopShell (button);
        // Semantic channel overlays stay transparent. Desktop navigation still
        // needs hover/focus feedback when its resting surface is transparent.
        const bool touchNavPill = nav && (bool) button.getProperties()["churchTouchNav"];
        if (backgroundColour.isTransparent() && ! desktopNavigation && ! touchNavPill) return;
        auto bounds = button.getLocalBounds().toFloat().reduced (1.0f);
        if (bounds.isEmpty()) return;
        // Dark graphite can be saturated blue; only semantic accent colours
        // indicate a primary/selected button when the caller uses colours as state.
        const bool redAccent = backgroundColour.getRed() > backgroundColour.getGreen() * 1.35f
                                && backgroundColour.getRed() > backgroundColour.getBlue() * 1.10f;
        const bool greenAccent = backgroundColour.getHue() > 0.20f
                                  && backgroundColour.getHue() < 0.48f
                                  && backgroundColour.getBrightness() > 0.30f;
        const bool active = button.getToggleState()
                             || (backgroundColour.getSaturation() > 0.30f && (redAccent || greenAccent));
        if (desktopNavigation)
        {
            const bool saved = id == "navSave";
            const bool selected = active && ! saved;
            const bool focus = button.hasKeyboardFocus (true);
            const float alpha = button.isEnabled() ? 1.0f : 0.38f;
            const bool hover = shouldDrawButtonAsHighlighted || shouldDrawButtonAsDown;
            const auto surface = selected ? juce::Colour (0xff30242c)
                                 : hover ? juce::Colour (0xff242931) : juce::Colour (0xff191d24);
            if (selected || hover || focus)
            {
                g.setColour ((shouldDrawButtonAsDown ? surface.darker (0.10f) : surface).withAlpha (alpha));
                g.fillRoundedRectangle (bounds, 9.0f);
                if (focus)
                {
                    g.setColour (nordRedBright().withAlpha (alpha));
                    g.drawRoundedRectangle (bounds.reduced (0.5f), 9.0f, 1.5f);
                }
            }
            if (selected)
            {
                const float cueWidth = juce::jlimit (16.0f, 26.0f, bounds.getWidth() * 0.22f);
                g.setColour (nordRedBright().withAlpha (0.78f * alpha));
                g.fillRoundedRectangle (bounds.getCentreX() - cueWidth * 0.5f,
                                        bounds.getBottom() - 2.0f, cueWidth, 2.0f, 1.0f);
            }
            return;
        }
        if (usesDesktopControls (button))
        {
            const bool danger = id == "panicOutline";
            const bool emptyAdd = id == "emptyChannelAdd";
            const bool focus = button.hasKeyboardFocus (true);
            const bool hover = shouldDrawButtonAsHighlighted || shouldDrawButtonAsDown;
            const float alpha = button.isEnabled() ? 1.0f : 0.4f;
            const bool amberAccent = backgroundColour.getHue() >= 0.08f
                                      && backgroundColour.getHue() <= 0.18f
                                      && backgroundColour.getSaturation() > 0.30f
                                      && backgroundColour.getBrightness() > 0.30f;
            const auto accent = id == "consoleSolo" || amberAccent ? amberSolo()
                              : greenAccent ? greenOk() : nordRed();
            // Library availability is contextual guidance, not a selected
            // channel. Its call to action stays neutral even if its old colour
            // was red; the empty-channel message carries the context.
            const bool selectedControl = active && ! emptyAdd;
            const auto base = emptyAdd ? juce::Colour (0xff252a32)
                            : selectedControl ? bgControl().interpolatedWith (accent, 0.12f)
                            : danger ? juce::Colour (0xff30232a) : juce::Colour (0xff292e36);
            const auto surface = shouldDrawButtonAsDown ? base.darker (0.08f)
                               : hover ? base.brighter (0.07f) : base;
            const float radius = juce::jmin (7.0f, bounds.getHeight() * 0.25f);
            g.setColour (surface.withAlpha (alpha));
            g.fillRoundedRectangle (bounds, radius);
            g.setColour ((focus ? nordRedBright() : selectedControl ? accent.withAlpha (0.30f)
                                : danger ? nordRed().withAlpha (0.26f)
                                : juce::Colours::white.withAlpha (hover ? 0.14f : emptyAdd ? 0.10f : 0.08f))
                             .withMultipliedAlpha (alpha));
            g.drawRoundedRectangle (bounds.reduced (0.5f), radius, focus ? 1.5f : 1.0f);
            return;
        }
        const bool danger = id == "panicOutline";
        const bool emptyAdd = id == "emptyChannelAdd";
        const bool solo = id == "consoleSolo";
        const bool amberAccent = backgroundColour.getHue() >= 0.07f
                                  && backgroundColour.getHue() <= 0.18f
                                  && backgroundColour.getSaturation() > 0.24f
                                  && backgroundColour.getBrightness() > 0.18f;
        const auto accent = solo || amberAccent ? amberSolo()
                          : greenAccent ? greenOk() : nordRed();
        const bool activeOrAmber = active || amberAccent;
        const bool primaryCta = redAccent && backgroundColour.getBrightness() >= 0.52f && ! emptyAdd;
        const float alpha = button.isEnabled() ? 1.0f : 0.42f;
        const float radius = juce::jmin (8.0f, bounds.getHeight() * 0.24f);

        if (nav && touchNavPill)
        {
            // Barra mobile: TODOS os destinos usam a mesma pilula, mesma altura.
            // Selecionado = estado real (toggle), nao a cor; o atalho Timbres
            // (CTA vermelho fixo) vira so um aro carmim, sem competir com a aba ativa.
            const bool selected = button.getToggleState() && id != "navSave";
            drawTouchNavPill (g, bounds, selected, ! selected && primaryCta,
                              shouldDrawButtonAsHighlighted, shouldDrawButtonAsDown, alpha);
            return;
        }

        if (nav)
        {
            const bool saved = id == "navSave";
            const bool selectedNav = active && ! saved;
            const bool touchNav = (bool) button.getProperties()["churchTouchNav"];
            if (selectedNav)
            {
                const auto topCol = primaryCta ? juce::Colour (0xffad1e35) : juce::Colour (0xff461a26);
                const auto botCol = primaryCta ? juce::Colour (0xff7d1324) : juce::Colour (0xff2b1018);
                g.setGradientFill (juce::ColourGradient (
                    (shouldDrawButtonAsDown ? topCol.darker (0.12f) : topCol).withMultipliedAlpha (alpha),
                    bounds.getX(), bounds.getY(),
                    (shouldDrawButtonAsDown ? botCol.darker (0.12f) : botCol).withMultipliedAlpha (alpha),
                    bounds.getX(), bounds.getBottom(), false));
                g.fillRoundedRectangle (bounds, radius);
                g.setColour (nordRedBright().withAlpha ((primaryCta ? 0.72f : 0.46f) * alpha));
                g.drawRoundedRectangle (bounds.reduced (0.5f), radius, 1.0f);
                if (! primaryCta)
                {
                    const float barW = juce::jlimit (16.0f, 26.0f, bounds.getWidth() * 0.34f);
                    g.setColour (nordRedBright().withAlpha (0.92f * alpha));
                    g.fillRoundedRectangle (bounds.getCentreX() - barW * 0.5f,
                                            bounds.getBottom() - 2.5f, barW, 2.0f, 1.0f);
                }
            }
            else if (touchNav || shouldDrawButtonAsHighlighted || shouldDrawButtonAsDown)
            {
                const float surfAlpha = shouldDrawButtonAsDown ? 0.085f
                                      : shouldDrawButtonAsHighlighted ? 0.060f : 0.028f;
                g.setColour (juce::Colours::white.withAlpha (surfAlpha * alpha));
                g.fillRoundedRectangle (bounds.reduced (0.0f, 2.0f), radius);
                g.setColour (juce::Colours::white.withAlpha (
                    (shouldDrawButtonAsHighlighted ? 0.11f : 0.055f) * alpha));
                g.drawRoundedRectangle (bounds.reduced (0.5f, 2.5f), radius, 1.0f);
            }
            return;
        }

        if (emptyAdd)
        {
            const auto fill = shouldDrawButtonAsDown ? juce::Colour (0xff222731)
                            : shouldDrawButtonAsHighlighted ? juce::Colour (0xff1f242d)
                                                            : juce::Colour (0xff191d25);
            g.setColour (fill.withMultipliedAlpha (alpha));
            g.fillRoundedRectangle (bounds, radius);
            g.setColour (juce::Colours::white.withAlpha (0.08f * alpha));
            g.drawRoundedRectangle (bounds.reduced (0.5f), radius, 1.0f);
            return;
        }

        juce::Colour topCol, botCol, rimCol;
        if (danger)
        {
            topCol = juce::Colour (0xff3c1b23);
            botCol = juce::Colour (0xff261117);
            rimCol = nordRedBright().withAlpha (0.58f);
        }
        else if (primaryCta)
        {
            topCol = juce::Colour (0xffc41f3a);
            botCol = juce::Colour (0xff8c1327);
            rimCol = juce::Colour (0xffff6b81).withAlpha (0.62f);
        }
        else if (activeOrAmber)
        {
            if (solo || amberAccent)
            {
                topCol = button.getToggleState() ? juce::Colour (0xff785216) : juce::Colour (0xff40321d);
                botCol = button.getToggleState() ? juce::Colour (0xff4e340d) : juce::Colour (0xff2c2214);
                rimCol = amberSolo().withAlpha (button.getToggleState() ? 0.78f : 0.48f);
            }
            else if (greenAccent)
            {
                topCol = juce::Colour (0xff1e593f);
                botCol = juce::Colour (0xff133b29);
                rimCol = greenOk().withAlpha (0.68f);
            }
            else
            {
                topCol = juce::Colour (0xff6e1b2b);
                botCol = juce::Colour (0xff46101b);
                rimCol = nordRedBright().withAlpha (0.68f);
            }
        }
        else
        {
            // Keep neutral R <= G <= B (cool studio gunmetal) so inactive controls
            // never read as warm/selected.
            topCol = juce::Colour (0xff252b35);
            botCol = juce::Colour (0xff1a1f27);
            rimCol = juce::Colour (0xff363e4c).withAlpha (0.78f);
        }

       #if CHURCHDAW_MOBILE_SHELL
        // Skin Church Live: superficies chapadas nos tokens novos (sem sombra
        // projetada nem filete especular). So a paleta e o raio mudam; a logica
        // que decide primario/ativo/perigo continua a de cima.
        {
            const float mobileRadius = juce::jmin (14.0f, bounds.getHeight() * 0.30f);
            if (danger)
            {
                topCol = juce::Colour (0xff3a1520);
                botCol = juce::Colour (0xff2a1018);
                rimCol = juce::Colour (0xffff5a6e).withAlpha (0.70f);
            }
            else if (primaryCta)
            {
                topCol = juce::Colour (0xffe8384f);
                botCol = juce::Colour (0xffb01b2e);
                rimCol = juce::Colour (0xffff7a8a).withAlpha (0.60f);
            }
            else if (activeOrAmber)
            {
                const auto tone = (solo || amberAccent) ? amberSolo() : greenAccent ? greenOk() : juce::Colour (0xffff5a6e);
                topCol = botCol = juce::Colour (0xff1c1c24).interpolatedWith (tone, 0.16f);
                rimCol = tone.withAlpha (0.70f);
            }
            else
            {
                topCol = botCol = juce::Colour (0xff1c1c24);
                rimCol = juce::Colour (0xff2b2b36);
            }

            if (shouldDrawButtonAsDown)
            {
                topCol = topCol.darker (0.12f);
                botCol = botCol.darker (0.12f);
            }
            else if (shouldDrawButtonAsHighlighted)
            {
                topCol = topCol.brighter (0.08f);
                botCol = botCol.brighter (0.08f);
            }

            if (primaryCta || danger)
                g.setGradientFill (juce::ColourGradient (topCol.withMultipliedAlpha (alpha), 0.0f, bounds.getY(),
                                                         botCol.withMultipliedAlpha (alpha), 0.0f, bounds.getBottom(), false));
            else
                g.setColour (topCol.withMultipliedAlpha (alpha));
            g.fillRoundedRectangle (bounds, mobileRadius);
            g.setColour ((button.hasKeyboardFocus (true) ? nordRedBright() : rimCol).withMultipliedAlpha (alpha));
            g.drawRoundedRectangle (bounds.reduced (0.5f), mobileRadius, button.hasKeyboardFocus (true) ? 1.5f : 1.0f);
            return;
        }
       #endif

        if (shouldDrawButtonAsDown)
        {
            topCol = topCol.darker (0.14f);
            botCol = botCol.darker (0.06f);
        }
        else if (shouldDrawButtonAsHighlighted)
        {
            topCol = topCol.brighter (0.08f);
            botCol = botCol.brighter (0.05f);
        }

        if (button.isEnabled() && bounds.getHeight() >= 26.0f && ! shouldDrawButtonAsDown)
        {
            g.setColour (juce::Colours::black.withAlpha (primaryCta ? 0.36f : 0.24f));
            g.fillRoundedRectangle (bounds.translated (0.0f, 1.2f), radius);
        }

        g.setGradientFill (juce::ColourGradient (
            topCol.withMultipliedAlpha (alpha), bounds.getX(), bounds.getY(),
            botCol.withMultipliedAlpha (alpha), bounds.getX(), bounds.getBottom(), false));
        g.fillRoundedRectangle (bounds, radius);

        if (bounds.getWidth() > 14.0f && bounds.getHeight() >= 22.0f && ! shouldDrawButtonAsDown)
        {
            auto spec = bounds.reduced (radius * 0.55f, 1.0f).removeFromTop (1.0f);
            if (spec.getWidth() > 4.0f)
            {
                g.setColour (juce::Colours::white.withAlpha ((primaryCta ? 0.18f : 0.075f) * alpha));
                g.fillRect (spec);
            }
        }

        const bool focus = button.hasKeyboardFocus (true);
        g.setColour ((focus ? nordRedBright() : rimCol).withMultipliedAlpha (alpha));
        g.drawRoundedRectangle (bounds.reduced (0.5f), radius, focus ? 1.5f : 1.0f);
    }

    //==============================================================================
    // Checkbox premium: caixinha de vidro escuro com check vermelho aceso
    // (substitui o tick padrao do JUCE nos toggles e no seletor de audio)
    void drawTickBox (juce::Graphics& g, juce::Component& component, float x, float y, float w, float h,
                      bool ticked, bool isEnabled, bool highlighted, bool down) override
    {
        if (usesDesktopControls (component))
        {
            const auto box = juce::Rectangle<float> (x, y, w, h).reduced (2.0f);
            if (box.isEmpty()) return;
            const float alpha = isEnabled ? 1.0f : 0.42f;
            const bool focus = component.hasKeyboardFocus (true);
            const auto fill = ticked ? juce::Colour (0xff8f3446)
                            : down || highlighted ? bgControlHi() : bgControl();
            g.setColour (fill.withMultipliedAlpha (alpha));
            g.fillRoundedRectangle (box, 4.0f);
            g.setColour ((focus ? focusOutline() : ticked ? nordRedBright().withAlpha (0.70f)
                                  : highlighted ? txtSecondary() : borderDefault()).withMultipliedAlpha (alpha));
            g.drawRoundedRectangle (box.reduced (0.5f), 4.0f, focus ? 1.5f : 1.0f);
            if (ticked)
            {
                juce::Path check;
                check.startNewSubPath (box.getX() + box.getWidth() * 0.23f, box.getCentreY());
                check.lineTo (box.getX() + box.getWidth() * 0.43f, box.getY() + box.getHeight() * 0.71f);
                check.lineTo (box.getX() + box.getWidth() * 0.77f, box.getY() + box.getHeight() * 0.29f);
                g.setColour (txtPrimary().withAlpha (alpha));
                g.strokePath (check, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved,
                                                                juce::PathStrokeType::rounded));
            }
            return;
        }
        auto box = juce::Rectangle<float> (x, y, w, h).reduced (2.0f);
        g.setColour (ticked ? nordRed().withAlpha (0.85f) : bgControl());
        g.fillRoundedRectangle (box, 4.0f);
        g.setColour (ticked ? nordRedBright() : borderDefault());
        g.drawRoundedRectangle (box, 4.0f, 1.0f);
        if (ticked)
        {
            juce::Path tick;
            tick.startNewSubPath (box.getX() + box.getWidth() * 0.22f, box.getCentreY());
            tick.lineTo (box.getX() + box.getWidth() * 0.43f, box.getBottom() - box.getHeight() * 0.25f);
            tick.lineTo (box.getRight() - box.getWidth() * 0.18f, box.getY() + box.getHeight() * 0.23f);
            g.setColour (isEnabled ? txtPrimary() : txtDisabled());
            g.strokePath (tick, juce::PathStrokeType (1.8f, juce::PathStrokeType::curved,
                                                           juce::PathStrokeType::rounded));
        }
    }

    //==============================================================================
    // Knob "metal escuro + arco de valor vermelho": corpo grafite esculpido,
    // ponteiro claro e um arco que ACENDE em vermelho conforme o valor. Acaba com
    // o "mar de bolinhas vermelhas" do rotary padrao do JUCE — vermelho vira
    // acento (o arco), nao o corpo inteiro.
    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPos, float rotaryStartAngle, float rotaryEndAngle,
                           juce::Slider& slider) override
    {
        auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (2.0f);
        if (bounds.isEmpty())
            return;
        if (usesDesktopControls (slider))
        {
            const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
            const auto centre = bounds.getCentre();
            const float angle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);
            const float arcRadius = radius * 0.94f;
            const float alpha = slider.isEnabled() ? 1.0f : 0.35f;
            const bool hover = slider.isMouseOverOrDragging();
            const auto accent = slider.findColour (juce::Slider::rotarySliderFillColourId);
            const auto stroke = juce::PathStrokeType (1.8f, juce::PathStrokeType::curved,
                                                           juce::PathStrokeType::rounded);
            juce::Path track;
            track.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f,
                                 rotaryStartAngle, rotaryEndAngle, true);
            g.setColour (juce::Colour (0xff4b515c).withAlpha (alpha));
            g.strokePath (track, stroke);
            const float origin = slider.getMinimum() < 0.0 && slider.getMaximum() > 0.0
                ? rotaryStartAngle + (float) slider.valueToProportionOfLength (0.0)
                                         * (rotaryEndAngle - rotaryStartAngle)
                : rotaryStartAngle;
            if (std::abs (angle - origin) > 0.002f)
            {
                juce::Path value;
                value.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f,
                                     juce::jmin (origin, angle), juce::jmax (origin, angle), true);
                g.setColour (accent.withAlpha (alpha));
                g.strokePath (value, stroke);
            }
            const float bodyRadius = radius * 0.72f;
            const auto body = juce::Rectangle<float> (bodyRadius * 2.0f, bodyRadius * 2.0f)
                                  .withCentre (centre);
            g.setColour (hover ? juce::Colour (0xff3d4550) : juce::Colour (0xff323943));
            g.fillEllipse (body);
            if (slider.hasKeyboardFocus (false))
            {
                g.setColour (accent.withMultipliedAlpha (alpha));
                g.drawEllipse (body.reduced (0.5f), 1.0f);
            }
            const auto pointAt = [&] (float r)
            { return centre + juce::Point<float> (std::sin (angle) * r, -std::cos (angle) * r); };
            g.setColour (juce::Colour (0xfff4f1ed).withAlpha (alpha));
            const juce::Line<float> indicator (pointAt (bodyRadius * 0.26f), pointAt (bodyRadius * 0.78f));
            g.drawLine (indicator, 1.8f);
            return;
        }
        const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
        const auto centre = bounds.getCentre();
        const float angle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);
        const float alpha = slider.isEnabled() ? 1.0f : 0.35f;
        const float arcRadius = radius * 0.90f;
        const auto accent = slider.findColour (juce::Slider::rotarySliderFillColourId);
        const auto trackStroke = juce::PathStrokeType (2.6f, juce::PathStrokeType::curved,
                                                             juce::PathStrokeType::rounded);

        if (radius >= 20.0f)
        {
            g.setColour (juce::Colours::white.withAlpha (0.12f * alpha));
            for (int i = 0; i <= 6; ++i)
            {
                const float ta = rotaryStartAngle + ((float) i / 6.0f) * (rotaryEndAngle - rotaryStartAngle);
                const float tr = radius * 0.985f;
                g.fillEllipse (centre.x + std::sin (ta) * tr - 0.9f,
                               centre.y - std::cos (ta) * tr - 0.9f, 1.8f, 1.8f);
            }
        }

        juce::Path track;
        track.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f,
                             rotaryStartAngle, rotaryEndAngle, true);
        g.setColour (juce::Colour (0xff12151c).withAlpha (alpha));
        g.strokePath (track, juce::PathStrokeType (3.8f, juce::PathStrokeType::curved,
                                                         juce::PathStrokeType::rounded));
        g.setColour (juce::Colour (0xff2f3542).withAlpha (alpha));
        g.strokePath (track, trackStroke);

        const bool bipolar = slider.getMinimum() < 0.0 && slider.getMaximum() > 0.0;
        const float origin = bipolar
            ? rotaryStartAngle + (float) slider.valueToProportionOfLength (0.0)
                                     * (rotaryEndAngle - rotaryStartAngle)
            : rotaryStartAngle;

        if (bipolar)
        {
            const auto p0 = centre + juce::Point<float> (std::sin (origin) * (arcRadius + 2.2f),
                                                         -std::cos (origin) * (arcRadius + 2.2f));
            const auto p1 = centre + juce::Point<float> (std::sin (origin) * (arcRadius - 2.2f),
                                                         -std::cos (origin) * (arcRadius - 2.2f));
            g.setColour (juce::Colours::white.withAlpha (0.34f * alpha));
            g.drawLine ({ p0, p1 }, 1.4f);
        }

        if (std::abs (angle - origin) > 0.003f)
        {
            juce::Path value;
            value.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f,
                                 juce::jmin (origin, angle), juce::jmax (origin, angle), true);
            g.setColour (accent.withAlpha (0.22f * alpha));
            g.strokePath (value, juce::PathStrokeType (5.2f, juce::PathStrokeType::curved,
                                                             juce::PathStrokeType::rounded));
            g.setColour (accent.withAlpha (0.96f * alpha));
            g.strokePath (value, juce::PathStrokeType (2.8f, juce::PathStrokeType::curved,
                                                             juce::PathStrokeType::rounded));
        }

        const float bodyRadius = radius * 0.68f;
        auto body = juce::Rectangle<float> (bodyRadius * 2.0f, bodyRadius * 2.0f)
                        .withCentre (centre);
        g.setColour (juce::Colours::black.withAlpha (0.48f * alpha));
        g.fillEllipse (body.translated (0.0f, 1.8f).expanded (0.5f));

        const auto bodyTop = slider.isMouseOverOrDragging() ? juce::Colour (0xff444d5c)
                                                            : juce::Colour (0xff333a47);
        g.setGradientFill (juce::ColourGradient (
            bodyTop.withMultipliedAlpha (alpha), body.getX(), body.getY(),
            juce::Colour (0xff191d25).withMultipliedAlpha (alpha), body.getX(), body.getBottom(), false));
        g.fillEllipse (body);

        auto capTop = body.reduced (bodyRadius * 0.16f);
        g.setGradientFill (juce::ColourGradient (
            juce::Colour (0xff262c37).withMultipliedAlpha (alpha), capTop.getX(), capTop.getY(),
            juce::Colour (0xff1d222b).withMultipliedAlpha (alpha), capTop.getX(), capTop.getBottom(), false));
        g.fillEllipse (capTop);

        g.setColour (juce::Colours::white.withAlpha (0.14f * alpha));
        g.drawEllipse (body.reduced (0.5f), 1.0f);

        const auto pointAt = [&] (float r)
        { return centre + juce::Point<float> (std::sin (angle) * r, -std::cos (angle) * r); };
        g.setColour (juce::Colour (0xfff6f4f0).withAlpha (0.96f * alpha));
        g.drawLine ({ pointAt (bodyRadius * 0.28f), pointAt (bodyRadius * 0.80f) }, 2.1f);
    }

    //==============================================================================
    // Visor de dB do MASTER: o numero que o tecladista mais consulta na mesa era
    // o unico texto grande do card sem o vocabulario do app -- caia na fonte
    // padrao do Label (regular, largura variavel), enquanto os dB de canal usam
    // monoespacada negrito. Com largura de algarismo fixa o valor para de "dançar"
    // enquanto o fader anda, e "+0.0 dB" / "-8.0 dB" ficam alinhados na coluna.
    // Vale SO pro slider marcado com o ID "masterFader"; todos os outros seguem
    // exatamente como estavam.
    // (O JUCE recria este Label a cada mudanca de cor ou de estilo de caixa --
    // ver Slider::colourChanged -- entao a fonte precisa nascer aqui, no ponto de
    // criacao, e nao ser aplicada de fora depois.)
    juce::Label* createSliderTextBox (juce::Slider& slider) override
    {
        auto* l = juce::LookAndFeel_V4::createSliderTextBox (slider);

        if (l != nullptr && slider.getComponentID() == "masterFader")
        {
            l->setFont (juce::Font (juce::FontOptions (
                juce::Font::getDefaultMonospacedFontName(), 12.5f, juce::Font::bold)));
            l->setBorderSize (juce::BorderSize<int> (0, 2, 0, 2));
        }

        return l;
    }

    void drawLevelMeter (juce::Graphics& g, int width, int height, float level) override
    {
        const auto bounds = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height).reduced (0.5f);
        if (bounds.isEmpty())
            return;

        const float radius = juce::jmin (6.0f, bounds.getHeight() * 0.24f);
        g.setColour (juce::Colour (0xff1a1e24));
        g.fillRoundedRectangle (bounds, radius);
        g.setColour (borderSoft());
        g.drawRoundedRectangle (bounds, radius, 1.0f);

        const float safeLevel = std::isfinite (level) ? juce::jlimit (0.0f, 1.0f, level) : 0.0f;
        auto inner = bounds.reduced (5.0f, 5.0f);
        if (inner.isEmpty())
            return;

        constexpr int segments = 12;
        const float gap = 2.0f;
        const float segW = (inner.getWidth() - gap * (float) (segments - 1)) / (float) segments;
        if (segW <= 0.5f)
            return;

        for (int i = 0; i < segments; ++i)
        {
            const float threshold = ((float) i + 0.35f) / (float) segments;
            const bool lit = safeLevel > 0.002f && safeLevel >= threshold;
            const auto litColour = i >= 11 ? redClip() : i >= 8 ? amberSolo() : greenOk();
            g.setColour (lit ? litColour : juce::Colour (0xff252a32));
            g.fillRoundedRectangle (inner.getX() + (float) i * (segW + gap), inner.getY(),
                                    segW, inner.getHeight(), 1.5f);
        }
    }

    static void installPortugueseStrings()
    {
        juce::LocalisedStrings::setCurrentMappings (new juce::LocalisedStrings (juce::String::fromUTF8 (
            "language: pt-BR\n"
            "countries: br\n"
            "\"Audio device type:\" = \"Tipo de driver:\"\n"
            "\"Device:\" = \"Dispositivo:\"\n"
            "\"Output:\" = \"Sa\xc3\xad" "da:\"\n"
            "\"Input:\" = \"Entrada:\"\n"
            "\"Active output channels:\" = \"Sa\xc3\xad" "das ativas:\"\n"
            "\"Active input channels:\" = \"Entradas ativas:\"\n"
            "\"Sample rate:\" = \"Taxa de amostragem:\"\n"
            "\"Audio buffer size:\" = \"Tamanho do buffer:\"\n"
            "\"Control Panel\" = \"Painel do driver\"\n"
            "\"Opens the device's own control panel\" = \"Abre o painel de controle da interface\"\n"
            "\"Reset Device\" = \"Resetar\"\n"
            "\"Resets the audio interface - sometimes needed after changing a device's properties in its custom control panel\" = \"Reinicia a interface de \xc3\xa1udio\"\n"
            "\"Test\" = \"Testar\"\n"
            "\"Plays a test tone\" = \"Toca um tom de teste\"\n"
            "\"Active MIDI inputs:\" = \"Entradas MIDI ativas:\"\n"
            "\"Bluetooth MIDI\" = \"MIDI Bluetooth\"\n"
            "\"Scan for bluetooth MIDI devices\" = \"Procurar dispositivos MIDI Bluetooth\"\n"
            "\"MIDI Output:\" = \"Sa\xc3\xad" "da MIDI:\"\n"
            "\"No MIDI inputs available\" = \"Nenhuma entrada MIDI\"\n"
            "\"(no audio output channels found)\" = \"(sem canais de sa\xc3\xad" "da)\"\n"
            "\"(no audio input channels found)\" = \"(sem canais de entrada)\"\n"
            "\"none\" = \"nenhum\"\n"
            "\"Show advanced settings...\" = \"Mostrar avan\xc3\xa7" "adas...\"\n"
            "\"Error when trying to open audio device!\" = \"Erro ao abrir o dispositivo de \xc3\xa1udio!\"\n"
            "\"OK\" = \"OK\"\n"
            "\"Name\" = \"Nome\"\n"
            "\"Format\" = \"Formato\"\n"
            "\"Category\" = \"Categoria\"\n"
            "\"Manufacturer\" = \"Fabricante\"\n"
            "\"Description\" = \"Descri\xc3\xa7\xc3\xa3o\"\n"
            "\"Options...\" = \"Op\xc3\xa7\xc3\xb5" "es...\"\n"
            "\"Scan\" = \"Escanear\"\n"
            "\"Cancel\" = \"Cancelar\"\n"
            "\"Clear list\" = \"Limpar lista\"\n"
            "\"Scan for new or updated XFMTX plug-ins\" = \"Escanear plugins XFMTX novos ou atualizados\"\n"
            "\"Remove all XFMTX plug-ins\" = \"Remover todos os plugins XFMTX\"\n"
            "\"Remove selected plug-in from list\" = \"Remover o plugin selecionado\"\n"
            "\"Remove plug-in from list\" = \"Remover o plugin da lista\"\n"
            "\"Remove any plug-ins whose files no longer exist\" = \"Remover plugins que n\xc3\xa3o existem mais\"\n"
            "\"Show folder containing selected plug-in\" = \"Abrir a pasta do plugin selecionado\"\n"
            "\"Show folder containing plug-in\" = \"Abrir a pasta do plugin\"\n"
            "\"Select folders to scan...\" = \"Escolha as pastas para escanear...\"\n"
            "\"Scanning for plug-ins...\" = \"Procurando plugins...\"\n"
            "\"Searching for all possible plug-in files...\" = \"Vasculhando os arquivos de plugin...\"\n"
            "\"Testing\" = \"Testando\"\n"
            "\"Scan complete\" = \"Escaneamento conclu\xc3\xad" "do\"\n"
            "\"Plugin Scanning\" = \"Escaneamento de plugins\"\n"
            "\"Deactivated after failing to initialise correctly\" = \"Desativado: consulte etapa, chamada e c\xc3\xb3" "digo no relat\xc3\xb3rio de scan\"\n"
            "\"The following files encountered fatal errors during validation\" = \"Falhas isoladas (crash, timeout ou incompatibilidade). O motivo exato foi salvo em Library/Logs/ChurchDAW/PluginScan\"\n"
            "\"The following files appeared to be plugin files, but failed to load correctly\" = \"Plugins recusados na valida\xc3\xa7\xc3\xa3o. Consulte etapa, chamada, c\xc3\xb3" "digo e stack no relat\xc3\xb3rio de scan\"\n"), false));
    }

    //==============================================================================
    // Fader vertical estilo console: cap de metal deslizando num sulco recuado
    // com preenchimento vermelho abaixo do thumb. Substitui a linha fina + bolinha
    // vermelha do slider padrao.
    void drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPos, float minSliderPos, float maxSliderPos,
                           juce::Slider::SliderStyle style, juce::Slider& slider) override
    {
        const bool desktopFader = usesDesktopShell (slider)
            && (slider.getComponentID() == "masterFader" || slider.getComponentID() == "channelFader")
            && (style == juce::Slider::LinearVertical || style == juce::Slider::LinearHorizontal);
        if (desktopFader && width > 0 && height > 0)
        {
            const bool vertical = style == juce::Slider::LinearVertical;
            const bool master = slider.getComponentID() == "masterFader";
            const float alpha = slider.isEnabled() ? 1.0f : 0.36f;
            const float trackThickness = master ? 7.0f : 6.0f;
            auto track = vertical
                ? juce::Rectangle<float> (x + width * 0.5f - trackThickness * 0.5f,
                                           (float) y, trackThickness, (float) height)
                : juce::Rectangle<float> ((float) x + 8.0f, y + height * 0.5f - trackThickness * 0.5f,
                                           juce::jmax (1.0f, width - 16.0f), trackThickness);
            const float level = vertical ? juce::jlimit (track.getY(), track.getBottom(), sliderPos)
                                         : juce::jlimit (track.getX(), track.getRight(), sliderPos);
            g.setColour (juce::Colours::black.withAlpha (0.44f));
            g.fillRoundedRectangle (track.expanded (1.5f), trackThickness * 0.5f + 1.5f);
            g.setColour (juce::Colour (0xff101319));
            g.fillRoundedRectangle (track, trackThickness * 0.5f);
            g.setColour (juce::Colours::white.withAlpha (0.075f));
            g.drawRoundedRectangle (track.expanded (0.5f), trackThickness * 0.5f + 0.5f, 1.0f);
            auto fill = (vertical ? track.withTop (level) : track.withRight (level)).reduced (1.0f);
            if (fill.getWidth() > 0.0f && fill.getHeight() > 0.0f)
            {
                g.setGradientFill (juce::ColourGradient (nordRedBright().withAlpha (alpha),
                    vertical ? fill.getCentreX() : fill.getRight(),
                    vertical ? fill.getY() : fill.getCentreY(),
                    nordRed().darker (0.20f).withAlpha (alpha), fill.getX(), fill.getBottom(), false));
                g.fillRoundedRectangle (fill, 2.0f);
            }
            if (slider.getMinimum() < 0.0 && slider.getMaximum() > 0.0)
            {
                const float zero = slider.getPositionOfValue (0.0);
                g.setColour (juce::Colour (0xffc7cbd3).withAlpha (0.50f * alpha));
                if (vertical)
                {
                    g.drawLine (track.getX() - 6.0f, zero, track.getX() - 2.5f, zero, 1.2f);
                    g.drawLine (track.getRight() + 2.5f, zero, track.getRight() + 6.0f, zero, 1.2f);
                }
                else
                {
                    g.drawLine (zero, track.getY() - 5.0f, zero, track.getY() - 2.0f, 1.2f);
                    g.drawLine (zero, track.getBottom() + 2.0f, zero, track.getBottom() + 5.0f, 1.2f);
                }
            }
            const float capW = juce::jmin ((float) width, vertical ? (master ? 42.0f : 32.0f) : 25.0f);
            const float capH = juce::jmin ((float) height, vertical ? 21.0f : 22.0f);
            const float cx = vertical ? track.getCentreX()
                : juce::jlimit ((float) x + capW * 0.5f, x + width - capW * 0.5f, level);
            const float cy = vertical
                ? juce::jlimit ((float) y + capH * 0.5f, y + height - capH * 0.5f, level)
                : track.getCentreY();
            auto cap = juce::Rectangle<float> (capW, capH).withCentre ({ cx, cy });
            g.setColour (juce::Colours::black.withAlpha (0.28f));
            g.fillRoundedRectangle (cap.translated (0.0f, 1.0f), 4.0f);
            const juce::ColourGradient satin (juce::Colour (0xffc4cbd3), cap.getX(), cap.getY(),
                                             juce::Colour (0xffa0aab6), cap.getX(), cap.getBottom(), false);
            if (slider.isEnabled()) g.setGradientFill (satin);
            else g.setColour (juce::Colour (0xff69717d));
            g.fillRoundedRectangle (cap, 4.0f);
            g.setColour ((slider.hasKeyboardFocus (false) ? nordRedBright()
                           : juce::Colour (0xffe0e5eb).withAlpha (0.42f)).withMultipliedAlpha (alpha));
            g.drawRoundedRectangle (cap.reduced (0.5f), 4.0f, 1.0f);
            // A single dark index doubles as the grip. No decorative ridges:
            // its contrast remains immediate at both ends of the same travel.
            g.setColour (juce::Colour (0xff3b434f).withAlpha (alpha));
            if (vertical)
                g.fillRoundedRectangle (cap.getX() + 4.0f, cy - 0.85f,
                                        juce::jmax (1.0f, capW - 8.0f), 1.7f, 0.85f);
            else
                g.fillRoundedRectangle (cx - 0.85f, cap.getY() + 3.0f,
                                        1.7f, juce::jmax (1.0f, capH - 6.0f), 0.85f);
            return;
        }

        // O fader horizontal de console abaixo nasceu pro layout mobile (no
        // celular canal e master viram horizontais). No DESKTOP so o master usa
        // fader horizontal, e ele caia no desenho padrao do toolkit -- risco fino
        // + bolinha, a unica peca grande do card do master sem o vocabulario da
        // tela. O ID "masterFader" liga o mesmo desenho ja provado no celular,
        // sem tocar em NENHUM outro slider horizontal do app.
       #if CHURCHDAW_MOBILE_SHELL
        const bool consoleHorizontal = slider.getComponentID() != "auditionVolumeSlider" && height >= 32;
       #else
        const bool consoleHorizontal = slider.getComponentID() == "masterFader";
       #endif

        if (style == juce::Slider::LinearHorizontal && consoleHorizontal)
        {
            juce::ignoreUnused (minSliderPos, maxSliderPos);

            // No layout mobile canal e master usam fader horizontal. O desenho
            // padrao do toolkit era uma linha fina com uma bolinha vermelha,
            // destoando do fader de console vertical e ficando dificil de tocar.
            const float inset = juce::jlimit (7.0f, 12.0f, (float) width * 0.045f);

            // trilha com mais PRESENCA: o fader e o controle principal do canal
            // e com 8 px lia como um risco; 10-14 px da corpo sem virar barra
            const float trackH = juce::jlimit (8.0f, 12.0f, (float) height * 0.30f);
            auto track = juce::Rectangle<float> ((float) x + inset,
                                                 (float) y + (float) height * 0.5f - trackH * 0.5f,
                                                 juce::jmax (1.0f, (float) width - inset * 2.0f),
                                                 trackH);
            const float trackR = trackH * 0.5f;
            const float pos = juce::jlimit (track.getX(), track.getRight(), sliderPos);

            g.setColour (juce::Colours::black.withAlpha (0.42f));
            g.fillRoundedRectangle (track.expanded (2.0f, 1.5f), trackR + 2.0f);
            g.setColour (juce::Colours::black.withAlpha (0.60f));
            g.fillRoundedRectangle (track.translated (0.0f, 1.0f), trackR);
            g.setGradientFill (juce::ColourGradient (juce::Colour (0xff090a0d),
                                                     track.getX(), track.getY(),
                                                     juce::Colour (0xff1c1f25),
                                                     track.getX(), track.getBottom(), false));
            g.fillRoundedRectangle (track, trackR);
            g.setColour (juce::Colours::black.withAlpha (0.22f));
            g.fillRoundedRectangle (track.reduced (2.0f, 2.2f),
                                    juce::jmax (1.0f, trackR - 2.0f));
            g.setColour (juce::Colours::white.withAlpha (0.07f));
            g.drawRoundedRectangle (track.reduced (0.5f), trackR, 1.0f);

            auto value = track.withRight (pos).reduced (2.0f);

            if (value.getWidth() > 1.0f)
            {
                g.setGradientFill (juce::ColourGradient (nordRedDark(), value.getX(), value.getCentreY(),
                                                         nordRedBright(), value.getRight(), value.getCentreY(),
                                                         false));
                g.fillRoundedRectangle (value, value.getHeight() * 0.5f);
                g.setColour (nordRedBright().withAlpha (0.32f));
                g.fillRoundedRectangle (value.withTrimmedLeft (value.getWidth() * 0.72f),
                                        value.getHeight() * 0.5f);
            }

            // MARCA DE 0 dB: a referencia que todo tecladista procura ao zerar o
            // ganho. Um risco discreto acima e abaixo da trilha, exatamente onde
            // o valor 0 cai -- calculado pelo proprio slider, entao respeita a
            // curva (skew) do fader em vez de chutar proporcao linear.
            {
                const auto range = slider.getRange();

                if (range.getStart() < 0.0 && range.getEnd() > 0.0)
                {
                    const float zx = slider.getPositionOfValue (0.0);

                    if (zx > track.getX() + 6.0f && zx < track.getRight() - 6.0f)
                    {
                        g.setColour (juce::Colours::white.withAlpha (0.34f));
                        g.fillRoundedRectangle (zx - 0.9f, track.getY() - 5.0f, 1.8f, 4.0f, 0.9f);
                        g.fillRoundedRectangle (zx - 0.9f, track.getBottom() + 1.0f, 1.8f, 4.0f, 0.9f);
                    }
                }
            }

            const float capW = juce::jlimit (22.0f, 30.0f, (float) height * 0.84f);
            const float capH = juce::jlimit (14.0f, 20.0f, (float) height - 4.0f);
            auto cap = juce::Rectangle<float> (pos - capW * 0.5f,
                                               track.getCentreY() - capH * 0.5f,
                                               capW, capH);

            g.setColour (juce::Colours::black.withAlpha (0.42f));
            g.fillRoundedRectangle (cap.translated (0.0f, 2.0f), 4.0f);

            g.setGradientFill (juce::ColourGradient (juce::Colour (0xff4a515d),
                                                     cap.getX(), cap.getY(),
                                                     juce::Colour (0xff232730),
                                                     cap.getX(), cap.getBottom(), false));
            g.fillRoundedRectangle (cap, 4.0f);
            g.setGradientFill (juce::ColourGradient (
                juce::Colours::white.withAlpha (0.115f), cap.getX(), cap.getY(),
                juce::Colours::transparentWhite, cap.getRight(), cap.getBottom(), true));
            g.fillRoundedRectangle (cap.reduced (1.0f), 3.2f);
            g.setColour (juce::Colours::white.withAlpha (0.18f));
            g.drawRoundedRectangle (cap.reduced (0.5f), 4.0f, 1.0f);
            g.setColour (juce::Colour (0xfff2f4f7));
            g.fillRect (cap.getCentreX() - 1.0f, cap.getY() + 3.0f,
                        2.0f, juce::jmax (1.0f, cap.getHeight() - 6.0f));
            return;
        }

       #if CHURCHDAW_MOBILE_SHELL
        if (style == juce::Slider::LinearHorizontal)
        {
            juce::ignoreUnused (minSliderPos, maxSliderPos);
            const float alpha = slider.isEnabled() ? 1.0f : 0.36f;
            const float trackH = 5.0f;
            const float cy = (float) y + (float) height * 0.5f;
            auto track = juce::Rectangle<float> ((float) x + 7.0f, cy - trackH * 0.5f,
                                                 juce::jmax (4.0f, (float) width - 14.0f), trackH);
            const float pos = juce::jlimit (track.getX(), track.getRight(), sliderPos);

            g.setColour (juce::Colour (0xff10131a).withAlpha (alpha));
            g.fillRoundedRectangle (track, 2.5f);
            g.setColour (juce::Colours::white.withAlpha (0.075f * alpha));
            g.drawRoundedRectangle (track.reduced (0.4f), 2.5f, 0.8f);

            auto fill = track.withRight (pos);
            if (fill.getWidth() > 1.5f)
            {
                g.setGradientFill (juce::ColourGradient (
                    nordRed().withAlpha (alpha), fill.getX(), cy,
                    nordRedBright().withAlpha (alpha), fill.getRight(), cy, false));
                g.fillRoundedRectangle (fill, 2.5f);
            }

            const float thumbD = 15.0f;
            auto thumb = juce::Rectangle<float> (thumbD, thumbD).withCentre ({ pos, cy });
            g.setColour (juce::Colours::black.withAlpha (0.45f * alpha));
            g.fillEllipse (thumb.translated (0.0f, 1.5f));
            g.setGradientFill (juce::ColourGradient (
                juce::Colour (0xffebeff5).withAlpha (alpha), thumb.getX(), thumb.getY(),
                juce::Colour (0xffaeb7c4).withAlpha (alpha), thumb.getX(), thumb.getBottom(), false));
            g.fillEllipse (thumb);
            g.setColour (juce::Colour (0xff1a1f28).withAlpha (alpha));
            g.fillEllipse (thumb.reduced (4.8f));
            return;
        }
       #endif

        if (style != juce::Slider::LinearVertical)
        {
            juce::LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos,
                                                    minSliderPos, maxSliderPos, style, slider);
            return;
        }

        const bool master = slider.getComponentID() == "masterFader";
        const bool channel = slider.getComponentID() == "channelFader";
        if (width <= 0 || height <= 0) return;

       #if CHURCHDAW_MOBILE_SHELL
        const float alpha = slider.isEnabled() ? 1.0f : 0.40f;
        const float capW = juce::jmin ((float) width, juce::jlimit (22.0f, master ? 34.0f : 29.0f, (float) width * 0.72f));
        const float capH = juce::jmin ((float) height * 0.42f, master ? 38.0f : 34.0f);
        const float slotPadY = juce::jmax (4.0f, capH * 0.28f);
        const float slotW = master ? 7.0f : 6.0f;
        auto track = juce::Rectangle<float> ((float) x + (float) width * 0.5f - slotW * 0.5f,
                                             (float) y + slotPadY,
                                             slotW,
                                             juce::jmax (12.0f, (float) height - slotPadY * 2.0f));

        // Laser-etched console graduation ticks along the fader slot.
        if (width >= 30 && track.getHeight() >= 48.0f)
        {
            const float tickFracs[] = { 0.08f, 0.22f, 0.38f, 0.56f, 0.74f, 0.90f };
            for (int ti = 0; ti < 6; ++ti)
            {
                const float ty = track.getY() + track.getHeight() * tickFracs[ti];
                const bool major = (ti == 1 || ti == 3);
                const float tw = major ? 5.0f : 3.2f;
                g.setColour (juce::Colours::white.withAlpha ((major ? 0.22f : 0.10f) * alpha));
                g.drawLine (track.getX() - 3.0f - tw, ty, track.getX() - 3.0f, ty, major ? 1.1f : 0.85f);
                g.drawLine (track.getRight() + 3.0f, ty, track.getRight() + 3.0f + tw, ty, major ? 1.1f : 0.85f);
            }
            if (slider.getMinimum() < 0.0 && slider.getMaximum() > 0.0)
            {
                const float zy = juce::jlimit (track.getY(), track.getBottom(),
                                               slider.getPositionOfValue (0.0));
                g.setColour (nordRedBright().withAlpha (0.55f * alpha));
                g.drawLine (track.getX() - 9.0f, zy, track.getX() - 2.5f, zy, 1.35f);
                g.drawLine (track.getRight() + 2.5f, zy, track.getRight() + 9.0f, zy, 1.35f);
            }
        }

        // Deep recessed console slot + subtle active crimson core.
        g.setColour (juce::Colour (0xff07090d).withAlpha (alpha));
        g.fillRoundedRectangle (track.expanded (1.2f, 1.0f), slotW * 0.5f + 1.0f);
        g.setGradientFill (juce::ColourGradient (
            juce::Colour (0xff090b10).withAlpha (alpha), track.getX(), track.getY(),
            juce::Colour (0xff131720).withAlpha (alpha), track.getRight(), track.getY(), false));
        g.fillRoundedRectangle (track, slotW * 0.5f);
        g.setColour (juce::Colours::white.withAlpha (0.065f * alpha));
        g.drawRoundedRectangle (track.reduced (0.35f), slotW * 0.5f, 0.8f);

        const float cy = juce::jlimit ((float) y + capH * 0.5f + 1.0f,
                                       (float) (y + height) - capH * 0.5f - 1.0f, sliderPos);
        const float levelTop = juce::jlimit (track.getY(), track.getBottom(), cy);
        if (track.getBottom() - levelTop > 2.0f)
        {
            auto core = track.withTop (levelTop).reduced (1.4f, 1.0f);
            g.setGradientFill (juce::ColourGradient (
                nordRedBright().withAlpha (0.88f * alpha), core.getCentreX(), core.getY(),
                nordRedDark().withAlpha (0.55f * alpha), core.getCentreX(), core.getBottom(), false));
            g.fillRoundedRectangle (core, core.getWidth() * 0.5f);
        }

        // Sculpted anodized concave studio fader cap.
        auto cap = juce::Rectangle<float> (track.getCentreX() - capW * 0.5f,
                                           cy - capH * 0.5f, capW, capH);
        g.setColour (juce::Colours::black.withAlpha (0.52f * alpha));
        g.fillRoundedRectangle (cap.translated (0.0f, 2.5f).expanded (0.5f, 0.0f), 4.5f);

        juce::ColourGradient bodyGrad (juce::Colour (0xff3c4452).withAlpha (alpha), cap.getX(), cap.getY(),
                                       juce::Colour (0xff232832).withAlpha (alpha), cap.getX(), cap.getBottom(), false);
        bodyGrad.addColour (0.20, juce::Colour (0xff545e70).withAlpha (alpha));
        bodyGrad.addColour (0.44, juce::Colour (0xff262b36).withAlpha (alpha));
        bodyGrad.addColour (0.56, juce::Colour (0xff222731).withAlpha (alpha));
        bodyGrad.addColour (0.80, juce::Colour (0xff454e5e).withAlpha (alpha));
        g.setGradientFill (bodyGrad);
        g.fillRoundedRectangle (cap, 4.2f);

        // Concave saddle ridges above and below the center index line.
        if (capH >= 24.0f)
        {
            const float ridgeInset = 4.0f;
            g.setColour (juce::Colours::white.withAlpha (0.16f * alpha));
            g.drawLine (cap.getX() + ridgeInset, cap.getY() + capH * 0.22f,
                        cap.getRight() - ridgeInset, cap.getY() + capH * 0.22f, 0.9f);
            g.drawLine (cap.getX() + ridgeInset, cap.getY() + capH * 0.78f,
                        cap.getRight() - ridgeInset, cap.getY() + capH * 0.78f, 0.9f);
        }

        g.setColour ((slider.hasKeyboardFocus (false) ? nordRedBright()
                                                      : juce::Colours::white.withAlpha (0.20f))
                         .withMultipliedAlpha (alpha));
        g.drawRoundedRectangle (cap.reduced (0.5f), 4.2f, 1.0f);

        // High-contrast precision center index line.
        auto indexLine = juce::Rectangle<float> (cap.getX() + 3.5f, cy - 1.0f,
                                                 juce::jmax (2.0f, capW - 7.0f), 2.0f);
        g.setColour (juce::Colours::black.withAlpha (0.55f * alpha));
        g.fillRoundedRectangle (indexLine.expanded (0.5f, 0.8f), 1.0f);
        g.setColour ((slider.isMouseOverOrDragging() ? juce::Colours::white
                                                     : juce::Colour (0xfff2f5f9)).withAlpha (alpha));
        g.fillRoundedRectangle (indexLine, 1.0f);
        return;
       #else
        const float trackW = master ? 10.0f : 7.0f;
        auto track = juce::Rectangle<float> ((float) x + width * 0.5f - trackW * 0.5f,
                                             (float) y, trackW, (float) height);
        g.setColour (bgApp());
        g.fillRoundedRectangle (track.expanded (2.0f, 0.0f), trackW * 0.5f);
        g.setColour (borderDefault());
        g.fillRoundedRectangle (track, trackW * 0.5f);
        const float level = juce::jlimit (track.getY(), track.getBottom(), sliderPos);
        if (track.getBottom() - level > 1.0f)
        {
            g.setColour (slider.isEnabled() ? nordRed() : txtDisabled());
            g.fillRoundedRectangle (track.withTop (level), trackW * 0.5f);
        }
        const float desiredWidth = master ? 46.0f : channel ? 34.0f : 42.0f;
        const float desiredHeight = channel ? 20.0f : 24.0f;
        const float capW = juce::jmin ((float) width, desiredWidth);
        const float capH = juce::jmin ((float) height, desiredHeight);
        // Keep the complete handle visible at both ends of the travel.
        const float cy = juce::jlimit (track.getY() + capH * 0.5f,
                                       track.getBottom() - capH * 0.5f, sliderPos);
        auto cap = juce::Rectangle<float> (track.getCentreX() - capW * 0.5f,
                                           cy - capH * 0.5f, capW, capH);
        // Machined-metal grip; the indicator keeps its exact parameter position.
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.fillRoundedRectangle (cap.translated (0.0f, 2.0f), 4.0f);
        juce::ColourGradient metal (juce::Colour (0xffaeb8c4), cap.getX(), cap.getY(),
                                    juce::Colour (0xff636d79), cap.getX(), cap.getBottom(), false);
        metal.addColour (0.18, juce::Colour (0xffd7dde4));
        metal.addColour (0.48, juce::Colour (0xffa5afbb));
        metal.addColour (0.52, juce::Colour (0xff919ca9));
        metal.addColour (0.82, juce::Colour (0xffbdc6d1));
        if (slider.isEnabled()) g.setGradientFill (metal);
        else g.setColour (txtDisabled());
        g.fillRoundedRectangle (cap, 4.0f);
        g.setColour (slider.hasKeyboardFocus (false) ? nordRedBright() : juce::Colour (0xffdce3ea).withAlpha (0.70f));
        g.drawRoundedRectangle (cap.reduced (0.5f), 4.0f, 1.0f);
        g.setColour (juce::Colours::black.withAlpha (0.15f));
        for (const float offset : { -5.0f, 5.0f })
            g.drawHorizontalLine (juce::roundToInt (cap.getCentreY() + offset), cap.getX() + 6, cap.getRight() - 6);
        g.setColour (nordRedDark());
        g.fillRoundedRectangle (cap.reduced (5.0f, 0.0f).withSizeKeepingCentre (
                                    juce::jmax (1.0f, capW - 10.0f), 2.0f), 1.0f);
       #endif
    }

    //==============================================================================
    // Interruptor deslizante no lugar do checkbox quadrado do toolkit (o elemento
    // mais generico que sobrava na tela, do lado de knobs de metal e cards de vidro).
    void drawToggleButton (juce::Graphics& g, juce::ToggleButton& button,
                           bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override
    {
        const auto bounds = button.getLocalBounds().toFloat();
        const bool on = button.getToggleState();
        if (usesDesktopControls (button))
        {
            if (bounds.isEmpty()) return;
            const bool compactToggle = bounds.getWidth() < 126.0f;
            const float h = juce::jlimit (13.0f, compactToggle ? 16.0f : 18.0f, bounds.getHeight() - (compactToggle ? 4.0f : 2.0f));
            const float w = h * (compactToggle ? 1.78f : 1.85f);
            const auto track = juce::Rectangle<float> (1.0f, bounds.getCentreY() - h * 0.5f, w, h);
            const float alpha = button.isEnabled() ? 1.0f : 0.42f;
            const bool hover = shouldDrawButtonAsHighlighted || shouldDrawButtonAsDown;
            const auto fill = on ? juce::Colour (0xffa43d53) : hover ? juce::Colour (0xff414954)
                                                                               : juce::Colour (0xff343b46);
            g.setColour (fill.withAlpha (alpha));
            g.fillRoundedRectangle (track, h * 0.5f);
            if (button.hasKeyboardFocus (true))
            {
                g.setColour (focusOutline().withAlpha (alpha));
                g.drawRoundedRectangle (track.reduced (0.5f), h * 0.5f, 1.5f);
            }
            const float d = h - 5.0f;
            const float x = on ? track.getRight() - d - 2.5f : track.getX() + 2.5f;
            g.setColour ((on ? txtPrimary() : juce::Colour (0xffb4bdc9)).withAlpha (alpha));
            g.fillEllipse (x, track.getCentreY() - d * 0.5f, d, d);
            g.setColour (button.findColour (juce::ToggleButton::textColourId).withMultipliedAlpha (alpha));
            g.setFont (juce::Font (juce::FontOptions (juce::jlimit (11.0f, compactToggle ? 12.0f : 13.0f, h * 0.75f))));
            g.drawFittedText (button.getButtonText(),
                              bounds.withTrimmedLeft (w + (compactToggle ? 6.5f : 8.0f)).toNearestInt(),
                              juce::Justification::centredLeft, 1, 0.88f);
            return;
        }
       #if CHURCHDAW_MOBILE_SHELL
        const float h = juce::jlimit (18.0f, 21.0f, bounds.getHeight() - 8.0f);
       #else
        const float h = juce::jlimit (14.0f, 20.0f, bounds.getHeight() - 2.0f);
       #endif
        const float w = h * 1.86f;
        const float alpha = button.isEnabled() ? 1.0f : 0.4f;
        auto sw = juce::Rectangle<float> (1.0f, bounds.getCentreY() - h * 0.5f, w, h);
        if (on)
        {
            g.setGradientFill (juce::ColourGradient (
                juce::Colour (0xffc21f3a).withMultipliedAlpha (alpha), sw.getX(), sw.getY(),
                juce::Colour (0xff8a1427).withMultipliedAlpha (alpha), sw.getX(), sw.getBottom(), false));
        }
        else
        {
            g.setColour (juce::Colour (0xff191e27).withMultipliedAlpha (alpha));
        }
        g.fillRoundedRectangle (sw, h * 0.5f);
        g.setColour ((button.hasKeyboardFocus (true) ? nordRedBright()
                      : on ? nordRedBright().withAlpha (0.55f)
                      : shouldDrawButtonAsHighlighted ? txtSecondary()
                                                      : juce::Colour (0xff343c4a)).withMultipliedAlpha (alpha));
        g.drawRoundedRectangle (sw.reduced (0.5f), h * 0.5f, 1.0f);
        const float d = h - 5.0f;
        const float kx = on ? sw.getRight() - d - 2.5f : sw.getX() + 2.5f;
        g.setColour (juce::Colours::black.withAlpha (0.35f * alpha));
        g.fillEllipse (kx, sw.getCentreY() - d * 0.5f + 1.0f, d, d);
        g.setColour ((on ? juce::Colours::white : juce::Colour (0xffb6bfcc)).withMultipliedAlpha (alpha));
        g.fillEllipse (kx, sw.getCentreY() - d * 0.5f, d, d);
        auto textArea = bounds.withTrimmedLeft (w + 9.0f);
        g.setColour (button.findColour (juce::ToggleButton::textColourId).withMultipliedAlpha (alpha));
        g.setFont (juce::Font (juce::FontOptions (juce::jlimit (11.5f, 13.0f, h * 0.62f))));
        g.drawFittedText (button.getButtonText(), textArea.toNearestInt(), juce::Justification::centredLeft, 1, 0.86f);
    }

    //==============================================================================
    // Menu (lista de timbres, menus de contexto) no MESMO tom dos cards. O padrao
    // do JUCE vinha bem mais claro que o app inteiro e o menu — que e onde se passa
    // mais tempo escolhendo timbre — parecia de outro programa.
    void drawPopupMenuBackground (juce::Graphics& g, int width, int height) override
    {
       #if CHURCHDAW_MOBILE_SHELL
        g.fillAll (bgCardHi());
        g.setColour (borderDefault());
        g.drawRoundedRectangle (juce::Rectangle<float> (0.5f, 0.5f, width - 1.0f, height - 1.0f), 8.0f, 1.0f);
       #else
        if (width <= 0 || height <= 0) return;
        const auto bounds = juce::Rectangle<float> ((float) width, (float) height);
        // Opaque-window backends still receive a fully painted surface.
        if (! juce::Desktop::canUseSemiTransparentWindows())
            g.fillAll (bgFloating());
        g.setColour (bgFloating());
        g.fillRoundedRectangle (bounds, 10.0f);
        g.setColour (juce::Colour (0xff434953));
        g.drawRoundedRectangle (bounds.reduced (0.5f), 10.0f, 1.0f);
       #endif
    }

    // Cabecalho de secao DE VERDADE no lugar do "=====  WORSHIP  =====" em ASCII:
    // rotulo curto em caixa alta e um filete que corre ate a borda.
    juce::Font getPopupMenuFont() override
    {
       #if CHURCHDAW_MOBILE_SHELL
        return juce::Font (juce::FontOptions (14.0f));
       #else
        return juce::Font (juce::FontOptions (13.5f));
       #endif
    }

    int getPopupMenuBorderSize() override { return 6; }

    void getIdealPopupMenuItemSize (const juce::String& text, bool separator,
                                    int standardHeight, int& width, int& height) override
    {
       #if JUCE_IOS
        height = separator ? 10 : juce::jmax (44, standardHeight);
       #elif JUCE_ANDROID || CHURCHDAW_MOBILE_UI_PREVIEW
        height = separator ? 10 : juce::jmax (32, standardHeight);
       #else
        height = separator ? 9 : juce::jmax (34, standardHeight);
       #endif
       #if CHURCHDAW_MOBILE_SHELL
        constexpr int horizontalSpace = 72;
       #else
        // Check, optional icon and submenu arrow may all be present together.
        constexpr int horizontalSpace = 100;
       #endif
        width = separator ? 50 : juce::roundToInt (juce::GlyphArrangement::getStringWidth (getPopupMenuFont(), text)) + horizontalSpace;
    }

    void drawPopupMenuItem (juce::Graphics& g, const juce::Rectangle<int>& area,
                            bool separator, bool enabled, bool highlighted, bool ticked,
                            bool submenu, const juce::String& text, const juce::String& shortcut,
                            const juce::Drawable* icon, const juce::Colour* customText) override
    {
       #if ! CHURCHDAW_MOBILE_SHELL
        auto row = area.reduced (2, 1);
        if (row.isEmpty()) return;
        if (separator)
        {
            g.setColour (juce::Colours::white.withAlpha (0.075f));
            g.fillRect (row.reduced (10, 0).withSizeKeepingCentre (juce::jmax (1, row.getWidth() - 20), 1));
            return;
        }
        if (enabled && (highlighted || ticked))
        {
            g.setColour (highlighted ? findColour (juce::PopupMenu::highlightedBackgroundColourId)
                                    : juce::Colour (0xff2a2c35));
            g.fillRoundedRectangle (row.toFloat(), 6.0f);
        }
        const auto ink = (customText != nullptr ? *customText
                          : highlighted ? findColour (juce::PopupMenu::highlightedTextColourId)
                                        : findColour (juce::PopupMenu::textColourId))
                            .withMultipliedAlpha (enabled ? 1.0f : 0.42f);
        auto textArea = row.reduced (10, 0);
        const auto mark = textArea.removeFromLeft (22).toFloat();
        if (ticked)
        {
            const auto c = mark.getCentre();
            juce::Path check;
            check.startNewSubPath (c.x - 4.5f, c.y);
            check.lineTo (c.x - 1.0f, c.y + 3.5f);
            check.lineTo (c.x + 5.0f, c.y - 3.5f);
            g.setColour (nordRedBright().withAlpha (enabled ? 1.0f : 0.42f));
            g.strokePath (check, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved,
                                                            juce::PathStrokeType::rounded));
        }
        // Icon and check are independent: a selected item never loses its
        // selection indicator merely because it also supplies artwork.
        if (icon != nullptr)
        {
            const auto imageArea = textArea.removeFromLeft (22).toFloat().withSizeKeepingCentre (16.0f, 16.0f);
            icon->drawWithin (g, imageArea, juce::RectanglePlacement::centred, enabled ? 1.0f : 0.42f);
            textArea.removeFromLeft (4);
        }
        if (submenu)
        {
            const auto end = textArea.removeFromRight (16).toFloat().getCentre();
            juce::Path arrow;
            arrow.startNewSubPath (end.x - 2.0f, end.y - 3.5f);
            arrow.lineTo (end.x + 1.5f, end.y);
            arrow.lineTo (end.x - 2.0f, end.y + 3.5f);
            g.setColour (ink.withMultipliedAlpha (0.80f));
            g.strokePath (arrow, juce::PathStrokeType (1.3f, juce::PathStrokeType::curved,
                                                            juce::PathStrokeType::rounded));
        }
        if (shortcut.isNotEmpty())
        {
            const auto font = juce::Font (juce::FontOptions (11.5f));
            const int keyWidth = juce::roundToInt (juce::GlyphArrangement::getStringWidth (font, shortcut)) + 12;
            // Leave usable label space even when a caller forces a narrow menu.
            const int available = juce::jmax (0, textArea.getWidth() - 40);
            auto keyArea = textArea.removeFromRight (juce::jmin (keyWidth, available));
            textArea.removeFromRight (8);
            g.setFont (font);
            g.setColour (ink.withMultipliedAlpha (0.62f));
            g.drawText (shortcut, keyArea, juce::Justification::centredRight, true);
        }
        g.setFont (getPopupMenuFont());
        g.setColour (ink);
        g.drawText (text, textArea, juce::Justification::centredLeft, true);
       #else
        auto row = area.reduced (4, 1);
        if (separator)
        {
            g.setColour (borderSoft());
            g.fillRect (row.getX() + 8, row.getCentreY(), juce::jmax (1, row.getWidth() - 16), 1);
            return;
        }
        if (highlighted && enabled)
        {
            g.setColour (nordRed().withAlpha (0.16f));
            g.fillRoundedRectangle (row.toFloat(), 5.0f);
        }
        auto ink = customText != nullptr ? *customText : txtPrimary();
        if (! enabled) ink = txtDisabled();
        auto textArea = row.reduced (10, 0);
        auto mark = textArea.removeFromLeft (22);
        if (icon != nullptr) icon->drawWithin (g, mark.toFloat().reduced (2, 5), juce::RectanglePlacement::centred, enabled ? 1.0f : 0.35f);
        else if (ticked)
        {
            const auto c = mark.toFloat().getCentre();
            juce::Path tick;
            tick.startNewSubPath (c.x - 5, c.y); tick.lineTo (c.x - 1, c.y + 4); tick.lineTo (c.x + 6, c.y - 4);
            g.setColour (nordRedBright());
            g.strokePath (tick, juce::PathStrokeType (1.8f));
        }
        if (submenu)
        {
            auto end = textArea.removeFromRight (16).toFloat().getCentre();
            juce::Path arrow;
            arrow.startNewSubPath (end.x - 2, end.y - 4); arrow.lineTo (end.x + 2, end.y); arrow.lineTo (end.x - 2, end.y + 4);
            g.setColour (ink); g.strokePath (arrow, juce::PathStrokeType (1.4f));
        }
        if (shortcut.isNotEmpty())
        {
            g.setFont (juce::Font (juce::FontOptions (11.0f)));
            const int keyW = juce::roundToInt (juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), shortcut)) + 16;
            g.setColour (txtDisabled());
            g.drawText (shortcut, textArea.removeFromRight (keyW), juce::Justification::centredRight);
        }
        g.setFont (getPopupMenuFont()); g.setColour (ink);
        g.drawText (text, textArea, juce::Justification::centredLeft, true);
       #endif
    }

    void drawPopupMenuSectionHeader (juce::Graphics& g, const juce::Rectangle<int>& area,
                                     const juce::String& sectionName) override
    {
       #if ! CHURCHDAW_MOBILE_SHELL
        g.setColour (findColour (juce::PopupMenu::headerTextColourId));
        g.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
        g.drawText (sectionName, area.reduced (12, 0).withTrimmedTop (4),
                    juce::Justification::centredLeft, true);
        return;
       #endif
        auto r = area.reduced (9, 0);

        auto font = juce::Font (juce::FontOptions (10.5f)).boldened().withExtraKerningFactor (0.16f);
        g.setFont (font);
        g.setColour (nordRedBright().withAlpha (0.85f));

        const int textW = juce::jmin (r.getWidth(),
                                      (int) std::ceil (juce::GlyphArrangement::getStringWidth (font, sectionName)) + 6);
        g.drawText (sectionName, r.removeFromLeft (textW), juce::Justification::centredLeft, false);

        if (r.getWidth() > 14)
        {
            g.setColour (juce::Colours::white.withAlpha (0.10f));
            g.fillRect (r.withTrimmedLeft (8).withSizeKeepingCentre (r.getWidth() - 8, 1));
        }
    }

    void getIdealPopupMenuSectionHeaderSizeWithOptions (const juce::String& text, int standardHeight,
                                                        int& width, int& height,
                                                        const juce::PopupMenu::Options& options) override
    {
       #if CHURCHDAW_MOBILE_SHELL
        juce::LookAndFeel_V4::getIdealPopupMenuSectionHeaderSizeWithOptions (text, standardHeight, width, height, options);
       #else
        juce::ignoreUnused (options);
        getIdealPopupMenuItemSize (text, false, standardHeight, width, height);
        height = juce::jmax (30, standardHeight);
       #endif
    }

    void drawPopupMenuUpDownArrow (juce::Graphics& g, int width, int height, bool up) override
    {
       #if CHURCHDAW_MOBILE_SHELL
        juce::LookAndFeel_V4::drawPopupMenuUpDownArrow (g, width, height, up);
       #else
        const float cx = width * 0.5f, cy = height * 0.5f;
        const float sign = up ? -1.0f : 1.0f;
        g.setColour (bgFloating());
        g.fillRect (0, 0, width, height);
        juce::Path arrow;
        arrow.startNewSubPath (cx - 4.0f, cy - sign * 1.5f);
        arrow.lineTo (cx, cy + sign * 2.0f);
        arrow.lineTo (cx + 4.0f, cy - sign * 1.5f);
        g.setColour (txtSecondary());
        g.strokePath (arrow, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved,
                                                       juce::PathStrokeType::rounded));
       #endif
    }

    //==============================================================================
    // ComboBox com o mesmo acabamento "vidro fosco" dos cards/chips + chevron
    // NEUTRO (disciplina de vermelho: a setinha deixa de ser vermelha).
    // O combo mostra o PRESET -- a informacao que muda. Depois de rebaixar a
    // categoria a rotulo de apoio, ele passa a ser o texto principal do card.
    void drawButtonText (juce::Graphics& g, juce::TextButton& button,
                         bool shouldDrawButtonAsHighlighted,
                         bool shouldDrawButtonAsDown) override
    {
        juce::ignoreUnused (shouldDrawButtonAsHighlighted,
                            shouldDrawButtonAsDown);
        const auto id = button.getComponentID();

        if (id == "timbrePickerClose")
        {
            g.setColour (juce::Colours::white.withAlpha (button.isEnabled() ? 0.94f : 0.42f));
            g.setFont (juce::Font (juce::FontOptions (20.0f, juce::Font::plain)));
            g.drawText (juce::String::fromUTF8 ("\xc3\x97"), button.getLocalBounds(),
                        juce::Justification::centred, false);
            return;
        }

        if (id == "timbrePickerRemove")
        {
            auto area = button.getLocalBounds().reduced (16, 5);
            const float iconSide = juce::jmin (18.0f, (float) area.getHeight() - 4.0f);
            auto icon = juce::Rectangle<float> ((float) area.getX(),
                                                (float) area.getCentreY() - iconSide * 0.5f,
                                                iconSide, iconSide);
            const auto colour = nordRedBright().withAlpha (button.isEnabled() ? 0.92f : 0.42f);
            g.setColour (colour);
            auto bin = icon.reduced (4.0f, 4.0f);
            g.drawRoundedRectangle (bin.withTrimmedTop (3.0f), 1.2f, 1.4f);
            g.drawLine (bin.getX() - 1.0f, bin.getY() + 2.0f,
                        bin.getRight() + 1.0f, bin.getY() + 2.0f, 1.4f);
            g.drawLine (bin.getCentreX() - 2.5f, bin.getY(),
                        bin.getCentreX() + 2.5f, bin.getY(), 1.4f);

            auto textArea = area.withTrimmedLeft (juce::roundToInt (iconSide + 10.0f));
            g.setFont (juce::Font (juce::FontOptions (11.5f, juce::Font::bold)));
            g.drawFittedText (button.getButtonText(), textArea,
                              juce::Justification::centredLeft, 1, 0.80f);
            return;
        }

        if (id == "panicOutline")
        {
            if (usesDesktopControls (button))
            {
                g.setColour (txtPrimary().withAlpha (button.isEnabled() ? 1.0f : 0.42f));
                g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)).withExtraKerningFactor (0.035f));
                g.drawFittedText (button.getButtonText(), button.getLocalBounds().reduced (5, 1),
                                  juce::Justification::centred, 1);
                return;
            }
            auto bounds = button.getLocalBounds().toFloat();
            g.setFont (juce::Font (juce::FontOptions (13.5f, juce::Font::bold))
                           .withExtraKerningFactor (0.16f));

            g.setColour (juce::Colours::black.withAlpha (0.80f));
            g.drawText (button.getButtonText(), bounds.translated (0.0f, 1.2f),
                        juce::Justification::centred, false);

            const auto textCol = shouldDrawButtonAsDown ? juce::Colours::white
                               : (shouldDrawButtonAsHighlighted ? juce::Colours::white : juce::Colour (0xfffff0f2));
            g.setColour (textCol);
            g.drawText (button.getButtonText(), bounds,
                        juce::Justification::centred, false);
            return;
        }

        if (id == "stripAction")
        {
            const auto colour = button.findColour (
                button.getToggleState() ? juce::TextButton::textColourOnId
                                        : juce::TextButton::textColourOffId)
                .withMultipliedAlpha (button.isEnabled() ? 1.0f : 0.42f);
            g.setColour (colour);
            g.setFont (juce::Font (juce::FontOptions (9.0f, juce::Font::bold)));
            g.drawFittedText (button.getButtonText().toUpperCase(),
                              button.getLocalBounds().reduced (2, 1),
                              juce::Justification::centred, 1, 0.62f);
            return;
        }

        if ((id.startsWith ("nav") || id == "headerNav") && usesDesktopShell (button))
        {
            drawDesktopNavigationContents (g, button, shouldDrawButtonAsHighlighted,
                                            button.getToggleState());
            return;
        }

        if (! id.startsWith ("nav"))
        {
            juce::LookAndFeel_V4::drawButtonText (
                g, button, shouldDrawButtonAsHighlighted,
                shouldDrawButtonAsDown);
            return;
        }

        const bool stackedTouch = (bool) button.getProperties()["churchTouchNav"]
                               && button.getWidth() < 120 && button.getHeight() >= 44;
        auto text = id == "navSave" ? (stackedTouch ? saveStatusLabel (button) : saveStatusLabel (button).toUpperCase())
                    : stackedTouch ? button.getButtonText() : button.getButtonText().toUpperCase();
        const auto font = getTextButtonFont (button, button.getHeight());
        auto area = button.getLocalBounds().reduced (6, 2);
        const float iconSide = juce::jmin (14.0f, (float) area.getHeight() - 4.0f);
        const float textWidth = juce::GlyphArrangement::getStringWidth (font, text);
        const float groupWidth = juce::jmin ((float) area.getWidth(),
                                             iconSide + 7.0f + textWidth);
        const float startX = (float) area.getCentreX() - groupWidth * 0.5f;
        auto icon = juce::Rectangle<float> (
            startX, (float) area.getCentreY() - iconSide * 0.5f,
            iconSide, iconSide);
        if (stackedTouch)icon.setPosition ((float) area.getCentreX() - iconSide * 0.5f, (float) area.getY() + 3.0f);

        const auto textColour = button.findColour (
            button.getToggleState() ? juce::TextButton::textColourOnId
                                    : juce::TextButton::textColourOffId)
            .withMultipliedAlpha (button.isEnabled() ? 1.0f : 0.45f);
        const auto navColour = id == "navSave" ? textColour
                             : id == "navLive" && button.getToggleState()
                                 ? nordRedBright()
                                 : textColour;
        g.setColour (navColour);

        if (id == "navInstruments")
        {
            g.drawRoundedRectangle (icon.reduced (0.5f), 1.5f, 1.0f);
            for (int key = 1; key < 4; ++key)
            {
                const float x = icon.getX() + icon.getWidth() * (float) key / 4.0f;
                g.drawVerticalLine (juce::roundToInt (x),
                                    icon.getY() + 1.5f, icon.getBottom() - 1.5f);
            }
        }
        else if (id == "navLive")
        {
            for (int bar = 0; bar < 5; ++bar)
            {
                const float h = iconSide * (0.28f + 0.16f
                    * (float) (2 - std::abs (2 - bar)));
                g.fillRoundedRectangle (icon.getX() + (float) bar * iconSide / 5.0f,
                                        icon.getCentreY() - h * 0.5f,
                                        1.2f, h, 0.6f);
            }
        }
        else if (id == "navLiveCam")
        {
            if ((bool) button.getProperties()["recording"])
            { g.setColour (nordRedBright()); g.fillEllipse (icon.reduced (2.0f)); }
            else
            {
                auto body = icon.withTrimmedRight (4.0f).reduced (0.7f, 2.5f);
                g.drawRoundedRectangle (body, 1.8f, 1.1f);
                juce::Path lens; lens.startNewSubPath (icon.getRight(), icon.getY()+3);
                lens.lineTo (body.getRight(), icon.getCentreY()-2); lens.lineTo (body.getRight(), icon.getCentreY()+2);
                lens.lineTo (icon.getRight(), icon.getBottom()-3); lens.closeSubPath();
                g.strokePath (lens, juce::PathStrokeType (1.1f));
            }
        }
        else if (id == "navMidi")
        {
            g.drawLine (icon.getX() + 1.0f, icon.getCentreY(),
                        icon.getRight() - 1.0f, icon.getCentreY(), 1.1f);
            g.drawEllipse (icon.getX(), icon.getCentreY() - 2.0f,
                           4.0f, 4.0f, 1.0f);
            g.drawEllipse (icon.getRight() - 4.0f, icon.getCentreY() - 2.0f,
                           4.0f, 4.0f, 1.0f);
        }
        else if (id == "navSave" && text.containsIgnoreCase ("falhou"))
        {
            drawDesktopNavigationIcon (g, "navSaveFailed", icon, false);
        }
        else if (id == "navSave")
        {
            g.drawLine (icon.getX() + 1.0f, icon.getCentreY(),
                        icon.getX() + 4.5f, icon.getBottom() - 2.0f, 1.7f);
            g.drawLine (icon.getX() + 4.5f, icon.getBottom() - 2.0f,
                        icon.getRight() - 1.0f, icon.getY() + 2.0f, 1.7f);
        }
        else if (id == "navPad")
        {
            const float cell = (iconSide - 2.0f) * 0.5f;
            for (int y = 0; y < 2; ++y)
                for (int x = 0; x < 2; ++x)
                    g.drawRoundedRectangle (icon.getX() + (float) x * (cell + 2.0f),
                                            icon.getY() + (float) y * (cell + 2.0f),
                                            cell, cell, 1.0f, 0.8f);
        }
        else if (id == "navPlugins")
        {
            juce::Path bolt;
            bolt.startNewSubPath (icon.getCentreX() + 1.0f, icon.getY());
            bolt.lineTo (icon.getX() + 2.0f, icon.getCentreY() + 1.0f);
            bolt.lineTo (icon.getCentreX(), icon.getCentreY() + 1.0f);
            bolt.lineTo (icon.getCentreX() - 1.0f, icon.getBottom());
            bolt.lineTo (icon.getRight() - 1.0f, icon.getCentreY() - 2.0f);
            bolt.lineTo (icon.getCentreX() + 1.0f, icon.getCentreY() - 2.0f);
            bolt.closeSubPath();
            g.fillPath (bolt);
        }
        else if (id == "navLibraries")
        {
            g.drawRoundedRectangle (icon.reduced (1.0f), 2.0f, 1.0f);
            g.drawHorizontalLine (juce::roundToInt (icon.getY() + 3.5f),
                                  icon.getX() + 1.0f, icon.getRight() - 1.0f);
        }

        g.setFont (font);
        g.setColour (navColour);
        auto textArea = juce::Rectangle<int> (
            juce::roundToInt (startX + iconSide + 7.0f), area.getY(),
            juce::jmax (1, area.getRight()
                           - juce::roundToInt (startX + iconSide + 7.0f)),
            area.getHeight());
        if (stackedTouch)textArea = area.withTrimmedTop (20).reduced (0, 1);
        g.drawFittedText (text, textArea, stackedTouch ? juce::Justification::centred : juce::Justification::centredLeft,
                          1, 0.72f);
    }

    juce::Font getTextButtonFont (juce::TextButton& button,
                                  int buttonHeight) override
    {
        float height = juce::jlimit (9.0f, 15.0f,
                                     (float) buttonHeight * 0.55f);

        const bool navigation = button.getComponentID() == "headerNav"
                             || button.getComponentID().startsWith ("nav");
        if (navigation && usesDesktopShell (button))
        {
            height = button.getComponentID() == "navSave" ? 11.5f : 12.5f;
            if (button.getWidth() < 100) height = juce::jmin (height, 11.5f);
            return juce::Font (juce::FontOptions (height, juce::Font::bold))
                .withExtraKerningFactor (0.008f);
        }
        if (navigation)
            height = juce::jmin (height, 11.6f);

        // As tres acoes no rodape de cada strip tem ~32 px. A fonte padrao
        // desktop era grande demais e transformava LOOP/ORGAO em reticencias.
       #if ! CHURCHDAW_MOBILE_SHELL
        if (button.getWidth() < 44)
            height = juce::jmin (height, 9.5f);
        else if (button.getWidth() < 64)
            height = juce::jmin (height, 11.0f);
       #endif

        if (! navigation && usesDesktopControls (button))
        {
            height = juce::jmin (height, 13.0f);
            const bool compactAction = button.getWidth() < 64;
            return juce::Font (juce::FontOptions (height, compactAction ? juce::Font::bold : juce::Font::plain));
        }
        auto font = juce::Font (juce::FontOptions (height, juce::Font::bold));
        return navigation ? font.withExtraKerningFactor (0.035f) : font;
    }

    juce::Font getComboBoxFont (juce::ComboBox& box) override
    {
        // Preset e o heroi do card: fonte sobe com a altura do chip (modo palco ~42).
        float height = juce::jlimit (13.0f, 18.0f,
                                     (float) box.getHeight() * 0.48f);

        // Presets e slots de FX precisam caber completos na mesa de oito
        // canais. No editor focado, onde ha largura, a fonte volta ao teto.
       #if CHURCHDAW_MOBILE_SHELL
        if (box.getWidth() < 72)
            height = juce::jmin (height, 9.5f);
        else if (box.getWidth() < 96)
            height = juce::jmin (height, 11.0f);
        else if (box.getWidth() < 130)
            height = juce::jmin (height, 13.0f);
        else if (box.getWidth() < 156)
            height = juce::jmin (height, 14.2f);
       #else
        if (box.getWidth() < 96)
            height = juce::jmin (height, 10.5f);
        else if (box.getWidth() < 126)
            height = juce::jmin (height, 12.5f);
       #endif

        return juce::Font (juce::FontOptions (height, juce::Font::plain));
    }

    void positionComboBoxText (juce::ComboBox& box, juce::Label& label) override
    {
        if (! usesDesktopControls (box))
        {
            const bool ultraCompact = box.getWidth() < 72;
            const int inset = ultraCompact ? 4 : box.getWidth() < 116 ? 6 : 9;
            const int arrowLane = ultraCompact ? 12 : juce::jmin (22, box.getHeight());
            label.setBorderSize (ultraCompact ? juce::BorderSize<int> (0, 1, 0, 1)
                                              : juce::BorderSize<int> (1, 5, 1, 5));
            label.setBounds (inset, 1,
                             juce::jmax (0, box.getWidth() - arrowLane - inset - (ultraCompact ? 1 : 2)),
                             juce::jmax (0, box.getHeight() - 2));
            label.setFont (getComboBoxFont (box));
            label.setMinimumHorizontalScale (ultraCompact ? 0.68f : 0.78f);
            return;
        }
        const int inset = box.getWidth() < 96 ? 5 : 9;
        const int arrowLane = juce::jmin (30, box.getHeight());
        label.setBounds (inset, 1, juce::jmax (0, box.getWidth() - arrowLane - inset - 2),
                          juce::jmax (0, box.getHeight() - 2));
        label.setFont (getComboBoxFont (box));
    }

    // Balao de valor do knob, na mesma linguagem do visor de programa: vidro
    // escuro, texto claro. O balao padrao do JUCE e cinza-claro com borda e
    // parecia um tooltip de sistema pousado em cima do painel.
    void drawBubble (juce::Graphics& g, juce::BubbleComponent& comp,
                     const juce::Point<float>& tip, const juce::Rectangle<float>& body) override
    {
        juce::ignoreUnused (tip);
        if (usesDesktopControls (comp))
        {
            g.setColour (bgFloating());
            g.fillRoundedRectangle (body, radiusControl);
            g.setColour (juce::Colour (0xff4a515d));
            g.drawRoundedRectangle (body.reduced (0.5f), radiusControl, 1.0f);
            return;
        }

        g.setColour (juce::Colours::black.withAlpha (0.55f));
        g.fillRoundedRectangle (body.translated (0.0f, 2.0f), radiusControl);

        g.setColour (juce::Colour (0xff0e1014));
        g.fillRoundedRectangle (body, radiusControl);
        g.setColour (nordRed().withAlpha (0.55f));
        g.drawRoundedRectangle (body.reduced (0.5f), radiusControl, 1.2f);
    }

    void setComponentEffectForBubbleComponent (juce::BubbleComponent&) override {}

    void drawComboBox (juce::Graphics& g, int width, int height, bool down,
                       int, int, int, int, juce::ComboBox& box) override
    {
        auto bounds = juce::Rectangle<float> (0, 0, (float) width, (float) height).reduced (0.5f);
        const float radius = juce::jmin (7.0f, height * 0.24f);
        const bool focus = box.hasKeyboardFocus (true);
        if (usesDesktopControls (box))
        {
            if (bounds.isEmpty()) return;
            const bool hover = box.isEnabled() && (box.isMouseOver (true) || down || box.isPopupActive());
            const float alpha = box.isEnabled() ? 1.0f : 0.42f;
            const auto surface = down || box.isPopupActive() ? juce::Colour (0xff302a34)
                               : hover ? juce::Colour (0xff2c333d) : juce::Colour (0xff252b34);
            g.setColour (surface.withAlpha (alpha));
            g.fillRoundedRectangle (bounds, radius);
            g.setColour ((focus ? nordRedBright() : juce::Colours::white.withAlpha (hover ? 0.18f : 0.11f))
                             .withMultipliedAlpha (alpha));
            g.drawRoundedRectangle (bounds, radius, focus ? 1.5f : 1.0f);
            const float arrowWidth = juce::jmin (30.0f, (float) height);
            const float cx = width - arrowWidth * 0.5f;
            const float cy = height * 0.5f;
            juce::Path chevron;
            chevron.startNewSubPath (cx - 3.2f, cy - 1.3f);
            chevron.lineTo (cx, cy + 1.8f);
            chevron.lineTo (cx + 3.2f, cy - 1.3f);
            g.setColour ((focus || hover ? txtPrimary() : txtSecondary()).withAlpha (alpha));
            g.strokePath (chevron, juce::PathStrokeType (1.3f, juce::PathStrokeType::curved,
                                                            juce::PathStrokeType::rounded));
            return;
        }
        const float alpha = box.isEnabled() ? 1.0f : 0.42f;
        const bool activePopup = down || box.isPopupActive();
        g.setGradientFill (juce::ColourGradient (
            (activePopup ? juce::Colour (0xff26202a) : box.isMouseOver (true) ? juce::Colour (0xff202631) : juce::Colour (0xff151922)).withMultipliedAlpha (alpha),
            bounds.getX(), bounds.getY(),
            (activePopup ? juce::Colour (0xff1e1821) : juce::Colour (0xff1c212c)).withMultipliedAlpha (alpha),
            bounds.getX(), bounds.getBottom(), false));
        g.fillRoundedRectangle (bounds, radius);
        g.setColour ((focus || activePopup ? nordRedBright().withAlpha (0.80f)
                                           : juce::Colour (0xff343c4a).withAlpha (0.85f)).withMultipliedAlpha (alpha));
        g.drawRoundedRectangle (bounds, radius, focus ? 1.5f : 1.0f);
        const float cx = width - (width < 72 ? 8.0f : juce::jmax (13.0f, height * 0.40f));
        const float cy = height * 0.5f;
        const float arm = width < 72 ? 2.6f : juce::jlimit (3.2f, 4.4f, height * 0.11f);
        juce::Path caret;
        caret.startNewSubPath (cx - arm, cy - 1.5f);
        caret.lineTo (cx, cy + 2.0f);
        caret.lineTo (cx + arm, cy - 1.5f);
        g.setColour ((activePopup || focus ? nordRedBright() : txtSecondary()).withMultipliedAlpha (alpha));
        g.strokePath (caret, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved,
                                                        juce::PathStrokeType::rounded));
    }

    void fillTextEditorBackground (juce::Graphics& g, int width, int height, juce::TextEditor& editor) override
    {
       #if ! CHURCHDAW_MOBILE_SHELL
        if (! usesDesktopControls (editor))
        {
            juce::LookAndFeel_V4::fillTextEditorBackground (g, width, height, editor);
            return;
        }
       #endif
        const auto colour = editor.findColour (juce::TextEditor::backgroundColourId);
        if (colour.isTransparent()) return;
        const auto bounds = juce::Rectangle<float> ((float) width, (float) height);
        // The editor may be opaque; paint its corners too before rounding the
        // inset frame, rather than leaving uninitialised white corner pixels.
        if (editor.isOpaque()) g.fillAll (colour);
        g.setColour (colour);
        g.fillRoundedRectangle (bounds, radiusControl);
    }

    void drawTextEditorOutline (juce::Graphics& g, int width, int height, juce::TextEditor& editor) override
    {
       #if ! CHURCHDAW_MOBILE_SHELL
        if (! usesDesktopControls (editor))
        {
            juce::LookAndFeel_V4::drawTextEditorOutline (g, width, height, editor);
            return;
        }
       #endif
        const bool focused = editor.hasKeyboardFocus (true) && ! editor.isReadOnly() && editor.isEnabled();
        auto colour = editor.findColour (focused ? juce::TextEditor::focusedOutlineColourId
                                                 : juce::TextEditor::outlineColourId);
        if (colour.isTransparent()) return;
        if (! editor.isEnabled()) colour = colour.withMultipliedAlpha (0.42f);
        else if (! focused && ! editor.isReadOnly() && editor.isMouseOver (true)) colour = colour.brighter (0.14f);
        g.setColour (colour);
        g.drawRoundedRectangle (juce::Rectangle<float> ((float) width, (float) height).reduced (0.75f),
                                radiusControl, focused ? 1.5f : 1.0f);
    }

    static juce::TextLayout desktopTooltipLayout (const juce::String& text, float maxWidth)
    {
        juce::AttributedString content;
        content.setJustification (juce::Justification::centredLeft);
        content.setLineSpacing (2.0f);
        content.append (text, juce::Font (juce::FontOptions (12.5f)), txtPrimary());
        juce::TextLayout layout;
        layout.createLayout (content, juce::jmax (1.0f, maxWidth));
        return layout;
    }

    juce::Rectangle<int> getTooltipBounds (const juce::String& text, juce::Point<int> screenPos,
                                          juce::Rectangle<int> parentArea) override
    {
       #if CHURCHDAW_MOBILE_SHELL
        return juce::LookAndFeel_V4::getTooltipBounds (text, screenPos, parentArea);
       #else
        const auto layout = desktopTooltipLayout (text, (float) juce::jmin (340, juce::jmax (1, parentArea.getWidth() - 24)));
        const int width = juce::jmin (parentArea.getWidth(), (int) std::ceil (layout.getWidth()) + 24);
        const int height = juce::jmin (parentArea.getHeight(), (int) std::ceil (layout.getHeight()) + 18);
        return juce::Rectangle<int> (screenPos.x > parentArea.getCentreX() ? screenPos.x - width - 12 : screenPos.x + 18,
                                     screenPos.y > parentArea.getCentreY() ? screenPos.y - height - 8 : screenPos.y + 12,
                                     width, height).constrainedWithin (parentArea);
       #endif
    }

    void drawTooltip (juce::Graphics& g, const juce::String& text, int width, int height) override
    {
       #if CHURCHDAW_MOBILE_SHELL
        juce::LookAndFeel_V4::drawTooltip (g, text, width, height);
       #else
        const auto bounds = juce::Rectangle<float> ((float) width, (float) height);
        g.setColour (findColour (juce::TooltipWindow::backgroundColourId));
        g.fillRoundedRectangle (bounds, 7.0f);
        g.setColour (findColour (juce::TooltipWindow::outlineColourId));
        g.drawRoundedRectangle (bounds.reduced (0.5f), 7.0f, 1.0f);
        const auto layout = desktopTooltipLayout (text, juce::jmax (1.0f, width - 24.0f));
        layout.draw (g, bounds.reduced (12.0f, 9.0f));
       #endif
    }

    void drawScrollbar (juce::Graphics& g, juce::ScrollBar& bar, int x, int y, int width, int height,
                        bool isScrollbarVertical, int thumbStartPosition, int thumbSize,
                        bool isMouseOver, bool isMouseDown) override
    {
        auto track = juce::Rectangle<float> ((float) x, (float) y, (float) width, (float) height);
        if (usesDesktopControls (bar))
        {
            if (thumbSize <= 0 || track.isEmpty()) return;
            const float thickness = juce::jmin (isScrollbarVertical ? track.getWidth() : track.getHeight(),
                                                isMouseOver || isMouseDown ? 7.0f : 5.0f);
            auto thumb = isScrollbarVertical
                ? juce::Rectangle<float> (track.getCentreX() - thickness * 0.5f, (float) thumbStartPosition,
                                           thickness, (float) thumbSize)
                : juce::Rectangle<float> ((float) thumbStartPosition, track.getCentreY() - thickness * 0.5f,
                                           (float) thumbSize, thickness);
            auto colour = bar.findColour (juce::ScrollBar::thumbColourId);
            if (isMouseDown) colour = colour.brighter (0.28f);
            else if (isMouseOver) colour = colour.brighter (0.16f);
            g.setColour (colour.withMultipliedAlpha (bar.isEnabled() ? 1.0f : 0.42f));
            g.fillRoundedRectangle (thumb.getIntersection (track).reduced (0.5f), thickness * 0.5f);
            return;
        }
       #if CHURCHDAW_MOBILE_SHELL
        if (thumbSize <= 0 || track.isEmpty())
            return;
        const float thickness = juce::jmin (isScrollbarVertical ? track.getWidth() : track.getHeight(),
                                            isMouseOver || isMouseDown ? 6.0f : 4.5f);
        auto thumb = isScrollbarVertical
            ? juce::Rectangle<float> (track.getCentreX() - thickness * 0.5f, (float) thumbStartPosition,
                                       thickness, (float) thumbSize)
            : juce::Rectangle<float> ((float) thumbStartPosition, track.getCentreY() - thickness * 0.5f,
                                       (float) thumbSize, thickness);
        const float alpha = isMouseDown ? 0.44f : isMouseOver ? 0.32f : 0.22f;
        g.setColour (juce::Colours::white.withAlpha (alpha * (bar.isEnabled() ? 1.0f : 0.4f)));
        g.fillRoundedRectangle (thumb.getIntersection (track).reduced (0.5f), thickness * 0.5f);
       #else
        g.setColour (bar.findColour (juce::ScrollBar::trackColourId));
        g.fillRoundedRectangle (track.reduced (isScrollbarVertical ? 2.0f : 0.0f,
                                               isScrollbarVertical ? 0.0f : 2.0f), 4.0f);

        juce::Rectangle<float> thumb;
        if (isScrollbarVertical)
            thumb = { track.getX() + 2.0f, (float) thumbStartPosition,
                      juce::jmax (4.0f, track.getWidth() - 4.0f), (float) thumbSize };
        else
            thumb = { (float) thumbStartPosition, track.getY() + 2.0f,
                      (float) thumbSize, juce::jmax (4.0f, track.getHeight() - 4.0f) };

        auto col = bar.findColour (juce::ScrollBar::thumbColourId);
        if (isMouseDown)
            col = col.brighter (0.16f);
        else if (isMouseOver)
            col = col.brighter (0.08f);
        g.setColour (col);
        g.fillRoundedRectangle (thumb.reduced (0.5f), 4.0f);
       #endif
    }
};
