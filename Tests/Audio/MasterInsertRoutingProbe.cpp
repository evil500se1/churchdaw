// =============================================================================
// SONDA DE ROTEAMENTO DO "PLUGIN NO MASTER" -- ChurchDAWMasterInsertRoutingProbe
// =============================================================================
//
// POR QUE ISTO EXISTE
// -------------------
// Queixa do cliente, verbatim: "quando coloca plugin no master exemplo de
// reverb nao ta chegando nos canais".
//
// O MASTER FX do Church Live tem DOIS caminhos de audio completamente
// diferentes para o mesmo slot de insert (TimbresProcessor.h, processBlock):
//
//   INSERT  ins->processBlock (buffer)     <- o mix dos canais passa dentro
//   ENVIO   ins->processBlock (reverbBus)  <- so a DOSE do knob REVERB de
//                                             cada canal chega ali
//
// ANTES DA CORRECAO quem decidia era o NOME do plugin: qualquer coisa com
// "bigsky" caia no ramo de ENVIO. Como o knob REVERB de canal nasce em 0.0,
// o plugin recebia SILENCIO ABSOLUTO -- o sintoma relatado.
//
// DEPOIS DA CORRECAO a escolha e explicita por slot (insertRouteInsert /
// insertRouteSend), um slot novo nasce em INSERT, e o roteamento pelo nome
// sobrevive apenas como insertRouteLegacy, usado somente ao restaurar um
// estado salvo por versao antiga -- para nenhum culto ja ensaiado mudar de som.
//
// Esta sonda nao le codigo: ela instala inserts e instrumentos de verdade
// (derivados de juce::AudioPluginInstance), toca notas pelo motor real e mede
// (a) quanto sinal ENTROU no plugin e (b) o que saiu no master.
//
// SAIDA: uma tabela, os vereditos e MASTER_INSERT_ROUTING_PROBE_OK / _FAIL.
// O cenario A imprime tambem um HASH do render sem nenhum insert: e ele que
// prova, comparando dois builds, que a correcao NAO mudou uma amostra sequer
// quando nao ha plugin no master.
// =============================================================================

#include "../../Source/TimbresProcessor.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

namespace
{

//==============================================================================
// Insert de teste. Modela um efeito HONESTO: silencio na entrada = silencio na
// saida (e o que qualquer reverb real faz). Assim, se o plugin nao receber o
// som dos canais, o master fica identico ao de sempre -- que e justamente o
// sintoma relatado.
class MedidorDeInsert final : public juce::AudioPluginInstance
{
public:
    MedidorDeInsert (juce::String nomeParaReportar, float ganho)
        : AudioPluginInstance (
              BusesProperties()
                  .withInput ("Entrada", juce::AudioChannelSet::stereo(), true)
                  .withOutput ("Saida", juce::AudioChannelSet::stereo(), true)),
          nome (std::move (nomeParaReportar)),
          ganhoAplicado (ganho)
    {
    }

    const juce::String getName() const override        { return nome; }

    void prepareToPlay (double, int) override          { ++prepares; }
    void releaseResources() override                   {}

    void processBlock (juce::AudioBuffer<float>& buffer,
                       juce::MidiBuffer&) override
    {
        ++blocos;

        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            picoDeEntrada = juce::jmax (
                picoDeEntrada,
                buffer.getMagnitude (ch, 0, buffer.getNumSamples()));

        buffer.applyGain (ganhoAplicado);
    }

