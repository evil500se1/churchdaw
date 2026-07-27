#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <thread>

static bool sendMidi (MIDIEndpointRef source, unsigned char status,
                      unsigned char data1, unsigned char data2)
{
    MIDIPacketList packetList {};
    auto* packet = MIDIPacketListInit (&packetList);
    const unsigned char message[] { status, data1, data2 };
    packet = MIDIPacketListAdd (&packetList, sizeof (packetList), packet,
                                0, sizeof (message), message);
    return packet != nullptr && MIDIReceived (source, &packetList) == noErr;
}

int main()
{
    MIDIClientRef client = 0;
    MIDIEndpointRef source = 0;

    if (MIDIClientCreate (CFSTR ("ChurchDAW CI Client"), nullptr, nullptr, &client) != noErr
         || MIDISourceCreate (client, CFSTR ("ChurchDAW CI Source"), &source) != noErr)
    {
        std::fputs ("Nao foi possivel criar a porta CoreMIDI\n", stderr);
        return 1;
    }

    std::puts ("MIDI_HELPER_READY");
    std::fflush (stdout);
    std::this_thread::sleep_for (std::chrono::seconds (2));

    constexpr std::array<std::array<unsigned char, 4>, 4> chords {{
        {{ 48, 60, 64, 67 }},
        {{ 47, 59, 62, 67 }},
        {{ 45, 57, 60, 64 }},
        {{ 41, 53, 57, 60 }}
    }};

    for (int pass = 0; pass < 8; ++pass)
    {
        const auto& chord = chords[(size_t) pass % chords.size()];

        for (const auto note : chord)
            if (! sendMidi (source, 0x90, note, (unsigned char) (58 + pass)))
                return 2;

        std::this_thread::sleep_for (std::chrono::milliseconds (650));

        for (const auto note : chord)
            if (! sendMidi (source, 0x80, note, 0))
                return 3;

        std::this_thread::sleep_for (std::chrono::milliseconds (300));
    }

    std::this_thread::sleep_for (std::chrono::seconds (1));
    MIDIEndpointDispose (source);
    MIDIClientDispose (client);
    return 0;
}
