#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <juce_audio_basics/juce_audio_basics.h>

// Telemetry only: no allocation, UI call or audio processing. A released key
// rises even with sustain held, just as a physical piano key does.
struct PianoVisualState
{
    static_assert (std::atomic<uint32_t>::is_always_lock_free, "MIDI telemetry must remain lock-free");
    std::array<std::atomic<uint32_t>, 128> channels {};
    // Separate sounding notes for harmony: sustain holds the chord while the
    // physical keys rise. Channels retain independent pedal/note ownership.
    std::array<std::atomic<uint32_t>, 128> sounding {};
    std::atomic<uint32_t> sustainChannels {0};
    std::array<std::atomic<uint32_t>, 128> strikes {};
    // The host aggregates physical pedals across ports and applies them to
    // every instrument channel. Standalone previews still use MIDI channels.
    void setHostGlobalSustain (bool enabled) noexcept
    {
        hostGlobalSustain.store(enabled,std::memory_order_relaxed);
        applyHostPedal();
    }
    void observeHost (const juce::MidiMessage& m,bool invertPedal=false) noexcept
    {
        if(m.isController() && m.getControllerNumber()==64)
        {
            hostPedalDown.store((m.getControllerValue()>=64)!=invertPedal,std::memory_order_relaxed);
            applyHostPedal();
        }
        else if(m.isController() && m.getControllerNumber()==121)
        {
            hostPedalDown.store(false,std::memory_order_relaxed);applyHostPedal();
        }
        else if(m.isAllNotesOff() || m.isAllSoundOff())reset();
        else observe(m);
    }
    void observe (const juce::MidiMessage& m) noexcept
    {
        const int ch = m.getChannel();
        if (ch < 1 || ch > 16) return;
        const uint32_t bit = uint32_t (1) << (ch - 1);
        if (m.isNoteOn())
        {
            channels[(size_t) m.getNoteNumber()].fetch_or (bit, std::memory_order_relaxed);
            sounding[(size_t) m.getNoteNumber()].fetch_or (bit, std::memory_order_relaxed);
            strikes[(size_t) m.getNoteNumber()].fetch_add (1, std::memory_order_relaxed);
        }
        else if (m.isNoteOff())
        {
            channels[(size_t) m.getNoteNumber()].fetch_and (~bit, std::memory_order_relaxed);
            if ((sustainChannels.load (std::memory_order_relaxed) & bit) == 0)
                sounding[(size_t) m.getNoteNumber()].fetch_and (~bit, std::memory_order_relaxed);
        }
        else if (m.isController() && (m.getControllerNumber()==64 || m.getControllerNumber()==121))
        {
            if (m.getControllerNumber()==64 && m.getControllerValue()>=64)
                sustainChannels.fetch_or (bit, std::memory_order_relaxed);
            else
            {
                sustainChannels.fetch_and (~bit, std::memory_order_relaxed);
                for (size_t n=0;n<sounding.size();++n)
                    if ((channels[n].load (std::memory_order_relaxed)&bit)==0)
                        sounding[n].fetch_and (~bit, std::memory_order_relaxed);
            }
        }
        else if (m.isAllNotesOff() || m.isAllSoundOff())
        {
            for (auto& n : channels) n.fetch_and (~bit, std::memory_order_relaxed);
            for (auto& n : sounding) n.fetch_and (~bit, std::memory_order_relaxed);
        }
    }
    void reset() noexcept
    {
        for (auto& n : channels) n.store (0, std::memory_order_relaxed);
        for (auto& n : sounding) n.store (0, std::memory_order_relaxed);
        sustainChannels.store (0, std::memory_order_relaxed);
        hostPedalDown.store (false, std::memory_order_relaxed);
    }
private:
    void applyHostPedal() noexcept
    {
        const bool down=hostPedalDown.load(std::memory_order_relaxed) || hostGlobalSustain.load(std::memory_order_relaxed);
        const auto before=sustainChannels.exchange(down?0xffffu:0u,std::memory_order_relaxed);
        if(!down && before!=0)
            for(size_t n=0;n<sounding.size();++n)
                sounding[n].fetch_and(channels[n].load(std::memory_order_relaxed),std::memory_order_relaxed);
    }
    std::atomic<bool> hostPedalDown{false},hostGlobalSustain{false};
};
