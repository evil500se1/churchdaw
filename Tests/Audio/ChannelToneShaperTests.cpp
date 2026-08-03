#include "ChannelToneShaper.h"
#include "TimbreToneMemory.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <vector>

namespace
{
bool check (bool condition, const char* message)
{
    if (! condition)
        std::cerr << "FALHA: " << message << '\n';

    return condition;
}

float renderRms (float frequency, float lowDb, float midDb, float highDb)
{
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 128;
    constexpr int blocks = 420;

    ChurchDAWChannelTone::ThreeBandEq eq;
    eq.prepare (sampleRate, blockSize);
    juce::AudioBuffer<float> block (2, blockSize);
    double phase = 0.0;
    const double step = juce::MathConstants<double>::twoPi
                      * (double) frequency / sampleRate;
    double sum = 0.0;
    int count = 0;

    for (int blockIndex = 0; blockIndex < blocks; ++blockIndex)
    {
        for (int sample = 0; sample < blockSize; ++sample)
        {
            const float value = (float) std::sin (phase);
            phase += step;

            if (phase >= juce::MathConstants<double>::twoPi)
                phase -= juce::MathConstants<double>::twoPi;

            block.setSample (0, sample, value);
            block.setSample (1, sample, value);
        }

        eq.process (block, blockSize, lowDb, midDb, highDb);

        if (blockIndex >= blocks / 2)
            for (int sample = 0; sample < blockSize; ++sample)
            {
                const auto value = block.getSample (0, sample);

                if (! std::isfinite (value))
                    return std::numeric_limits<float>::quiet_NaN();

                sum += (double) value * (double) value;
                ++count;
            }
    }

    return count > 0 ? (float) std::sqrt (sum / (double) count) : 0.0f;
}
}