    double getTailLengthSeconds() const override       { return 0.0; }
    bool acceptsMidi() const override                  { return false; }
    bool producesMidi() const override                 { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override                    { return false; }
    int getNumPrograms() override                      { return 1; }
    int getCurrentProgram() override                   { return 0; }
    void setCurrentProgram (int) override              {}
    const juce::String getProgramName (int) override   { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}

    void fillInPluginDescription (juce::PluginDescription& d) const override
    {
        d.name = nome;
        d.pluginFormatName = "Teste";
        d.fileOrIdentifier = nome;
        d.isInstrument = false;
        d.numInputChannels = 2;
        d.numOutputChannels = 2;
    }

    float picoDeEntrada = 0.0f;
    int blocos = 0;
    int prepares = 0;

private:
    juce::String nome;
    float ganhoAplicado = 1.0f;
};

//==============================================================================
// VSTi de teste hospedado NUM CANAL. Existe para responder a pergunta "e se o
// cliente testou com um plugin de instrumento no canal em vez de um timbre?".
// Emite uma senoide enquanto houver nota pressionada.
class InstrumentoDeTeste final : public juce::AudioPluginInstance
{
public:
    InstrumentoDeTeste()
        : AudioPluginInstance (
              BusesProperties()
                  .withOutput ("Saida", juce::AudioChannelSet::stereo(), true))
    {
    }

    const juce::String getName() const override        { return "Gerador de Teste"; }

    void prepareToPlay (double sr, int) override
    {
        taxa = sr > 0.0 ? sr : 48000.0;
        fase = 0.0;
        tocando = false;
    }

    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& buffer,
                       juce::MidiBuffer& midi) override
    {
        for (const auto meta : midi)
        {
            const auto m = meta.getMessage();

            if (m.isNoteOn())       tocando = true;
            else if (m.isNoteOff()) tocando = false;
        }

        buffer.clear();

        if (! tocando)
            return;

        const double passo = 2.0 * juce::MathConstants<double>::pi * 220.0 / taxa;

        for (int s = 0; s < buffer.getNumSamples(); ++s)
        {
            const float v = 0.5f * (float) std::sin (fase);
            fase += passo;

            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                buffer.setSample (ch, s, v);
        }
    }

