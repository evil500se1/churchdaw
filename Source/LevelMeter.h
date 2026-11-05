#pragma once

// EXPERIMENTO TEMPORARIO: 1 congela o desenho para medir quanto ele custa.
#ifndef CHURCHDAW_UI_BISECT
 #define CHURCHDAW_UI_BISECT 0
#endif

#include <juce_audio_basics/juce_audio_basics.h>   // juce::Decibels
#include <juce_gui_basics/juce_gui_basics.h>

#include "NordLookAndFeel.h"

#include <algorithm>
#include <functional>
#include <vector>

//==============================================================================
// Um relogio so para todos os medidores. Cada medidor com timer proprio acordava
// a thread da interface na sua propria fase: medido no Xiaomi parado e em
// silencio, ~158 despertares/s na thread principal (+97/s na thread de timers do
// JUCE) para ~7 medidores a 24 Hz. Com um relogio, todos rodam no mesmo tick.
// Apenas thread de mensagens (como o proprio juce::Timer).
//==============================================================================
class MeterClock : private juce::Timer
{
public:
    struct Client
    {
        virtual void meterTick() = 0;

    protected:
        ~Client() = default;
    };

    static MeterClock& get() { static MeterClock clock; return clock; }

    void add (Client& client, int hz)
    {
        jassert (clients.empty() || hz == currentHz);   // um relogio: todos os medidores da plataforma usam o mesmo Hz

        clients.push_back (&client);

        if (! isTimerRunning())
        {
            currentHz = hz;
            startTimerHz (hz);
        }
    }

    void remove (Client& client)
    {
        clients.erase (std::remove (clients.begin(), clients.end(), &client), clients.end());

        if (clients.empty())
            stopTimer();
    }

    size_t size() const { return clients.size(); }

private:
    void timerCallback() override
    {
        const auto snapshot = clients;   // um tick pode destruir outro medidor

        for (auto* client : snapshot)
            if (std::find (clients.begin(), clients.end(), client) != clients.end())
                client->meterTick();
    }

    std::vector<Client*> clients;
    int currentHz = 0;
};

