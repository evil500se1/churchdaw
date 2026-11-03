// =============================================================================
// SONDA ADVERSARIAL DO ROTEAMENTO DO MASTER FX
// =============================================================================
// Objetivo: DERRUBAR a correcao de insertRoute. Nao repete o que a sonda
// original ja mede -- ataca o que ela NAO mede.
//
//  1) VAZAMENTO DE ROTA ENTRE PROJETOS: a rota e escrita no restore apenas
//     quando existe <INSERT> no XML. Slot vazio nao tem <INSERT>. Logo,
//     abrir um projeto com o slot vazio NAO reseta a rota do projeto anterior.
//  2) REVERB NATIVO MORTO POR PLUGIN NAO-REVERB EM SND.
//  3) BLOCO 0 / CANAIS A MAIS com insert em SND.
//  4) TROCA DE SAMPLE RATE com insert em SND.
//  5) PLUGIN QUE FALHA AO PREPARAR.
//  6) PLUGIN REMOVIDO ENQUANTO TOCA (clique / NaN).
// =============================================================================

#include "../../Source/TimbresProcessor.h"

#include <cmath>
#include <iostream>
#include <memory>

namespace
{

class InsertDeTeste final : public juce::AudioPluginInstance
{
public:
    InsertDeTeste (juce::String n, float g, bool falharNoPrepare = false)
        : AudioPluginInstance (
              BusesProperties()
                  .withInput ("In", juce::AudioChannelSet::stereo(), true)
                  .withOutput ("Out", juce::AudioChannelSet::stereo(), true)),
          nome (std::move (n)), ganho (g), falhar (falharNoPrepare) {}

    const juce::String getName() const override { return nome; }

    void prepareToPlay (double, int) override
    {
        if (falhar)
            throw std::runtime_error ("prepare falhou de proposito");
        ++prepares;
    }

    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
    {
        ++blocos;
        canaisVistos = juce::jmax (canaisVistos, b.getNumChannels());
        amostrasVistas = juce::jmax (amostrasVistas, b.getNumSamples());

        for (int ch = 0; ch < b.getNumChannels(); ++ch)
            pico = juce::jmax (pico, b.getMagnitude (ch, 0, b.getNumSamples()));

        if (forceSamples)
            for (int ch = 0; ch < b.getNumChannels(); ++ch)
                juce::FloatVectorOperations::fill (b.getWritePointer (ch), forcedSample, b.getNumSamples());
        else
            b.applyGain (ganho);
    }

    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}

    void fillInPluginDescription (juce::PluginDescription& d) const override
    {
        d.name = nome; d.pluginFormatName = "Teste"; d.fileOrIdentifier = nome;
        d.isInstrument = false; d.numInputChannels = 2; d.numOutputChannels = 2;
    }

    bool forceSamples = false;
    float forcedSample = 0.0f;
    float pico = 0.0f;
    int blocos = 0, prepares = 0, canaisVistos = 0, amostrasVistas = 0;

private:
    juce::String nome; float ganho; bool falhar;
};

class GeradorDeTeste final : public juce::AudioPluginInstance
{
public:
    GeradorDeTeste()
        : AudioPluginInstance (BusesProperties()
              .withOutput ("Out", juce::AudioChannelSet::stereo(), true)) {}

    const juce::String getName() const override { return "Gerador"; }
    void prepareToPlay (double sr, int) override { taxa = sr > 0 ? sr : 48000.0; fase = 0; toca = false; }
    void releaseResources() override {}