    double getTailLengthSeconds() const override       { return 0.0; }
    bool acceptsMidi() const override                  { return true; }
    bool producesMidi() const override                 { return false; }
    bool isMidiEffect() const override                 { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override                    { return false; }
    int getNumPrograms() override                      { return 1; }
    int getCurrentProgram() override                   { return 0; }
    void setCurrentProgram (int) override              {}
    const juce::String getProgramName (int) override   { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}

    void fillInPluginDescription (juce::PluginDescription& d) const override
    {
        d.name = "Gerador de Teste";
        d.pluginFormatName = "Teste";
        d.fileOrIdentifier = "Gerador de Teste";
        d.isInstrument = true;
        d.numInputChannels = 0;
        d.numOutputChannels = 2;
    }

private:
    double taxa = 48000.0;
    double fase = 0.0;
    bool tocando = false;
};

//==============================================================================
struct Cenario
{
    juce::String rotulo;
    juce::String nomeDoPlugin;        // vazio = sem insert
    float ganhoDoPlugin = 1.0f;
    float reverbSendDoCanal = 0.0f;
    int rota = TimbresProcessor::insertRouteInsert;
    bool usarVstiNoCanal = false;
};

struct Resultado
{
    juce::String rotulo;
    float picoDoMaster = 0.0f;
    double rmsDoMaster = 0.0;
    float picoQueEntrouNoPlugin = 0.0f;
    int blocosDoPlugin = 0;
    bool ehEnvio = false;
    int slotDeReverb = -1;
    juce::uint64 hashDoRender = 0;
    bool valido = false;
};

bool esperarCarga (TimbresProcessor& p, int segundos)
{
    const auto limite = juce::Time::getMillisecondCounter()
                      + (juce::uint32) (segundos * 1000);

    while (p.isLoading() && juce::Time::getMillisecondCounter() < limite)
        juce::MessageManager::getInstance()->runDispatchLoopUntil (10);

    juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
    return ! p.isLoading();
}

void ajustar (TimbresProcessor& p, const juce::String& id, float normalizado)
{
    for (auto* par : p.getParameters())
        if (auto* comId = dynamic_cast<juce::AudioProcessorParameterWithID*> (par))
            if (comId->paramID == id)
            {
                comId->setValueNotifyingHost (normalizado);
                return;
            }
}

// FNV-1a sobre os BITS das amostras. Serve para comparar dois builds: qualquer
// diferenca de uma unica amostra muda o hash.
void misturarHash (juce::uint64& h, float amostra) noexcept
{
    juce::uint32 bits = 0;
    std::memcpy (&bits, &amostra, sizeof (bits));

    for (int b = 0; b < 4; ++b)
    {
        h ^= (juce::uint64) ((bits >> (8 * b)) & 0xffu);
        h *= 0x100000001b3ull;
    }
}

//==============================================================================
// Um cenario completo: motor novo, timbre (ou VSTi) carregado, insert instalado
// (ou nao), nota tocada, medida tirada.
Resultado rodar (const TimbresProcessor::TimbreEntry& entry,
                 const Cenario& c,
                 double sampleRate,
                 int blockSize,
                 double segundos)
{
    Resultado r;
    r.rotulo = c.rotulo;

    TimbresProcessor proc;
    proc.setPlayConfigDetails (0, 2, sampleRate, blockSize);
    proc.prepareToPlay (sampleRate, blockSize);

    if (c.usarVstiNoCanal)
    {
        if (! proc.installInstrumentForTest (
                0, std::make_unique<InstrumentoDeTeste>()))
        {
            std::cerr << "MASTER_INSERT_ROUTING_PROBE_FAIL: "
                         "installInstrumentForTest recusou\n";
            return r;
        }
    }
    else
    {
        proc.loadTimbreIntoLayer (0, entry, false);

        if (! esperarCarga (proc, 300))
        {
            std::cerr << "MASTER_INSERT_ROUTING_PROBE_FAIL: timbre nao carregou\n";
            return r;
        }
    }

    ajustar (proc, "rvb0", c.reverbSendDoCanal);

    MedidorDeInsert* medidor = nullptr;

    if (c.nomeDoPlugin.isNotEmpty())
    {
        // A rota vale ANTES de o plugin entrar, exatamente como no restore de
        // estado: e assim que insertRouteLegacy consegue reproduzir o que um
        // setup salvo por versao antiga ouvia.
        proc.setInsertRoute (0, c.rota);

        auto instancia = std::make_unique<MedidorDeInsert> (c.nomeDoPlugin,
                                                            c.ganhoDoPlugin);
        medidor = instancia.get();

        if (! proc.installInsertForTest (0, std::move (instancia)))
        {
            std::cerr << "MASTER_INSERT_ROUTING_PROBE_FAIL: installInsertForTest recusou\n";
            return r;
        }
    }

    r.slotDeReverb = proc.getReverbSlot();
    r.ehEnvio = proc.isUsingExternalReverb() && r.slotDeReverb == 0;

    const juce::int64 total = (juce::int64) (segundos * sampleRate);
    juce::AudioBuffer<float> bloco (2, blockSize);
    juce::MidiBuffer midi;

    double soma = 0.0;
    juce::int64 contadas = 0;
    juce::int64 pos = 0;
    bool notaDisparada = false;
    r.hashDoRender = 0xcbf29ce484222325ull;

    while (pos < total)
    {
        const int n = (int) juce::jmin ((juce::int64) blockSize, total - pos);
        bloco.setSize (2, n, false, false, true);
        bloco.clear();
        midi.clear();

        if (! notaDisparada)
        {
            midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
            notaDisparada = true;
        }
        else if (pos <= (juce::int64) (sampleRate * 1.0)
                  && pos + n > (juce::int64) (sampleRate * 1.0))
        {
            midi.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
        }

        proc.processBlock (bloco, midi);

        for (int ch = 0; ch < 2; ++ch)
        {
            r.picoDoMaster = juce::jmax (r.picoDoMaster,
                                         bloco.getMagnitude (ch, 0, n));
            const auto* d = bloco.getReadPointer (ch);

            for (int s = 0; s < n; ++s)
            {
                soma += (double) d[s] * d[s];
                misturarHash (r.hashDoRender, d[s]);
            }
        }

        contadas += 2 * n;
        pos += n;
        juce::MessageManager::getInstance()->runDispatchLoopUntil (0);
    }

    r.rmsDoMaster = contadas > 0 ? std::sqrt (soma / (double) contadas) : 0.0;

    if (medidor != nullptr)
    {
        r.picoQueEntrouNoPlugin = medidor->picoDeEntrada;
        r.blocosDoPlugin = medidor->blocos;
    }

    r.valido = true;
    return r;
}

juce::String db (double v)
{
    return v > 0.0 ? juce::String (juce::Decibels::gainToDecibels (v, -200.0), 2)
                   : juce::String ("-inf");
}

//==============================================================================
// Persistencia da escolha de caminho. Prova que:
//   - uma escolha explicita e gravada no XML do projeto;
//   - um slot ainda em LEGADO sai SEM o atributo, e portanto continua sendo
//     resolvido pela heuristica antiga na proxima abertura (compatibilidade).
bool checarPersistenciaDaRota (double sampleRate, int blockSize)
{
    auto rotaGravada = [&] (int rota, juce::String& saida) -> bool
    {
        TimbresProcessor proc;
        proc.setPlayConfigDetails (0, 2, sampleRate, blockSize);
        proc.prepareToPlay (sampleRate, blockSize);
        proc.setInsertRoute (0, rota);

        if (! proc.installInsertForTest (
                0, std::make_unique<MedidorDeInsert> ("Strymon BigSky", 1.0f)))
            return false;

        auto xml = proc.getStateXmlForTest();

        if (xml == nullptr)
            return false;

        for (const auto* filho : xml->getChildIterator())
            if (filho->hasTagName ("INSERT")
                 && filho->getIntAttribute ("slot", -1) == 0)
            {
                saida = filho->hasAttribute ("route")
                          ? filho->getStringAttribute ("route")
                          : juce::String ("<ausente>");
                return true;
            }

        return false;
    };

    juce::String comoInsert, comoEnvio, comoLegado;
    const bool leu =
        rotaGravada (TimbresProcessor::insertRouteInsert, comoInsert)
        && rotaGravada (TimbresProcessor::insertRouteSend, comoEnvio)
        && rotaGravada (TimbresProcessor::insertRouteLegacy, comoLegado);

    std::cout << "\nPERSISTENCIA DA ROTA (atributo route do elemento INSERT)\n";

    if (! leu)
    {
        std::cout << "  !! nao consegui ler o estado salvo\n";
        return false;
    }

    std::cout << "  escolha INSERT gravada como .................. "
              << comoInsert << '\n'
              << "  escolha ENVIO  gravada como .................. "
              << comoEnvio << '\n'
              << "  slot ainda LEGADO gravado como .............. "
              << comoLegado
              << "  (tem de ficar ausente: setup antigo nao muda)\n";

    return comoInsert == "insert"
        && comoEnvio == "send"
        && comoLegado == "<ausente>";
}

}   // namespace

//==============================================================================
int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    juce::String alvo = "EP Ballad";
    const double sampleRate = 48000.0;
    const int blockSize = 256;
    const double segundos = 3.0;