//==============================================================================
// Medidor de nivel generico (1 ou 2 barras): gradiente verde -> amarelo ->
// vermelho, linha de pico com queda suave e luz de clip opcional (clique
// para apagar). As fontes de nivel/clip sao funcoes "le e zera".
//==============================================================================
class LevelMeter : public juce::Component,
                   private MeterClock::Client
{
public:
    using LevelFn = std::function<float (int channel)>;
    using ClipFn  = std::function<bool()>;

    // O que a luz acesa SIGNIFICA. No master o limiter brick-wall e o ultimo
    // estagio: passar de 0 dBFS ali vira reducao de ganho, nao distorcao -- e
    // informacao (ambar, acompanha o momento). No canal nao existe rede: passar
    // de 0 dBFS e clipping de verdade -- e alerta (vermelho, trava ate o clique).
    enum class ClipRole
    {
        channelClip,
        limiterWorking
    };

    LevelMeter (int numChannels, LevelFn levelSource, ClipFn clipSource = {})
        : channels (juce::jlimit (1, 2, numChannels)),
          levelFn (std::move (levelSource)),
          clipFn (std::move (clipSource))
    {
        // No celular o JUCE desenha por SOFTWARE, e a tela e grande (2460x1080).
        // Medido com o app tocando: a thread da interface saturava um nucleo
        // inteiro (100%) e o audio, que precisa de prazo, ficava sem CPU -- era
        // essa a origem do pipoco, nao falta de memoria nem o streaming.
        // 15 Hz ainda le como movimento continuo num medidor de nivel; 30 Hz e
        // luxo de PC.
       #if CHURCHDAW_UI_BISECT
        // experimento: medidores congelados, para medir quanto eles custam
       #elif JUCE_ANDROID
        // 24 Hz no Android: fluido para leitura de palco e ainda leve na UI
        // porque canais em silencio saem em fast-path (zero dB math/repaint) e
        // mudancas apenas de nivel repintam somente a regiao interna das barras.
        timerHzUsado = 24;
        MeterClock::get().add (*this, timerHzUsado);
       #elif JUCE_IOS
        timerHzUsado = 24;
        MeterClock::get().add (*this, timerHzUsado);
       #else
        // 45 Hz no desktop: ataque e queda deixam de revelar degraus de frame
        // sem transformar o meter em um repaint permanente de 60 Hz.
        timerHzUsado = 45;
        MeterClock::get().add (*this, timerHzUsado);
       #endif

        // Balistica em TEMPO (nao em quadros): mesma queda/hold em qualquer
        // taxa. 36 dB/s de queda da barra, 24 dB/s de queda do pico, 1,5 s de
        // peak-hold -- a velocidade fisica independe da taxa do desktop.
        quedaBarraDb = 36.0f / (float) timerHzUsado;
        quedaPicoDb  = 24.0f / (float) timerHzUsado;
        ticksHold    = juce::jmax (1, juce::roundToInt (1.5f * (float) timerHzUsado));
    }

    ~LevelMeter() override { MeterClock::get().remove (*this); }

    void mouseDown (const juce::MouseEvent&) override
    {
        if (! clipSticky)
            return;

        // A trava pode morar no motor (master): apagar so a copia local faria a
        // luz voltar no proximo quadro. Ver setClipClearFn().
        if (clipClearFn != nullptr)
            clipClearFn();

        clipSticky = false;
        repaint();   // sem isto a luz seguia acesa ate algum outro quadro mudar
    }

    // mostra "L" / "R" embaixo de cada barra (so faz sentido em medidor estereo)
    void setChannelLabels (bool shouldShow) { channelLabels = shouldShow; repaint(); }

    // Escala dB compacta ao lado do bargraph. Os strips do mixer usam isto
    // para reproduzir a leitura de console da referencia; o master conserva
    // sua propria escala junto ao fader.
    void setScaleLabels (bool shouldShow) { scaleLabels = shouldShow; repaint(); }

    // No master, alem do peak-hold grafico, mostra o pico numerico de cada lado.
    // Nos meters de canal isto permanece desligado para conservar a grade enxuta.
    void setPeakReadout (bool shouldShow) { peakReadout = shouldShow; repaint(); }

    // Papel da luz (ver ClipRole). Padrao: canal / "clipou".
    void setClipRole (ClipRole newRole) { clipRole = newRole; repaint(); }

    // Fonte INFORMATIVA separada da fonte de ALERTA. Com as duas ligadas o mesmo
    // chip fala duas coisas diferentes sem duplicar elemento na tela:
    //   ambar "LIM"     -> o limiter esta segurando AGORA (acompanha o momento)
    //   vermelho "CLIP" -> passou de 0 dBFS (trava ate o clique)
    // Ate aqui as duas viviam num "||" so: o clip de 20 ms ficava indistinguivel
    // do limiter trabalhando normalmente, e o musico nunca via que clipou.
    // Esta funcao deve ser NAO destrutiva (varios medidores podem consulta-la).
    void setLimiterActivityFn (ClipFn fn) { limiterFn = std::move (fn); }

    // Como apagar a fonte do alerta quando o musico clica. Necessario quando a
    // trava de clip mora no MOTOR (e nao aqui): sem isto o proximo quadro releria
    // a trava ainda ligada e a luz reacenderia sozinha, nunca apagando.
    void setClipClearFn (std::function<void()> fn) { clipClearFn = std::move (fn); }

    // Teto da escala em dBFS, por instancia. Padrao +6 (mostra o headroom acima
    // de 0). Um medidor cuja saida ja passou pelo limiter nunca usa esses 6 dB e
    // pode pedir 0 para ganhar altura util na faixa que interessa.
    void setTopDb (float newTopDb)
    {
        maxDb = juce::jmax (minDb + 12.0f, newTopDb);
        repaint();
    }

    // Skin Church Live: barras de segmentos 7 px / 3 px (verde ate 60% da altura,
    // ambar ate 82%, vermelho acima), apagados a ~12%. `unit` e a escala da
    // referencia 1600x900. O alerta de clip travado continua pela moldura.
    void setSkin (bool enabled, float unit)
    {
        if (skinOn == enabled && skinUnit == unit)
            return;

        skinOn = enabled;
        skinUnit = unit;
        repaint();
    }

    // avisa o nivel exibido (0..1) a cada quadro. Serve pra acender o card do
    // canal sem criar um segundo timer nem um segundo leitor do nivel (o
    // readAndResetLayerLevel do processador ZERA a cada leitura: so pode ter um).
    std::function<void (float)> onLevel;

    void paintSkin (juce::Graphics& g)
    {
        auto bounds = getLocalBounds().toFloat();
        const float u = skinUnit;
        const bool alerta = clipSticky && clipRole != ClipRole::limiterWorking;

        juce::Rectangle<float> labelRow;
        if (channelLabels && channels == 2)
            labelRow = bounds.removeFromBottom (juce::jmax (12.0f, 20.0f * u));

        const float gap = channels == 2 ? juce::jmax (2.0f, 6.0f * u) : 0.0f;
        const float barW = (bounds.getWidth() - gap * (float) (channels - 1)) / (float) channels;
        const float segH = juce::jmax (3.0f, 7.0f * u);
        const float segGap = juce::jmax (1.5f, 3.0f * u);
        const float step = segH + segGap;

        for (int ch = 0; ch < channels; ++ch)
        {
            const auto bar = juce::Rectangle<float> (bounds.getX() + (float) ch * (barW + gap),
                                                     bounds.getY(), barW, bounds.getHeight());
            const int numSeg = juce::jmax (1, (int) ((bar.getHeight() + segGap) / step));
            const float ladderTop = bar.getBottom() - ((float) numSeg * step - segGap);
            const float lvl = displayDb[ch];
            const float peak = peakDb[ch];
            const float radius = juce::jmax (1.0f, segH * 0.18f);

            for (int i = 0; i < numSeg; ++i)
            {
                const auto seg = juce::Rectangle<float> (bar.getX(), ladderTop + (float) i * step, barW, segH);
                const float pEntry = 1.0f - (seg.getBottom() - bar.getY()) / bar.getHeight();
                const float pColour = 1.0f - (seg.getCentreY() - bar.getY()) / bar.getHeight();
                const float dbEntry = proportionToDb (pEntry);
                const bool active = lvl > minDb + 0.05f && dbEntry <= lvl + 0.01f;
                const float p = dbToProportion (proportionToDb (pColour));
                const auto tone = p >= 0.82f ? juce::Colour (0xfff2495c)
                                : p >= 0.60f ? juce::Colour (0xfff4b73b)
                                             : juce::Colour (0xff2fcb74);
                g.setColour (active ? tone : tone.withAlpha (0.12f));
                g.fillRoundedRectangle (seg, radius);
            }

            if (peak > minDb + 0.5f)
            {
                const float py = bar.getBottom() - bar.getHeight() * dbToProportion (peak);
                g.setColour (peak >= -0.05f ? juce::Colour (0xfff2495c) : juce::Colours::white.withAlpha (0.85f));
                g.fillRect (juce::Rectangle<float> (bar.getX(), py - 1.0f, barW, 2.0f).getIntersection (bar));
            }
        }

        if (alerta)
        {
            g.setColour (NordLookAndFeel::redClip().withAlpha (0.9f));
            g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.75f), 3.0f, 1.5f);
        }

        if (! labelRow.isEmpty())
        {
            g.setFont (juce::Font (juce::FontOptions (juce::jmax (9.0f, 14.0f * u))).boldened());
            g.setColour (juce::Colour (0xffa3a1ad));
            const char* names[] = { "L", "R" };
            for (int ch = 0; ch < channels; ++ch)
                g.drawText (names[ch], juce::Rectangle<float> (labelRow.getX() + (float) ch * (barW + gap),
                                                               labelRow.getY(), barW, labelRow.getHeight()),
                            juce::Justification::centred);
        }
    }

    void paint (juce::Graphics& g) override
    {
        if (skinOn)
        {
            paintSkin (g);
            return;
        }

        auto bounds = getLocalBounds().toFloat();

        const bool temChip = clipFn != nullptr || limiterFn != nullptr;

        // QUANDO NAO CABE, A BARRA E A ULTIMA A SAIR.
        //
        // A cromagem come altura FIXA: 13 px dos rotulos L/R, 17 px do pico
        // numerico, 12+1 px do chip e 2x5 px da margem interna -- 53 px antes do
        // primeiro pixel de barra. Em janela baixa (laptop 720p, que e a maquina
        // que vai pro culto) o layout entrega ao medidor MENOS que isso, e a
        // barra nascia com altura negativa: as marcas de escala e a linha de -1
        // dBFS iam parar dentro do chip e o medidor virava um risco solto. Em vez
        // de pintar lixo, o medidor abre mao do acessorio na ordem em que ele
        // informa menos -- primeiro o pico numerico, depois os rotulos L/R --
        // porque o chip e a escada sao o que o musico le de longe, no meio da
        // musica. Ver tambem o piso do grupo OUTPUT no layout do editor.
        const float labelH = 13.0f, peakH = 17.0f;
        const float cromagemFixa = (temChip ? 13.0f : 0.0f)
                                 + (channels == 1 ? 3.0f : 5.0f) * 2.0f;
        const float barraMinima = 12.0f;   // ~3 celulas: abaixo disso nao le

        bool mostraLabels = channelLabels && channels == 2;
        bool mostraPico   = peakReadout && channels == 2;

        float sobra = bounds.getHeight() - cromagemFixa
                        - (mostraLabels ? labelH : 0.0f)
                        - (mostraPico ? peakH : 0.0f);

        if (mostraPico && sobra < barraMinima)     { mostraPico = false;   sobra += peakH; }
        if (mostraLabels && sobra < barraMinima)   { mostraLabels = false; sobra += labelH; }

        // Nem uma celula cabe: so o chip sobrevive. Escada, marcas de escala e
        // linha de -1 dBFS ficam de fora -- pintadas aqui elas cairiam por cima
        // do chip, que e a unica coisa ainda legivel neste tamanho.
        const bool temBarra = sobra >= 3.0f;

        juce::Rectangle<float> labelRow;
        if (mostraLabels)
            labelRow = bounds.removeFromBottom (labelH);

        juce::Rectangle<float> peakRow;
        if (mostraPico)
            peakRow = bounds.removeFromBottom (peakH);

        // TRES ESTADOS PARA A LUZ, sem duplicar elemento na tela:
        //   apagado          -> nada acontecendo
        //   ambar    "LIM"   -> o limiter esta segurando AGORA. Informativo:
        //                       num louvor forte ele trabalha o tempo todo,
        //                       entao acompanha o momento em vez de travar.
        //   vermelho "CLIP"  -> o que a fonte de alerta daquele medidor
        //                       significa (canal: passou de 0 dBFS; master:
        //                       sobrecarga de gain-staging). TRAVA ate o clique,
        //                       porque o musico estava tocando e nao viu.
        // Os dois nao disputam o mesmo pixel: quem carrega o alerta travado e a
        // MOLDURA; o chip fica com a informacao ao vivo.
        //
        // ambarLegado = papel antigo (ClipRole::limiterWorking), onde a propria
        // clipSticky ja significava "limiter trabalhando". Hoje nenhum medidor o
        // usa (os dois do master passaram a separar alerta de informacao), mas o
        // caminho fica de pe pra quem so tem uma fonte momentanea pra oferecer.
        const bool ambarLegado = clipRole == ClipRole::limiterWorking;
        const bool alerta      = clipSticky && ! ambarLegado;
        // O ALERTA NAO PODE CALAR A LUZ DO LIMITER.
        // Antes isto era "limiterActive && ! alerta": como a trava vermelha so
        // apaga no clique, bastava UM alerta pra que o "LIM" -- a unica
        // informacao AO VIVO do chip -- nunca mais aparecesse no resto do culto.
        // Os dois avisos passam a andar em canais diferentes: a MOLDURA carrega
        // o alerta travado (area muito maior, ver logo abaixo) e o CHIP continua
        // contando o que esta acontecendo AGORA.
        const bool trabalhando = (clipSticky && ambarLegado) || limiterActive;
        const bool aceso       = alerta || trabalhando;

        // Chip que tem fonte de limiter descansa escrito "LIM": e o que ele
        // mostra na maior parte do tempo. So o alerta SEM limiter ativo troca o
        // rotulo -- com os dois valendo, quem grita e a moldura.
        const bool falaDeLimiter = ambarLegado || limiterFn != nullptr;
        const bool chipDeAlerta  = alerta && ! trabalhando;

        const auto alertaColour = juce::Colour (0xffff1744);

        // Rebaixo de vidro fumê: a barra parece instalada na chapa, nao apenas
        // desenhada sobre ela. O centro mais escuro aumenta a leitura dos LEDs.
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff151014),
                                                 bounds.getX(), bounds.getY(),
                                                 juce::Colour (0xff050607),
                                                 bounds.getRight(), bounds.getBottom(), true));
        g.fillRoundedRectangle (bounds, 4.0f);

        // MOLDURA COMO SEGUNDO AVISO DE CLIP. O chip de 12x40 px e legivel de
        // perto, mas vermelho saturado nao passa de ~3,8:1 contra a cromagem
        // escura -- e o tecladista olha essa tela de longe, de lado, no meio da
        // musica. A moldura inteira mudando de cor e uma area MUITO maior pelo
        // mesmo preco: nao ha repaint extra nenhum, e o contorno ja era
        // desenhado aqui todo quadro. Some junto com a trava, no clique.
        g.setColour (alerta ? alertaColour.withAlpha (0.85f)
                            : juce::Colours::white.withAlpha (0.028f));
        g.drawRoundedRectangle (bounds.reduced (0.5f), 4.0f, alerta ? 1.5f : 0.8f);

        // luz de clip DENTRO da moldura, como tampa do medidor: flutuando acima
        // com um vao ela lia como um elemento vermelho perdido/cortado
        const bool slimMeter = getWidth() <= 30;
        if (clipFn || limiterFn)
        {
            auto clipArea = bounds.removeFromTop (slimMeter ? 9.0f : 12.0f)
                                  .reduced (slimMeter ? 2.5f : 4.0f, slimMeter ? 2.0f : 2.5f);

            const auto onColour  = chipDeAlerta ? alertaColour               // vermelho: alerta
                                                : juce::Colour (0xffffb020);  // ambar: informativo
            const auto offColour = falaDeLimiter ? juce::Colour (0xff2e2718)
                                                 : juce::Colour (0xff3a2226);
            const char* const rotulo = slimMeter ? ""
                                      : chipDeAlerta ? "CLIP"
                                      : falaDeLimiter ? "LIM"
                                      : getWidth() < 50 ? "CL" : "CLIP";

            g.setColour (aceso ? onColour : offColour);
            g.fillRoundedRectangle (clipArea, slimMeter ? 2.2f : 3.0f);

            if (aceso)
            {
                g.setColour (onColour.withAlpha (0.38f));
                g.drawRoundedRectangle (clipArea.expanded (1.2f), slimMeter ? 3.0f : 4.0f, 1.3f);
            }

            if (rotulo[0] != '\0')
            {
                // Texto escuro sobre o ambar (o branco sumia); branco sobre o vermelho.
                g.setColour (aceso ? (chipDeAlerta ? juce::Colours::white : juce::Colour (0xff1a1207))
                                   : juce::Colours::white.withAlpha (0.34f));
                g.setFont (juce::Font (juce::FontOptions (7.5f, juce::Font::bold))
                               .withExtraKerningFactor (0.08f));
                g.drawText (rotulo, clipArea.toNearestInt(),
                            juce::Justification::centred, false);
            }

            bounds.removeFromTop (1.0f);
        }

        juce::Rectangle<float> scaleArea;
        if (scaleLabels && bounds.getWidth() >= 34.0f)
            scaleArea = bounds.removeFromLeft (14.0f);

        auto meterArea = bounds.reduced (slimMeter ? 1.5f : channels == 1 ? 2.0f : 2.5f);

        // Um VU estereo de console e uma dupla de colunas finas, nao um painel
        // vermelho largo. O componente conserva sua area de clique/leitura e
        // centraliza a escada dentro dela; no master isto deixa espaco para a
        // escala respirar e aproxima a proporcao do hardware da referencia.
        if (channels == 2 && meterArea.getWidth() > 31.0f)
        {
            const float ladderW = meterArea.getWidth() >= 52.0f ? 35.0f : 31.0f;
            meterArea = meterArea.withSizeKeepingCentre (ladderW,
                                                          meterArea.getHeight());
        }

        const float gap = channels == 1 ? 0.0f : 1.5f;
        const float barWidth = (meterArea.getWidth() - gap * (float) (channels - 1)) / (float) channels;

        if (! peakRow.isEmpty())
        {
            const auto mono = juce::Font (juce::FontOptions (
                juce::Font::getDefaultMonospacedFontName(), 9.0f,
                juce::Font::bold));
            g.setFont (mono);

            for (int ch = 0; ch < channels; ++ch)
            {
                auto cell = juce::Rectangle<float> (
                    meterArea.getX() + (float) ch * (barWidth + gap),
                    peakRow.getY(), barWidth, peakRow.getHeight());
                const float db = peakDb[ch];
                const auto value = db <= minDb + 0.5f
                    ? juce::String::fromUTF8 ("\xe2\x88\x92\xe2\x88\x9e")
                    : (db >= 0.0f ? "+" : juce::String())
                        + juce::String (db, 1);

                g.setColour (db >= -0.05f ? NordLookAndFeel::redClip()
                                          : juce::Colours::white.withAlpha (0.72f));
                g.drawFittedText (value, cell.toNearestInt(),
                                  juce::Justification::centred, 1, 0.72f);
            }
        }

        for (int ch = 0; ch < channels && temBarra; ++ch)
        {
            auto barArea = juce::Rectangle<float> (meterArea.getX() + (float) ch * (barWidth + gap),
                                                   meterArea.getY(), barWidth, meterArea.getHeight());

            g.setGradientFill (juce::ColourGradient (juce::Colour (0xff1b0d12),
                                                     barArea.getX(), barArea.getY(),
                                                     juce::Colour (0xff07080a),
                                                     barArea.getRight(), barArea.getY(), false));
            g.fillRoundedRectangle (barArea, juce::jmin (2.0f, barWidth * 0.24f));
            g.setColour (juce::Colours::white.withAlpha (0.030f));
            g.drawRoundedRectangle (barArea.reduced (0.35f),
                                    juce::jmin (2.0f, barWidth * 0.24f), 0.7f);

            // ESCADA SEGMENTADA em dBFS. A cor de cada celula vem da FAIXA DE NIVEL
            // que ela representa -- nao de um degrade continuo. Assim a fronteira
            // dos -6 e dos -3 dBFS fica no mesmo lugar em todos os canais, e o
            // musico aprende a posicao em vez de julgar o tom da cor.
            const float lvl = displayDb[ch];
            // Segmentacao fina de VU profissional: mais degraus e menos massa
            // colorida, mantendo a leitura das fronteiras de dB.
            const float segH = barArea.getWidth() >= 9.0f ? 1.28f : 1.12f;
            const float segGap = 0.50f, step = segH + segGap;
            const float barH = juce::jmax (1.0f, barArea.getHeight());

            // PISO 1, nao 4. Com jmax (4, ...) uma barra baixa demais recebia
            // quatro celulas que NAO cabiam nela: a escada transbordava a area do
            // medidor e ia pintar por cima da leitura de pico -- com altura util
            // negativa eram quatro celulas solidas, verdes, completamente fora do
            // lugar. Barra curta agora mostra MENOS celulas, nunca celulas fora.
            const int numSeg = juce::jmax (1, (int) ((barArea.getHeight() + segGap) / step));

            // ESCADA ANCORADA EMBAIXO. A altura da barra quase nunca e multiplo
            // exato do passo, e a sobra ficava no fundo: a celula mais baixa
            // "valia" -57 em vez de -60, abrindo uma faixa morta de ~3 dB logo
            // acima do piso (sinal real de -59 dBFS nao acendia nada). Com a
            // sobra no TOPO -- onde a escala mostra +3..+6 dBFS, terreno que o
            // sinal pos-limiter nunca alcanca -- a base da celula mais baixa cai
            // exatamente no piso e as marcas de -3/-6/-12/-30 passam a coincidir
            // com as fronteiras das celulas.
            const float ladderH   = (float) numSeg * step - segGap;
            const float ladderTop = barArea.getBottom() - ladderH;

            for (int i = 0; i < numSeg; ++i)
            {
                const float segTop = ladderTop + (float) i * step;

                // recorte defensivo: nenhuma celula pinta fora da barra
                auto seg = juce::Rectangle<float> (barArea.getX(), segTop,
                                                   barArea.getWidth(), segH)
                               .getIntersection (barArea);

                if (seg.isEmpty())
                    continue;

                // A celula OCUPA uma faixa de dB, e a faixa e lida de dois jeitos:
                //  - dbEntrada (BASE da celula): o nivel em que ela ACENDE, como
                //    num bargraph de verdade -- a celula acende quando o nivel
                //    ENTRA nela. Antes o acender usava o centro, e isso deixava
                //    a celula de baixo exigindo bem mais que o piso.
                //  - dbCor (CENTRO da celula): a faixa a que ela PERTENCE, que
                //    decide a COR. Assim a fronteira de -30/-12/-6/-3 dBFS cai
                //    sempre na mesma altura em todos os canais.
                //
                // Os dois saem do retangulo JA RECORTADO ('seg'), nao do segTop
                // cru. Com o segTop cru, uma barra mais baixa que uma celula
                // punha o centro ACIMA do topo da barra: a proporcao passava de
                // 1, o jlimit de proportionToDb saturava e dbCor virava +6 dBFS
                // -- ou seja, a celula pintava no VERMELHO DE ALERTA com o sinal
                // em -55 dBFS. Mentir "clipando" e a mesma classe de erro que a
                // guarda de piso logo abaixo existe pra eliminar.
                const float dbEntrada = proportionToDb (1.0f - (seg.getBottom()  - barArea.getY()) / barH);
                const float dbCor     = proportionToDb (1.0f - (seg.getCentreY() - barArea.getY()) / barH);

                // SEM SINAL NENHUM, NENHUMA CELULA ACENDE.
                //
                // "piso da escala" e "silencio" sao coisas diferentes, e confundir
                // as duas era a origem da barra verde sem som algum: em silencio
                // absoluto o nivel exibido vale EXATAMENTE minDb (ver silenceGain)
                // e a celula mais baixa tambem vale minDb na entrada, entao
                // "-60 <= -60" acendia ela para sempre -- e colourForDb (-60) e
                // verde. A guarda de piso abaixo resolve isso sem depender da
                // geometria: enquanto o nivel estiver NO piso, a barra inteira
                // fica escura; qualquer sinal acima do piso ja acende a primeira
                // celula. E a mesma guarda que o marcador de pico logo abaixo ja
                // usava e que faltava aqui.
                const bool active = lvl > minDb + 0.05f
                                 && dbEntrada <= lvl + 0.01f;

                const float energy = dbToProportion (dbCor);
                const auto ledColour = colourForDb (dbCor)
                    .withMultipliedBrightness (0.62f + energy * 0.38f);

                // O halo nasce apenas em LED aceso e cresce perto do limite.
                // Segmento desligado continua material fosco, nunca luminoso.
                if (active && energy > 0.42f)
                {
                    g.setColour (ledColour.withAlpha (
                        0.035f + (energy - 0.42f) * 0.12f));
                    g.fillRect (seg.expanded (0.55f, 0.25f));
                }

                g.setColour (active ? ledColour : segOff());
                g.fillRect (seg);

                if (active && seg.getHeight() > 1.0f)
                {
                    g.setColour (juce::Colours::white.withAlpha (
                        0.075f + energy * 0.085f));
                    g.fillRect (seg.withHeight (juce::jmin (0.55f,
                                                           seg.getHeight() * 0.24f)));
                }
            }

            // marcador de pico: linha fina alinhada aos segmentos. Recortado
            // como as celulas -- perto do topo/fundo a linha de 1,8 px passava
            // da barra e ia riscar a cromagem vizinha.
            if (peakDb[ch] > minDb + 0.5f)
            {
                const float y = barArea.getBottom() - barArea.getHeight() * dbToProportion (peakDb[ch]);
                g.setColour (peakDb[ch] >= -0.05f ? NordLookAndFeel::redClip() : juce::Colours::white);
                g.fillRect (juce::Rectangle<float> (barArea.getX(), y - 0.9f, barArea.getWidth(), 1.8f)
                                .getIntersection (barArea));
            }
        }

        // Escala: as finas caem EXATAMENTE nas fronteiras de cor (-3, -6, -12,
        // -30), entao a altura ensina a faixa em vez de o musico julgar o tom.
        // O -24 saiu porque nao era fronteira de nada; entraram -3 e -30.
        // So existem se existe barra: sem ela estes riscos cairiam dentro do
        // chip e o medidor virava um punhado de linhas soltas.
        if (temBarra)
        {
            for (float mark : { -3.0f, -6.0f, -12.0f, -30.0f, -48.0f })
            {
                const float y = meterArea.getBottom() - meterArea.getHeight() * dbToProportion (mark);
                g.setColour (juce::Colours::white.withAlpha (0.28f));
                g.drawHorizontalLine ((int) juce::jlimit (meterArea.getY(),
                                                          meterArea.getBottom() - 1.0f, y),
                                      meterArea.getX(), meterArea.getRight());
            }

            // A linha grossa mudou de 0 para -1 dBFS: em 0 o estrago ja
            // aconteceu. -1 e onde o musico ainda tem tempo de puxar o fader.
            const float y = meterArea.getBottom() - meterArea.getHeight() * dbToProportion (-1.0f);
            g.setColour (juce::Colour (0xffffd740).withAlpha (0.85f));
            g.fillRect (juce::Rectangle<float> (meterArea.getX(), y - 1.1f,
                                                meterArea.getWidth(), 2.2f)
                            .getIntersection (meterArea));

            if (! scaleArea.isEmpty())
            {
                g.setFont (juce::Font (juce::FontOptions (
                    juce::Font::getDefaultMonospacedFontName(), 7.0f,
                    juce::Font::plain)));
                g.setColour (juce::Colours::white.withAlpha (0.44f));
                for (const auto mark : { 0, -6, -12, -24, -40 })
                {
                    const float markY = meterArea.getBottom()
                        - meterArea.getHeight() * dbToProportion ((float) mark);
                    g.drawText (juce::String (mark),
                                juce::Rectangle<int> (
                                    scaleArea.getX(), juce::roundToInt (markY) - 4,
                                    scaleArea.getWidth() - 1.0f, 8.0f),
                                juce::Justification::centredRight, false);
                }
            }
        }

        if (! labelRow.isEmpty())
        {
            g.setFont (juce::Font (juce::FontOptions (10.0f)).boldened());
            g.setColour (juce::Colours::white.withAlpha (0.45f));
            const char* names[] = { "L", "R" };
            for (int ch = 0; ch < channels; ++ch)
            {
                auto cell = juce::Rectangle<float> (meterArea.getX() + (float) ch * (barWidth + gap),
                                                    labelRow.getY(), barWidth, labelRow.getHeight());
                g.drawText (names[ch], cell, juce::Justification::centred);
            }
        }
    }

