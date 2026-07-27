#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

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

    // ---- superficies ----
    static juce::Colour bgApp()          { return juce::Colour (0xff101216); }   // fundo geral
    static juce::Colour bgCard()         { return juce::Colour (0xff1a1e24); }   // painel/card
    static juce::Colour bgCardSel()      { return juce::Colour (0xff242932); }   // card selecionado
    static juce::Colour bgControl()      { return juce::Colour (0xff22272f); }   // campos/controles
    static juce::Colour borderDefault()  { return juce::Colour (0xff343a45); }

    // ---- texto ----
    static juce::Colour txtPrimary()     { return juce::Colour (0xfff4f6f8); }
    static juce::Colour txtSecondary()   { return juce::Colour (0xffa9afb8); }
    static juce::Colour txtDisabled()    { return juce::Colour (0xff707680); }

    // ---- acento e status ----
    static juce::Colour nordRed()        { return juce::Colour (0xffe91845); }   // acento global
    static juce::Colour nordRedBright()  { return juce::Colour (0xffff2a58); }   // hover/destaque
    static juce::Colour nordRedDark()    { return juce::Colour (0xff9c0f2e); }
    static juce::Colour amberSolo()      { return juce::Colour (0xfff9a825); }   // solo/atencao
    static juce::Colour greenOk()        { return juce::Colour (0xff55c878); }   // status normal
    static juce::Colour redClip()        { return juce::Colour (0xffff1744); }   // erro/clip

    // ---- cor por categoria de instrumento (apoio, nunca protagonista) ----
    static juce::Colour catPiano()       { return juce::Colour (0xff5b8a9a); }
    static juce::Colour catPad()         { return juce::Colour (0xff8a6ba8); }
    static juce::Colour catBell()        { return juce::Colour (0xffc4a35a); }
    static juce::Colour catStrings()     { return juce::Colour (0xff4a9b8c); }
    static juce::Colour catOrgan()       { return juce::Colour (0xffb7773c); }
    static juce::Colour catLead()        { return juce::Colour (0xff4e86c5); }
    static juce::Colour catPlugin()      { return juce::Colour (0xffc9823a); }

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
    // Antes havia card=10, botao=6, combo=5, chip=7, medidor=3 na mesma tela.
    // O olho nao le o numero, mas sente a bagunca.
    static constexpr float radiusControl = 4.0f;   // botoes, combos, chips
    static constexpr float radiusCard    = 8.0f;   // cards de canal
    static constexpr float radiusPanel   = 12.0f;  // paineis e visores

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
    }

    void drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour& backgroundColour,
                               bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override
    {
        auto bounds = button.getLocalBounds().toFloat().reduced (0.5f);
        const float r = radiusControl;
        auto base = backgroundColour;

        // botao "fantasma" (fundo transparente): so um contorno sutil, sem chapa
        if (base.isTransparent())
        {
            g.setColour (juce::Colours::white.withAlpha (shouldDrawButtonAsHighlighted ? 0.16f : 0.09f));
            g.drawRoundedRectangle (bounds, r, 1.0f);
            return;
        }

        // (o tratamento "chapa preta + LED redondo ao lado" saiu daqui: em botao
        // pequeno o pontinho virava sujeira e o estado ficava MENOS claro que a
        // chapa inteira acesa)

        // Antes qualquer clique pintava o botao de VERMELHO — o vermelho deixava de
        // significar "ativo". Agora clicar so escurece; vermelho fica reservado pro
        // estado ligado (buttonOnColourId).
        if (shouldDrawButtonAsDown)             base = base.darker (0.28f);
        else if (shouldDrawButtonAsHighlighted) base = base.brighter (0.14f);

        const bool accent = base.getSaturation() > 0.30f;

        g.setColour (juce::Colours::black.withAlpha (0.25f));
        g.fillRoundedRectangle (bounds.translated (0.0f, 1.0f), r);

        g.setGradientFill (juce::ColourGradient (base.brighter (0.10f), 0.0f, bounds.getY(),
                                                 base.darker (0.16f), 0.0f, bounds.getBottom(), false));
        g.fillRoundedRectangle (bounds, r);

        g.setColour (juce::Colours::white.withAlpha (0.07f));   // brilho de 1px no topo
        g.drawLine (bounds.getX() + 5.0f, bounds.getY() + 1.0f, bounds.getRight() - 5.0f, bounds.getY() + 1.0f, 1.0f);

        g.setColour (accent ? base.brighter (0.45f).withAlpha (0.7f) : panelOutline());
        g.drawRoundedRectangle (bounds, r, 1.0f);
    }

    //==============================================================================
    // Checkbox premium: caixinha de vidro escuro com check vermelho aceso
    // (substitui o tick padrao do JUCE nos toggles e no seletor de audio)
    void drawTickBox (juce::Graphics& g, juce::Component&, float x, float y, float w, float h,
                      bool ticked, bool isEnabled, bool, bool) override
    {
        auto box = juce::Rectangle<float> (x, y, w, h).reduced (1.5f);

        g.setColour (juce::Colours::black.withAlpha (0.30f));
        g.fillRoundedRectangle (box.translated (0.0f, 1.0f), 4.0f);

        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff2b2e35), box.getX(), box.getY(),
                                                 juce::Colour (0xff1a1c21), box.getX(), box.getBottom(), false));
        g.fillRoundedRectangle (box, 4.0f);

        g.setColour (ticked ? nordRedBright().withAlpha (0.9f) : panelOutline());
        g.drawRoundedRectangle (box, 4.0f, ticked ? 1.4f : 1.0f);

        if (ticked)
        {
            juce::Path tick;
            tick.startNewSubPath (box.getX() + box.getWidth() * 0.24f, box.getCentreY() + box.getHeight() * 0.02f);
            tick.lineTo          (box.getX() + box.getWidth() * 0.43f, box.getBottom() - box.getHeight() * 0.26f);
            tick.lineTo          (box.getRight() - box.getWidth() * 0.20f, box.getY() + box.getHeight() * 0.24f);

            g.setColour (nordRed().withAlpha (0.35f));   // brilho
            g.strokePath (tick, juce::PathStrokeType (3.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setColour (isEnabled ? nordRedBright() : nordRed());
            g.strokePath (tick, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
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
        // arco na cor da CATEGORIA do canal (o strip publica em rotarySliderFill);
        // sem isso volta ao vermelho da marca
        const auto arcColour = slider.findColour (juce::Slider::rotarySliderFillColourId);
        auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (2.0f);
        const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
        const float cx = bounds.getCentreX();
        const float cy = bounds.getCentreY();
        const float angle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);

        auto onCircle = [cx, cy] (float a, float r)
        {
            return juce::Point<float> (cx + std::sin (a) * r, cy - std::cos (a) * r);
        };

        // sombra: levanta o knob do card
        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.fillEllipse (cx - radius * 0.94f, cy - radius * 0.94f + 1.5f, radius * 1.88f, radius * 1.88f);

        // Corpo com MAIS CONTRASTE contra o card: o preto quase puro que estava
        // aqui sumia no fundo escuro e o knob virava uma mancha.
        const float bodyR = radius * 0.70f;
        juce::ColourGradient body (juce::Colour (0xff41454e), cx - bodyR * 0.45f, cy - bodyR * 0.7f,
                                   juce::Colour (0xff15181d), cx + bodyR * 0.4f, cy + bodyR * 0.8f, true);
        g.setGradientFill (body);
        g.fillEllipse (cx - bodyR, cy - bodyR, bodyR * 2.0f, bodyR * 2.0f);

        g.setColour (juce::Colours::black.withAlpha (0.8f));
        g.drawEllipse (cx - bodyR, cy - bodyR, bodyR * 2.0f, bodyR * 2.0f, 1.3f);
        g.setColour (juce::Colours::white.withAlpha (0.16f));   // luz na aresta de cima
        {
            juce::Path hi;
            hi.addCentredArc (cx, cy, bodyR - 1.0f, bodyR - 1.0f, 0.0f, -2.3f, -0.6f, true);
            g.strokePath (hi, juce::PathStrokeType (1.6f));
        }

        // ARCO DE VALOR vermelho. Cheguei a trocar isto por pontinhos de escala
        // serigrafada -- e mais fiel ao knob fisico, mas na tela o arco le melhor
        // e conversa com o vermelho do visor de programa.
        const float arcR = radius * 0.92f;
        {
            juce::Path track;
            track.addCentredArc (cx, cy, arcR, arcR, 0.0f, rotaryStartAngle, rotaryEndAngle, true);
            g.setColour (juce::Colours::white.withAlpha (0.10f));
            g.strokePath (track, juce::PathStrokeType (2.6f, juce::PathStrokeType::curved,
                                                       juce::PathStrokeType::rounded));
        }

        if (angle > rotaryStartAngle + 0.001f)
        {
            juce::Path value;
            value.addCentredArc (cx, cy, arcR, arcR, 0.0f, rotaryStartAngle, angle, true);
            g.setColour (arcColour.withAlpha (0.28f));
            g.strokePath (value, juce::PathStrokeType (5.5f, juce::PathStrokeType::curved,
                                                       juce::PathStrokeType::rounded));
            g.setColour (arcColour);
            g.strokePath (value, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved,
                                                       juce::PathStrokeType::rounded));
        }

        // MARCADOR mais visivel: sombra escura por baixo, traco branco mais grosso
        // e mais longo por cima. Le de longe, que era a queixa.
        g.setColour (juce::Colours::black.withAlpha (0.7f));
        g.drawLine ({ onCircle (angle, bodyR * 0.16f), onCircle (angle, bodyR * 0.96f) }, 4.4f);
        g.setColour (juce::Colours::white);
        g.drawLine ({ onCircle (angle, bodyR * 0.18f), onCircle (angle, bodyR * 0.92f) }, 2.8f);
    }

    //==============================================================================
    // Fader vertical estilo console: cap de metal deslizando num sulco recuado
    // com preenchimento vermelho abaixo do thumb. Substitui a linha fina + bolinha
    // vermelha do slider padrao.
    void drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPos, float minSliderPos, float maxSliderPos,
                           juce::Slider::SliderStyle style, juce::Slider& slider) override
    {
        if (style != juce::Slider::LinearVertical)
        {
            juce::LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos,
                                                    minSliderPos, maxSliderPos, style, slider);
            return;
        }

        // Calha LARGA: com 6px o fader lia como "um risco vermelho na tela".
        // Console de verdade tem sulco fresado — ~14px, com sombra interna.
        const float trackW = juce::jlimit (10.0f, 16.0f, (float) width * 0.34f);
        auto track = juce::Rectangle<float> ((float) x + (float) width * 0.5f - trackW * 0.5f,
                                             (float) y, trackW, (float) height);
        const float trackR = trackW * 0.5f;

        // sulco recuado (gradiente lateral = parece escavado, nao pintado)
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff0b0c0f), track.getX(), 0.0f,
                                                 juce::Colour (0xff191b20), track.getRight(), 0.0f, false));
        g.fillRoundedRectangle (track, trackR);
        g.setColour (juce::Colours::black.withAlpha (0.65f));
        g.drawRoundedRectangle (track.reduced (0.5f), trackR, 1.2f);
        g.setColour (juce::Colours::white.withAlpha (0.05f));   // luz na aresta de baixo do sulco
        g.drawLine (track.getX() + 2.0f, track.getBottom() - 1.0f,
                    track.getRight() - 2.0f, track.getBottom() - 1.0f, 1.0f);

        // Coluna LISA em degrade. Cheguei a fazer isto como escada de LED verde,
        // copiando o fader do Stage 4 -- na tela virou ruido: dezenas de celulas
        // competindo com o resto. O visor de programa define a linguagem da casa
        // (vidro preto + vermelho como acento unico) e o fader segue ela.
        if (sliderPos < track.getBottom() - 1.5f)
        {
            auto filled = track.withTop (sliderPos).reduced (2.0f, 0.0f).withTrimmedBottom (2.0f);

            if (filled.getHeight() > 1.0f)
            {
                juce::ColourGradient fill (nordRedDark(), track.getCentreX(), track.getBottom(),
                                           nordRedBright(), track.getCentreX(), sliderPos, false);
                g.setGradientFill (fill);
                g.fillRoundedRectangle (filled, filled.getWidth() * 0.5f);

                g.setColour (nordRedBright().withAlpha (0.35f));   // brilho no topo da coluna
                g.fillRoundedRectangle (filled.withHeight (juce::jmin (10.0f, filled.getHeight())),
                                        filled.getWidth() * 0.5f);
            }
        }

        // cap de console: mais largo que a calha, com ranhuras de aderencia
        const float capW = juce::jlimit (24.0f, 40.0f, (float) width * 0.82f);
        const float capH = 22.0f;
        auto cap = juce::Rectangle<float> (track.getCentreX() - capW * 0.5f,
                                           sliderPos - capH * 0.5f, capW, capH);

        {
            juce::Path sp;
            sp.addRoundedRectangle (cap, 4.0f);
            juce::DropShadow (juce::Colours::black.withAlpha (0.55f), 6, { 0, 2 }).drawForPath (g, sp);
        }

        // corpo: claro em cima, escuro embaixo, com um vinco no meio (dois planos)
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff4c505a), cap.getX(), cap.getY(),
                                                 juce::Colour (0xff2b2e35), cap.getX(), cap.getCentreY(), false));
        g.fillRoundedRectangle (cap.withBottom (cap.getCentreY() + 2.0f), 4.0f);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff23262c), cap.getX(), cap.getCentreY(),
                                                 juce::Colour (0xff34383f), cap.getX(), cap.getBottom(), false));
        g.fillRoundedRectangle (cap.withTop (cap.getCentreY() - 2.0f), 4.0f);

        g.setColour (juce::Colour (0xff53575f));
        g.drawRoundedRectangle (cap, 4.0f, 1.0f);
        g.setColour (juce::Colours::white.withAlpha (0.16f));   // brilho superior
        g.drawLine (cap.getX() + 4.0f, cap.getY() + 1.2f, cap.getRight() - 4.0f, cap.getY() + 1.2f, 1.2f);

        // ranhuras de aderencia acima e abaixo do risco central
        g.setColour (juce::Colours::black.withAlpha (0.30f));
        for (float dy : { -5.0f, 5.0f })
            g.drawLine (cap.getX() + 6.0f, cap.getCentreY() + dy, cap.getRight() - 6.0f, cap.getCentreY() + dy, 1.0f);

        // risco indicador claro no centro do cap. Nas fotos os caps da Nord sao
        // cinza lisos -- quem informa o nivel e a escada de LED ao lado, entao o
        // risco aqui e so referencia de posicao, nao acento de cor.
        g.setColour (juce::Colour (0xffd8dbe0));
        g.fillRect (juce::Rectangle<float> (cap.getX() + 3.0f, cap.getCentreY() - 1.1f,
                                            cap.getWidth() - 6.0f, 2.2f));
    }

    //==============================================================================
    // Interruptor deslizante no lugar do checkbox quadrado do toolkit (o elemento
    // mais generico que sobrava na tela, do lado de knobs de metal e cards de vidro).
    void drawToggleButton (juce::Graphics& g, juce::ToggleButton& button,
                           bool shouldDrawButtonAsHighlighted, bool) override
    {
        auto bounds = button.getLocalBounds().toFloat();
        const bool on = button.getToggleState();

        const float h = juce::jlimit (14.0f, 20.0f, bounds.getHeight() - 2.0f);
        const float w = h * 1.9f;
        auto sw = juce::Rectangle<float> (bounds.getX() + 1.0f, bounds.getCentreY() - h * 0.5f, w, h);

        // trilho
        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.fillRoundedRectangle (sw.translated (0.0f, 1.0f), h * 0.5f);
        g.setColour (on ? nordRed() : juce::Colour (0xff2a2d34));
        g.fillRoundedRectangle (sw, h * 0.5f);
        g.setColour (on ? nordRedBright().withAlpha (0.9f)
                        : juce::Colours::white.withAlpha (shouldDrawButtonAsHighlighted ? 0.22f : 0.12f));
        g.drawRoundedRectangle (sw.reduced (0.5f), h * 0.5f, 1.0f);

        // botao deslizante
        const float knobD = h - 5.0f;
        const float kx = on ? sw.getRight() - knobD - 2.5f : sw.getX() + 2.5f;
        auto knob = juce::Rectangle<float> (kx, sw.getCentreY() - knobD * 0.5f, knobD, knobD);

        g.setColour (juce::Colours::black.withAlpha (0.40f));
        g.fillEllipse (knob.translated (0.0f, 1.0f));
        g.setGradientFill (juce::ColourGradient (juce::Colours::white, knob.getX(), knob.getY(),
                                                 juce::Colour (0xffc9ccd3), knob.getX(), knob.getBottom(), false));
        g.fillEllipse (knob);

        // texto
        auto textArea = bounds.withTrimmedLeft (w + 9.0f);

        if (textArea.getWidth() > 4.0f)
        {
            g.setColour (button.findColour (juce::ToggleButton::textColourId)
                               .withMultipliedAlpha (button.isEnabled() ? 1.0f : 0.5f));
            g.setFont (juce::Font (juce::FontOptions (juce::jlimit (11.0f, 14.0f, h * 0.78f))));
            g.drawText (button.getButtonText(), textArea.toNearestInt(),
                        juce::Justification::centredLeft, true);
        }
    }

    //==============================================================================
    // Menu (lista de timbres, menus de contexto) no MESMO tom dos cards. O padrao
    // do JUCE vinha bem mais claro que o app inteiro e o menu — que e onde se passa
    // mais tempo escolhendo timbre — parecia de outro programa.
    void drawPopupMenuBackground (juce::Graphics& g, int width, int height) override
    {
        auto r = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height);

        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff22252c), 0.0f, 0.0f,
                                                 juce::Colour (0xff15171b), 0.0f, (float) height, false));
        g.fillRect (r);

        g.setColour (juce::Colours::white.withAlpha (0.05f));   // brilho no topo
        g.drawLine (1.0f, 1.0f, (float) width - 1.0f, 1.0f, 1.0f);
        g.setColour (juce::Colours::black.withAlpha (0.6f));
        g.drawRect (r, 1.0f);
    }

    // Cabecalho de secao DE VERDADE no lugar do "=====  WORSHIP  =====" em ASCII:
    // rotulo curto em caixa alta e um filete que corre ate a borda.
    void drawPopupMenuSectionHeader (juce::Graphics& g, const juce::Rectangle<int>& area,
                                     const juce::String& sectionName) override
    {
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

    //==============================================================================
    // ComboBox com o mesmo acabamento "vidro fosco" dos cards/chips + chevron
    // NEUTRO (disciplina de vermelho: a setinha deixa de ser vermelha).
    // O combo mostra o PRESET -- a informacao que muda. Depois de rebaixar a
    // categoria a rotulo de apoio, ele passa a ser o texto principal do card.
    juce::Font getComboBoxFont (juce::ComboBox& box) override
    {
        return juce::Font (juce::FontOptions (juce::jlimit (13.0f, 16.0f, (float) box.getHeight() * 0.52f),
                                              juce::Font::bold));
    }

    // Balao de valor do knob, na mesma linguagem do visor de programa: vidro
    // escuro, texto claro. O balao padrao do JUCE e cinza-claro com borda e
    // parecia um tooltip de sistema pousado em cima do painel.
    void drawBubble (juce::Graphics& g, juce::BubbleComponent& comp,
                     const juce::Point<float>& tip, const juce::Rectangle<float>& body) override
    {
        juce::ignoreUnused (comp, tip);

        g.setColour (juce::Colours::black.withAlpha (0.55f));
        g.fillRoundedRectangle (body.translated (0.0f, 2.0f), radiusControl);

        g.setColour (juce::Colour (0xff0e1014));
        g.fillRoundedRectangle (body, radiusControl);
        g.setColour (nordRed().withAlpha (0.55f));
        g.drawRoundedRectangle (body.reduced (0.5f), radiusControl, 1.2f);
    }

    void setComponentEffectForBubbleComponent (juce::BubbleComponent&) override {}

    juce::Font getPopupMenuFont() override
    {
        return juce::Font (juce::FontOptions (15.0f));
    }

    void drawComboBox (juce::Graphics& g, int width, int height, bool,
                       int, int, int, int, juce::ComboBox& box) override
    {
        // MINI-VISOR, nao caixa de formulario. Num painel de instrumento o nome do
        // preset aparece atras de vidro escuro embutido, com o mesmo vocabulario do
        // visor grande de programa -- nao numa chapa clara com bordinha de app.
        auto bounds = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height).reduced (0.5f);
        const float r = radiusControl;

        // rebaixo: sombra INTERNA no topo faz o vidro parecer afundado na chapa
        g.setColour (juce::Colour (0xff0b0c0e));
        g.fillRoundedRectangle (bounds, r);

        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff17191d), 0.0f, bounds.getY(),
                                                 juce::Colour (0xff0c0d10), 0.0f, bounds.getBottom(), false));
        g.fillRoundedRectangle (bounds.reduced (1.0f), r - 1.0f);

        g.setColour (juce::Colours::black.withAlpha (0.75f));
        g.drawLine (bounds.getX() + 4.0f, bounds.getY() + 1.2f, bounds.getRight() - 4.0f, bounds.getY() + 1.2f, 1.4f);

        // luz na aresta de BAIXO: assinatura otica de peca embutida
        g.setColour (juce::Colours::white.withAlpha (0.09f));
        g.drawLine (bounds.getX() + 4.0f, bounds.getBottom() - 1.0f,
                    bounds.getRight() - 4.0f, bounds.getBottom() - 1.0f, 1.0f);

        g.setColour (box.hasKeyboardFocus (true) ? nordRedBright().withAlpha (0.75f)
                                                 : juce::Colours::black.withAlpha (0.85f));
        g.drawRoundedRectangle (bounds, r, 1.2f);

        // chevron VERMELHO: e o unico ponto de cor, e sinaliza "isto se abre"
        const float cx = (float) width - 14.0f, cy = (float) height * 0.5f;
        juce::Path caret;
        caret.startNewSubPath (cx - 4.0f, cy - 2.5f);
        caret.lineTo (cx, cy + 2.5f);
        caret.lineTo (cx + 4.0f, cy - 2.5f);
        g.setColour (nordRedBright().withAlpha (0.9f));
        g.strokePath (caret, juce::PathStrokeType (1.9f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
};
