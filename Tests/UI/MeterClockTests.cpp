#include "LevelMeter.h"
#include <iostream>
#include <memory>
#include <utility>

namespace
{
struct FakeMeter : MeterClock::Client
{
    void meterTick() override
    {
        ticks.push_back (juce::Time::getHighResolutionTicks());

        if (onTick != nullptr)
            onTick();
    }

    std::vector<juce::int64> ticks;
    std::function<void()> onTick;
};
}

int runMeterClockTests()
{
    int failures = 0;
    auto expect = [&] (bool ok, const char* message)
    {
        if (! ok) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
    };

    auto& clock = MeterClock::get();
    expect (clock.size() == 0, "o relogio deve comecar vazio");

    // (a) N medidores no mesmo Hz recebem os MESMOS ticks, todos no mesmo instante:
    // e isso que troca N despertares por tick por um so.
    constexpr int hz = 20, n = 7;
    std::vector<std::unique_ptr<FakeMeter>> meters;
    for (int i = 0; i < n; ++i)
    {
        meters.push_back (std::make_unique<FakeMeter>());
        clock.add (*meters.back(), hz);
    }
    expect (clock.size() == (size_t) n, "todos os medidores devem entrar no relogio");

    juce::MessageManager::getInstance()->runDispatchLoopUntil (1200);

    const auto ticks = meters[0]->ticks.size();
    expect (ticks >= 15 && ticks <= 26, "20 Hz por 1,2 s deveria dar ~24 ticks");
    for (auto& m : meters)
        expect (m->ticks.size() == ticks, "todos os medidores devem receber o mesmo numero de ticks");

    double worstSpreadMs = 0.0;
    for (size_t k = 0; k < ticks; ++k)
    {
        juce::int64 lo = meters[0]->ticks[k], hi = lo;
        for (auto& m : meters)
        {
            if (k >= m->ticks.size()) break;
            lo = juce::jmin (lo, m->ticks[k]);
            hi = juce::jmax (hi, m->ticks[k]);
        }
        worstSpreadMs = juce::jmax (worstSpreadMs, 1000.0 * juce::Time::highResolutionTicksToSeconds (hi - lo));
    }
    expect (worstSpreadMs < 10.0, "os medidores de um mesmo tick devem rodar juntos (< 10 ms), nao em fases diferentes");

    // (b) um tick que remove OUTRO medidor nao pode chamar o removido no mesmo tick
    auto& killer = *meters[0];
    auto& victim = *meters[1];
    const auto victimBefore = victim.ticks.size();
    bool fired = false;
    killer.onTick = [&] { if (! std::exchange (fired, true)) clock.remove (victim); };
    juce::MessageManager::getInstance()->runDispatchLoopUntil (150);
    expect (victim.ticks.size() == victimBefore, "medidor removido durante o tick nao pode ser chamado depois");
    expect (clock.size() == (size_t) n - 1, "o removido deve sair do relogio");

    // (c) esvaziar o relogio para o timer (nenhum despertar sem medidores)
    for (size_t i = 0; i < meters.size(); ++i)
        if (i != 1)
            clock.remove (*meters[i]);
    expect (clock.size() == 0, "o relogio deve ficar vazio");
    const auto after = meters[0]->ticks.size();
    juce::MessageManager::getInstance()->runDispatchLoopUntil (300);
    expect (meters[0]->ticks.size() == after, "sem medidores o relogio nao pode mais disparar");

    // (d) o LevelMeter de verdade se registra no construtor e sai no destrutor
    {
        LevelMeter a (2, [] (int) { return 0.0f; });
        LevelMeter b (1, [] (int) { return 0.0f; });
       #if ! CHURCHDAW_UI_BISECT
        expect (clock.size() == 2, "LevelMeter deve se registrar no relogio compartilhado");
       #endif
    }
    expect (clock.size() == 0, "LevelMeter destruido deve sair do relogio");

    if (failures == 0)
        std::cout << "MeterClock OK\n";
    return failures == 0 ? 0 : 1;
}