    for (int i = 1; i < argc; ++i)
    {
        const juce::String a (juce::String::fromUTF8 (argv[i]));

        if (a == "--timbre" && i + 1 < argc)
            alvo = juce::String::fromUTF8 (argv[++i]);
    }

    TimbresProcessor::TimbreEntry entry;
    bool achou = false;

    {
        TimbresProcessor scanner;

        for (const auto& e : scanner.getEntries())
            if (e.label.containsIgnoreCase (alvo))
            {
                entry = e;
                achou = true;
                break;
            }
    }

    if (! achou)
    {
        std::cerr << "MASTER_INSERT_ROUTING_PROBE_FAIL: timbre \""
                  << alvo << "\" nao encontrado\n";
        return 1;
    }

    std::cout << "TIMBRE      " << entry.label
              << "   arquivos " << entry.files.size() << "\n\n";

    const Cenario cenarios[] =
    {
        // A) referencia: nenhum plugin no master. O hash deste render e o que
        //    compara build antigo x build corrigido amostra a amostra.
        { "A) sem insert", {}, 1.0f, 0.0f,
          TimbresProcessor::insertRouteInsert, false },

        // B) plugin comum: ja funcionava antes, tem de continuar funcionando.
        { "B) \"Valhalla Room\" g=0.5 (padrao)", "Valhalla Room", 0.5f, 0.0f,
          TimbresProcessor::insertRouteInsert, false },

        // C) O CENARIO DO CLIENTE. Plugin com "bigsky" no nome, knob REVERB do
        //    canal no padrao de fabrica (0.0). ANTES: caia no ENVIO e recebia
        //    silencio. DEPOIS: slot novo nasce INSERT e recebe o mix inteiro.
        { "C) \"Strymon BigSky\" g=0.5 rvb0=0", "Strymon BigSky", 0.5f, 0.0f,
          TimbresProcessor::insertRouteInsert, false },

        // D) mesmo plugin com o knob REVERB do canal aberto: em INSERT o envio
        //    do canal alimenta o reverb NATIVO, e o plugin segue vendo o mix.
        { "D) \"Strymon BigSky\" g=0.5 rvb0=1", "Strymon BigSky", 0.5f, 1.0f,
          TimbresProcessor::insertRouteInsert, false },

        // E) COMPATIBILIDADE. Setup salvo por versao antiga (sem o atributo
        //    "route") -> insertRouteLegacy -> continua caindo no ENVIO pelo
        //    nome, exatamente como o musico ja ouvia. NAO pode mudar.
        { "E) LEGADO \"Strymon BigSky\" rvb0=0", "Strymon BigSky", 0.5f, 0.0f,
          TimbresProcessor::insertRouteLegacy, false },

        // F) ENVIO escolhido pelo musico num plugin que NAO se chama BigSky.
        //    Antes da correcao isto era impossivel: a marca decidia por ele.
        { "F) ENVIO \"Valhalla Room\" rvb0=1", "Valhalla Room", 0.5f, 1.0f,
          TimbresProcessor::insertRouteSend, false },

        // G) FONTE ALTERNATIVA: VSTi hospedado no canal 1 (nao um timbre de
        //    samples). O cliente pode ter testado assim.
        { "G) VSTi no canal + \"Valhalla Room\"", "Valhalla Room", 0.5f, 0.0f,
          TimbresProcessor::insertRouteInsert, true },

        // H) A MESMA fonte VSTi com um plugin chamado BigSky: era o pior caso
        //    (VSTi + nome de marca = silencio garantido no plugin).
        { "H) VSTi no canal + \"Strymon BigSky\"", "Strymon BigSky", 0.5f, 0.0f,
          TimbresProcessor::insertRouteInsert, true },
    };

