#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace churchorgan
{
    enum class OrganParameterId : std::uint16_t
    {
        family,
        quality,
        masterGainDb,
        expression,

        upper16,
        upper5AndOneThird,
        upper8,
        upper4,
        upper2AndTwoThirds,
        upper2,
        upper1AndThreeFifths,
        upper1AndOneThird,
        upper1,

        lower16,
        lower5AndOneThird,
        lower8,
        lower4,
        lower2AndTwoThirds,
        lower2,
        lower1AndThreeFifths,
        lower1AndOneThird,
        lower1,

        pedal16,
        pedal8,
        pedalMode,
        manualSplitNote,

        percussionEnabled,
        percussionHarmonic,
        percussionVolume,
        percussionDecay,
        percussionRetrigger,

        scannerMode,
        scannerMix,

        tonewheelProfile,
        age,
        leakage,
        crosstalk,
        hum,
        contactSpread,
        keyClick,
        transformer,
        drive,
        ampProfile,
        tone,

        rotaryMode,
        rotaryMix,
        rotaryHornLevel,
        rotaryDrumLevel,
        rotaryCrossoverHz,
        rotaryMicDistance,
        rotaryMicAngle,
        rotaryStereoWidth,
        rotaryRoom,

        worshipIntensity,
        worshipPad,
        worshipAir,
        worshipStrings,
        worshipShimmer,
        worshipDucking,
        worshipAutoMix,
        worshipWidth,
        worshipAttackMs,
        worshipReleaseMs,

        reverbAmount,

        // Shared performance envelope published by the host channel. A value
        // of zero means "retain the native family articulation"; non-zero
        // values are applied by the real per-note envelopes of every family.
        performanceAttackMs,
        performanceReleaseMs,
        count
    };

    enum class OrganParameterKind : std::uint8_t
    {
        continuous,
        boolean,
        choice
    };

    struct OrganParameterDescriptor
    {
        OrganParameterId id {};
        const char* stableId = "";
        const char* shortLabel = "";
        const char* unit = "";
        float minimum = 0.0f;
        float maximum = 1.0f;
        float defaultValue = 0.0f;
        float step = 0.0f;
        float smoothingMilliseconds = 0.0f;
        OrganParameterKind kind = OrganParameterKind::continuous;
    };

    inline constexpr std::size_t organParameterCount =
        static_cast<std::size_t> (OrganParameterId::count);

    struct OrganParameterSnapshot
    {
        std::array<float, organParameterCount> values {};

        float get (OrganParameterId id) const noexcept
        {
            return values[static_cast<std::size_t> (id)];
        }
    };

    const std::array<OrganParameterDescriptor, organParameterCount>&
        organParameterCatalog() noexcept;

    const OrganParameterDescriptor& descriptor (
        OrganParameterId id) noexcept;

    float sanitiseOrganParameter (
        OrganParameterId id,
        float plainValue) noexcept;

    float organParameterToNormalised (
        OrganParameterId id,
        float plainValue) noexcept;

    float organParameterFromNormalised (
        OrganParameterId id,
        float normalisedValue) noexcept;

    // Ponte de parâmetros independente da UI. Cada parâmetro é deliberadamente
    // atômico e independente: o callback recebe um snapshot válido sem mutex,
    // alocação ou seqlock com payload não-atômico.
    class OrganParameterStore
    {
    public:
        static_assert (
            std::atomic<std::uint32_t>::is_always_lock_free,
            "Church Organ parameters require lock-free 32-bit atomics");

        OrganParameterStore() noexcept;

        OrganParameterStore (const OrganParameterStore&) = delete;
        OrganParameterStore& operator= (const OrganParameterStore&) = delete;

        void resetToDefaults() noexcept;
        void setPlain (OrganParameterId id, float value) noexcept;
        void setNormalised (OrganParameterId id, float value) noexcept;

        float getPlain (OrganParameterId id) const noexcept;
        float getNormalised (OrganParameterId id) const noexcept;

        OrganParameterSnapshot snapshot() const noexcept;
        std::uint32_t revision() const noexcept;

    private:
        static std::uint32_t floatBits (float value) noexcept;
        static float bitsFloat (std::uint32_t value) noexcept;

        std::array<std::atomic<std::uint32_t>, organParameterCount> values_ {};
        std::atomic<std::uint32_t> revision_ { 0 };
    };
}
