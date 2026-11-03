#pragma once

#if JUCE_ANDROID

#include <atomic>
#include <mutex>
#include <sched.h>

namespace churchdaw::androidPerformance
{
struct FastCoreMask
{
    cpu_set_t cores {};
    int selected = 0;
    int available = 0;
    long maximumFrequencyKhz = 0;
    std::atomic<bool> ready { false };
};

inline FastCoreMask& fastCoreMask() noexcept
{
    static FastCoreMask mask;
    return mask;
}

// Execute fora da thread de audio: a descoberta consulta sysfs. Depois disso,
// o callback faz apenas sched_setaffinity(), sem arquivo, lock ou alocacao.
inline void prepareFastCoreMask() noexcept
{
    auto& result = fastCoreMask();
    if (result.ready.load (std::memory_order_acquire))
        return;

    static std::once_flag preparation;
    std::call_once (preparation, [&result]
    {
        long frequencies[CPU_SETSIZE] = { 0 };
        const int count = juce::jmin ((int) CPU_SETSIZE,
                                      juce::SystemStats::getNumCpus());
        long maximum = 0;

        for (int core = 0; core < count; ++core)
        {
            const juce::File frequencyFile (
                "/sys/devices/system/cpu/cpu" + juce::String (core)
                + "/cpufreq/cpuinfo_max_freq");

            if (! frequencyFile.existsAsFile())
                continue;

            frequencies[core] = (long) frequencyFile.loadFileAsString()
                                                    .trim().getLargeIntValue();
            maximum = juce::jmax (maximum, frequencies[core]);
        }

        CPU_ZERO (&result.cores);
        int selected = 0;

        if (maximum > 0)
            for (int core = 0; core < count; ++core)
                if (frequencies[core] * 10 >= maximum * 8)
                {
                    CPU_SET (core, &result.cores);
                    ++selected;
                }

        // Fixar em todos os nucleos nao muda o escalonamento; em nenhum seria
        // invalido. Nesses casos o app segue com a politica normal do Android.
        result.available = count;
        result.maximumFrequencyKhz = maximum;
        result.selected = selected > 0 && selected < count ? selected : 0;
        result.ready.store (true, std::memory_order_release);
    });
}

inline bool pinCurrentThreadToFastCores() noexcept
{
    auto& mask = fastCoreMask();
    if (! mask.ready.load (std::memory_order_acquire))
        return false;

    return mask.selected > 0
        && sched_setaffinity (0, sizeof (mask.cores), &mask.cores) == 0;
}
} // namespace churchdaw::androidPerformance

#endif
