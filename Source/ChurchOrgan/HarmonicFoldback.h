#pragma once

#include "ChurchOrganTypes.h"

#include <array>
#include <cstddef>

namespace churchorgan
{
    struct DrawbarDefinition
    {
        const char* label = "";
        int semitoneOffset = 0;
        int minimumWheelMidi = 24;
        int maximumWheelMidi = 119;
    };

    struct FoldbackResult
    {
        int requestedMidi = 0;
        int foldedMidi = 0;
        int wheelIndex = -1;
        int octaveShift = 0;
        bool valid = false;
    };

    // Tabela clean-room ChurchDAW. Ela nao reproduz a matriz de nenhum
    // instrumento comercial: define janelas progressivas dentro de um banco
    // cromatico C0..B7 e dobra somente por oitavas.
    class HarmonicFoldback
    {
    public:
        static constexpr int lowestWheelMidi = 24;
        static constexpr int highestWheelMidi = 119;
        static constexpr int wheelCount =
            highestWheelMidi - lowestWheelMidi + 1;

        static const std::array<DrawbarDefinition, drawbarCount>&
            definitions() noexcept;

        static FoldbackResult map (std::size_t drawbar,
                                   int upperManualMidiNote) noexcept;
    };
}