int main()
{
    bool ok = true;

    const auto piano = ChurchDAWChannelTone::defaultsFor (
        "Studio Grand", "PIANOS");
    const auto felt = ChurchDAWChannelTone::defaultsFor (
        "Felt Upright", "PIANOS");
    const auto pad = ChurchDAWChannelTone::defaultsFor (
        "Airy Pad", "PADS");

    ok &= check (piano.attackSeconds <= 0.005f,
                 "piano precisa preservar o ataque natural");
    ok &= check (felt.attackSeconds == piano.attackSeconds
                  && felt.releaseSeconds == piano.releaseSeconds
                  && pad.attackSeconds == piano.attackSeconds
                  && pad.releaseSeconds == piano.releaseSeconds,
                 "defaults precisam preservar o som da versao anterior");
    ok &= check (
        ChurchDAWChannelTone::releaseForNote (0.55f, 21)
            > ChurchDAWChannelTone::releaseForNote (0.55f, 108),
        "release acustico deve ser maior nos graves");

    const float neutralLow = renderRms (80.0f, 0.0f, 0.0f, 0.0f);
    const float boostedLow = renderRms (80.0f, 6.0f, 0.0f, 0.0f);
    const float neutralMid = renderRms (850.0f, 0.0f, 0.0f, 0.0f);
    const float cutMid = renderRms (850.0f, 0.0f, -6.0f, 0.0f);
    // Prova a faixa onde existe energia em timbres reais, nao apenas um tom
    // artificial de 10 kHz que mascarava o knob percebido como morto.
    const float neutralHigh = renderRms (5000.0f, 0.0f, 0.0f, 0.0f);
    const float boostedHigh = renderRms (5000.0f, 0.0f, 0.0f, 6.0f);
    const float cutHigh = renderRms (5000.0f, 0.0f, 0.0f, -6.0f);
    const float boundedExtreme = renderRms (
        850.0f, 1000.0f, -1000.0f,
        std::numeric_limits<float>::quiet_NaN());

    ok &= check (std::isfinite (neutralLow)
                  && std::isfinite (boostedLow)
                  && boostedLow > neutralLow * 1.45f,
                 "LOW +6 dB precisa aumentar graves");
    ok &= check (std::isfinite (neutralMid)
                  && std::isfinite (cutMid)
                  && cutMid < neutralMid * 0.62f,
                 "MID -6 dB precisa reduzir a regiao central");
    ok &= check (std::isfinite (neutralHigh)
                  && std::isfinite (boostedHigh)
                  && boostedHigh > neutralHigh * 1.35f
                  && std::isfinite (cutHigh)
                  && cutHigh < neutralHigh * 0.75f,
                 "HIGH +/-6 dB precisa responder na faixa musical de 5 kHz");
    ok &= check (std::isfinite (boundedExtreme),
                 "EQ precisa permanecer finito com valores adversos");

    // 0 dB nao pode colorir nem arredondar o audio.
    {
        ChurchDAWChannelTone::ThreeBandEq neutralEq;
        neutralEq.prepare (48000.0, 128);
        juce::AudioBuffer<float> before (2, 128);

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 128; ++i)
                before.setSample (
                    ch, i,
                    0.41f * std::sin (
                        0.037f * (float) (i + ch * 17)));

        juce::AudioBuffer<float> after;
        after.makeCopyOf (before);
        neutralEq.process (after, 128, 0.0f, 0.0f, 0.0f);
        bool exact = true;

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 128; ++i)
                exact = exact
                     && after.getSample (ch, i)
                            == before.getSample (ch, i);

        ok &= check (exact, "EQ 0 dB precisa ser bit-transparent");
    }

    // Alterar o EQ durante silencio deve gastar os 45 ms da rampa mesmo sem
    // buffer ativo; a primeira nota precisa nascer no alvo, nao no preset velho.
    {
        ChurchDAWChannelTone::ThreeBandEq silentEq;
        ChurchDAWChannelTone::ThreeBandEq directEq;
        silentEq.prepare (48000.0, 128);
        directEq.prepare (48000.0, 128);
        silentEq.advanceSilently (4800, 6.0f, -3.0f, 4.0f);
        directEq.syncToTargets (6.0f, -3.0f, 4.0f);

        juce::AudioBuffer<float> silentResult (2, 128);
        juce::AudioBuffer<float> directResult (2, 128);

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 128; ++i)
            {
                const float sample =
                    0.25f * std::sin (
                        juce::MathConstants<float>::twoPi
                        * 440.0f * (float) i / 48000.0f);
                silentResult.setSample (ch, i, sample);
                directResult.setSample (ch, i, sample);
            }

        silentEq.process (
            silentResult, 128, 6.0f, -3.0f, 4.0f);
        directEq.process (
            directResult, 128, 6.0f, -3.0f, 4.0f);
        float maxDifference = 0.0f;

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 128; ++i)
                maxDifference = juce::jmax (
                    maxDifference,
                    std::abs (
                        silentResult.getSample (ch, i)
                        - directResult.getSample (ch, i)));

        ok &= check (
            maxDifference < 1.0e-5f,
            "primeira nota apos silencio precisa usar o EQ alvo");
    }

    // A regulagem pertence ao timbre dentro de cada canal: A -> B -> A deve
    // recuperar os cinco controles, e packs com o mesmo nome nao colidem.
    ChurchDAWTimbreTone::Store<2, 3> memories;
    ChurchDAWTimbreTone::Settings pianoA;
    pianoA.attackSeconds = 0.137f;
    pianoA.releaseSeconds = 3.25f;
    pianoA.lowDb = 2.5f;
    pianoA.midDb = -3.5f;
    pianoA.highDb = 1.75f;
    memories.remember (0, "Felt Upright", "Church Live", pianoA);

    auto pianoB = pianoA;
    pianoB.attackSeconds = 0.42f;
    pianoB.releaseSeconds = 6.1f;
    pianoB.lowDb = -5.0f;
    memories.remember (0, "Felt Upright", "Worship", pianoB);

    ChurchDAWTimbreTone::Settings recalled;
    ok &= check (
        memories.recall (
            0, "Felt Upright", "Church Live", recalled)
            && std::abs (recalled.attackSeconds - 0.137f) < 0.0001f
            && std::abs (recalled.releaseSeconds - 3.25f) < 0.0001f
            && std::abs (recalled.lowDb - 2.5f) < 0.0001f
            && std::abs (recalled.midDb + 3.5f) < 0.0001f
            && std::abs (recalled.highDb - 1.75f) < 0.0001f,
        "memoria A-B-A precisa recuperar Attack/Release/LOW/MID/HIGH");
    ok &= check (
        memories.recall (
            0, "Felt Upright", "Worship", recalled)
            && std::abs (recalled.attackSeconds - 0.42f) < 0.0001f
            && std::abs (recalled.lowDb + 5.0f) < 0.0001f,
        "mesmo nome em bibliotecas diferentes nao pode colidir");
    ok &= check (
        ! memories.recall (
            1, "Felt Upright", "Church Live", recalled),
        "memoria de um canal nao pode vazar para outro");

    auto legacyNoLibrary = pianoA;
    legacyNoLibrary.attackSeconds = 0.333f;
    memories.remember (
        1, "Legacy Piano", {}, legacyNoLibrary);
    ok &= check (
        memories.recall (
            1, "Legacy Piano", "Church Live", recalled)
            && std::abs (
                recalled.attackSeconds - 0.333f) < 0.0001f,
        "perfil legado sem biblioteca precisa migrar pelo nome");
    ok &= check (
        ! memories.recall (
            1, "Legacy Piano", "Worship", recalled),
        "perfil legado promovido nao pode vazar para outro pack");

    // O banco faz parte do estado salvo e rejeita numeros hostis/corrompidos.
    juce::XmlElement toneXml ("TIMBRES2");
    memories.writeToXml (toneXml);
    ChurchDAWTimbreTone::Store<2, 3> restoredMemories;
    restoredMemories.readFromXml (toneXml);
    ok &= check (
        restoredMemories.recall (
            0, "Felt Upright", "Church Live", recalled)
            && std::abs (recalled.midDb + 3.5f) < 0.0001f,
        "memoria por timbre precisa sobreviver ao XML");

    juce::XmlElement hostileXml ("TIMBRES2");
    auto* hostile = hostileXml.createNewChildElement ("TONE");
    hostile->setAttribute ("layer", 0);
    hostile->setAttribute ("timbre", "Hostile");
    hostile->setAttribute ("lib", "Pack");
    hostile->setAttribute ("attack", "nan");
    hostile->setAttribute ("release", 999999.0);
    hostile->setAttribute ("eqLow", -999999.0);
    hostile->setAttribute ("eqMid", 999999.0);
    hostile->setAttribute ("eqHigh", "nan");
    hostile->setAttribute ("rotMode", 999);
    hostile->setAttribute ("rotAmt", 999.0);
    restoredMemories.readFromXml (hostileXml);
    ok &= check (
        restoredMemories.recall (
            0, "Hostile", "Pack", recalled)
            && recalled.attackSeconds >= 0.001f
            && recalled.releaseSeconds == 12.0f
            && recalled.lowDb == -12.0f
            && recalled.midDb == 12.0f
            && recalled.highDb == 0.0f
            // O Store aceita o quarto estado canonico do Church Organ
            // (Brake=3). O host limita sampled/VST a 0..2 ao aplicar.
            && recalled.rotaryMode == 3
            && recalled.rotaryAmount == 1.0f,
        "estado adversarial da memoria precisa ser limitado");

    // Limite/LRU: o timbre recem-usado fica; o mais antigo sai.
    ChurchDAWTimbreTone::Store<1, 3> boundedMemories;
    boundedMemories.remember (0, "A", "P", {});
    boundedMemories.remember (0, "B", "P", {});
    boundedMemories.remember (0, "C", "P", {});
    boundedMemories.recall (0, "A", "P", recalled);
    boundedMemories.remember (0, "A", "P", recalled);
    boundedMemories.remember (0, "D", "P", {});
    ok &= check (
        boundedMemories.size (0) == 3
            && ! boundedMemories.recall (0, "B", "P", recalled)
            && boundedMemories.recall (0, "A", "P", recalled),
        "banco por timbre precisa ser limitado e preservar o item recente");

    if (! ok)
        return 1;

    std::cout << "CHANNEL_TONE_OK"
              << " piano_attack_ms=" << piano.attackSeconds * 1000.0f
              << " piano_release_s=" << piano.releaseSeconds
              << " pad_attack_s=" << pad.attackSeconds
              << " low_ratio=" << boostedLow / neutralLow
              << " mid_ratio=" << cutMid / neutralMid
              << " high_ratio=" << boostedHigh / neutralHigh
              << '\n';
    return 0;
}