    std::vector<Resultado> resultados;

    for (const auto& c : cenarios)
        resultados.push_back (rodar (entry, c, sampleRate, blockSize, segundos));

    std::cout << "cenario                                        "
                 "picoMaster   rmsMaster    entrouNoPlugin  blocos  ramo\n";

    for (const auto& r : resultados)
        std::cout << r.rotulo.paddedRight (' ', 47)
                  << juce::String (db (r.picoDoMaster)).paddedRight (' ', 13)
                  << juce::String (db (r.rmsDoMaster)).paddedRight (' ', 13)
                  << juce::String (db (r.picoQueEntrouNoPlugin)).paddedRight (' ', 16)
                  << juce::String (r.blocosDoPlugin).paddedRight (' ', 8)
                  << (r.blocosDoPlugin == 0 ? "-"
                      : r.ehEnvio ? "ENVIO (reverbBus)" : "INSERT (buffer)")
                  << '\n';

    // ------------------------------------------------------------------
    // Vereditos automaticos
    // ------------------------------------------------------------------
    const auto& A = resultados[0];
    const auto& B = resultados[1];
    const auto& C = resultados[2];
    const auto& D = resultados[3];
    const auto& E = resultados[4];
    const auto& F = resultados[5];
    const auto& G = resultados[6];
    const auto& H = resultados[7];