private:
    static constexpr float minDb = -60.0f;
    float maxDb = 6.0f;   // teto por instancia -- ver setTopDb()

    // SILENCIO ABSOLUTO x PISO DA ESCALA -- nao sao a mesma coisa.
    // juce::Decibels::gainToDecibels (0.0f, -60.0f) devolve EXATAMENTE -60: o
    // silencio digital chega na conversao com o MESMO numero que um sinal real
    // de -60 dBFS produziria. Por isso a pergunta "tem sinal?" e respondida
    // ANTES da conversao, na amplitude LINEAR que vem do motor, em vez de
    // depender do valor de saturacao. -100 dBFS fica dezenas de dB abaixo de
    // qualquer coisa audivel num sistema de som e MUITO acima de um denormal.
    static constexpr float silenceGain = 1.0e-5f;   // ~ -100 dBFS

    float dbToProportion (float db) const noexcept
    {
        return juce::jlimit (0.0f, 1.0f, (db - minDb) / (maxDb - minDb));
    }

    float proportionToDb (float p) const noexcept
    {
        return minDb + juce::jlimit (0.0f, 1.0f, p) * (maxDb - minDb);
    }

    // A paleta do medidor vive AQUI, e nao no NordLookAndFeel, porque ela e
    // escolhida por CONTRASTE, nao por harmonia com o resto da tela: todo
    // segmento aceso fica acima de 4.5:1 contra o apagado, que e o minimo pra
    // ler a alguns metros sob refletor. A do look and feel dava 1,4:1 na faixa
    // de -60 a -30 (sumia no palco) e 1,1:1 entre o clip e o -3 (o musico nao
    // via que tinha clipado). Nenhum outro componente usava essas cores.
    static juce::Colour segOff() noexcept { return juce::Colour (0xff140d10); }

    // Cada celula pinta pela faixa a que PERTENCE, entao as fronteiras de -30,
    // -12, -6, -3 e 0 dBFS caem sempre na mesma altura em todos os canais.
    static juce::Colour colourForDb (float db) noexcept
    {
        // Escala pedida pelo dono: seguro = verde, elevado = ambar, PERTO DO
        // CLIP = laranja, clipou = vermelho.
        //
        // O -3..0 PRECISA ficar laranja. Ja se tentou pinta-lo de vermelho
        // (0xffff3b30) "porque no master, depois do limiter, o sinal nunca chega
        // a 0 dBFS" -- mas esta funcao e static e serve TAMBEM os medidores de
        // CANAL, que sao pos-fader e PRE-limiter e cruzam 0 dBFS de verdade. Com
        // as duas ultimas faixas no mesmo vermelho o contraste entre elas cai
        // pra 1,08:1 e a fronteira de 0 dBFS -- a unica coisa na barra que diz
        // "este canal esta distorcendo" -- some a alguns metros. E exatamente o
        // 1,1:1 que fez a paleta do NordLookAndFeel ser rejeitada (ver acima).
        const auto smooth = [] (float t)
        {
            t = juce::jlimit (0.0f, 1.0f, t);
            return t * t * (3.0f - 2.0f * t);
        };
        const auto blend = [&smooth] (juce::Colour a, juce::Colour b, float t)
        {
            return a.interpolatedWith (b, smooth (t));
        };

        if (db >= 0.0f)
            return juce::Colour (0xffff1744);   // clip real: vermelho inequivoco
        if (db >= -6.0f)
            return blend (juce::Colour (0xffef4055), juce::Colour (0xffffb020),
                          (db + 6.0f) / 6.0f);  // vermelho -> ambar perto do limite
        if (db >= -12.0f)
            return blend (juce::Colour (0xffb6263b), juce::Colour (0xffef4055),
                          (db + 12.0f) / 6.0f);
        if (db >= -30.0f)
            return blend (juce::Colour (0xff741829), juce::Colour (0xffb6263b),
                          (db + 30.0f) / 18.0f);

        return blend (juce::Colour (0xff43101a), juce::Colour (0xff741829),
                      (db - minDb) / 30.0f);
    }

    void meterTick() override
    {
        // escondido (ex.: tela da cadeia sob o Church Live fullscreen): nao
        // consome a fonte le-e-zera -- roubava quadros e clips do medidor
        // visivel. Zera o estado de exibicao pra nao mostrar nivel velho ao
        // reaparecer.
        //
        // TAMANHO ZERO CONTA COMO ESCONDIDO. juce::Component::isShowing() olha
        // so a cadeia de visibilidade e IGNORA o tamanho -- e varios layouts
        // "guardam" um medidor com setBounds ({}) em vez de setVisible (false).
        // Esse medidor de 0x0 continuava com o timer rodando, drenando a fonte
        // de nivel e repintando nada. Testar a area aqui resolve para TODOS os
        // medidores de uma vez, sem depender de cada chamada de layout lembrar
        // de esconder (e sem o risco de algum caminho esquecer de mostrar).
        if (getWidth() <= 0 || getHeight() <= 0 || ! isShowing())
        {
            if (wasHidden)
                return;

            for (int ch = 0; ch < channels; ++ch)
            {
                displayDb[ch] = minDb;
                peakDb[ch] = minDb;
                lastShownDb[ch] = minDb;
                lastShownPeak[ch] = minDb;
                peakHoldFrames[ch] = 0;
            }

            // luz informativa do limiter tambem apaga: ela e "esta segurando
            // AGORA", e escondido nao existe agora nenhum pra mostrar
            limiterActive = false;

            wasHidden = true;   // marca pra drenar o acumulado ao reaparecer

            // o card de atividade (onLevel) tambem apaga: senao ele reaparece
            // aceso com o brilho de antes enquanto a barra ja pinta zerada
            if (onLevel != nullptr)
                onLevel (0.0f);

            return;
        }

        // primeiro tick de volta apos periodo escondido (isShowing() tambem e
        // false com a janela MINIMIZADA, quando NENHUM medidor consome as
        // fontes): o le-e-zera acumulou o pico maximo e a trava de clip do
        // periodo inteiro. Le UMA vez e DESCARTA, senao o acumulado viraria
        // spike falso, peak-hold de evento antigo e clipSticky aceso por um
        // clip de minutos atras. Display segue zerado neste tick; do proximo
        // em diante, comportamento normal.
        if (wasHidden)
        {
            wasHidden = false;

            // Fonte destrutiva (canal, e a regua do MainComponent): drena sem
            // acender. Fonte com trava no motor (master, ver setClipClearFn):
            // esta chamada nao apaga nada e o alerta reaparece no proximo tick --
            // e o certo, porque a trava e justamente "clipou e ninguem viu".
            if (clipFn)
                clipFn();

            if (levelFn)
                for (int ch = 0; ch < channels; ++ch)
                    levelFn (ch);   // drena o pico acumulado sem exibir

            return;
        }

        bool chromeChanged = false;
        bool barsChanged = false;

        if (clipFn)
        {
            const bool agora = clipFn();

            if (clipRole == ClipRole::limiterWorking)
            {
                if (agora != clipSticky)
                {
                    clipSticky = agora;
                    chromeChanged = true;
                }
            }
            else if (clipClearFn != nullptr)
            {
                if (agora != clipSticky)
                {
                    clipSticky = agora;
                    chromeChanged = true;
                }
            }
            else if (agora && ! clipSticky)
            {
                clipSticky = true;
                chromeChanged = true;
            }
        }

        if (limiterFn)
        {
            const bool segurando = limiterFn();

            if (segurando != limiterActive)
            {
                limiterActive = segurando;
                chromeChanged = true;
            }
        }

        bool allRestingAtFloor = true;
        for (int ch = 0; ch < channels; ++ch)
        {
            const float raw = levelFn ? levelFn (ch) : 0.0f;

            // Fast-path: canal ja em repouso no piso e sem sinal novo -> zero math.
            if (raw <= silenceGain
                && displayDb[ch] <= minDb
                && peakDb[ch] <= minDb
                && lastShownDb[ch] <= minDb
                && lastShownPeak[ch] <= minDb)
            {
                peakHoldFrames[ch] = 0;
                continue;
            }

            allRestingAtFloor = false;
            const float newDb = raw > silenceGain
                                  ? juce::Decibels::gainToDecibels (raw, minDb)
                                  : minDb;

            if (newDb > displayDb[ch])
            {
                const float attackBlend = timerHzUsado >= 40 ? 0.86f : 1.0f;
                displayDb[ch] += (newDb - displayDb[ch]) * attackBlend;

                if (newDb - displayDb[ch] < 0.08f)
                    displayDb[ch] = newDb;
            }
            else
            {
                displayDb[ch] = juce::jmax (displayDb[ch] - quedaBarraDb, minDb);
            }

            if (newDb > peakDb[ch])
            {
                peakDb[ch] = newDb;
                peakHoldFrames[ch] = ticksHold;
            }
            else if (peakHoldFrames[ch] > 0)
            {
                --peakHoldFrames[ch];
            }
            else
            {
                peakDb[ch] = juce::jmax (peakDb[ch] - quedaPicoDb, minDb);
            }

            const bool floorCrossed =
                ((displayDb[ch] > minDb + 0.05f) != (lastShownDb[ch] > minDb + 0.05f))
                || ((peakDb[ch] > minDb + 0.5f) != (lastShownPeak[ch] > minDb + 0.5f));
            if (floorCrossed
                 || std::abs (displayDb[ch] - lastShownDb[ch]) > 0.16f
                 || std::abs (peakDb[ch] - lastShownPeak[ch]) > 0.30f)
            {
                lastShownDb[ch] = displayDb[ch];
                lastShownPeak[ch] = peakDb[ch];
                barsChanged = true;
            }
        }

        // Repinta moldura+chip apenas quando alerta/limiter mudou; quando so o
        // nivel mudou, invalida somente o miolo das barras (poupando borda e chip).
        if (chromeChanged || (skinOn && barsChanged))
        {
            repaint();
        }
        else if (barsChanged)
        {
            const int topInset = (clipFn || limiterFn) ? (getWidth() <= 30 ? 10 : 13) : 1;
            const int bottomInset = (channelLabels && channels == 2 && ! peakReadout) ? 13 : 1;
            auto dirty = getLocalBounds()
                             .withTrimmedTop (topInset)
                             .withTrimmedBottom (bottomInset)
                             .reduced (1, 0);
            if (! dirty.isEmpty())
                repaint (dirty);
            else
                repaint();
        }

        if (allRestingAtFloor && lastOnLevel == 0.0f)
            return;

        if (onLevel != nullptr)
        {
            float top = 0.0f;

            for (int ch = 0; ch < channels; ++ch)
                top = juce::jmax (top, dbToProportion (displayDb[ch]));

            // Nao acorda ActivityBar a cada tick se o nivel mal mudou.
            if (std::abs (top - lastOnLevel) > 0.04f || (top < 0.02f && lastOnLevel > 0.02f))
            {
                lastOnLevel = top;
                onLevel (top);
            }
        }
    }

    int channels = 2;
    LevelFn levelFn;
    ClipFn clipFn;
    ClipFn limiterFn;                  // informativa (ambar) -- ver setLimiterActivityFn
    std::function<void()> clipClearFn; // apaga a trava na FONTE -- ver setClipClearFn
    bool limiterActive = false;
    float displayDb[2] { minDb, minDb };
    float peakDb[2] { minDb, minDb };
    float lastShownDb[2] { minDb, minDb };
    float lastShownPeak[2] { minDb, minDb };
    int peakHoldFrames[2] { 0, 0 };
    bool clipSticky = false;
    ClipRole clipRole = ClipRole::channelClip;
    bool channelLabels = false;
    bool scaleLabels = false;
    bool peakReadout = false;
    bool skinOn = false;
    float skinUnit = 1.0f;

    // Balistica do medidor derivada da TAXA REAL do timer, nao de constantes
    // por quadro. Antes a queda e o peak-hold eram contados em QUADROS fixos
    // (1.2 dB/quadro, hold 45), entao no touch (10 Hz) o medidor ficava 3x
    // mais lento que no PC. Com estes valores a balistica e a MESMA
    // em qualquer taxa: 36 dB/s de queda da barra, 24 dB/s de queda do pico,
    // 1,5 s de peak-hold -- mesmo com o desktop agora em 45 Hz.
    int timerHzUsado = 45;
    float quedaBarraDb = 1.2f;   // 36 dB/s @30 Hz
    float quedaPicoDb  = 0.8f;   // 24 dB/s @30 Hz
    int   ticksHold    = 45;     // 1,5 s @30 Hz
    // Nasce true: medidor recem-construido (re-dock do editor) tambem encontra
    // acumulo de quando nao existia consumidor -- o primeiro tick sempre drena
    bool wasHidden = true;
    float lastOnLevel = -1.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LevelMeter)
};