    bool forceSamples = false;
    float forcedSample = 0.0f;
    void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer& m) override
    {
        if (forceSamples)
        {
            for (int ch = 0; ch < b.getNumChannels(); ++ch)
                juce::FloatVectorOperations::fill (b.getWritePointer (ch), forcedSample, b.getNumSamples());
            return;
        }
        for (const auto meta : m)
        {
            const auto msg = meta.getMessage();
            if (msg.isNoteOn()) toca = true; else if (msg.isNoteOff()) toca = false;
        }
        b.clear();
        if (! toca) return;
        const double passo = 2.0 * juce::MathConstants<double>::pi * 220.0 / taxa;
        for (int s = 0; s < b.getNumSamples(); ++s)
        {
            const float v = 0.5f * (float) std::sin (fase); fase += passo;
            for (int ch = 0; ch < b.getNumChannels(); ++ch) b.setSample (ch, s, v);
        }
    }

    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}
    void fillInPluginDescription (juce::PluginDescription& d) const override
    {
        d.name = "Gerador"; d.pluginFormatName = "Teste"; d.fileOrIdentifier = "Gerador";
        d.isInstrument = true; d.numInputChannels = 0; d.numOutputChannels = 2;
    }
private:
    double taxa = 48000.0, fase = 0.0; bool toca = false;
};

void ajustar (TimbresProcessor& p, const juce::String& id, float v)
{
    for (auto* par : p.getParameters())
        if (auto* c = dynamic_cast<juce::AudioProcessorParameterWithID*> (par))
            if (c->paramID == id) { c->setValueNotifyingHost (v); return; }
}

struct Medida { float pico = 0.0f; double rms = 0.0; bool naoFinito = false; float maiorSalto = 0.0f; };

// Renderiza n segundos com nota presa. Se acaoNoMeio != nullptr, e chamada
// exatamente na metade (para simular remover o plugin no meio do culto).
Medida render (TimbresProcessor& p, double sr, int bloco, double segundos,
               std::function<void()> acaoNoMeio = {})
{
    Medida m;
    const juce::int64 total = (juce::int64) (segundos * sr);
    juce::AudioBuffer<float> b (2, bloco);
    juce::MidiBuffer midi;
    juce::int64 pos = 0; double soma = 0.0; juce::int64 n2 = 0;
    bool disparou = false; float anterior = 0.0f; bool temAnterior = false;

    while (pos < total)
    {
        const int n = (int) juce::jmin ((juce::int64) bloco, total - pos);
        b.setSize (2, n, false, false, true); b.clear(); midi.clear();
        if (! disparou) { midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0); disparou = true; }
        if (acaoNoMeio && pos <= total / 2 && pos + n > total / 2) acaoNoMeio();

        p.processBlock (b, midi);

        for (int s = 0; s < n; ++s)
        {
            const float v = b.getSample (0, s);
            if (! std::isfinite (v)) m.naoFinito = true;
            if (temAnterior && std::isfinite (v) && std::isfinite (anterior))
                m.maiorSalto = juce::jmax (m.maiorSalto, std::abs (v - anterior));
            anterior = v; temAnterior = true;
        }
        for (int ch = 0; ch < 2; ++ch)
        {
            m.pico = juce::jmax (m.pico, b.getMagnitude (ch, 0, n));
            const auto* d = b.getReadPointer (ch);
            for (int s = 0; s < n; ++s) soma += (double) d[s] * d[s];
        }
        n2 += 2 * n; pos += n;
        juce::MessageManager::getInstance()->runDispatchLoopUntil (0);
    }
    m.rms = n2 > 0 ? std::sqrt (soma / (double) n2) : 0.0;
    return m;
}

juce::String db (double v)
{
    return v > 0.0 ? juce::String (juce::Decibels::gainToDecibels (v, -200.0), 2) : juce::String ("-inf");
}

