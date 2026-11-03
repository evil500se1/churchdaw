// Sonda MANUAL da emenda do Pad Continuo -- ver PadProcessor.h ("LOOP DO PAD
// CONTINUO"). Ela instancia o MESMO PadProcessor do produto, pede um tom real
// da biblioteca instalada e renderiza offline ate DEPOIS da volta do laco,
// gravando uma janela em WAV. Sem interface e sem placa de som.
//
// Serve para responder uma pergunta so, com numero: a volta do pad de 5 min
// ainda da clique e ainda deixa o buraco de alguns segundos?
//
// NAO entra em nenhum add_test: depende da biblioteca da maquina, igual a
// ChurchDAWTimbreRenderProbe.
//
// uso: ChurchDAWPadLoopProbe --tom C --de 294 --ate 306 --out saida.wav
//      ChurchDAWPadLoopProbe --pasta          (so diz onde achou os WAV)

#include "PadProcessor.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>
#include <vector>

namespace
{
    int    argInt    (const juce::StringArray& a, juce::StringRef k, int d)
    {
        const int i = a.indexOf (k);
        return (i >= 0 && i + 1 < a.size()) ? a[i + 1].getIntValue() : d;
    }

    double argDouble (const juce::StringArray& a, juce::StringRef k, double d)
    {
        const int i = a.indexOf (k);
        return (i >= 0 && i + 1 < a.size()) ? a[i + 1].getDoubleValue() : d;
    }

    juce::String argStr (const juce::StringArray& a, juce::StringRef k, juce::StringRef d)
    {
        const int i = a.indexOf (k);
        return (i >= 0 && i + 1 < a.size()) ? a[i + 1] : juce::String (d);
    }

    // "C" .. "B" -> 0..11, a mesma ordem do PadProcessor::setKey.
    int tomDeTexto (const juce::String& s)
    {
        static const char* nomes[] = { "C", "C#", "D", "D#", "E", "F",
                                       "F#", "G", "G#", "A", "A#", "B" };
        for (int i = 0; i < 12; ++i)
            if (s.equalsIgnoreCase (nomes[i]))
                return i;

        return 0;
    }
}

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    juce::StringArray args;
    for (int i = 1; i < argc; ++i)
        args.add (juce::String (juce::CharPointer_UTF8 (argv[i])));

    const auto pasta = PadProcessor::padsFolder();
    std::printf ("pasta de pads: %s\n",
                 pasta.isDirectory() ? pasta.getFullPathName().toRawUTF8() : "(NAO ACHOU)");

    if (! pasta.isDirectory())
        return 2;

    if (args.contains ("--pasta"))
        return 0;

    const int    tom      = tomDeTexto (argStr (args, "--tom", "C"));
    const double sr       = argDouble (args, "--sr", 44100.0);
    const int    bloco    = argInt    (args, "--block", 512);
    const double de       = argDouble (args, "--de", 294.0);
    const double ate      = argDouble (args, "--ate", 306.0);
    const auto   saida    = argStr    (args, "--out", "pad_emenda.wav");

    if (ate <= de || bloco <= 0)
        return 2;

    PadProcessor pad;
    pad.setPlayConfigDetails (0, 2, sr, bloco);
    pad.prepareToPlay (sr, bloco);
    pad.setGain (1.0f);          // ganho unitario: o numero medido e o do LOOP
    pad.setCutoff (1.0f);        // corte no topo

    // ATENCAO A QUEM FOR LER OS NUMEROS DESTA SONDA: com o corte no topo o
    // filtro do Pad NAO entra em bypass em 44,1 kHz. O bypass exige
    // alvoSeguro >= 0,45*sr, e o corte maximo e 120*160 = 19,2 kHz contra um
    // teto de 19,845 kHz -- ou seja, o passa-baixa de 2 polos continua ligado
    // em 19,2 kHz e ARREDONDA um degrau ao longo de 2 ou 3 amostras. O degrau
    // medido aqui sai ~0,7x do que esta no WAV. Isso e o que o musico ouve, e
    // por isso a medida vale; mas nao se deve compara-la direto com a leitura
    // crua do arquivo.
    pad.setKey (tom);

    juce::AudioBuffer<float> buf (2, bloco);
    juce::MidiBuffer midi;

    // O WAV e carregado por uma thread de fundo; ate ele chegar o processBlock
    // devolve silencio. Espera ATIVA de no maximo 60 s, medindo o proprio
    // sinal -- e o mesmo criterio que o audio usa.
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    bool chegou = false;
    for (int t = 0; t < 6000 && ! chegou; ++t)
    {
        buf.clear();
        pad.processBlock (buf, midi);

        if (buf.getMagnitude (0, bloco) > 0.0f)
            chegou = true;
        else
            juce::Thread::sleep (10);
    }

    if (! chegou)
    {
        std::printf ("ERRO: o pad nao produziu som (WAV nao carregou)\n");
        return 3;
    }

    // Quanto o musico espera entre pedir o tom e o pad soar. E o numero que
    // diz se o trabalho extra do loader (envelope + crossfade) atrapalha uma
    // troca de tom no meio do culto.
    std::printf ("carga do WAV ate o primeiro som: %.0f ms\n",
                 juce::Time::getMillisecondCounterHiRes() - t0);

    // Volta do zero: o que interessa e a PRIMEIRA passada inteira seguida da
    // virada, exatamente como no culto (tecla apertada e esquecida).
    pad.setKey (-1);
    for (int i = 0; i < 400; ++i) { buf.clear(); pad.processBlock (buf, midi); }
    pad.setKey (tom);

    const juce::int64 primeiro = (juce::int64) (de  * sr);
    const juce::int64 ultimo   = (juce::int64) (ate * sr);

    juce::AudioBuffer<float> janela (2, (int) (ultimo - primeiro));
    janela.clear();

    juce::int64 pos = 0;
    double picoGlobal = 0.0;

    while (pos < ultimo)
    {
        buf.clear();
        pad.processBlock (buf, midi);

        for (int n = 0; n < bloco; ++n)
        {
            const juce::int64 abs = pos + n;

            if (abs >= primeiro && abs < ultimo)
                for (int c = 0; c < 2; ++c)
                    janela.setSample (c, (int) (abs - primeiro), buf.getSample (c, n));
        }

        picoGlobal = juce::jmax (picoGlobal, (double) buf.getMagnitude (0, bloco));
        pos += bloco;
    }

    juce::File arq = juce::File::getCurrentWorkingDirectory().getChildFile (saida);
    arq.deleteFile();

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::FileOutputStream> os (arq.createOutputStream());

    if (os == nullptr)
    {
        std::printf ("ERRO: nao consegui escrever %s\n", arq.getFullPathName().toRawUTF8());
        return 4;
    }

    // 32 bits float: a descontinuidade que se quer medir vale ~0,001 do fundo
    // de escala. Quantizar para 16 bits jogaria a medida no meio do ruido do
    // proprio formato.
    std::unique_ptr<juce::AudioFormatWriter> w (
        wav.createWriterFor (os.release(), sr, 2, 32, {}, 0));

    if (w == nullptr)
    {
        std::printf ("ERRO: nao consegui criar o writer\n");
        return 4;
    }

    w->writeFromAudioSampleBuffer (janela, 0, janela.getNumSamples());
    w.reset();

    std::printf ("tom=%d sr=%.0f bloco=%d janela=[%.2f s, %.2f s] pico=%.5f\n",
                 tom, sr, bloco, de, ate, picoGlobal);
    std::printf ("gravado: %s (%d frames)\n",
                 arq.getFullPathName().toRawUTF8(), janela.getNumSamples());

    return 0;
}
