// O LevelMeter da skin Church Live so repinta quando o SkinFrame muda (segmentos
// acesos, linha e cor do pico). Este teste trava a premissa disso: dois estados
// com o mesmo SkinFrame produzem a MESMA imagem, pixel a pixel. Se alguem mexer em
// paintSkin sem mexer em skinFrame, o medidor passa a ficar com a imagem velha.

#include <juce_audio_basics/juce_audio_basics.h>   // LevelMeter usa juce::Decibels

#include "LevelMeter.h"

#include <cstdint>
#include <iostream>
#include <map>
#include <tuple>

struct LevelMeterTestAccess
{
    static void set (LevelMeter& m, int ch, float level, float peak)
    {
        m.displayDb[ch] = level;
        m.peakDb[ch] = peak;
    }

    static auto frame (const LevelMeter& m, int ch) { return m.skinFrame (ch); }
};

namespace
{
std::uint64_t hashImage (const juce::Image& img)
{
    const juce::Image::BitmapData data (img, juce::Image::BitmapData::readOnly);
    std::uint64_t h = 1469598103934665603ull;

    for (int y = 0; y < data.height; ++y)
    {
        const auto* row = data.getLinePointer (y);
        for (int i = 0; i < data.width * data.pixelStride; ++i)
            h = (h ^ row[i]) * 1099511628211ull;
    }

    return h;
}
}

int runLevelMeterSkinTests()
{
    int failures = 0;
    const auto expect = [&] (bool ok, const char* message)
    {
        if (! ok) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
    };

    constexpr int w = 72, h = 640;
    LevelMeter meter (2, [] (int) { return 0.0f; });
    meter.setSize (w, h);
    meter.setSkin (true, 1.9f);
    meter.setChannelLabels (true);

    const auto render = [&]
    {
        juce::Image img (juce::Image::ARGB, w, h, true);
        {
            juce::Graphics g (img);
            meter.paintEntireComponent (g, false);
        } // Direct2D commits drawing when the Graphics context is released.
        return hashImage (img);
    };

    // repouso: nada aceso, sem linha de pico
    LevelMeterTestAccess::set (meter, 0, -60.0f, -60.0f);
    const auto rest = LevelMeterTestAccess::frame (meter, 0);
    expect (rest.lit == 0 && rest.peakY < 0.0f && ! rest.peakHot, "resting channel must be an empty frame");

    // (a) mesmo SkinFrame => mesma imagem, varrendo nivel e pico
    // 0,3 dB por passo contra ~2 dB por segmento: cada quadro e visitado ~6x, que
    // e o que prova a igualdade. Passos menores so deixam o teste lento (~30 ms/imagem).
    const float peaks[] { -60.0f, -20.1f, -0.04f, 0.5f };
    std::map<std::tuple<int, float, bool, int, float, bool>, std::uint64_t> seen;
    int collisions = 0;
    int states = 0;
    int lastLit = -1;

    for (float level = -60.0f; level <= 6.0f; level += 0.3f)
        for (const float peak : peaks)
        {
            LevelMeterTestAccess::set (meter, 0, level, peak);
            LevelMeterTestAccess::set (meter, 1, juce::jmax (-60.0f, level - 7.3f), -30.5f);

            const auto a = LevelMeterTestAccess::frame (meter, 0);
            const auto b = LevelMeterTestAccess::frame (meter, 1);
            const auto key = std::make_tuple (a.lit, a.peakY, a.peakHot, b.lit, b.peakY, b.peakHot);
            const auto image = render();
            const auto it = seen.find (key);
            ++states;

            if (it == seen.end())
                seen.emplace (key, image);
            else if (it->second != image)
                ++collisions;

            if (peak == peaks[0])
            {
                expect (a.lit >= lastLit, "lit segments must not decrease as the level rises");
                lastLit = a.lit;
            }
        }

    expect (collisions == 0, "two states with the same SkinFrame rendered different images");
    // cada quadro visitado >= 1,5x em media: com menos revisitas a igualdade nao e testada
    expect ((int) seen.size() * 3 < states * 2, "sweep must revisit frames, or it proves nothing");

    // (b) um Pad parado oscila +-0,2 dB: isso nao pode virar mais de um quadro novo
    for (const float centre : { -20.0f, -12.0f, -6.5f })
    {
        std::map<std::tuple<int, float, bool>, int> frames;
        for (float level = centre - 0.1f; level <= centre + 0.1f; level += 0.01f)
        {
            LevelMeterTestAccess::set (meter, 0, level, centre + 3.0f);
            const auto f = LevelMeterTestAccess::frame (meter, 0);
            ++frames[std::make_tuple (f.lit, f.peakY, f.peakHot)];
        }
        expect (frames.size() <= 2, "a +-0.1 dB wobble must change at most one segment");
    }

    // (c) a mudanca de escala invalida o quadro: mesma chave, imagem diferente
    LevelMeterTestAccess::set (meter, 0, -6.0f, -3.0f);
    const auto before = render();
    meter.setTopDb (0.0f);
    expect (render() != before, "setTopDb must change the rendered image");

    if (failures == 0)
        std::cout << "LevelMeter skin: OK (" << seen.size() << " frames em " << states << " estados)\n";

    return failures == 0 ? 0 : 1;
}
