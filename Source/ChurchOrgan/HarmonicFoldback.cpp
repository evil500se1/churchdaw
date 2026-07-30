#include "HarmonicFoldback.h"

namespace churchorgan
{
    namespace
    {
        // As janelas foram desenhadas para:
        // 1. manter o banco compartilhado limitado a C0..B7;
        // 2. preservar a classe cromatica ao dobrar;
        // 3. deixar registros agudos dobrarem progressivamente mais cedo;
        // 4. nunca depender de uma tabela historica/proprietaria.
        constexpr std::array<DrawbarDefinition, drawbarCount> table {{
            { "16'",     -12, 24,  95 },
            { "5 1/3'",    7, 31, 102 },
            { "8'",        0, 24, 107 },
            { "4'",       12, 36, 107 },
            { "2 2/3'",   19, 43, 110 },
            { "2'",       24, 48, 111 },
            { "1 3/5'",   28, 52, 112 },
            { "1 1/3'",   31, 55, 114 },
            { "1'",       36, 60, 119 }
        }};
    }

    const std::array<DrawbarDefinition, drawbarCount>&
    HarmonicFoldback::definitions() noexcept
    {
        return table;
    }

    FoldbackResult HarmonicFoldback::map (
        std::size_t drawbar,
        int upperManualMidiNote) noexcept
    {
        FoldbackResult result;

        if (drawbar >= table.size()
             || upperManualMidiNote < 0
             || upperManualMidiNote >= static_cast<int> (midiNoteCount))
            return result;

        const auto& definition = table[drawbar];
        result.requestedMidi =
            upperManualMidiNote + definition.semitoneOffset;
        result.foldedMidi = result.requestedMidi;

        while (result.foldedMidi < definition.minimumWheelMidi)
            result.foldedMidi += 12;

        while (result.foldedMidi > definition.maximumWheelMidi)
            result.foldedMidi -= 12;

        if (result.foldedMidi < definition.minimumWheelMidi
             || result.foldedMidi > definition.maximumWheelMidi
             || result.foldedMidi < lowestWheelMidi
             || result.foldedMidi > highestWheelMidi)
            return result;

        result.wheelIndex = result.foldedMidi - lowestWheelMidi;
        result.octaveShift =
            (result.foldedMidi - result.requestedMidi) / 12;
        result.valid = true;
        return result;
    }
}
