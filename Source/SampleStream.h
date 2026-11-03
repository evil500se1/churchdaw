#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <atomic>
#include <memory>
#include <vector>

#if JUCE_ANDROID
 #include "AndroidPerformance.h"
#endif

//==============================================================================
// LEITURA DOS SAMPLES DIRETO DO DISCO, EM VEZ DE TUDO NA MEMORIA.
//
// POR QUE ISTO EXISTE
// -------------------
// O motor carregava cada sample INTEIRO na RAM, em float de 32 bits. Isto virou
// o teto tanto no celular quanto no desktop: um Amber Upright de 2,86 GiB em
// AIFF/24-bit expande para ~3,81 GiB em float; durante a troca, antigo + novo
// passam de 7,6 GiB e ainda podem coexistir com decodes cancelados. No aparelho
// de teste com DOIS pianos abertos tambem foi medido:
//
//     RES = 3,8 GB   (1,9 GB por piano)
//
// e o motor tem CINCO canais. Cheios seriam ~9,5 GB num aparelho com 8,6 GB
// livres: nao cabe. No Windows/macOS, uma troca rapida podia terminar o processo
// por bad_alloc antes mesmo de o timbre novo ficar pronto. No Android, muito
// antes de faltar memoria o sistema ja comeca a comprimir e mover paginas -- foi
// dai que veio o audio pipocando, e NAO de falta de CPU (7-11% em repouso).
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
// graca. 32768 amostras x 2 janelas x 2 canais x 4 bytes = 512 KB por voz.
// As 256 vozes reservam ~134 MB, sem crescer com o tamanho da biblioteca.
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
    // 32768 = 0,74 s a 44,1 kHz. E uma janela longa o bastante para amortizar
    // seek/decode do FLAC, mas sem reservar os ~268 MB da versao de 65536.
    // O aparelho ficava perto de 1 GB com UM piano porque as janelas sozinhas
    // consumiam ~268 MB; esta medida devolve ~134 MB ao Android e reduz muito a
    // chance de ele comprimir/mover paginas enquanto a thread de audio toca.
    static constexpr int windowSamples = 32768;

    // As janelas precisam se SOBREPOR. Sem isso, o bloco que atravessa a emenda
    // nunca cabe inteiro nem na janela antiga nem na nova: acquire() exige o
    // bloco completo para a interpolacao Hermite e devolvia "passou", inserindo
    // um bloco de silencio exatamente a cada troca de janela. O motor reserva
    // blocos de ate 8192 samples e pede mais 4 para a Hermite; por isso 4096 nao
    // cobria nem o pior bloco 1:1. A voz aumenta esta sobreposicao dinamicamente
    // quando a transposicao percorre mais samples de origem no mesmo bloco.
    static constexpr int windowOverlap = 8192 + 4;

    // Uma janela precisa sustentar pelo menos um bloco inteiro depois que a
    // voz migra para ela, enquanto o worker reaproveita a janela anterior para
    // montar a continuacao. Com apenas dois buffers, spans maiores que metade
    // da janela podem atravessar mais de uma emenda no mesmo bloco. O renderer
    // divide esses blocos antes de chamar acquire().
    static constexpr int maxContinuousSpan = (windowSamples + 4) / 2;

    static_assert (windowOverlap > 0 && windowOverlap < windowSamples,
                   "A sobreposicao precisa caber dentro da janela");
    static_assert (maxContinuousSpan >= windowOverlap
                    && maxContinuousSpan < windowSamples,
                   "O span continuo precisa preservar progresso com duas janelas");

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
        static constexpr juce::int64 noRequest = -1;
        static constexpr juce::int64 requestInProgress = -2;

        // Reserva curta de uma janela para UM bloco de audio. O produtor usa -1
        // enquanto escreve; o audio usa +1 enquanto le. Assim nenhum dos dois
        // toca o mesmo AudioBuffer ao mesmo tempo.
        //
        // Antes winStart/winValid eram publicados sem sincronizacao e fill()
        // podia limpar uma janela que acquire() ainda estava lendo. Isso e uma
        // data race real em C++ e aparece como estalo aleatorio, especialmente
        // quando a nota cruza a emenda entre as duas janelas.
        struct ReadAccess
        {
            ReadAccess() = default;
            ReadAccess (const ReadAccess&) = delete;
            ReadAccess& operator= (const ReadAccess&) = delete;

            ~ReadAccess() { reset(); }

            void reset() noexcept
            {
                if (owner != nullptr && index >= 0)
                    owner->releaseWindow (index);

                owner = nullptr;
                index = -1;
            }

            const VoiceStream* owner = nullptr;
            int index = -1;
        };

        // A thread de audio publica apenas o endereco possuido pela Zone. O
        // shared_ptr antigo incrementava/decrementava refcount em note-on/off e
        // podia destruir Source/File dentro do callback. workerSource e o
        // hazard pointer que impede o coletor de liberar a Zone durante decode.
        std::atomic<const Source*> source { nullptr };
        std::atomic<const Source*> workerSource { nullptr };
        static_assert (std::atomic<const Source*>::is_always_lock_free,
                       "ponteiros do streamer precisam ser lock-free");

        // Seqlock de geracao: par = fonte estavel, impar = restart/stop trocando
        // source + pedido. A thread de leitura so aceita snapshots com o mesmo
        // valor par antes/depois. Sem isto ela podia copiar a Source antiga e
        // depois carimbar a janela com a geracao nova.
        std::atomic<int> generation { 0 };

        juce::AudioBuffer<float> buf[2];
        juce::int64 winStart[2] { 0, 0 };
        int winValid[2] { 0, 0 };
        int winGen[2] { -1, -1 };
        mutable std::atomic<int> windowAccess[2] { 0, 0 }; // -1 escritor, 0 livre, +1 leitor

        std::atomic<int>  live { -1 };            // indice que o audio pode ler
        // Ultima janela efetivamente escolhida pelo audio. Diferente de live,
        // que aponta para a publicacao mais recente, este valor continua valido
        // entre callbacks e impede o worker de reciclar o trecho ainda tocado.
        mutable std::atomic<int> playingWindow { -1 };
        std::atomic<juce::int64> wantStart { noRequest };
        std::atomic<bool>* ownerWakeNeeded = nullptr;

        // so a thread de leitura toca nestes
        std::unique_ptr<juce::AudioFormatReader> reader;
        const Source* readerSource = nullptr;
        int readerGeneration = -1;

        // Backoff de falha, tocado exclusivamente pela thread de leitura. O
        // pedido continua em wantStart para poder ser retentado, mas um arquivo
        // temporariamente indisponivel nao e reaberto milhares de vezes/segundo.
        const Source* retrySource = nullptr;
        juce::int64 retryStart = noRequest;
        int retryGeneration = -1;
        int retryFailures = 0;
        double retryNotBeforeMs = 0.0;

        // Somente a thread de audio toca neste valor. acquire() observa o span
        // real do bloco (ja convertido pela ratio) e prefetch() usa o maior da
        // nota, para a proxima janela conter inteira a emenda mesmo transposta.
        mutable int requiredOverlap = windowOverlap;

        // JANELAS ALOCADAS SOB DEMANDA.
        //
        // Cada VoiceStream custa 2 x 32768 x 2 canais x 4 bytes = 512 KiB. Com
        // o pool de 5 canais eram 256 vozes (128 MiB); com 14 canais o pool foi
        // para 616 vozes e isso viraria 308 MiB reservados no prepareToPlay,
        // mesmo com um unico timbre carregado. E MAIS do que os 268 MiB que
        // fizeram a versao de janela 65536 ser rejeitada por consumo no Android
        // (ver o comentario de windowSamples acima) — o app passava de 1 GB e
        // era morto pelo sistema.
        //
        // Quem realmente toca ao mesmo tempo e uma fracao do pool. Entao a
        // janela nasce vazia e so ganha memoria quando a voz vai de fato
        // transmitir do disco. Quem aloca e SEMPRE a thread de leitura
        // (garantirJanelas, chamado de fillImpl) — nunca a thread de audio.
        void prepare()
        {
            for (auto& b : buf)
                b.setSize (0, 0, false, false, false);

            playingWindow.store (-1, std::memory_order_release);
            requiredOverlap = windowOverlap;
        }

        // So a thread de leitura chama. Devolve false se a alocacao falhar,
        // para o chamador desistir da leitura em vez de tocar em buffer vazio.
        bool garantirJanelas()
        {
            for (auto& b : buf)
            {
                if (b.getNumSamples() >= windowSamples && b.getNumChannels() >= 2)
                    continue;

                try
                {
                    b.setSize (2, windowSamples, false, true, false);
                }
                catch (...)
                {
                    return false;
                }
            }

            return true;
        }

        // Chamado pela thread de AUDIO no note-on.
        void restart (const Source* s) noexcept
        {
            generation.fetch_add (1, std::memory_order_acq_rel); // impar: escrita
            live.store (-1, std::memory_order_release);
            playingWindow.store (-1, std::memory_order_release);
            wantStart.store (noRequest, std::memory_order_release);
            source.store (s, std::memory_order_release);
            requiredOverlap = windowOverlap;
            generation.fetch_add (1, std::memory_order_release); // par: snapshot pronto
        }

        void stop() noexcept
        {
            generation.fetch_add (1, std::memory_order_acq_rel); // impar: escrita
            source.store (nullptr, std::memory_order_release);
            live.store (-1, std::memory_order_release);
            playingWindow.store (-1, std::memory_order_release);
            wantStart.store (noRequest, std::memory_order_release);
            requiredOverlap = windowOverlap;
            generation.fetch_add (1, std::memory_order_release); // par: snapshot pronto
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
                      const float*& l, const float*& r, int& offsetOut,
                      ReadAccess& readAccess) const noexcept
        {
            readAccess.reset();

            if (needed <= 0 || needed > maxContinuousSpan)
            {
                lastFailReason = 3;
                return false;
            }

            // Nao diminui durante a nota: blocos de tamanho variavel nao podem
            // fazer uma continuacao ja pronta deixar de coincidir no bloco
            // seguinte. O teto de metade da janela garante que um callback nao
            // precise atravessar duas emendas; o renderer subdivide acima dele.
            requiredOverlap = juce::jlimit (
                windowOverlap, maxContinuousSpan,
                juce::jmax (requiredOverlap, needed));

            const int gen = generation.load (std::memory_order_acquire);

            if ((gen & 1) != 0)
            {
                lastFailReason = 2;
                return false;
            }

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
                int expected = 0;

                // Se o produtor esta escrevendo (-1), esta janela simplesmente
                // nao existe para este bloco. O compare_exchange com acquire
                // tambem publica winStart/winValid e os samples preenchidos antes
                // do store(release) do produtor.
                if (! windowAccess[i].compare_exchange_strong (
                        expected, 1, std::memory_order_acquire, std::memory_order_relaxed))
                    continue;

                if (winValid[i] <= 0)
                {
                    releaseWindow (i);
                    continue;
                }

                sawWindow = true;

                if (winGen[i] != gen)
                {
                    releaseWindow (i);
                    continue;
                }

                sawGen = true;

                const auto s = winStart[i];

                if (pos < s || pos + needed > s + winValid[i])
                {
                    releaseWindow (i);
                    continue;
                }

                offsetOut = (int) (pos - s);
                l = buf[i].getReadPointer (0);
                r = buf[i].getReadPointer (juce::jmin (1, buf[i].getNumChannels() - 1));
                readAccess.owner = this;
                readAccess.index = i;
                playingWindow.store (i, std::memory_order_release);
                return true;
            }

            lastFailReason = ! sawWindow ? 1 : (! sawGen ? 2 : 3);
            return false;
        }

        void releaseWindow (int index) const noexcept
        {
            if (juce::isPositiveAndBelow (index, 2))
                windowAccess[index].store (0, std::memory_order_release);
        }

        // Pede a PROXIMA janela assim que esta janela entra em uso. Esperar a
        // metade deixava apenas ~336 ms para uma rajada de dezenas de FLACs no
        // Android; acordes sustentados conseguiam chegar ao fim antes do worker.
        // O pedido imediato dobra a antecedencia sem aumentar a RAM.
        bool prefetch (juce::int64 pos, const ReadAccess& currentRead) noexcept
        {
            if (wantStart.load (std::memory_order_acquire) != noRequest)
                return false;   // ja ha um pedido em andamento

            const int gen = generation.load (std::memory_order_acquire);

            if ((gen & 1) != 0)
                return false;

            // Pede a continuacao da janela que esta TOCANDO agora -- nao da
            // "atual" segundo o ponteiro live, que depois da correcao acima pode
            // muito bem ser a outra.
            const int i = currentRead.owner == this ? currentRead.index : -1;

            if (! juce::isPositiveAndBelow (i, 2)
                 || winValid[i] <= 0 || winGen[i] != gen)
                return false;

            {
                const auto s   = winStart[i];
                const auto end = s + winValid[i];

                if (pos < s || pos >= end)
                    return false;

                // A ultima janela pode ser menor que windowSamples; nesse caso
                // ela ja termina junto com o arquivo e nao existe continuacao.
                if (winValid[i] < windowSamples)
                    return false;

                // Ja existe a continuacao SOBREPOSTA carregada? Entao nao ha o
                // que pedir. Comecar antes do fim garante que o bloco que cruza
                // a emenda caiba inteiro na janela nova.
                const int other = 1 - i;
                const auto nextStart = end - requiredOverlap;
                bool continuationReady = false;
                int expected = 0;

                // Metadados da OUTRA janela tambem so podem ser lidos depois de
                // reserva-la. Sem isso prefetch() ainda corria contra fill(),
                // mesmo depois de acquire() ter sido protegido.
                if (windowAccess[other].compare_exchange_strong (
                        expected, 1, std::memory_order_acquire, std::memory_order_relaxed))
                {
                    continuationReady = winValid[other] > 0 && winGen[other] == gen
                                         && winStart[other] == nextStart;
                    releaseWindow (other);
                }
                else if (expected == -1)
                {
                    return false;   // o produtor ja esta preenchendo a continuacao
                }

                if (continuationReady)
                    return false;

                return request (nextStart);
            }
        }

        // Pede a janela que comeca em "start". Barato e idempotente: chamado a
        // cada bloco enquanto a nota se aproxima do fim da janela atual.
        //
        // Devolve TRUE se o pedido entrou de fato. Quem so pede uma vez por
        // nota precisa dessa resposta: se marcar "ja pedi" sem o pedido ter
        // entrado, uma falha passageira da leitura (arquivo que nao abriu,
        // memoria que faltou) deixaria de ser retentada e viraria um clique
        // audivel no fim do preload.
        bool request (juce::int64 start) noexcept
        {
            // Nunca sobrescreve um decode em andamento. O store antigo podia
            // trocar wantStart enquanto o worker lia; ao terminar, ele ja nao
            // conseguia limpar o pedido e fazia uma leitura extra/desalinhada.
            juce::int64 expected = noRequest;
            if (! wantStart.compare_exchange_strong (expected, start,
                                                     std::memory_order_acq_rel,
                                                     std::memory_order_relaxed))
                return false;

            if (ownerWakeNeeded != nullptr)
                ownerWakeNeeded->store (true, std::memory_order_release);
            return true;
        }

        // Primeira janela sobrepoe o fim do preload pelo pior span que este
        // callback pode consumir. Usar a constante antiga aqui deixava um
        // buraco quando transposicao x bloco percorria mais de 8196 amostras.
        bool requestFirstWindow (juce::int64 preloadEnd, int needed) noexcept
        {
            if (preloadEnd <= 0 || needed <= 0 || needed > maxContinuousSpan)
                return false;

            requiredOverlap = juce::jlimit (
                windowOverlap, maxContinuousSpan,
                juce::jmax (requiredOverlap, needed));

            return request (juce::jmax (
                (juce::int64) 0,
                preloadEnd - (juce::int64) requiredOverlap));
        }
    };

    //==========================================================================
    SampleStreamer() : juce::Thread ("ChurchDAW stream")
    {
        formats.registerBasicFormats();
    }

    ~SampleStreamer() override
    {
        // Nunca force-kill a thread no meio de um decoder/lock de filesystem.
        // Um arquivo lento pode atrasar o fechamento, mas corromper heap/locks
        // do processo e pior. O loop checa threadShouldExit entre leituras.
        stopThread (-1);
    }

    // O dono chama isto antes de destruir Zones. Assim nenhum decoder conserva
    // um hazard para Source durante a ordem reversa de destruicao dos membros.
    void shutdown() noexcept
    {
        stopThread (-1);

        for (auto& stream : streams)
            stream->workerSource.store (nullptr, std::memory_order_release);
    }

    bool referencesSource (const Source* sourceToFind) const noexcept
    {
        if (sourceToFind == nullptr)
            return false;

        for (const auto& stream : streams)
            if (stream->source.load (std::memory_order_acquire) == sourceToFind
                 || stream->workerSource.load (std::memory_order_acquire)
                        == sourceToFind)
                return true;

        return false;
    }

    void prepare (int numVoices)
    {
        stopThread (-1);

        streams.clear();
        streams.reserve ((size_t) numVoices);

        for (int i = 0; i < numVoices; ++i)
        {
            auto v = std::make_unique<VoiceStream>();
            v->ownerWakeNeeded = &wakeNeeded;
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

    // Marca que ha trabalho urgente para a thread de leitura.
    void signalWorkNeeded() noexcept { wakeNeeded.store (true, std::memory_order_release); }

    // Acorda a thread de leitura. Chamado no fim do bloco de audio, uma vez --
    // nao por voz. Evita syscalls no kernel se nao houver janelas requisitadas.
    void wake() noexcept
    {
        if (wakeNeeded.exchange (false, std::memory_order_acq_rel))
            notify();
    }

   #if CHURCHDAW_INTERNAL_AUDIO_TEST_HOOKS
    juce::uint64 getWorkerPassCountForTest() const noexcept { return workerPassCount.load(); }
   #endif
private:
   #if CHURCHDAW_INTERNAL_AUDIO_TEST_HOOKS
    std::atomic<juce::uint64> workerPassCount { 0 };
   #endif
    std::atomic<bool> wakeNeeded { true };

    void run() override
    {
       #if JUCE_ANDROID
        // O decoder FLAC termina cada janela mais cedo nos nucleos grandes,
        // reduzindo a chance de faltar dados sem elevar sua prioridade acima
        // da thread de audio. Falha de afinidade apenas mantem o padrao.
        churchdaw::androidPerformance::prepareFastCoreMask();
        churchdaw::androidPerformance::pinCurrentThreadToFastCores();
       #endif

        while (! threadShouldExit())
        {
           #if CHURCHDAW_INTERNAL_AUDIO_TEST_HOOKS
            workerPassCount.fetch_add (1, std::memory_order_relaxed);
           #endif
            bool didWork = false;
            bool hasActiveStream = false;

            for (auto& sp : streams)
            {
                if (threadShouldExit())
                    return;

                if (fill (*sp))
                    didWork = true;
                hasActiveStream = hasActiveStream
                    || sp->source.load (std::memory_order_acquire) != nullptr;
            }

            // First-window requests explicitly notify the worker. Keep the
            // 2 ms fallback during active streaming/prefetch, but avoid 500
            // complete voice scans per second when every voice is stopped.
            if (! didWork)
                wait (hasActiveStream ? 2 : 100);
        }
    }

    // Enche UMA janela pendente. O wrapper noexcept e a reserva explicita
    // garantem que um decoder/alloc que lance excecao nao encerre para sempre a
    // thread de streaming nem deixe windowAccess preso em -1.
    static void releaseReservedWindow (VoiceStream& v, int& reservedWindow,
                                       bool invalidate) noexcept
    {
        const int index = reservedWindow;

        if (! juce::isPositiveAndBelow (index, 2))
            return;

        if (invalidate)
        {
            v.winStart[index] = 0;
            v.winValid[index] = 0;
            v.winGen[index] = -1;
        }

        // Depois deste ponto o audio pode reservar a janela; nenhum acesso aos
        // metadados/buffer dela e permitido.
        reservedWindow = -1;
        v.windowAccess[index].store (0, std::memory_order_release);
    }

    static void discardReader (VoiceStream& v) noexcept
    {
        v.reader.reset();
        v.readerSource = nullptr;
        v.readerGeneration = -1;
    }

    static void clearRetry (VoiceStream& v) noexcept
    {
        v.retrySource = nullptr;
        v.retryStart = VoiceStream::noRequest;
        v.retryGeneration = -1;
        v.retryFailures = 0;
        v.retryNotBeforeMs = 0.0;
    }

    static bool retryIsDeferred (VoiceStream& v, int generation,
                                 const Source* source,
                                 juce::int64 start) noexcept
    {
        if (v.retryGeneration != generation
             || v.retrySource != source
             || v.retryStart != start)
        {
            clearRetry (v);
            return false;
        }

        return juce::Time::getMillisecondCounterHiRes()
                 < v.retryNotBeforeMs;
    }

    static void finishRequest (VoiceStream& v,
                               juce::int64& claimedStart) noexcept
    {
        auto expected = VoiceStream::requestInProgress;
        v.wantStart.compare_exchange_strong (
            expected, VoiceStream::noRequest, std::memory_order_acq_rel);
        claimedStart = VoiceStream::noRequest;
    }

    static void restoreRequest (VoiceStream& v, juce::int64 start,
                                juce::int64& claimedStart) noexcept
    {
        auto expected = VoiceStream::requestInProgress;
        v.wantStart.compare_exchange_strong (
            expected, start, std::memory_order_acq_rel);
        claimedStart = VoiceStream::noRequest;
    }

    // Rearma somente o pedido que ESTA funcao reclamou. Se restart()/stop() ja
    // publicou outra geracao ou o audio conseguiu colocar trabalho novo, o CAS
    // falha e nada e apagado. O backoff exponencial limita uma falha persistente
    // a quatro tentativas por segundo, sem atrasar a primeira repeticao curta.
    static void rearmRequestForRetry (
        VoiceStream& v, int generation,
        const Source* source,
        juce::int64 start, juce::int64& claimedStart) noexcept
    {
        const bool sameRequest =
            v.retryGeneration == generation
            && v.retrySource == source
            && v.retryStart == start;

        const bool stillCurrent =
            generation >= 0 && (generation & 1) == 0
            && start >= 0 && source != nullptr
            && v.generation.load (std::memory_order_acquire) == generation
            && v.source.load (std::memory_order_acquire) == source;

        auto expected = VoiceStream::requestInProgress;
        const bool restored = stillCurrent
            && v.wantStart.compare_exchange_strong (
                   expected, start, std::memory_order_acq_rel,
                   std::memory_order_relaxed);

        claimedStart = VoiceStream::noRequest;

        if (! restored)
        {
            if (! sameRequest)
                clearRetry (v);

            return;
        }

        v.retryGeneration = generation;
        v.retrySource = source;
        v.retryStart = start;
        v.retryFailures = sameRequest ? juce::jmin (v.retryFailures + 1, 8) : 1;

        const int delayMs = juce::jmin (
            250, 2 << juce::jmin (v.retryFailures - 1, 7));
        v.retryNotBeforeMs =
            juce::Time::getMillisecondCounterHiRes() + (double) delayMs;
    }

    bool fill (VoiceStream& v) noexcept
    {
        int reservedWindow = -1;
        int claimedGeneration = -1;
        juce::int64 claimedStart = VoiceStream::noRequest;
        const Source* claimedSource = nullptr;

        try
        {
            return fillImpl (v, reservedWindow, claimedGeneration,
                             claimedStart, claimedSource);
        }
        catch (...)
        {
            releaseReservedWindow (v, reservedWindow, true);
            discardReader (v);
            rearmRequestForRetry (v, claimedGeneration, claimedSource,
                                  claimedStart, claimedStart);
            return false;
        }
    }

    bool fillImpl (VoiceStream& v, int& reservedWindow,
                   int& claimedGeneration, juce::int64& claimedStart,
                   const Source*& claimedSource)
    {
        // Snapshot coerente de {geracao, Source, pedido}. restart()/stop() deixam
        // generation impar durante a troca; qualquer mistura e descartada antes
        // de abrir o decoder.
        const int gen = v.generation.load (std::memory_order_acquire);

        if ((gen & 1) != 0)
            return false;

        const auto src = v.source.load (std::memory_order_acquire);

        // Hazard protocol: publique antes de confirmar o snapshot. Se source
        // mudou nesse intervalo, src pode ser apenas um valor antigo e nao sera
        // desreferenciado. Se nao mudou, o coletor ve workerSource e conserva a
        // Zone ate esta funcao terminar.
        v.workerSource.store (src, std::memory_order_release);
        const juce::ScopeGuard clearWorkerSource { [&v]
        {
            v.workerSource.store (nullptr, std::memory_order_release);
        } };
        const auto want = v.wantStart.load (std::memory_order_acquire);
        const int confirmedGen = v.generation.load (std::memory_order_acquire);

        if (confirmedGen != gen || (confirmedGen & 1) != 0
             || v.source.load (std::memory_order_acquire) != src)
            return true;

        if (want < 0)
        {
            if (want == VoiceStream::noRequest)
                clearRetry (v);

            return false;
        }

        if (retryIsDeferred (v, gen, src, want))
            return false;

        // Reclama o pedido antes do decode. restart() pode substituir -2 por -1
        // e publicar um pedido novo; por isso toda liberacao abaixo usa CAS e
        // jamais apaga cegamente trabalho pertencente a outra nota.
        auto expectedWant = want;

        if (! v.wantStart.compare_exchange_strong (
                expectedWant, VoiceStream::requestInProgress,
                std::memory_order_acq_rel, std::memory_order_relaxed))
            return false;

        claimedGeneration = gen;
        claimedStart = want;
        claimedSource = src;

        // O hazard segura a vida da fonte ate o fim. Esta segunda validacao
        // fecha restart() entre o snapshot e o claim do pedido.
        if (v.generation.load (std::memory_order_acquire) != gen
             || v.source.load (std::memory_order_acquire) != src)
        {
            finishRequest (v, claimedStart);
            return true;
        }

        if (src == nullptr || ! src->isValid())
        {
            finishRequest (v, claimedStart);
            return false;
        }

        // Reaproveita o leitor enquanto a fonte nao muda: abrir um FLAC custa
        // parse de cabecalho e seek, e fazer isso por janela seria pagar o preco
        // varias vezes por segundo em cada voz.
        if (v.reader == nullptr || v.readerSource != src || v.readerGeneration != gen)
        {
            discardReader (v);
            v.reader.reset (formats.createReaderFor (src->file));

            if (v.reader != nullptr)
            {
                v.readerSource = src;
                v.readerGeneration = gen;
            }
        }

        if (v.reader == nullptr)
        {
            rearmRequestForRetry (v, gen, src, want, claimedStart);
            return false;
        }

        // As janelas desta voz podem nunca ter sido alocadas (ver prepare()).
        // Este e o unico lugar onde elas ganham memoria, e estamos na thread
        // de leitura — a de audio so le janela ja publicada.
        if (! v.garantirJanelas())
        {
            rearmRequestForRetry (v, gen, src, want, claimedStart);
            return false;
        }

        // Escolhe o destino pela POSICAO DA NOTA, nao por alternancia cega.
        const int published = v.live.load (std::memory_order_acquire);
        const int preferred = published == 0 ? 1 : 0;

        // Reserva primeiro a janela que nao foi publicada por ultimo. O CAS
        // fecha a corrida entre "parece livre" e o audio comecar a le-la.
        for (const int candidate : { preferred, 1 - preferred })
        {
            int expected = 0;

            if (v.windowAccess[candidate].compare_exchange_strong (
                    expected, -1, std::memory_order_acquire, std::memory_order_relaxed))
            {
                // windowAccess protege apenas durante a leitura fisica deste
                // callback. playingWindow preserva a escolha entre callbacks:
                // sem esta segunda checagem o worker podia sobrescrever A logo
                // depois de seu ReadAccess ser solto, embora a proxima posicao
                // da voz ainda dependesse de A.
                if (v.playingWindow.load (std::memory_order_acquire) == candidate)
                {
                    v.windowAccess[candidate].store (0, std::memory_order_release);
                    continue;
                }

                reservedWindow = candidate;
                break;
            }
        }

        // O audio esta usando as duas janelas. Devolve o pedido reclamado para
        // a fila; em no maximo um callback uma janela sera liberada.
        if (reservedWindow < 0)
        {
            restoreRequest (v, want, claimedStart);
            return false;
        }

        const auto remaining = src->totalSamples - want;

        if (remaining <= 0)
        {
            releaseReservedWindow (v, reservedWindow, true);
            clearRetry (v);
            finishRequest (v, claimedStart);
            return false;
        }

        const int toRead = (int) juce::jmin ((juce::int64) windowSamples, remaining);

        // AudioFormatReader considera leitura alem do EOF um sucesso e preenche
        // com zeros. Source normalmente vem do mesmo reader, mas arquivo
        // truncado/substituido entre carga e playback nao pode virar uma janela
        // silenciosa marcada como valida.
        const auto readerLength = v.reader->lengthInSamples;
        const bool rangeFitsReader =
            src->fileOffset >= 0 && readerLength >= 0
            && src->fileOffset <= readerLength
            && want <= readerLength - src->fileOffset
            && (juce::int64) toRead
                   <= readerLength - src->fileOffset - want;

        if (! rangeFitsReader)
        {
            releaseReservedWindow (v, reservedWindow, true);
            discardReader (v);
            rearmRequestForRetry (v, gen, src, want, claimedStart);
            return false;
        }

        v.buf[reservedWindow].clear();
        const bool readOk = v.reader->read (
            &v.buf[reservedWindow], 0, toRead,
            src->fileOffset + want, true, true);

        if (! readOk)
        {
            // read() pode deixar dados parciais no buffer. A janela fica
            // explicitamente invalida antes de liberar a reserva, o decoder e
            // descartado, e exatamente este pedido volta para retry.
            releaseReservedWindow (v, reservedWindow, true);
            discardReader (v);
            rearmRequestForRetry (v, gen, src, want, claimedStart);
            return false;
        }

        // Revalida TODOS os membros do snapshot enquanto a janela ainda esta
        // exclusivamente reservada. Resultado velho fica explicitamente
        // invalido; nunca recebe o carimbo da nota nova.
        const auto currentSource =
            v.source.load (std::memory_order_acquire);
        const bool stillCurrent =
            v.generation.load (std::memory_order_acquire) == gen
            && currentSource == src
            && v.wantStart.load (std::memory_order_acquire)
                   == VoiceStream::requestInProgress;

        if (! stillCurrent)
        {
            releaseReservedWindow (v, reservedWindow, true);
            finishRequest (v, claimedStart);
            return true;
        }

        const int completedWindow = reservedWindow;
        v.winStart[completedWindow] = want;
        v.winValid[completedWindow] = toRead;
        v.winGen[completedWindow] = gen;

        // Publica metadados + amostras. acquire() so le depois de trocar 0 por
        // +1 com memoria acquire, portanto ve um quadro completo e coerente.
        releaseReservedWindow (v, reservedWindow, false);
        v.live.store (completedWindow, std::memory_order_release);
        clearRetry (v);
        finishRequest (v, claimedStart);
        return true;
    }

    juce::AudioFormatManager formats;
    std::vector<std::unique_ptr<VoiceStream>> streams;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SampleStreamer)
};