int falhas = 0;
void checar (const char* rotulo, bool ok)
{
    std::cout << "  " << (ok ? "[OK]   " : "[FALHA]") << ' ' << rotulo << '\n';
    if (! ok) ++falhas;
}

} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI init;
    const double sr = 48000.0; const int bloco = 256;

    // ========================================================================
    std::cout << "\n== 1) VAZAMENTO DE ROTA AO ABRIR OUTRO PROJETO ==\n";
    {
        // Projeto B: slot 0 VAZIO (nenhum elemento <INSERT> e emitido).
        juce::MemoryBlock projetoVazio;
        { TimbresProcessor tmp; tmp.setPlayConfigDetails (0, 2, sr, bloco);
          tmp.prepareToPlay (sr, bloco); tmp.getStateInformation (projetoVazio); }

        TimbresProcessor p; p.setPlayConfigDetails (0, 2, sr, bloco); p.prepareToPlay (sr, bloco);
        p.installInstrumentForTest (0, std::make_unique<GeradorDeTeste>());

        // Projeto A: o musico escolheu ENVIO de proposito.
        p.setInsertRoute (0, TimbresProcessor::insertRouteSend);
        p.installInsertForTest (0, std::make_unique<InsertDeTeste> ("Strymon BigSky", 1.0f));
        const bool eraEnvio = p.insertRoutesToSend (0);
        std::cout << "  projeto A: slot 0 em " << (eraEnvio ? "SND" : "INS") << '\n';

        // Abre o projeto B (slot 0 vazio).
        p.setStateInformation (projetoVazio.getData(), (int) projetoVazio.getSize());
        juce::MessageManager::getInstance()->runDispatchLoopUntil (200);

        const int escolha = p.getInsertRouteChoice (0);
        std::cout << "  depois de abrir o projeto B (slot vazio): escolha = "
                  << escolha << "  (0=INSERT esperado para slot novo, 1=SEND vazado)\n";

        // O musico agora carrega um plugin NOVO nesse slot vazio.
        p.installInstrumentForTest (0, std::make_unique<GeradorDeTeste>());
        auto* novo = new InsertDeTeste ("Valhalla Room", 0.5f);
        p.installInsertForTest (0, std::unique_ptr<InsertDeTeste> (novo));
        ajustar (p, "rvb0", 0.0f);

        const auto m = render (p, sr, bloco, 1.5);
        std::cout << "  plugin novo: ramo = " << (p.getReverbSlot() == 0 ? "ENVIO" : "INSERT")
                  << "   entrou no plugin = " << db (novo->pico)
                  << "   blocos = " << novo->blocos
                  << "   master pico = " << db (m.pico) << '\n';

        checar ("slot vazio de projeto novo volta ao padrao INSERT",
                escolha == TimbresProcessor::insertRouteInsert);
        checar ("plugin novo carregado depois do projeto B RECEBE o mix",
                novo->pico > 0.0f);
    }

    // ========================================================================
    std::cout << "\n== 2) REVERB NATIVO x PLUGIN NAO-REVERB EM SND ==\n";
    {
        auto medir = [&] (bool rotaSend, float& picoPlugin) -> Medida
        {
            TimbresProcessor p; p.setPlayConfigDetails (0, 2, sr, bloco); p.prepareToPlay (sr, bloco);
            p.installInstrumentForTest (0, std::make_unique<GeradorDeTeste>());
            ajustar (p, "rvb0", 0.8f);
            p.setInsertRoute (0, rotaSend ? TimbresProcessor::insertRouteSend
                                          : TimbresProcessor::insertRouteInsert);
            auto* ins = new InsertDeTeste ("Compressor Xpto", 1.0f);
            p.installInsertForTest (0, std::unique_ptr<InsertDeTeste> (ins));
            auto m = render (p, sr, bloco, 1.5);
            picoPlugin = ins->pico;
            std::cout << "  rota=" << (rotaSend ? "SND" : "INS")
                      << "  reverbSlot=" << p.getReverbSlot()
                      << "  getReverbMachine=" << p.getReverbMachine()
                      << "  master rms=" << db (m.rms) << '\n';
            return m;
        };
        float pIns = 0, pSnd = 0;
        const auto mIns = medir (false, pIns);
        const auto mSnd = medir (true, pSnd);
        checar ("com rvb aberto, INS e SND produzem masters diferentes (esperado)",
                std::abs (mIns.rms - mSnd.rms) > 1e-9);
        std::cout << "  (um compressor em SND assume o papel de 'reverb externo':"
                     " o Church Reverb nativo deixa de processar o envio)\n";
    }

    // ========================================================================
    std::cout << "\n== 3) BLOCO 0 E CANAIS A MAIS, COM INSERT EM SND ==\n";
    {
        TimbresProcessor p; p.setPlayConfigDetails (0, 2, sr, bloco); p.prepareToPlay (sr, bloco);
        p.installInstrumentForTest (0, std::make_unique<GeradorDeTeste>());
        ajustar (p, "rvb0", 0.8f);
        p.setInsertRoute (0, TimbresProcessor::insertRouteSend);
        auto* ins = new InsertDeTeste ("Valhalla Room", 1.0f);
        p.installInsertForTest (0, std::unique_ptr<InsertDeTeste> (ins));

        bool sobreviveu = true;
        try {
            juce::AudioBuffer<float> zero (2, 0); juce::MidiBuffer m0;
            m0.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
            p.processBlock (zero, m0);
            juce::AudioBuffer<float> quatro (4, bloco); quatro.clear();
            juce::MidiBuffer m1; p.processBlock (quatro, m1);
            for (int ch = 0; ch < 4; ++ch)
                for (int s = 0; s < bloco; ++s)
                    if (! std::isfinite (quatro.getSample (ch, s))) sobreviveu = false;
        } catch (...) { sobreviveu = false; }
        checar ("bloco 0 e buffer de 4 canais nao quebram nem geram NaN", sobreviveu);
    }

    // ========================================================================
    std::cout << "\n== 4) TROCA DE SAMPLE RATE COM INSERT EM SND ==\n";
    {
        TimbresProcessor p; p.setPlayConfigDetails (0, 2, 44100.0, 128); p.prepareToPlay (44100.0, 128);
        p.installInstrumentForTest (0, std::make_unique<GeradorDeTeste>());
        ajustar (p, "rvb0", 0.6f);
        p.setInsertRoute (0, TimbresProcessor::insertRouteSend);
        auto* ins = new InsertDeTeste ("Valhalla Room", 1.0f);
        p.installInsertForTest (0, std::unique_ptr<InsertDeTeste> (ins));
        const auto a = render (p, 44100.0, 128, 0.5);
        const int slotAntes = p.getReverbSlot();
        p.setPlayConfigDetails (0, 2, 96000.0, 512); p.prepareToPlay (96000.0, 512);
        const auto b = render (p, 96000.0, 512, 0.5);
        std::cout << "  44k1 pico=" << db (a.pico) << "  96k pico=" << db (b.pico)
                  << "  slot antes=" << slotAntes << " depois=" << p.getReverbSlot()
                  << "  prepares do insert=" << ins->prepares << '\n';
        checar ("rota SND preservada na troca de sample rate", p.getReverbSlot() == slotAntes);
        checar ("sem NaN apos a troca de sample rate", ! a.naoFinito && ! b.naoFinito);
        checar ("ainda ha audio depois da troca", b.pico > 0.0f);
    }

    // ========================================================================
    std::cout << "\n== 5) PLUGIN QUE FALHA AO PREPARAR ==\n";
    {
        TimbresProcessor p; p.setPlayConfigDetails (0, 2, sr, bloco); p.prepareToPlay (sr, bloco);
        p.installInstrumentForTest (0, std::make_unique<GeradorDeTeste>());
        p.setInsertRoute (0, TimbresProcessor::insertRouteSend);
        bool aceitou = true;
        try { aceitou = p.installInsertForTest (0, std::make_unique<InsertDeTeste> ("Quebrado", 1.0f, true)); }
        catch (...) { aceitou = false; }
        const auto m = render (p, sr, bloco, 1.0);
        std::cout << "  install aceitou=" << (aceitou ? "sim" : "nao")
                  << "  reverbSlot=" << p.getReverbSlot()
                  << "  master pico=" << db (m.pico) << '\n';
        checar ("plugin que falha ao preparar nao silencia o master", m.pico > 0.0f);
        checar ("plugin que falha ao preparar nao gera NaN", ! m.naoFinito);
    }

    // ========================================================================
    std::cout << "\n== 6) PLUGIN REMOVIDO NO MEIO DO CULTO ==\n";
    {
        auto teste = [&] (int rota, const char* nome)
        {
            TimbresProcessor p; p.setPlayConfigDetails (0, 2, sr, bloco); p.prepareToPlay (sr, bloco);
            p.installInstrumentForTest (0, std::make_unique<GeradorDeTeste>());
            ajustar (p, "rvb0", 0.7f);
            p.setInsertRoute (0, rota);
            p.installInsertForTest (0, std::make_unique<InsertDeTeste> ("Valhalla Room", 0.5f));
            const auto m = render (p, sr, bloco, 2.0, [&] { p.unloadInsert (0); });
            std::cout << "  " << nome << ": pico=" << db (m.pico)
                      << "  maior salto entre amostras=" << m.maiorSalto
                      << "  NaN=" << (m.naoFinito ? "SIM" : "nao") << '\n';
            checar (m.naoFinito ? "remover plugin tocando gerou NaN" : "remover plugin tocando: sem NaN",
                    ! m.naoFinito);
        };
        teste (TimbresProcessor::insertRouteInsert, "removido de INS");
        teste (TimbresProcessor::insertRouteSend,   "removido de SND");
    }

    // ========================================================================
    std::cout << "\n== 7) TROCA DE ROTA COM AUDIO RODANDO ==\n";
    {
        TimbresProcessor p; p.setPlayConfigDetails (0, 2, sr, bloco); p.prepareToPlay (sr, bloco);
        p.installInstrumentForTest (0, std::make_unique<GeradorDeTeste>());
        ajustar (p, "rvb0", 0.7f);
        p.installInsertForTest (0, std::make_unique<InsertDeTeste> ("Valhalla Room", 0.5f));
        const auto m = render (p, sr, bloco, 2.0,
            [&] { p.setInsertRoute (0, TimbresProcessor::insertRouteSend); });
        std::cout << "  pico=" << db (m.pico) << "  maior salto=" << m.maiorSalto
                  << "  NaN=" << (m.naoFinito ? "SIM" : "nao") << '\n';
        checar ("clicar INS->SND ao vivo nao gera NaN", ! m.naoFinito);
        checar ("clicar INS->SND ao vivo nao gera salto grosseiro (<0.5)", m.maiorSalto < 0.5f);
    }

    // Finite float samples can overflow a float sum without any NaN in the
    // plugin output. Keep audio audible through emergency attenuation; genuine
    // non-finite samples remain covered by the containment/recovery tests.
    for (const bool instrument : { true, false })
    {
        TimbresProcessor p;
        ajustar (p, "rvb0", 0.0f);
        p.setPlayConfigDetails (0, 2, sr, bloco);
        p.prepareToPlay (sr, bloco);
        if (instrument)
        {
            auto plugin = std::make_unique<GeradorDeTeste>();
            plugin->forceSamples = true; plugin->forcedSample = 1.0e37f;
            checar ("instalar gerador de sinal finito extremo", p.installInstrumentForTest (0, std::move (plugin)));
        }
        else
        {
            auto plugin = std::make_unique<InsertDeTeste> ("Finite output", 1.0f);
            plugin->forceSamples = true; plugin->forcedSample = 1.0e37f;
            checar ("instalar insert de sinal finito extremo", p.installInsertForTest (0, std::move (plugin)));
        }
        juce::AudioBuffer<float> b (2, bloco); juce::MidiBuffer midi;
        p.processBlock (b, midi);
        bool finite = true;
        for (int ch = 0; ch < 2; ++ch)
            for (int sample = 0; sample < bloco; ++sample)
                finite = finite && std::isfinite (b.getSample (ch, sample));
        checar (instrument ? "VSTi finito extremo atenuado sem emudecer" : "Insert finito extremo atenuado sem emudecer",
                 finite && b.getMagnitude (0, bloco) > 0.1f && b.getMagnitude (0, bloco) <= 24.001f);
    }

    std::cout << "\n" << (falhas == 0 ? "ADVERSARIAL_OK" : "ADVERSARIAL_FAIL")
              << "  falhas=" << falhas << '\n';
    return falhas == 0 ? 0 : 1;
}
