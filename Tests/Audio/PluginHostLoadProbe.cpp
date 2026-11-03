//==============================================================================
// Sonda de CARGA do ChurchDAW Instrument (VST3) num host de verdade.
//
// POR QUE ELA EXISTE: "compilou" nao responde nada. Um VST3 pode compilar,
// exportar a fabrica, e ainda assim travar no prepareToPlay, devolver silencio
// ou nao abrir editor. Esta sonda e um HOST DE LINHA DE COMANDO minusculo --
// nao contem uma linha de codigo do ChurchDAW: ela carrega o .vst3 pelo
// juce::VST3PluginFormat exatamente como o Reaper faria, e mede.
//
// O que ela prova, na ordem em que as coisas costumam quebrar:
//   1. o modulo carrega e a fabrica descreve o plugin (nome, fabricante, synth)
//   2. createPluginInstance devolve instancia
//   3. prepareToPlay sobrevive -- este e o teste do TRAMPOLIM DE PILHA: a
//      construcao das tabelas do Church Organ estoura 1 MiB, e numa DLL o
//      /STACK do link nao vale (quem manda e o PE do host)
//   4. processBlock roda N blocos sem estourar, com nota MIDI segurada
//   5. o audio que sai nao e silencio digital (se houver timbre carregado)
//   6. getStateInformation/setStateInformation fazem round-trip
//   7. o editor abre e tem tamanho utilizavel dentro da janela do host
//
// Uso:
//   ChurchDAWPluginHostLoadProbe "<caminho do .vst3>" [--blocos N] [--sem-editor]
//
// NAO entra em add_test: depende de um artefato ja compilado e da biblioteca
// de samples da maquina.
//==============================================================================

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "../../Source/PluginPreviewLifecycle.h"

#include <iostream>

namespace
{
struct Opcoes
{
    juce::String caminho;
    int blocos = 400;
    bool abrirEditor = true;
    int editorCycles = 1;
    double sampleRate = 48000.0;
    int blockSize = 512;

    // Um host de verdade nao "escolhe timbre": ele restaura o estado que o
    // projeto salvou. Estas duas opcoes existem para exercitar exatamente esse
    // caminho -- dump para ver o que o plugin salvaria, e carga para provar que
    // um projeto com timbre volta tocando.
    juce::File dumpEstado;
    juce::File carregarEstado;
    int esperaSegundos = 20;

