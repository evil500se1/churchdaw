#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <atomic>
#include <memory>
#include <vector>

//==============================================================================
// LEITURA DOS SAMPLES DIRETO DO DISCO, EM VEZ DE TUDO NA MEMORIA.
//
// POR QUE ISTO EXISTE
// -------------------
// O motor carregava cada sample INTEIRO na RAM, em float de 32 bits. No PC essa
// e uma boa decisao: zero latencia de disco, zero complexidade. No celular ela
// vira o teto do produto. Medido no aparelho de teste com DOIS pianos abertos:
//
//     RES = 3,8 GB   (1,9 GB por piano)
//
// e o motor tem CINCO canais. Cheios seriam ~9,5 GB num aparelho com 8,6 GB
// livres: nao cabe. E muito antes de faltar memoria o Android ja comeca a
// comprimir e mover paginas -- foi dai que veio o audio pipocando, e NAO de
// falta de CPU (medido: 7-11% em repouso).
//
// COMO FUNCIONA
// -------------
// De cada zona fica na RAM so o COMECO. E o bastante para a nota soar no
// instante do toque enquanto a thread de leitura busca o resto. Cada VOZ tocando
// recebe uma janela deslizante do arquivo:
//
//     zona (RAM):  [==]                                      ~120 ms
//     arquivo:     [==|----------------------------------]
//     voz:            [~~~~]  janela que anda com a nota
//
// DUAS janelas por voz, alternadas: a thread de audio le UMA enquanto a de
// leitura enche a OUTRA, e a troca e um inteiro atomico. Sem trava nenhuma no
// caminho do audio -- travar ali e exatamente o que produz o estalo que estamos
// tentando eliminar.
//
// CUSTO: cada voz precisa das proprias janelas, entao polifonia deixa de ser de
// graca. 16384 amostras x 2 janelas x 2 canais x 4 bytes = 256 KB por voz.
// Por isso o teto de vozes cai no celular (ver o budget em TimbresProcessor).
//
// CUIDADO COM O FLAC: na RAM era decodificado UMA vez, na carga; aqui e o tempo
// todo. Um nucleo ARM decodifica FLAC muito acima do tempo real, entao dezenas
// de vozes cabem -- mas e o ponto onde esta mudanca pode trocar um problema por
// outro. Se faltar, o caminho de volta e guardar os samples do celular sem
// compressao (espaco sobra: 408 GB livres no aparelho).
//==============================================================================
class SampleStreamer : private juce::Thread
{
public:
    // 65536 = 1,5 s a 44,1 kHz. Era 16384 (0,37 s), e cada janela curta obriga
    // uma nova busca no arquivo pouco depois -- com FLAC, buscar significa
    // decodificar desde o ponto de sincronismo anterior, e isso quatro vezes mais
    // vezes por nota. Janela maior = menos idas ao disco, que e o recurso escasso
    // aqui. Custo: 256 vozes x 2 janelas x 65536 x 2 canais x 4 bytes = ~268 MB,
    // fixo -- nao cresce com o tamanho da biblioteca.
    static constexpr int windowSamples = 65536;

    // As janelas precisam se SOBREPOR. Sem isso, o bloco que atravessa a emenda
    // nunca cabe inteiro nem na janela antiga nem na nova: acquire() exige o
    // bloco completo para a interpolacao Hermite e devolvia "passou", inserindo
    // um bloco de silencio exatamente a cada troca de janela. 8192 samples
    // cobrem com folga varios blocos mesmo com transposicao e custam apenas
    // cerca de 14% de leituras adicionais.
    static constexpr int windowOverlap = 8192;

    //== Uma fonte no disco. Uma por ZONA; varias vozes podem apontar para ela. ==
    struct Source
    {
        juce::File file;
        juce::int64 totalSamples = 0;   // ja DESCONTADO o fileOffset
        int numChannels = 2;

        // Tempo morto cortado do inicio do arquivo (ver headSilence no motor:
        // 97,5% dos samples comecam com silencio gravado, mediana de 12,4 ms).
        // A posicao 0 da VOZ corresponde a esta posicao no ARQUIVO -- sem isto o
        // streaming reintroduziria o atraso que o corte tirou.
        juce::int64 fileOffset = 0;

        bool isValid() const noexcept { return totalSamples > 0; }
    };

    //== O estado de streaming de UMA voz ======================================
    struct VoiceStream
    {
        // Trocado pela thread de audio no note-on. A de leitura so LE.
        std::atomic<const Source*> source { nullptr };

        // Geracao: incrementada a cada note-on. A thread de leitura carimba o
        // valor que viu ao comecar; se mudou quando termina, o resultado e de
        // uma nota que ja morreu e vai fora. Sem isto, uma leitura lenta entrega
        // audio da nota ANTERIOR na voz reciclada -- que soa como um glitch
        // aleatorio, do tipo que so aparece tocando rapido.
        std::atomic<int> generation { 0 };

