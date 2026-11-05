#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "ChurchLiveSkin.h"
#include "LiveScenes.h"

#include <cmath>
#include <functional>

//==============================================================================
// Pagina CENAS do desktop (Windows/Mac).
//
// Uma cena guarda o som COMPLETO de uma musica: instrumentos de cada canal,
// volumes, Pad e Master (o motor de cenas ja existia em LiveScenes.h; faltava
// uma superficie que o musico entendesse). A lista da esquerda e o setlist
// pronto para o culto; o painel da direita mostra e edita a musica selecionada.
//
// Mesma regra de ouro do painel Live: UM TOQUE NUNCA GRAVA. Tocar numa musica
// salva a CARREGA (gesto de culto, imediato); tocar num slot vazio so o
// seleciona. Gravar e substituir sao gestos de ensaio, em botao proprio, e
// substituir pede um segundo toque de confirmacao.
//
// O componente nao conhece o motor: quem carrega, grava e persiste e o
// MainComponent, que devolve o Resultado para o aviso aparecer aqui.
//==============================================================================
class ScenesPage final : public juce::Component,
                         private juce::Timer
{
public:
    using Cena = ChurchDAWLive::Cena;
    using Resultado = ChurchDAWLive::Resultado;

    std::function<Resultado (int)> onCarregar;
    std::function<Resultado (int)> onGravar;
    std::function<void (int, const Cena&)> onDetalhes;
    std::function<void (int)> onApagar;

    ScenesPage()
    {
        setOpaque (true);
        setWantsKeyboardFocus (false);

        for (auto* b : { &carregarBtn, &salvarBtn, &limparBtn, &anteriorBtn, &proximaBtn })
        {
            b->setWantsKeyboardFocus (false);
            addAndMakeVisible (*b);
        }

        carregarBtn.setButtonText (utf8 (u8"CARREGAR AGORA"));
        anteriorBtn.setButtonText (utf8 (u8"‹ ANTERIOR"));
        proximaBtn.setButtonText (utf8 (u8"PRÓXIMA ›"));
        limparBtn.setButtonText ("LIMPAR");

        carregarBtn.onClick = [this] { carregar (sel); };
        anteriorBtn.onClick = [this] { carregarVizinha (-1); };
        proximaBtn.onClick  = [this] { carregarVizinha (+1); };
        salvarBtn.onClick   = [this] { pedirGravar(); };
        limparBtn.onClick   = [this] { pedirLimpar(); };

        for (auto* e : { &musicaEd, &secaoEd, &tomEd, &bpmEd, &compassoEd })
        {
            e->setMultiLine (false);
            e->setReturnKeyStartsNewLine (false);
            e->setSelectAllWhenFocused (true);
            e->setScrollbarsShown (false);
            e->onReturnKey = [this] { commitDetalhes(); };
            e->onFocusLost = [this] { commitDetalhes(); };
            e->onEscapeKey = [this] { atualizarEditores(); unfocusAllComponents(); };
            e->setColour (juce::TextEditor::backgroundColourId, ChurchLiveSkin::inset());
            e->setColour (juce::TextEditor::outlineColourId, ChurchLiveSkin::line());
            e->setColour (juce::TextEditor::focusedOutlineColourId, ChurchLiveSkin::accent());
            e->setColour (juce::TextEditor::textColourId, ChurchLiveSkin::text());
            e->setColour (juce::TextEditor::highlightColourId, ChurchLiveSkin::redFill().withAlpha (0.45f));
            e->setColour (juce::TextEditor::highlightedTextColourId, juce::Colours::white);
            e->setColour (juce::CaretComponent::caretColourId, ChurchLiveSkin::accentSoft());
            addAndMakeVisible (*e);
        }

        musicaEd.setTextToShowWhenEmpty (utf8 (u8"Nome da música"), ChurchLiveSkin::textMute());
        secaoEd.setTextToShowWhenEmpty (utf8 (u8"Intro, Verso, Refrão…"), ChurchLiveSkin::textMute());
        tomEd.setInputRestrictions (4, "ABCDEFGabcdefg#b");
        bpmEd.setInputRestrictions (6, "0123456789.");
        compassoEd.setInputRestrictions (5, "0123456789/");
    }

    // Copia as cenas do MainComponent. Chamado a cada mudanca (carregar, gravar,
    // renomear, apagar), entao nunca guarda estado proprio alem da selecao.
    void atualizar (const Cena* origem, int cenaAtiva)
    {
        for (int i = 0; i < ChurchDAWLive::numeroDeCenas; ++i)
            cenas[(size_t) i] = origem[i];

        ativa = cenaAtiva;

        if (! juce::isPositiveAndBelow (sel, ChurchDAWLive::numeroDeCenas))
            sel = juce::isPositiveAndBelow (ativa, ChurchDAWLive::numeroDeCenas) ? ativa : primeiraPreenchida();

        desarmar();
        atualizarEditores();
        resized();
        repaint();
    }

    void selecionar (int indice)
    {
        if (! juce::isPositiveAndBelow (indice, ChurchDAWLive::numeroDeCenas) || indice == sel)
            return;

        commitDetalhes();
        sel = indice;
        desarmar();
        atualizarEditores();
        resized();
        repaint();
    }

    //==========================================================================
    void paint (juce::Graphics& g) override
    {
        using namespace ChurchLiveSkin;
        const auto m = metricsOf (*this);
        const auto L = calcularLayout (m);

        g.fillAll (bg());

        // cabecalho
        g.setColour (accent());
        g.fillRoundedRectangle (L.cabecalho.getX(), L.cabecalho.getY() + m.fpx (7), m.fpx (3), m.fpx (14), 1.0f);
        drawText (g, "CENAS", L.cabecalho.withTrimmedLeft (m.px (12)).withHeight (m.px (28)),
                  font (Face::barlowBold, m.fpx (14, 9.0f), 0.28f), text(), juce::Justification::centredLeft);
        drawText (g, utf8 (u8"Cada cena guarda o som completo de uma música: instrumentos, volumes, Pad e Master."),
                  L.cabecalho.withTrimmedTop (m.px (30)),
                  font (Face::barlow, m.fpx (17, 11.0f)), textSec(), juce::Justification::topLeft);

        if (juce::isPositiveAndBelow (ativa, ChurchDAWLive::numeroDeCenas))
            desenharNoAr (g, L.noAr, m);

        for (int i = 0; i < ChurchDAWLive::numeroDeCenas; ++i)
            desenharLinha (g, L.linhas[(size_t) i], i, m);

        desenharDetalhe (g, L, m);
    }

    void resized() override
    {
        using namespace ChurchLiveSkin;
        const auto m = metricsOf (*this);
        const auto L = calcularLayout (m);
        const bool preenchida = cenaSel().preenchida;

        anteriorBtn.setBounds (L.anterior);
        proximaBtn.setBounds (L.proxima);

        const auto fonteEditor = font (Face::barlowSemi, m.fpx (19, 12.0f));
        for (auto* e : { &musicaEd, &secaoEd, &tomEd, &bpmEd, &compassoEd })
        {
            e->applyFontToAllText (fonteEditor, true);
            e->setIndents (m.px (12), m.px (8));
            e->setVisible (preenchida);
        }

        musicaEd.setBounds (L.campoMusica);
        secaoEd.setBounds (L.campoSecao);
        tomEd.setBounds (L.campoTom);
        bpmEd.setBounds (L.campoBpm);
        compassoEd.setBounds (L.campoCompasso);

        carregarBtn.setBounds (L.botaoCarregar);
        salvarBtn.setBounds (L.botaoSalvar);
        limparBtn.setBounds (L.botaoLimpar);
        carregarBtn.setVisible (preenchida);
        limparBtn.setVisible (preenchida);

        estilizarBotoes();
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const int h = linhaEm (e.getPosition());
        if (h != hover)
        {
            hover = h;
            setMouseCursor (h >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
            repaint();
        }
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        if (hover != -1)
        {
            hover = -1;
            repaint();
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const int i = linhaEm (e.getPosition());
        if (i < 0)
        {
            unfocusAllComponents();
            return;
        }

        selecionar (i);

        // Musica gravada: um toque carrega (gesto de culto). Slot vazio: so
        // seleciona -- carregar um slot vazio nao pode apagar o som atual.
        if (cenas[(size_t) i].preenchida)
            carregar (i);
    }

private:
    //==========================================================================
    struct Layout
    {
        juce::Rectangle<int> cabecalho, noAr, anterior, proxima, lista, detalhe;
        juce::Rectangle<int> titulo, campoMusica, campoSecao, campoTom, campoBpm, campoCompasso;
        juce::Rectangle<int> somTitulo, somLista, botaoCarregar, botaoSalvar, botaoLimpar, aviso;
        std::array<juce::Rectangle<int>, ChurchDAWLive::numeroDeCenas> linhas;
    };

    Layout calcularLayout (const ChurchLiveSkin::Metrics& m) const
    {
        Layout L;
        auto area = getLocalBounds().reduced (m.px (28, 10), m.px (20, 8));

        L.cabecalho = area.removeFromTop (m.px (62, 40));
        auto botoes = L.cabecalho.removeFromRight (m.px (280, 200));
        L.proxima = botoes.removeFromRight (m.px (132, 92)).withSizeKeepingCentre (m.px (132, 92), m.px (44, 32));
        botoes.removeFromRight (m.px (8, 4));
        L.anterior = botoes.removeFromRight (m.px (132, 92)).withSizeKeepingCentre (m.px (132, 92), m.px (44, 32));
        L.noAr = L.cabecalho.removeFromRight (m.px (260, 150)).withSizeKeepingCentre (m.px (260, 150), m.px (38, 28));
        area.removeFromTop (m.px (14, 6));

        const int gap = m.px (20, 8);
        L.detalhe = area.removeFromRight (juce::jlimit (m.px (320, 260), m.px (470, 340), (int) (area.getWidth() * 0.40f)));
        area.removeFromRight (gap);
        L.lista = area;

        const int gapLinha = m.px (7, 3);
        // Piso baixo de proposito: em janela pequena as 10 cenas precisam caber (sem rolagem).
        const int h = juce::jmax (m.px (50, 28), (L.lista.getHeight() - gapLinha * 9) / ChurchDAWLive::numeroDeCenas);
        for (int i = 0; i < ChurchDAWLive::numeroDeCenas; ++i)
            L.linhas[(size_t) i] = juce::Rectangle<int> (L.lista.getX(), L.lista.getY() + i * (h + gapLinha), L.lista.getWidth(), h);

        // detalhe
        auto d = L.detalhe.reduced (m.px (22, 10));
        L.titulo = d.removeFromTop (m.px (34, 24));
        d.removeFromTop (m.px (14, 6));

        const int rot = m.px (16, 12), ed = m.px (40, 32), esp = m.px (12, 6);
        const auto campo = [&] (juce::Rectangle<int> faixa) { return faixa.withTrimmedTop (rot).withHeight (ed); };

        auto linha1 = d.removeFromTop (rot + ed);
        L.campoMusica = campo (linha1);
        d.removeFromTop (esp);

        auto linha2 = d.removeFromTop (rot + ed);
        auto tomFaixa = linha2.removeFromRight (linha2.getWidth() * 2 / 5);
        linha2.removeFromRight (esp);
        L.campoSecao = campo (linha2);
        L.campoTom = campo (tomFaixa);
        d.removeFromTop (esp);

        auto linha3 = d.removeFromTop (rot + ed);
        auto compFaixa = linha3.removeFromRight (linha3.getWidth() / 2);
        linha3.removeFromRight (esp / 2);
        L.campoBpm = campo (linha3);
        L.campoCompasso = campo (compFaixa.withTrimmedLeft (esp / 2));
        d.removeFromTop (m.px (18, 8));

        // rodape de acoes (de baixo para cima)
        L.aviso = d.removeFromBottom (m.px (22, 16));
        const int hb = m.px (44, 34);
        auto linhaBaixo = d.removeFromBottom (hb);
        L.botaoLimpar = linhaBaixo.removeFromRight (m.px (96, 70));
        linhaBaixo.removeFromRight (esp);
        L.botaoSalvar = linhaBaixo;
        d.removeFromBottom (esp);
        L.botaoCarregar = d.removeFromBottom (m.px (48, 36));
        d.removeFromBottom (m.px (14, 6));

        L.somTitulo = d.removeFromTop (rot);
        d.removeFromTop (m.px (6, 3));
        L.somLista = d;

        // slot vazio: o botao de salvar ocupa a largura toda
        if (! cenaSel().preenchida)
            L.botaoSalvar = L.botaoSalvar.withRight (L.botaoLimpar.getRight());

        return L;
    }

    const Cena& cenaSel() const
    {
        return cenas[(size_t) juce::jlimit (0, ChurchDAWLive::numeroDeCenas - 1, sel)];
    }

    int linhaEm (juce::Point<int> p) const
    {
        const auto L = calcularLayout (ChurchLiveSkin::metricsOf (*this));
        for (int i = 0; i < ChurchDAWLive::numeroDeCenas; ++i)
            if (L.linhas[(size_t) i].contains (p))
                return i;
        return -1;
    }

    int primeiraPreenchida() const
    {
        for (int i = 0; i < ChurchDAWLive::numeroDeCenas; ++i)
            if (cenas[(size_t) i].preenchida)
                return i;
        return 0;
    }

    static juce::String utf8 (const char* s) { return juce::String::fromUTF8 (s); }

    static juce::String tituloDaCena (const Cena& c, int i)
    {
        if (c.musica.isNotEmpty()) return c.musica;
        if (c.nome.isNotEmpty())   return c.nome;
        return "Cena " + juce::String (i + 1);
    }

    static juce::String nomeDoTom (int tom)
    {
        static const char* nomes[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        return nomes[juce::jlimit (0, 11, tom)];
    }

    // O que toca nesta cena, na ordem da mesa: timbre de cada canal ligado e o Pad.
    static juce::StringArray instrumentosDaCena (const Cena& c)
    {
        juce::StringArray nomes;
        for (const auto& canal : c.canais)
            if (canal.ligado && canal.rotuloDoTimbre.isNotEmpty())
                nomes.add (canal.rotuloDoTimbre);
        if (c.padTom >= 0)
            nomes.add ("Pad " + nomeDoTom (c.padTom));
        return nomes;
    }

    static juce::String resumoDoSom (const Cena& c)
    {
        const auto nomes = instrumentosDaCena (c);
        if (nomes.isEmpty())
            return {};

        auto s = nomes.joinIntoString (utf8 (u8" · "), 0, 3);
        if (nomes.size() > 3)
            s << "  +" << (nomes.size() - 3);
        return s;
    }

    static void drawText (juce::Graphics& g, const juce::String& s, juce::Rectangle<int> r,
                          const juce::Font& f, juce::Colour c, juce::Justification j)
    {
        g.setColour (c);
        g.setFont (f);
        g.drawText (s, r, j, true);
    }

    // Ficha (chip) de tamanho do proprio texto; devolve a area ocupada.
    static juce::Rectangle<float> ficha (juce::Graphics& g, juce::Point<float> direitaCentro,
                                         const juce::String& s, const juce::Font& f,
                                         juce::Colour tom, const ChurchLiveSkin::Metrics& m, bool cheia = false)
    {
        using namespace ChurchLiveSkin;
        const float padX = m.fpx (11, 6.0f);
        const float w = textWidth (f, s) + padX * 2.0f;
        const float h = m.fpx (26, 18.0f);
        const juce::Rectangle<float> r (direitaCentro.x - w, direitaCentro.y - h * 0.5f, w, h);
        g.setColour (cheia ? tom : tom.withAlpha (0.14f));
        g.fillRoundedRectangle (r, h * 0.5f);
        if (! cheia)
        {
            g.setColour (tom.withAlpha (0.55f));
            g.drawRoundedRectangle (r.reduced (0.5f), h * 0.5f, 1.0f);
        }
        g.setColour (cheia ? juce::Colours::white : tom);
        g.setFont (f);
        g.drawText (s, r.toNearestInt(), juce::Justification::centred, false);
        return r;
    }

    void desenharNoAr (juce::Graphics& g, juce::Rectangle<int> r, const ChurchLiveSkin::Metrics& m) const
    {
        using namespace ChurchLiveSkin;
        drawStatusChip (g, r.toFloat(),
                        "NO AR  " + juce::String (ativa + 1).paddedLeft ('0', 2) + "  " + tituloDaCena (cenas[(size_t) ativa], ativa),
                        redFill(), m);
    }

    void desenharLinha (juce::Graphics& g, juce::Rectangle<int> r, int i, const ChurchLiveSkin::Metrics& m) const
    {
        using namespace ChurchLiveSkin;
        const auto& c = cenas[(size_t) i];
        const bool noAr = i == ativa && c.preenchida;
        const bool selecionada = i == sel;
        const float radius = m.fpx (12, 6.0f);
        const auto rf = r.toFloat();

        if (c.preenchida)
        {
            g.setColour (selecionada ? elevated() : (i == hover ? elevated().interpolatedWith (panel(), 0.5f) : panel()));
            g.fillRoundedRectangle (rf, radius);
            g.setColour (selecionada ? accent().withAlpha (0.9f) : line());
            g.drawRoundedRectangle (rf.reduced (0.5f), radius, selecionada ? 1.6f : 1.0f);
        }
        else
        {
            g.setColour (selecionada ? panel() : inset());
            g.fillRoundedRectangle (rf, radius);
            if (selecionada)
            {
                g.setColour (accent().withAlpha (0.9f));
                g.drawRoundedRectangle (rf.reduced (0.5f), radius, 1.6f);
            }
            else
            {
                drawDashedRoundRect (g, rf.reduced (0.6f), radius, 1.2f, i == hover ? textMute() : dashed());
            }
        }

        if (noAr)
        {
            g.setColour (redFill());
            g.fillRoundedRectangle (rf.getX() + 1.0f, rf.getY() + m.fpx (10), m.fpx (4), rf.getHeight() - m.fpx (20), 2.0f);
        }

        // numero
        const float lado = juce::jmin (rf.getHeight() - m.fpx (18), m.fpx (46));
        const juce::Rectangle<float> selo (rf.getX() + m.fpx (20), rf.getCentreY() - lado * 0.5f, lado, lado);
        if (noAr)
        {
            g.setColour (redFill());
            g.fillRoundedRectangle (selo, m.fpx (10, 5.0f));
        }
        else if (c.preenchida)
        {
            g.setColour (inset());
            g.fillRoundedRectangle (selo, m.fpx (10, 5.0f));
            g.setColour (line());
            g.drawRoundedRectangle (selo.reduced (0.5f), m.fpx (10, 5.0f), 1.0f);
        }
        drawText (g, juce::String (i + 1).paddedLeft ('0', 2), selo.toNearestInt(),
                  font (Face::mono, m.fpx (17, 11.0f)),
                  noAr ? juce::Colours::white : (c.preenchida ? textSec() : textMute()),
                  juce::Justification::centred);

        // direita: fichas
        float x = rf.getRight() - m.fpx (18);
        const float cy = rf.getCentreY();
        const auto fMono = font (Face::mono, m.fpx (13, 9.0f));

        if (c.preenchida)
        {
            if (noAr)
                x = ficha (g, { x, cy }, "NO AR", font (Face::barlowBold, m.fpx (13, 9.0f), 0.12f), redFill(), m, true).getX() - m.fpx (8);

            x = ficha (g, { x, cy }, juce::String (juce::roundToInt ((float) c.bpm)) + " BPM", fMono, textSec(), m).getX() - m.fpx (8);
            x = ficha (g, { x, cy }, c.tonalidade.isNotEmpty() ? c.tonalidade : "-", fMono, accentSoft(), m).getX() - m.fpx (14);
        }

        // texto
        auto txt = r.withTrimmedLeft (juce::roundToInt (selo.getRight() - rf.getX() + m.fpx (16)))
                    .withTrimmedRight (juce::roundToInt (rf.getRight() - x));
        // Uma linha so: titulo | secao + instrumentos | fichas. Cabe em qualquer altura
        // de janela e deixa o "preset" (o que toca) sempre visivel, sem abrir a cena.
        // A coluna e medida pela LINHA, nao pelo que sobra apos as fichas: assim o
        // resumo comeca no mesmo x em todas as musicas (a linha NO AR tem mais fichas).
        const int colunaTitulo = juce::jmin (juce::roundToInt ((float) r.getWidth() * 0.30f), m.px (330, 150));
        const auto areaTitulo = txt.removeFromLeft (colunaTitulo);
        txt.removeFromLeft (m.px (14, 6));

        if (c.preenchida)
        {
            const auto resumo = resumoDoSom (c);
            const auto detalhe = c.secao.isNotEmpty() && c.secao != "Cena"
                                     ? (resumo.isEmpty() ? c.secao : c.secao + utf8 (u8" · ") + resumo)
                                     : resumo;
            drawText (g, tituloDaCena (c, i), areaTitulo, font (Face::barlowBold, m.fpx (22, 12.5f)), text(),
                      juce::Justification::centredLeft);
            drawText (g, detalhe.isEmpty() ? utf8 (u8"Sem instrumentos gravados") : detalhe, txt,
                      font (Face::barlow, m.fpx (17, 10.5f)), textSec(), juce::Justification::centredLeft);
        }
        else
        {
            drawText (g, "Cena vazia", areaTitulo, font (Face::barlowSemi, m.fpx (21, 12.0f)), textMute(),
                      juce::Justification::centredLeft);
            drawText (g, utf8 (u8"Toque para preparar e salvar o som atual aqui"), txt,
                      font (Face::barlow, m.fpx (16, 10.0f)), textMute().withAlpha (0.8f), juce::Justification::centredLeft);
        }
    }

    void desenharDetalhe (juce::Graphics& g, const Layout& L, const ChurchLiveSkin::Metrics& m) const
    {
        using namespace ChurchLiveSkin;
        const auto& c = cenaSel();
        const float radius = m.fpx (16, 8.0f);

        g.setColour (panel());
        g.fillRoundedRectangle (L.detalhe.toFloat(), radius);
        g.setColour (line());
        g.drawRoundedRectangle (L.detalhe.toFloat().reduced (0.5f), radius, 1.0f);

        // titulo + estado
        const auto estadoTom = sel == ativa && c.preenchida ? redFill() : (c.preenchida ? okGreen() : textMute());
        const auto estado = sel == ativa && c.preenchida ? "NO AR" : (c.preenchida ? "GRAVADA" : "VAZIA");
        const auto fChip = font (Face::barlowBold, m.fpx (13, 9.0f), 0.12f);
        const auto chip = ficha (g, { (float) L.titulo.getRight(), (float) L.titulo.getCentreY() }, estado, fChip, estadoTom, m);
        drawText (g, "CENA " + juce::String (sel + 1).paddedLeft ('0', 2),
                  L.titulo.withTrimmedRight (juce::roundToInt (chip.getWidth()) + m.px (10)),
                  font (Face::condExtra, m.fpx (28, 16.0f), 0.06f), text(), juce::Justification::centredLeft);

        const auto fRot = font (Face::barlowBold, m.fpx (12, 8.5f), 0.22f);

        if (! c.preenchida)
        {
            // convite
            const auto miolo = juce::Rectangle<float> ((float) L.campoMusica.getX(), (float) L.campoMusica.getY(),
                                                         (float) L.campoMusica.getWidth(),
                                                         (float) (L.somLista.getBottom() - L.campoMusica.getY()));
            const auto centro = miolo.getCentre().translated (0.0f, -m.fpx (20));
            drawDashedCircle (g, centro, m.fpx (34), 1.4f, dashed());
            g.setColour (textMute());
            drawPlus (g, centro, m.fpx (11), 1.6f);
            drawText (g, utf8 (u8"Cena vazia"),
                      juce::Rectangle<int> ((int) miolo.getX(), (int) (centro.y + m.fpx (48)), (int) miolo.getWidth(), m.px (30)),
                      font (Face::barlowBold, m.fpx (22, 13.0f)), text(), juce::Justification::centred);
            g.setColour (textSec());
            g.setFont (font (Face::barlow, m.fpx (16, 10.5f)));
            g.drawFittedText (utf8 (u8"Monte o som na mesa e grave aqui. Cada cena guarda instrumentos, volumes, Pad e Master."),
                              juce::Rectangle<int> ((int) miolo.getX() + m.px (16), (int) (centro.y + m.fpx (82)),
                                                    (int) miolo.getWidth() - m.px (32), m.px (64)),
                              juce::Justification::centredTop, 3, 1.0f);
        }
        else
        {
            const auto rotulo = [&] (juce::Rectangle<int> campo, const char* s)
            {
                drawText (g, utf8 (s), campo.withY (campo.getY() - m.px (16, 12)).withHeight (m.px (16, 12)), fRot, textMute(),
                          juce::Justification::centredLeft);
            };
            rotulo (L.campoMusica, u8"MÚSICA");
            rotulo (L.campoSecao, u8"SEÇÃO");
            rotulo (L.campoTom, "TOM");
            rotulo (L.campoBpm, "BPM");
            rotulo (L.campoCompasso, "COMPASSO");

            drawText (g, "O SOM DESTA CENA", L.somTitulo, fRot, textMute(), juce::Justification::centredLeft);
            if (c.padTom >= 0)
                ficha (g, { (float) L.somTitulo.getRight(), (float) L.somTitulo.getCentreY() },
                       utf8 (u8"PAD CONTÍNUO · ") + nomeDoTom (c.padTom),
                       font (Face::barlowBold, m.fpx (12, 8.5f), 0.1f), accentSoft(), m);
            desenharSom (g, L.somLista, c, m);
        }

        if (aviso.isNotEmpty())
            drawText (g, aviso, L.aviso, font (Face::barlowSemi, m.fpx (15, 10.0f)),
                      avisoErro ? redText() : okGreen(), juce::Justification::centredLeft);
        else
            drawText (g, utf8 (u8"Um toque na lista carrega a música. Gravar só pelo botão."), L.aviso,
                      font (Face::barlow, m.fpx (14, 9.5f)), textMute(), juce::Justification::centredLeft);
    }

    void desenharSom (juce::Graphics& g, juce::Rectangle<int> area, const Cena& c, const ChurchLiveSkin::Metrics& m) const
    {
        using namespace ChurchLiveSkin;
        const int h = m.px (28, 22);
        const int maxLinhas = juce::jmax (1, area.getHeight() / h);

        struct Item { int canal; juce::String nome; float volume; };
        std::array<Item, ChurchDAWLive::numeroDeCanais> itens;
        int n = 0;
        for (int ch = 0; ch < ChurchDAWLive::numeroDeCanais; ++ch)
            if (c.canais[ch].ligado && c.canais[ch].rotuloDoTimbre.isNotEmpty())
                itens[(size_t) n++] = { ch, c.canais[ch].rotuloDoTimbre, c.canais[ch].volume };

        // O Pad vai numa ficha no titulo da secao (desenharDetalhe), nao ocupa linha.
        if (n == 0)
        {
            drawText (g, c.padTom >= 0 ? utf8 (u8"Só o Pad contínuo foi gravado.")
                                       : utf8 (u8"Cena legada: só o mixer foi gravado."),
                      area.withHeight (h), font (Face::barlow, m.fpx (16, 10.5f)), textMute(),
                      juce::Justification::centredLeft);
            return;
        }

        const int visiveis = juce::jmax (0, juce::jmin (n, maxLinhas - (n > maxLinhas ? 1 : 0)));

        auto y = area.getY();
        for (int k = 0; k < visiveis; ++k, y += h)
        {
            const auto linha = juce::Rectangle<int> (area.getX(), y, area.getWidth(), h);
            const auto& it = itens[(size_t) k];
            drawText (g, juce::String (it.canal + 1).paddedLeft ('0', 2), linha.withWidth (m.px (28)),
                      font (Face::mono, m.fpx (13, 9.0f)), textMute(), juce::Justification::centredLeft);

            auto resto = linha.withTrimmedLeft (m.px (34));
            const auto dbArea = resto.removeFromRight (m.px (62));
            const auto barra = resto.removeFromRight (m.px (76, 40)).withSizeKeepingCentre (m.px (76, 40), m.px (5, 4));
            resto.removeFromRight (m.px (10, 4));
            drawText (g, it.nome, resto, font (Face::barlowSemi, m.fpx (17, 11.0f)), text(), juce::Justification::centredLeft);

            g.setColour (trackBg());
            g.fillRoundedRectangle (barra.toFloat(), 2.5f);
            g.setColour (it.volume > 0.001f ? accent() : offGrey());
            g.fillRoundedRectangle (barra.toFloat().withWidth (barra.getWidth() * juce::jlimit (0.0f, 1.0f, it.volume)), 2.5f);

            const float ganho = it.volume * 1.5f;
            const float db = ganho > 1.0e-4f ? 20.0f * std::log10 (ganho) : -100.0f;
            drawText (g, db < -90.0f ? utf8 (u8"-∞ dB") : juce::String (db, 1) + " dB", dbArea,
                      font (Face::mono, m.fpx (12, 8.5f)), textSec(), juce::Justification::centredRight);
        }

        if (n > visiveis)
        {
            drawText (g, "+" + juce::String (n - visiveis) + " canais", juce::Rectangle<int> (area.getX(), y, area.getWidth(), h),
                      font (Face::barlow, m.fpx (15, 10.0f)), textMute(), juce::Justification::centredLeft);
        }
    }

    //==========================================================================
    void estilizarBotoes()
    {
        using namespace ChurchLiveSkin;
        const bool preenchida = cenaSel().preenchida;
        const bool salvarArmado = armada == Acao::gravar && armadaIdx == sel;

        // Papeis da skin Church Live: "primary" = acao principal (vermelho),
        // "pill" = secundario. Confirmar a substituicao vira vermelho.
        const double u = (double) metricsOf (*this).u;
        const auto papel = [u] (juce::TextButton& b, const char* role)
        {
            b.getProperties().set ("churchSkin", role);
            b.getProperties().set ("churchU", u);
            b.setLookAndFeel (&ChurchLiveSkin::lookAndFeel());
        };

        papel (carregarBtn, "primary");
        papel (anteriorBtn, "pill");
        papel (proximaBtn, "pill");
        papel (limparBtn, "pill");
        // Numa cena vazia gravar e a unica acao: vira o botao principal.
        papel (salvarBtn, (salvarArmado || ! preenchida) ? "primary" : "pill");

        salvarBtn.setButtonText (salvarArmado ? utf8 (u8"CONFIRMAR: SUBSTITUIR O SOM")
                                              : (preenchida ? utf8 (u8"SALVAR SOM ATUAL AQUI")
                                                            : utf8 (u8"SALVAR SOM ATUAL NESTA CENA")));
        carregarBtn.setButtonText (sel == ativa ? "RECARREGAR" : "CARREGAR AGORA");
        salvarBtn.setTooltip (utf8 (u8"Grava o som que está na mesa agora nesta cena"));
    }

    void atualizarEditores()
    {
        const auto& c = cenaSel();

        if (! c.preenchida)
        {
            for (auto* e : { &musicaEd, &secaoEd, &tomEd, &bpmEd, &compassoEd })
                e->clear();
            return;
        }

        musicaEd.setText (c.musica.isNotEmpty() ? c.musica : c.nome, false);
        secaoEd.setText (c.secao == "Cena" ? juce::String() : c.secao, false);
        tomEd.setText (c.tonalidade, false);
        bpmEd.setText (juce::String (c.bpm, 0), false);
        compassoEd.setText (c.compasso, false);
    }

    void commitDetalhes()
    {
        if (! cenaSel().preenchida || onDetalhes == nullptr)
            return;

        auto nova = cenaSel();
        const auto musica = musicaEd.getText().trim();
        nova.musica = musica;
        nova.secao = secaoEd.getText().trim().isEmpty() ? juce::String ("Cena") : secaoEd.getText().trim();
        nova.tonalidade = tomEd.getText().trim().isEmpty() ? juce::String ("C") : tomEd.getText().trim();
        nova.bpm = juce::jlimit (30.0, 300.0, bpmEd.getText().getDoubleValue() > 0.0 ? bpmEd.getText().getDoubleValue() : nova.bpm);
        nova.compasso = compassoEd.getText().trim().containsChar ('/') ? compassoEd.getText().trim() : nova.compasso;

        const auto& atual = cenaSel();
        if (nova.musica == atual.musica && nova.secao == atual.secao && nova.tonalidade == atual.tonalidade
             && nova.bpm == atual.bpm && nova.compasso == atual.compasso)
            return;

        onDetalhes (sel, nova);
    }

    void carregar (int indice)
    {
        if (onCarregar == nullptr || ! juce::isPositiveAndBelow (indice, ChurchDAWLive::numeroDeCenas)
             || ! cenas[(size_t) indice].preenchida)
            return;

        commitDetalhes();
        avisarResultado (onCarregar (indice), utf8 (u8"Cena carregada"));
    }

    void carregarVizinha (int passo)
    {
        const int base = juce::isPositiveAndBelow (ativa, ChurchDAWLive::numeroDeCenas) ? ativa : (passo > 0 ? -1 : 0);

        for (int k = 1; k <= ChurchDAWLive::numeroDeCenas; ++k)
        {
            const int i = ((base + passo * k) % ChurchDAWLive::numeroDeCenas + ChurchDAWLive::numeroDeCenas)
                              % ChurchDAWLive::numeroDeCenas;
            if (cenas[(size_t) i].preenchida)
            {
                selecionar (i);
                carregar (i);
                return;
            }
        }
    }

    void pedirGravar()
    {
        if (onGravar == nullptr)
            return;

        // Substituir uma musica gravada e destrutivo: segundo toque confirma.
        if (cenaSel().preenchida && ! (armada == Acao::gravar && armadaIdx == sel))
        {
            armar (Acao::gravar);
            return;
        }

        desarmar();
        avisarResultado (onGravar (sel), utf8 (u8"Som salvo na cena ") + juce::String (sel + 1).paddedLeft ('0', 2));
    }

    void pedirLimpar()
    {
        // O MainComponent ja pede confirmacao (apagar nao tem desfazer).
        if (onApagar != nullptr && cenaSel().preenchida)
            onApagar (sel);
    }

    void avisarResultado (Resultado r, const juce::String& textoOk)
    {
        switch (r)
        {
            case Resultado::ok:            mostrarAviso (textoOk, false); break;
            case Resultado::vazia:         break;
            case Resultado::carregando:    mostrarAviso (utf8 (u8"Aguarde o timbre terminar de carregar"), true); break;
            case Resultado::naoGravou:     mostrarAviso (utf8 (u8"Não foi possível gravar em disco"), true); break;
            case Resultado::indisponivel:  mostrarAviso (utf8 (u8"O motor de som ainda não iniciou"), true); break;
            case Resultado::paginaInvalida: mostrarAviso (utf8 (u8"A página desta cena está inválida"), true); break;
        }
    }

    enum class Acao { nenhuma, gravar };

    void armar (Acao a)
    {
        armada = a;
        armadaIdx = sel;
        estilizarBotoes();
        startTimer (4000);
        repaint();
    }

    void desarmar()
    {
        if (armada == Acao::nenhuma)
            return;

        armada = Acao::nenhuma;
        armadaIdx = -1;
        estilizarBotoes();
    }

    void mostrarAviso (const juce::String& s, bool erro)
    {
        aviso = s;
        avisoErro = erro;
        startTimer (erro ? 5000 : 3000);
        repaint();
    }

    void timerCallback() override
    {
        stopTimer();
        aviso = {};
        desarmar();
        repaint();
    }

    //==========================================================================
    std::array<Cena, ChurchDAWLive::numeroDeCenas> cenas;
    int ativa = -1;
    int sel = -1;
    int hover = -1;

    Acao armada = Acao::nenhuma;
    int armadaIdx = -1;
    juce::String aviso;
    bool avisoErro = false;

    juce::TextButton carregarBtn, salvarBtn, limparBtn, anteriorBtn, proximaBtn;
    juce::TextEditor musicaEd, secaoEd, tomEd, bpmEd, compassoEd;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ScenesPage)
};