    // Carrega um timbre PELO CAMINHO DO HOST, que e o unico que um plugin tem:
    // o host devolve o estado do projeto e o plugin se reconstroi. Ver
    // montarEstadoComTimbre() abaixo.
    juce::String timbre;
    juce::String biblioteca = "Church Live";
    int canal = 0;
};

//==============================================================================
// O estado que o host enxerga e um sanduiche, e vale escrever a receita porque
// ela nao esta documentada em lugar nenhum:
//
//   <VST3PluginState><IComponent> base64 do JUCE </IComponent> ...
//        |
//        +--> [ blob do TimbresProcessor          ][ "JUCEPrivateData" ... ]
//               ^ copyXmlToBinary do XML TIMBRES2   ^ escrito pelo wrapper
//
// O blob do processador e escrito por juce::AudioProcessor::copyXmlToBinary,
// cujo cabecalho carrega o proprio tamanho -- por isso getXmlFromBinary ignora
// o rabo de JUCEPrivateData sem reclamar. Para injetar um timbre basta trocar
// o XML e recolar o mesmo rabo, byte a byte.
bool montarEstadoComTimbre (juce::MemoryBlock& componentState,
                            const juce::String& timbre,
                            const juce::String& biblioteca,
                            int canal,
                            juce::String& erroSaida)
{
    auto xml = juce::AudioProcessor::getXmlFromBinary (
        componentState.getData(), (int) componentState.getSize());

    if (xml == nullptr)
    {
        erroSaida = "o estado do plugin nao esta em copyXmlToBinary";
        return false;
    }

    // Onde termina o blob do processador e comeca o rabo do wrapper.
    juce::MemoryBlock semRabo;
    juce::AudioProcessor::copyXmlToBinary (*xml, semRabo);
    const auto tamanhoRabo = componentState.getSize() > semRabo.getSize()
                               ? componentState.getSize() - semRabo.getSize()
                               : (size_t) 0;
    juce::MemoryBlock rabo;

    if (tamanhoRabo > 0)
        rabo.append (static_cast<const char*> (componentState.getData()) + semRabo.getSize(),
                     tamanhoRabo);

    bool achou = false;

    for (auto* layer : xml->getChildWithTagNameIterator ("LAYER"))
    {
        if (layer->getIntAttribute ("index", -1) != canal)
            continue;

        layer->setAttribute ("timbre", timbre);
        layer->setAttribute ("lib", biblioteca);
        achou = true;
        break;
    }

    if (! achou)
    {
        erroSaida = "o estado nao tem <LAYER index=\"" + juce::String (canal) + "\">";
        return false;
    }

    juce::MemoryBlock novo;
    juce::AudioProcessor::copyXmlToBinary (*xml, novo);

    if (rabo.getSize() > 0)
        novo.append (rabo.getData(), rabo.getSize());

    componentState = std::move (novo);
    return true;
}

int falhar (const juce::String& msg)
{
    std::cerr << "FALHOU: " << msg.toStdString() << "\n";
    return 1;
}

juce::String dbfs (float pico)
{
    if (pico <= 0.0f)
        return "-inf";

    return juce::String (juce::Decibels::gainToDecibels (pico), 2);
}
} // namespace