        juce::AudioBuffer<float> buf[2];
        juce::int64 winStart[2] { 0, 0 };
        int winValid[2] { 0, 0 };
        int winGen[2] { -1, -1 };

        std::atomic<int>  live { -1 };            // indice que o audio pode ler
        std::atomic<juce::int64> wantStart { -1 };// pedido pendente (-1 = nenhum)

        // so a thread de leitura toca nestes
        std::unique_ptr<juce::AudioFormatReader> reader;
        const Source* readerSource = nullptr;
        int fillIndex = 0;

        void prepare()
        {
            for (auto& b : buf)
                b.setSize (2, windowSamples, false, true, false);
        }

        // Chamado pela thread de AUDIO no note-on.
        void restart (const Source* s) noexcept
        {
            generation.fetch_add (1, std::memory_order_acq_rel);
            live.store (-1, std::memory_order_release);
            wantStart.store (-1, std::memory_order_release);
            source.store (s, std::memory_order_release);
        }

        void stop() noexcept
        {
            generation.fetch_add (1, std::memory_order_acq_rel);
            source.store (nullptr, std::memory_order_release);
            live.store (-1, std::memory_order_release);
            wantStart.store (-1, std::memory_order_release);
        }

        //== Chamado pela thread de AUDIO, uma vez por bloco ===================
        // Entrega os DOIS canais de uma vez: pedir canal por canal dobraria as
        // leituras atomicas e abriria a chance de os dois virem de janelas
        // diferentes se a troca caisse no meio.
        //
        // false = a janela ainda nao chegou. O chamador toca silencio naquele
        // bloco em vez de ler lixo -- uma falha audivel e ruim, ler memoria de
        // outra nota e pior.
        // motivo da ultima falha, so para diagnostico:
        // 1 = nenhuma janela carregada ainda   2 = janela de outra nota
        // 3 = a nota passou do trecho que a janela cobre
        mutable int lastFailReason = 0;

        bool acquire (juce::int64 pos, int needed,
                      const float*& l, const float*& r, int& offsetOut) const noexcept
        {
            // Onde a nota esta, para a thread de leitura nunca escrever por cima
            // da janela que estamos tocando (ver escolha do destino em fill()).
            playPos.store (pos, std::memory_order_release);

            const int gen = generation.load (std::memory_order_acquire);

            // OLHA AS DUAS JANELAS, nao so a "atual".
            //
            // Era aqui o defeito que causava o pipoco: quando a proxima janela
            // ficava pronta, ela era publicada na hora -- mas a nota ainda estava
            // tocando o FIM da anterior. Como so a publicada era consultada, e ela
            // comeca adiante da nota, TODA leitura falhava ate a nota alcancar.
            // Cada falha e um bloco de silencio, ou seja um clique. O buffer duplo
            // que existe para evitar buraco estava CRIANDO o buraco.
            //
            // Consultando as duas, a antiga continua servindo enquanto a nova
            // espera a vez -- que e o comportamento que o buffer duplo sempre
            // deveria ter tido.
            bool sawWindow = false, sawGen = false;

            for (int i = 0; i < 2; ++i)
            {
                if (winValid[i] <= 0)
                    continue;

                sawWindow = true;

                if (winGen[i] != gen)
                    continue;

                sawGen = true;

                const auto s = winStart[i];

                if (pos < s || pos + needed > s + winValid[i])
                    continue;

                offsetOut = (int) (pos - s);
                l = buf[i].getReadPointer (0);
                r = buf[i].getReadPointer (juce::jmin (1, buf[i].getNumChannels() - 1));
                return true;
            }

            lastFailReason = ! sawWindow ? 1 : (! sawGen ? 2 : 3);
            return false;
        }

        // Ultima posicao tocada, publicada pela thread de audio.
        mutable std::atomic<juce::int64> playPos { 0 };

        // Pede a PROXIMA janela quando a nota passa da metade da atual. Sem isto
        // a leitura so comecaria quando a janela acabasse, e ai o disco teria
        // zero de vantagem -- exatamente a falha que o streaming causa quando e
        // feito ingenuamente.
        void prefetch (juce::int64 pos) noexcept
        {
            if (wantStart.load (std::memory_order_acquire) >= 0)
                return;   // ja ha um pedido em andamento

            const int gen = generation.load (std::memory_order_acquire);

            // Pede a continuacao da janela que esta TOCANDO agora -- nao da
            // "atual" segundo o ponteiro live, que depois da correcao acima pode
            // muito bem ser a outra.
            for (int i = 0; i < 2; ++i)
            {
                if (winValid[i] <= 0 || winGen[i] != gen)
                    continue;

                const auto s   = winStart[i];
                const auto end = s + winValid[i];

                if (pos < s || pos >= end)
                    continue;

                // A ultima janela pode ser menor que windowSamples; nesse caso
                // ela ja termina junto com o arquivo e nao existe continuacao.
                if (winValid[i] < windowSamples)
                    return;

                // Ja existe a continuacao SOBREPOSTA carregada? Entao nao ha o
                // que pedir. Comecar antes do fim garante que o bloco que cruza
                // a emenda caiba inteiro na janela nova.
                const int other = 1 - i;
                const auto nextStart = end - windowOverlap;

                if (winValid[other] > 0 && winGen[other] == gen
                     && winStart[other] == nextStart)
                    return;

                if (pos >= s + winValid[i] / 2)
                    request (nextStart);

                return;
            }
        }