    bool ok = true;
    auto exigir = [&ok] (const char* texto, bool condicao)
    {
        std::cout << "  " << juce::String (texto).paddedRight (' ', 62)
                  << (condicao ? "OK" : "FALHOU") << '\n';
        ok = ok && condicao;
    };

    std::cout << "\nRENDER SEM INSERT (comparar entre dois builds)\n"
              << "  HASH_SEM_INSERT  0x"
              << juce::String::toHexString ((juce::int64) A.hashDoRender)
              << "   pico " << db (A.picoDoMaster)
              << "   rms " << db (A.rmsDoMaster) << '\n';

    std::cout << "\nVEREDITOS\n";

    for (const auto& r : resultados)
        exigir (("cenario valido: " + r.rotulo).toRawUTF8(), r.valido);

    exigir ("A) referencia nao saiu muda", A.picoDoMaster > 0.0f);

    exigir ("B) plugin comum recebe o mix (nada regrediu)",
            B.picoQueEntrouNoPlugin > 0.0001f && ! B.ehEnvio);

    exigir ("C) CORRECAO: plugin chamado BigSky recebe o mix dos canais",
            C.picoQueEntrouNoPlugin > 0.0001f && ! C.ehEnvio);
    exigir ("C) CORRECAO: o master mudou em relacao a A (da para ouvir)",
            std::abs (C.picoDoMaster - A.picoDoMaster) > 0.0005f);

    exigir ("D) com o knob REVERB aberto o plugin continua vendo o mix",
            D.picoQueEntrouNoPlugin > 0.0001f && ! D.ehEnvio);

    exigir ("E) COMPAT: setup antigo continua no ENVIO", E.ehEnvio);
    exigir ("E) COMPAT: com rvb0=0 o envio segue mudo, como antes",
            E.picoQueEntrouNoPlugin <= 0.0001f);
    exigir ("E) COMPAT: e o master fica identico ao cenario sem plugin",
            std::abs (E.picoDoMaster - A.picoDoMaster) <= 0.0005f);

    exigir ("F) ENVIO explicito funciona com qualquer marca", F.ehEnvio);
    exigir ("F) ENVIO explicito recebe a dose do canal",
            F.picoQueEntrouNoPlugin > 0.0001f);

    exigir ("G) VSTi hospedado no canal chega ao plugin do master",
            G.picoQueEntrouNoPlugin > 0.0001f && ! G.ehEnvio);
    exigir ("H) VSTi + plugin chamado BigSky tambem chega",
            H.picoQueEntrouNoPlugin > 0.0001f && ! H.ehEnvio);

    exigir ("persistencia da rota no estado do projeto",
            checarPersistenciaDaRota (sampleRate, blockSize));

    std::cout << "\nFORA DO ALCANCE DESTA SONDA (por arquitetura, nao por bug)\n"
                 "  O Pad Continuo (padNode) e a ENTRADA DE AUDIO sao nos do\n"
                 "  AudioProcessorGraph do MainComponent e nao passam dentro do\n"
                 "  Church Live: padNode liga direto no masterNode\n"
                 "  (MainComponent.cpp, connectAudio pad_to_master) e o\n"
                 "  audioInNode entra na fxChain. Logo o MASTER FX do Church\n"
                 "  Live nunca os alcancou e continua nao alcancando. Religar\n"
                 "  isso muda o som de setups ja salvos e exige opt-in.\n";

    std::cout << (ok ? "\nMASTER_INSERT_ROUTING_PROBE_OK\n"
                     : "\nMASTER_INSERT_ROUTING_PROBE_FAIL\n");
    return ok ? 0 : 1;
}