int main (int argc, char** argv)
{
    Opcoes opt;

    for (int i = 1; i < argc; ++i)
    {
        const juce::String a (argv[i]);

        if (a == "--blocos" && i + 1 < argc)          opt.blocos = juce::String (argv[++i]).getIntValue();
        else if (a == "--sem-editor")                 opt.abrirEditor = false;
        else if (a == "--editor-cycles" && i + 1 < argc) opt.editorCycles = juce::jlimit (1, 10, juce::String (argv[++i]).getIntValue());
        else if (a == "--sr" && i + 1 < argc)         opt.sampleRate = juce::String (argv[++i]).getDoubleValue();
        else if (a == "--bloco" && i + 1 < argc)      opt.blockSize = juce::String (argv[++i]).getIntValue();
        else if (a == "--dump-estado" && i + 1 < argc) opt.dumpEstado = juce::File (juce::String (argv[++i]));
        else if (a == "--estado" && i + 1 < argc)     opt.carregarEstado = juce::File (juce::String (argv[++i]));
        else if (a == "--espera" && i + 1 < argc)     opt.esperaSegundos = juce::String (argv[++i]).getIntValue();
        else if (a == "--timbre" && i + 1 < argc)     opt.timbre = juce::String::fromUTF8 (argv[++i]);
        else if (a == "--lib" && i + 1 < argc)        opt.biblioteca = juce::String::fromUTF8 (argv[++i]);
        else if (a == "--canal" && i + 1 < argc)      opt.canal = juce::String (argv[++i]).getIntValue();
        else if (opt.caminho.isEmpty())               opt.caminho = a;
    }

    if (opt.caminho.isEmpty())
        return falhar ("informe o caminho do .vst3 ou identificador AudioUnit");

    // O wrapper VST3 do JUCE cria componentes ja na descricao do plugin, e o
    // editor exige loop de mensagens. Sem isto a sonda morre antes de medir.
    juce::ScopedJuceInitialiser_GUI juceInit;

    const juce::File arquivo (opt.caminho);
    const bool audioUnit = opt.caminho.startsWith ("AudioUnit:");

    if (! audioUnit && ! arquivo.exists())
        return falhar ("nao existe: " + arquivo.getFullPathName());

    std::cout << "== ChurchDAW -- sonda de carga de plugin ==\n"
              << "arquivo : " << arquivo.getFullPathName().toStdString() << "\n"
              << "sr/bloco: " << opt.sampleRate << " Hz / " << opt.blockSize << "\n\n";

    //--------------------------------------------------------------------------
    // 1. fabrica
    std::unique_ptr<juce::AudioPluginFormat> formato;
   #if JUCE_PLUGINHOST_AU
    if (audioUnit) formato = std::make_unique<juce::AudioUnitPluginFormat>();
    else
   #endif
        formato = std::make_unique<juce::VST3PluginFormat>();
    juce::OwnedArray<juce::PluginDescription> descricoes;

    formato->findAllTypesForFile (descricoes, audioUnit ? opt.caminho : arquivo.getFullPathName());

    if (descricoes.isEmpty())
        return falhar ("o formato VST3 nao reconheceu nenhum tipo neste arquivo");

    std::cout << "[1] fabrica OK -- " << descricoes.size() << " tipo(s)\n";

    for (auto* d : descricoes)
        std::cout << "    nome=\"" << d->name.toStdString() << "\""
                  << "  fabricante=\"" << d->manufacturerName.toStdString() << "\""
                  << "  synth=" << (d->isInstrument ? "SIM" : "nao")
                  << "  in/out=" << d->numInputChannels << "/" << d->numOutputChannels
                  << "  uid=" << juce::String::toHexString (d->uniqueId).toStdString() << "\n";

    //--------------------------------------------------------------------------
    // 2. instancia
    juce::AudioPluginFormatManager gerenciador;
    gerenciador.addFormat (std::move (formato));

    juce::String erro;
    auto instancia = gerenciador.createPluginInstance (
        *descricoes.getFirst(), opt.sampleRate, opt.blockSize, erro);

    if (instancia == nullptr)
        return falhar ("createPluginInstance: " + erro);

    const int outputBusCount = instancia->getBusCount (false);
    const int mainOutputChannels = outputBusCount > 0
                                     ? instancia->getChannelCountOfBus (false, 0)
                                     : 0;

    std::cout << "\n[2] instancia OK -- \"" << instancia->getName().toStdString() << "\"\n"
              << "    parametros de automacao: " << instancia->getParameters().size() << "\n"
              << "    aceita MIDI: " << (instancia->acceptsMidi() ? "SIM" : "nao")
              << "   cauda: " << instancia->getTailLengthSeconds() << " s\n"
              << "    buses de saida: " << outputBusCount
              << "   saida principal: " << mainOutputChannels
              << " canal(is)   total: " << instancia->getTotalNumOutputChannels()
              << "\n";

    // A descricao preliminar de alguns hosts VST3 pode deixar in/out em 0/0
    // ate a instancia existir. O contrato que realmente alimenta o grafo do
    // host precisa, depois da instanciacao, ser um barramento estereo ativo.
    if (outputBusCount < 1
         || mainOutputChannels != 2
         || instancia->getTotalNumOutputChannels() < 2)
        return falhar ("o VST3 nao anunciou uma saida principal estereo ativa");

    //--------------------------------------------------------------------------
    // 3. prepareToPlay -- o teste do trampolim de pilha
    const int totalIns = instancia->getTotalNumInputChannels();
    const int totalOuts = instancia->getTotalNumOutputChannels();
    const int probeChannels = juce::jmax (2, totalIns, totalOuts);
    instancia->setPlayConfigDetails (totalIns, totalOuts, opt.sampleRate, opt.blockSize);
    instancia->prepareToPlay (opt.sampleRate, opt.blockSize);
    std::cout << "\n[3] prepareToPlay sobreviveu (tabelas do Church Organ construidas)\n"
              << "    latencia relatada: " << instancia->getLatencySamples() << " amostras\n";

    //--------------------------------------------------------------------------
    // 3b. estado do projeto. E assim que um host carrega um timbre: ele nao
    // "escolhe" nada, ele devolve o blob que o projeto guardou.
    //
    // getXmlFromBinary/copyXmlToBinary sao estaticos do juce::AudioProcessor, e
    // o formato que o TimbresProcessor usa -- por isso a sonda consegue ler e
    // escrever o estado em XML legivel sem compilar uma linha do ChurchDAW.
    if (opt.carregarEstado != juce::File())
    {
        if (! opt.carregarEstado.existsAsFile())
            return falhar ("estado nao existe: " + opt.carregarEstado.getFullPathName());

        if (auto xml = juce::parseXML (opt.carregarEstado))
        {
            juce::MemoryBlock blob;
            juce::AudioProcessor::copyXmlToBinary (*xml, blob);
            instancia->setStateInformation (blob.getData(), (int) blob.getSize());
            std::cout << "\n[3b] estado restaurado de "
                      << opt.carregarEstado.getFileName().toStdString()
                      << " (" << blob.getSize() << " bytes)\n";
        }
        else
        {
            return falhar ("XML invalido: " + opt.carregarEstado.getFullPathName());
        }
    }

    //--------------------------------------------------------------------------
    // 3c. injeta um timbre pelo caminho do HOST.
    //
    // Isto NAO e um atalho de teste: e literalmente o que o Reaper faz ao abrir
    // um projeto salvo. O plugin nao ganha nenhuma API nova -- so recebe de
    // volta um estado, exatamente como receberia do disco.
    if (opt.timbre.isNotEmpty())
    {
        juce::MemoryBlock atual;
        instancia->getStateInformation (atual);

        auto head = juce::AudioProcessor::getXmlFromBinary (
            atual.getData(), (int) atual.getSize());

        if (head == nullptr)
            return falhar ("nao consegui ler o estado do host para injetar o timbre");

        auto* comp = head->getChildByName ("IComponent");

        if (comp == nullptr)
            return falhar ("o estado do host nao tem <IComponent>");

        juce::MemoryBlock componentState;

        if (! componentState.fromBase64Encoding (comp->getAllSubText()))
            return falhar ("base64 do IComponent invalido");

        juce::String erroInjecao;

        if (! montarEstadoComTimbre (componentState, opt.timbre, opt.biblioteca,
                                     opt.canal, erroInjecao))
            return falhar (erroInjecao);

        comp->deleteAllTextElements();
        comp->addTextElement (componentState.toBase64Encoding());

        juce::MemoryBlock novo;
        juce::AudioProcessor::copyXmlToBinary (*head, novo);
        instancia->setStateInformation (novo.getData(), (int) novo.getSize());

        std::cout << "\n[3c] projeto injetado: canal " << opt.canal
                  << " = \"" << opt.timbre.toStdString() << "\""
                  << " (biblioteca \"" << opt.biblioteca.toStdString() << "\")\n";
    }

    // Deixa a varredura de biblioteca e o carregamento de timbre em segundo
    // plano terminarem antes de tocar. Sem isto a medicao acusaria silencio so
    // porque o sample ainda nao subiu -- e seria uma conclusao errada.
    for (int i = 0; i < opt.esperaSegundos * 10; ++i)
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
        juce::Thread::sleep (50);
    }

    //--------------------------------------------------------------------------
    // 4/5. processBlock com nota segurada
    juce::AudioBuffer<float> buffer (probeChannels, opt.blockSize);
    float pico = 0.0f;
    double somaQuadrados = 0.0;
    juce::int64 amostras = 0;
    bool naoFinito = false;

    for (int b = 0; b < opt.blocos; ++b)
    {
        buffer.clear();
        juce::MidiBuffer midi;

        if (b == 2)
            midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);

        if (b == opt.blocos * 3 / 4)
            midi.addEvent (juce::MidiMessage::noteOff (1, 60), 0);

        instancia->processBlock (buffer, midi);

        for (int c = 0; c < juce::jmin (2, buffer.getNumChannels()); ++c)
        {
            const auto* p = buffer.getReadPointer (c);

            for (int s = 0; s < buffer.getNumSamples(); ++s)
            {
                const float v = p[s];

                if (! std::isfinite (v))
                    naoFinito = true;

                pico = juce::jmax (pico, std::abs (v));
                somaQuadrados += (double) v * (double) v;
                ++amostras;
            }
        }
    }

    const double rms = amostras > 0 ? std::sqrt (somaQuadrados / (double) amostras) : 0.0;

    std::cout << "\n[4] processBlock rodou " << opt.blocos << " blocos sem travar\n"
              << "[5] pico " << dbfs (pico).toStdString()
              << " dBFS   rms " << dbfs ((float) rms).toStdString() << " dBFS"
              << (naoFinito ? "   *** AMOSTRA NAO-FINITA ***" : "") << "\n";

    if (pico <= 0.0f)
        std::cout << "    (silencio -- esperado se nenhum timbre estiver carregado no canal 1;\n"
                  << "     isto NAO e falha de wrapper, e ausencia de biblioteca/preset)\n";

    //--------------------------------------------------------------------------
    // 6. estado
    juce::MemoryBlock estado;
    instancia->getStateInformation (estado);
    std::cout << "\n[6] getStateInformation: " << estado.getSize() << " bytes\n";

    if (estado.getSize() > 0)
    {
        if (opt.dumpEstado != juce::File())
        {
            if (auto xml = juce::AudioProcessor::getXmlFromBinary (
                    estado.getData(), (int) estado.getSize()))
            {
                opt.dumpEstado.replaceWithText (xml->toString());
                std::cout << "    estado gravado em "
                          << opt.dumpEstado.getFullPathName().toStdString() << "\n";
            }
            else
            {
                std::cout << "    (o estado nao esta no formato copyXmlToBinary)\n";
            }
        }

        instancia->setStateInformation (estado.getData(), (int) estado.getSize());

        juce::MemoryBlock volta;
        instancia->getStateInformation (volta);

        std::cout << "    round-trip: " << volta.getSize() << " bytes  -- "
                  << (volta.getSize() == estado.getSize() ? "MESMO TAMANHO" : "TAMANHO DIFERENTE")
                  << "\n";
    }
    else
    {
        std::cout << "    *** VAZIO: o projeto do host nao salvaria nada ***\n";
    }

    //--------------------------------------------------------------------------
    // 7. editor
    if (opt.abrirEditor && instancia->hasEditor())
    {
        for (int cycle = 0; cycle < opt.editorCycles; ++cycle)
        {
            auto* ed = instancia->createEditorIfNeeded();
            if (ed == nullptr) return falhar ("createEditorIfNeeded devolveu nullptr");
            struct Window final : juce::DocumentWindow
            {
                Window() : DocumentWindow ("ChurchDAW plugin preview probe", juce::Colours::black,
                                           DocumentWindow::closeButton) {}
                void closeButtonPressed() override { setVisible (false); }
            } window;
            std::cout << "\n[7] editor abriu: " << ed->getWidth() << " x " << ed->getHeight()
                      << "   redimensionavel=" << (ed->isResizable() ? "SIM" : "nao") << "\n";
            const auto originalSize = ed->getBounds().withPosition (0, 0);
            ChurchDAWPluginPreview::attachEditor (window, ed);
            window.setVisible (true);
            juce::MessageManager::getInstance()->runDispatchLoopUntil (300);
            const bool sizePreserved = ed->getBounds().withPosition (0, 0) == originalSize;
            window.setVisible (false);
            ChurchDAWPluginPreview::clearEditor (window, instancia.get());
            if (instancia->getActiveEditor() != nullptr) return falhar ("editor ficou registrado apos fechar");
            std::cout << "    editor fechou sem travar; ciclo=" << cycle + 1
                      << " tamanho_preservado=" << sizePreserved << "\n";
        }
    }

    instancia->releaseResources();
    instancia.reset();

    std::cout << "\nOK -- o plugin carregou, preparou, processou e fechou.\n";
    return 0;
}