        // Pede a janela que comeca em "start". Barato e idempotente: chamado a
        // cada bloco enquanto a nota se aproxima do fim da janela atual.
        void request (juce::int64 start) noexcept
        {
            wantStart.store (start, std::memory_order_release);
        }
    };

    //==========================================================================
    SampleStreamer() : juce::Thread ("ChurchDAW stream")
    {
        formats.registerBasicFormats();
    }

    ~SampleStreamer() override
    {
        stopThread (2000);
    }

    void prepare (int numVoices)
    {
        stopThread (2000);

        streams.clear();
        streams.reserve ((size_t) numVoices);

        for (int i = 0; i < numVoices; ++i)
        {
            auto v = std::make_unique<VoiceStream>();
            v->prepare();
            streams.push_back (std::move (v));
        }

        // Prioridade ALTA, mas nao a do audio: se esta thread empatasse com a de
        // audio, atrasar a leitura viraria atrasar a saida -- que e o defeito que
        // ela existe para evitar.
        startThread (juce::Thread::Priority::high);
    }

    VoiceStream& stream (int voiceIndex) noexcept   { return *streams[(size_t) voiceIndex]; }
    int numStreams() const noexcept                 { return (int) streams.size(); }

    // Acorda a thread de leitura. Chamado no fim do bloco de audio, uma vez --
    // nao por voz.
    void wake() noexcept { notify(); }

private:
    void run() override
    {
        while (! threadShouldExit())
        {
            bool didWork = false;

            for (auto& sp : streams)
            {
                if (threadShouldExit())
                    return;

                if (fill (*sp))
                    didWork = true;
            }

            if (! didWork)
                wait (5);
        }
    }

    // Enche UMA janela pendente. Devolve true se fez algo.
    bool fill (VoiceStream& v)
    {
        const auto want = v.wantStart.load (std::memory_order_acquire);

        if (want < 0)
            return false;

        const auto* src = v.source.load (std::memory_order_acquire);

        if (src == nullptr || ! src->isValid())
        {
            v.wantStart.store (-1, std::memory_order_release);
            return false;
        }

        const int gen = v.generation.load (std::memory_order_acquire);

        // Reaproveita o leitor enquanto a fonte nao muda: abrir um FLAC custa
        // parse de cabecalho e seek, e fazer isso por janela seria pagar o preco
        // varias vezes por segundo em cada voz.
        if (v.reader == nullptr || v.readerSource != src)
        {
            v.reader.reset (formats.createReaderFor (src->file));
            v.readerSource = src;
        }

        if (v.reader == nullptr)
        {
            v.wantStart.store (-1, std::memory_order_release);
            return false;
        }

        // Escolhe o destino pela POSICAO DA NOTA, nao por alternancia cega.
        //
        // Agora que a leitura consulta as duas janelas, "a que nao e a atual"
        // deixou de ser seguro: a nota pode estar justamente na outra. Entao
        // escrevemos naquela que NAO contem a posicao tocada agora.
        const auto pos = v.playPos.load (std::memory_order_acquire);

        auto holdsPos = [&v, pos] (int i)
        {
            return v.winValid[i] > 0 && pos >= v.winStart[i] && pos < v.winStart[i] + v.winValid[i];
        };

        int idx = holdsPos (0) ? 1 : (holdsPos (1) ? 0 : (v.live.load (std::memory_order_acquire) == 0 ? 1 : 0));

        const auto remaining = src->totalSamples - want;

        if (remaining <= 0)
        {
            v.wantStart.store (-1, std::memory_order_release);
            return false;
        }

        const int toRead = (int) juce::jmin ((juce::int64) windowSamples, remaining);

        v.buf[idx].clear();
        v.reader->read (&v.buf[idx], 0, toRead, src->fileOffset + want, true, true);

        v.winStart[idx] = want;
        v.winValid[idx] = toRead;
        v.winGen[idx]   = gen;

        // A nota morreu (ou outra nasceu nesta voz) enquanto liamos: o resultado
        // e de uma nota que nao existe mais. Descarta em vez de publicar.
        if (v.generation.load (std::memory_order_acquire) != gen)
        {
            v.wantStart.store (-1, std::memory_order_release);
            return true;
        }

        v.live.store (idx, std::memory_order_release);

        // So limpa o pedido se ninguem pediu OUTRA coisa no meio tempo.
        auto expected = want;
        v.wantStart.compare_exchange_strong (expected, -1, std::memory_order_acq_rel);

        return true;
    }

    juce::AudioFormatManager formats;
    std::vector<std::unique_ptr<VoiceStream>> streams;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SampleStreamer)
};
