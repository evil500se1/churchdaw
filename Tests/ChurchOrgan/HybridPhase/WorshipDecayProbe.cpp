// Mede quanto tempo uma nota do Worship Hybrid leva para silenciar depois do
// note-off, reproduzindo os PADROES DE PRODUCAO (OrganParameters.cpp), nao a
// configuracao "clean" usada pelos testes de correcao.
//
// Motivo: o cliente relatou que a nota "fica soando um bom tempo". Numero em
// vez de opiniao -- a sonda imprime o tempo ate -60 dB do nivel sustentado.
//
// Uso: WorshipDecayProbe [attackMs] [releaseMs]
//      sem argumentos usa os padroes atuais de producao.

#include "WorshipHybridEngine.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <vector>

namespace
{
    using namespace churchorgan;

    constexpr double sampleRate = 48000.0;
    constexpr std::size_t blockSize = 512;

    // Padroes declarados em OrganParameters.cpp para as camadas worship.
    constexpr float defaultWorshipAttackMs  = 850.0f;
    constexpr float defaultWorshipReleaseMs = 4200.0f;
    constexpr float defaultIntensity        = 0.18f;
    constexpr float defaultWidth            = 0.68f;
    constexpr float defaultLevels[4] { 0.35f, 0.20f, 0.18f, 0.08f };

    float rmsOf (const std::vector<float>& left,
                 const std::vector<float>& right,
                 std::size_t from,
                 std::size_t count)
    {
        double sum = 0.0;
        const auto end = std::min (from + count, left.size());
        for (std::size_t i = from; i < end; ++i)
            sum += double (left[i]) * left[i]
                 + double (right[i]) * right[i];
        const auto n = double ((end - from) * 2);
        return n > 0.0 ? float (std::sqrt (sum / n)) : 0.0f;
    }
}

int main (int argc, char* argv[])
{
    const float attackMs = argc > 1
        ? float (std::atof (argv[1])) : defaultWorshipAttackMs;
    const float releaseMs = argc > 2
        ? float (std::atof (argv[2])) : defaultWorshipReleaseMs;

    auto engine = std::make_unique<WorshipHybridEngine>();
    if (! engine->prepare ({ sampleRate, blockSize, 20260730u,
                             WorshipHybridQuality::standard }))
    {
        std::cerr << "prepare falhou" << std::endl;
        return 2;
    }

    for (std::size_t index = 0; index < worshipLayerCount; ++index)
    {
        const auto layer = static_cast<WorshipLayer> (index);
        auto parameters = engine->worshipLayer().layerParameters (layer);
        parameters.enabled = true;
        parameters.level = index < 4 ? defaultLevels[index] : 0.0f;
        parameters.attackMilliseconds = attackMs;
        parameters.releaseMilliseconds = releaseMs;
        parameters.stereoWidth = defaultWidth;
        engine->worshipLayer().setLayerParameters (layer, parameters);
    }
    engine->setWorshipIntensity (defaultIntensity);

    const std::size_t heldSamples = std::size_t (sampleRate * 2.0);
    const std::size_t tailSamples = std::size_t (sampleRate * 14.0);
    const std::size_t total = heldSamples + tailSamples;

    std::vector<float> left (total, 0.0f);
    std::vector<float> right (total, 0.0f);

    auto renderRange = [&] (std::size_t from, std::size_t count)
    {
        for (std::size_t offset = 0; offset < count; offset += blockSize)
        {
            const auto n = std::min (blockSize, count - offset);
            if (! engine->render ({ left.data() + from + offset,
                                    right.data() + from + offset,
                                    nullptr, nullptr, n }))
            {
                std::cerr << "render falhou" << std::endl;
                std::exit (3);
            }
        }
    };

    engine->queueNoteOn (Manual::upper, 60, 100, 0);
    renderRange (0, heldSamples);

    // nivel sustentado: ultimo meio segundo com a tecla ainda pressionada
    const auto sustained = rmsOf (left, right,
                                  heldSamples - std::size_t (sampleRate * 0.5),
                                  std::size_t (sampleRate * 0.5));

    engine->queueNoteOff (Manual::upper, 60, 0);
    renderRange (heldSamples, tailSamples);

    std::cout << "attack=" << attackMs << "ms release=" << releaseMs << "ms"
              << "  sustentado=" << sustained << std::endl;

    if (sustained <= 0.0f)
    {
        std::cerr << "sem sinal sustentado -- nada a medir" << std::endl;
        return 4;
    }

    const std::size_t window = std::size_t (sampleRate * 0.05);
    const float floor60 = sustained * 0.001f;   // -60 dB
    const float floor40 = sustained * 0.01f;    // -40 dB
    const float floor20 = sustained * 0.1f;     // -20 dB

    double t20 = -1.0, t40 = -1.0, t60 = -1.0;
    for (std::size_t at = heldSamples; at + window <= total; at += window)
    {
        const auto level = rmsOf (left, right, at, window);
        const auto seconds = double (at - heldSamples) / sampleRate;
        if (t20 < 0.0 && level <= floor20) t20 = seconds;
        if (t40 < 0.0 && level <= floor40) t40 = seconds;
        if (t60 < 0.0 && level <= floor60) { t60 = seconds; break; }
    }

    auto show = [] (const char* rotulo, double value)
    {
        std::cout << "  " << rotulo << " ";
        if (value < 0.0) std::cout << "NAO ATINGIU em 14 s";
        else             std::cout << value << " s";
        std::cout << std::endl;
    };

    show ("-20 dB apos soltar:", t20);
    show ("-40 dB apos soltar:", t40);
    show ("-60 dB apos soltar:", t60);

    // Referencia: um orgao real corta praticamente junto com a tecla.
    // Acima de 1,5 s ate -40 dB ja soa como "nota presa" ao vivo.
    if (t40 < 0.0 || t40 > 1.5)
    {
        std::cout << "VEREDITO: cauda longa demais para tocar ao vivo"
                  << std::endl;
        return 1;
    }

    std::cout << "VEREDITO: articulacao aceitavel" << std::endl;
    return 0;
}
