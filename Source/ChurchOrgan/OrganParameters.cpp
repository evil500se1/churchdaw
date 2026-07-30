#include "OrganParameters.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace churchorgan
{
    namespace
    {
        constexpr OrganParameterDescriptor continuous (
            OrganParameterId id,
            const char* stableId,
            const char* label,
            const char* unit,
            float minimum,
            float maximum,
            float defaultValue,
            float smoothingMs,
            float step = 0.0f) noexcept
        {
            return {
                id, stableId, label, unit,
                minimum, maximum, defaultValue, step, smoothingMs,
                OrganParameterKind::continuous
            };
        }

        constexpr OrganParameterDescriptor boolean (
            OrganParameterId id,
            const char* stableId,
            const char* label,
            bool defaultValue) noexcept
        {
            return {
                id, stableId, label, "",
                0.0f, 1.0f, defaultValue ? 1.0f : 0.0f, 1.0f, 0.0f,
                OrganParameterKind::boolean
            };
        }

        constexpr OrganParameterDescriptor choice (
            OrganParameterId id,
            const char* stableId,
            const char* label,
            float maximum,
            float defaultValue) noexcept
        {
            return {
                id, stableId, label, "",
                0.0f, maximum, defaultValue, 1.0f, 0.0f,
                OrganParameterKind::choice
            };
        }

        constexpr std::array<OrganParameterDescriptor, organParameterCount>
            catalog {{
                choice (OrganParameterId::family,
                        "organ.family", "FAMILY", 3.0f, 0.0f),
                choice (OrganParameterId::quality,
                        "organ.quality", "QUALITY", 2.0f, 1.0f),
                continuous (OrganParameterId::masterGainDb,
                            "organ.master.gain", "MASTER", "dB",
                            -24.0f, 6.0f, -6.0f, 18.0f),
                continuous (OrganParameterId::expression,
                            "organ.expression", "EXPRESSION", "%",
                            0.0f, 1.0f, 1.0f, 12.0f),

                continuous (OrganParameterId::upper16,
                            "organ.drawbar.upper.16", "16'", "",
                            0.0f, 8.0f, 6.0f, 18.0f, 1.0f),
                continuous (OrganParameterId::upper5AndOneThird,
                            "organ.drawbar.upper.5_1_3", "5 1/3'", "",
                            0.0f, 8.0f, 0.0f, 18.0f, 1.0f),
                continuous (OrganParameterId::upper8,
                            "organ.drawbar.upper.8", "8'", "",
                            0.0f, 8.0f, 8.0f, 18.0f, 1.0f),
                continuous (OrganParameterId::upper4,
                            "organ.drawbar.upper.4", "4'", "",
                            0.0f, 8.0f, 4.0f, 18.0f, 1.0f),
                continuous (OrganParameterId::upper2AndTwoThirds,
                            "organ.drawbar.upper.2_2_3", "2 2/3'", "",
                            0.0f, 8.0f, 0.0f, 18.0f, 1.0f),
                continuous (OrganParameterId::upper2,
                            "organ.drawbar.upper.2", "2'", "",
                            0.0f, 8.0f, 0.0f, 18.0f, 1.0f),
                continuous (OrganParameterId::upper1AndThreeFifths,
                            "organ.drawbar.upper.1_3_5", "1 3/5'", "",
                            0.0f, 8.0f, 0.0f, 18.0f, 1.0f),
                continuous (OrganParameterId::upper1AndOneThird,
                            "organ.drawbar.upper.1_1_3", "1 1/3'", "",
                            0.0f, 8.0f, 0.0f, 18.0f, 1.0f),
                continuous (OrganParameterId::upper1,
                            "organ.drawbar.upper.1", "1'", "",
                            0.0f, 8.0f, 0.0f, 18.0f, 1.0f),

                continuous (OrganParameterId::lower16,
                            "organ.drawbar.lower.16", "16'", "",
                            0.0f, 8.0f, 0.0f, 18.0f, 1.0f),
                continuous (OrganParameterId::lower5AndOneThird,
                            "organ.drawbar.lower.5_1_3", "5 1/3'", "",
                            0.0f, 8.0f, 0.0f, 18.0f, 1.0f),
                continuous (OrganParameterId::lower8,
                            "organ.drawbar.lower.8", "8'", "",
                            0.0f, 8.0f, 8.0f, 18.0f, 1.0f),
                continuous (OrganParameterId::lower4,
                            "organ.drawbar.lower.4", "4'", "",
                            0.0f, 8.0f, 4.0f, 18.0f, 1.0f),
                continuous (OrganParameterId::lower2AndTwoThirds,
                            "organ.drawbar.lower.2_2_3", "2 2/3'", "",
                            0.0f, 8.0f, 0.0f, 18.0f, 1.0f),
                continuous (OrganParameterId::lower2,
                            "organ.drawbar.lower.2", "2'", "",
                            0.0f, 8.0f, 0.0f, 18.0f, 1.0f),
                continuous (OrganParameterId::lower1AndThreeFifths,
                            "organ.drawbar.lower.1_3_5", "1 3/5'", "",
                            0.0f, 8.0f, 0.0f, 18.0f, 1.0f),
                continuous (OrganParameterId::lower1AndOneThird,
                            "organ.drawbar.lower.1_1_3", "1 1/3'", "",
                            0.0f, 8.0f, 0.0f, 18.0f, 1.0f),
                continuous (OrganParameterId::lower1,
                            "organ.drawbar.lower.1", "1'", "",
                            0.0f, 8.0f, 0.0f, 18.0f, 1.0f),

                continuous (OrganParameterId::pedal16,
                            "organ.drawbar.pedal.16", "PEDAL 16'", "",
                            0.0f, 8.0f, 8.0f, 18.0f, 1.0f),
                continuous (OrganParameterId::pedal8,
                            "organ.drawbar.pedal.8", "PEDAL 8'", "",
                            0.0f, 8.0f, 4.0f, 18.0f, 1.0f),
                choice (OrganParameterId::pedalMode,
                        "organ.pedal.mode", "PEDAL MODE",
                        1.0f, 0.0f),
                continuous (OrganParameterId::manualSplitNote,
                            "organ.manual.split_note", "SPLIT", "MIDI",
                            24.0f, 96.0f, 60.0f, 0.0f, 1.0f),

                boolean (OrganParameterId::percussionEnabled,
                         "organ.percussion.enabled", "PERC", false),
                choice (OrganParameterId::percussionHarmonic,
                        "organ.percussion.harmonic", "HARMONIC",
                        1.0f, 1.0f),
                choice (OrganParameterId::percussionVolume,
                        "organ.percussion.volume", "VOLUME",
                        1.0f, 0.0f),
                choice (OrganParameterId::percussionDecay,
                        "organ.percussion.decay", "DECAY",
                        1.0f, 0.0f),
                choice (OrganParameterId::percussionRetrigger,
                        "organ.percussion.retrigger", "RETRIGGER",
                        2.0f, 0.0f),

                choice (OrganParameterId::scannerMode,
                        "organ.scanner.mode", "SCANNER",
                        6.0f, 0.0f),
                continuous (OrganParameterId::scannerMix,
                            "organ.scanner.mix", "SCANNER MIX", "%",
                            0.0f, 1.0f, 0.5f, 24.0f),

                choice (OrganParameterId::tonewheelProfile,
                        "organ.tonewheel.profile", "PROFILE",
                        3.0f, 0.0f),
                continuous (OrganParameterId::age,
                            "organ.character.age", "AGE", "%",
                            0.0f, 1.0f, 0.12f, 40.0f),
                continuous (OrganParameterId::leakage,
                            "organ.character.leakage", "LEAKAGE", "%",
                            0.0f, 1.0f, 0.08f, 40.0f),
                continuous (OrganParameterId::crosstalk,
                            "organ.character.crosstalk", "CROSSTALK", "%",
                            0.0f, 1.0f, 0.05f, 40.0f),
                continuous (OrganParameterId::hum,
                            "organ.character.hum", "HUM", "%",
                            0.0f, 1.0f, 0.0f, 40.0f),
                continuous (OrganParameterId::contactSpread,
                            "organ.contacts.spread", "CONTACT", "%",
                            0.0f, 1.0f, 0.28f, 18.0f),
                continuous (OrganParameterId::keyClick,
                            "organ.contacts.click", "CLICK", "%",
                            0.0f, 1.0f, 0.34f, 18.0f),
                continuous (OrganParameterId::transformer,
                            "organ.electronics.transformer", "TRANSFORMER", "%",
                            0.0f, 1.0f, 0.32f, 32.0f),
                continuous (OrganParameterId::drive,
                            "organ.drive.amount", "DRIVE", "%",
                            0.0f, 1.0f, 0.18f, 22.0f),
                choice (OrganParameterId::ampProfile,
                        "organ.amp.profile", "AMP",
                        4.0f, 0.0f),
                continuous (OrganParameterId::tone,
                            "organ.amp.tone", "TONE", "%",
                            0.0f, 1.0f, 0.52f, 32.0f),

                choice (OrganParameterId::rotaryMode,
                        "organ.rotary.mode", "ROTARY",
                        3.0f, 1.0f),
                continuous (OrganParameterId::rotaryMix,
                            "organ.rotary.mix", "ROTARY MIX", "%",
                            0.0f, 1.0f, 0.82f, 24.0f),
                continuous (OrganParameterId::rotaryHornLevel,
                            "organ.rotary.horn.level", "HORN", "dB",
                            -18.0f, 6.0f, 0.0f, 30.0f),
                continuous (OrganParameterId::rotaryDrumLevel,
                            "organ.rotary.drum.level", "DRUM", "dB",
                            -18.0f, 6.0f, 0.0f, 30.0f),
                continuous (OrganParameterId::rotaryCrossoverHz,
                            "organ.rotary.crossover", "CROSSOVER", "Hz",
                            420.0f, 1200.0f, 760.0f, 50.0f),
                continuous (OrganParameterId::rotaryMicDistance,
                            "organ.rotary.mic.distance", "DISTANCE", "%",
                            0.0f, 1.0f, 0.38f, 50.0f),
                continuous (OrganParameterId::rotaryMicAngle,
                            "organ.rotary.mic.angle", "ANGLE", "deg",
                            -90.0f, 90.0f, 28.0f, 50.0f),
                continuous (OrganParameterId::rotaryStereoWidth,
                            "organ.rotary.stereo.width", "WIDTH", "%",
                            0.0f, 1.0f, 0.72f, 50.0f),
                continuous (OrganParameterId::rotaryRoom,
                            "organ.rotary.room", "ROOM", "%",
                            0.0f, 1.0f, 0.16f, 60.0f),

                continuous (OrganParameterId::worshipIntensity,
                            "organ.worship.intensity", "INTENSITY", "%",
                            0.0f, 1.0f, 0.18f, 70.0f),
                continuous (OrganParameterId::worshipPad,
                            "organ.worship.pad", "PAD", "%",
                            0.0f, 1.0f, 0.35f, 80.0f),
                continuous (OrganParameterId::worshipAir,
                            "organ.worship.air", "AIR", "%",
                            0.0f, 1.0f, 0.20f, 80.0f),
                continuous (OrganParameterId::worshipStrings,
                            "organ.worship.strings", "STRINGS", "%",
                            0.0f, 1.0f, 0.18f, 80.0f),
                continuous (OrganParameterId::worshipShimmer,
                            "organ.worship.shimmer", "SHIMMER", "%",
                            0.0f, 1.0f, 0.08f, 100.0f),
                continuous (OrganParameterId::worshipDucking,
                            "organ.worship.ducking", "DUCKING", "%",
                            0.0f, 1.0f, 0.24f, 80.0f),
                continuous (OrganParameterId::worshipAutoMix,
                            "organ.worship.auto_mix", "AUTO MIX", "%",
                            0.0f, 1.0f, 0.24f, 120.0f),
                continuous (OrganParameterId::worshipWidth,
                            "organ.worship.width", "WIDTH", "%",
                            0.0f, 1.0f, 0.68f, 80.0f),
                continuous (OrganParameterId::worshipAttackMs,
                            "organ.worship.attack", "ATTACK", "ms",
                            20.0f, 4000.0f, 850.0f, 100.0f),
                continuous (OrganParameterId::worshipReleaseMs,
                            "organ.worship.release", "RELEASE", "ms",
                            100.0f, 12000.0f, 4200.0f, 100.0f),

                continuous (OrganParameterId::reverbAmount,
                            "organ.reverb.amount", "REVERB", "%",
                            0.0f, 1.0f, 0.22f, 80.0f),

                continuous (OrganParameterId::performanceAttackMs,
                            "organ.performance.attack", "ATTACK", "ms",
                            0.0f, 2000.0f, 0.0f, 40.0f),
                continuous (OrganParameterId::performanceReleaseMs,
                            "organ.performance.release", "RELEASE", "ms",
                            0.0f, 12000.0f, 0.0f, 60.0f)
            }};

        constexpr bool catalogIdsMatchIndices() noexcept
        {
            for (std::size_t i = 0; i < catalog.size(); ++i)
                if (static_cast<std::size_t> (catalog[i].id) != i)
                    return false;

            return true;
        }

        static_assert (
            catalogIdsMatchIndices(),
            "Organ parameter catalog must follow the enum order");

        float clamp01 (float value) noexcept
        {
            if (! std::isfinite (value))
                return 0.0f;

            return std::max (0.0f, std::min (1.0f, value));
        }
    }

    const std::array<OrganParameterDescriptor, organParameterCount>&
        organParameterCatalog() noexcept
    {
        return catalog;
    }

    const OrganParameterDescriptor& descriptor (
        OrganParameterId id) noexcept
    {
        const auto index = static_cast<std::size_t> (id);
        return catalog[index < catalog.size() ? index : 0];
    }

    float sanitiseOrganParameter (
        OrganParameterId id,
        float plainValue) noexcept
    {
        const auto& d = descriptor (id);

        if (! std::isfinite (plainValue))
            return d.defaultValue;

        auto result = std::max (d.minimum, std::min (d.maximum, plainValue));

        if (d.kind != OrganParameterKind::continuous && d.step > 0.0f)
            result = d.minimum
                   + std::round ((result - d.minimum) / d.step) * d.step;

        return std::max (d.minimum, std::min (d.maximum, result));
    }

    float organParameterToNormalised (
        OrganParameterId id,
        float plainValue) noexcept
    {
        const auto& d = descriptor (id);
        const auto range = d.maximum - d.minimum;

        if (range <= 0.0f)
            return 0.0f;

        return clamp01 (
            (sanitiseOrganParameter (id, plainValue) - d.minimum) / range);
    }

    float organParameterFromNormalised (
        OrganParameterId id,
        float normalisedValue) noexcept
    {
        const auto& d = descriptor (id);
        return sanitiseOrganParameter (
            id,
            d.minimum + clamp01 (normalisedValue) * (d.maximum - d.minimum));
    }

    OrganParameterStore::OrganParameterStore() noexcept
    {
        resetToDefaults();
    }

    void OrganParameterStore::resetToDefaults() noexcept
    {
        for (std::size_t i = 0; i < values_.size(); ++i)
            values_[i].store (
                floatBits (catalog[i].defaultValue),
                std::memory_order_relaxed);

        revision_.fetch_add (1, std::memory_order_release);
    }

    void OrganParameterStore::setPlain (
        OrganParameterId id,
        float value) noexcept
    {
        const auto index = static_cast<std::size_t> (id);

        if (index >= values_.size())
            return;

        const auto bits =
            floatBits (sanitiseOrganParameter (id, value));
        const auto previous = values_[index].exchange (
            bits, std::memory_order_relaxed);
        if (previous != bits)
            revision_.fetch_add (1, std::memory_order_release);
    }

    void OrganParameterStore::setNormalised (
        OrganParameterId id,
        float value) noexcept
    {
        setPlain (id, organParameterFromNormalised (id, value));
    }

    float OrganParameterStore::getPlain (
        OrganParameterId id) const noexcept
    {
        const auto index = static_cast<std::size_t> (id);

        if (index >= values_.size())
            return descriptor (OrganParameterId::family).defaultValue;

        return bitsFloat (
            values_[index].load (std::memory_order_relaxed));
    }

    float OrganParameterStore::getNormalised (
        OrganParameterId id) const noexcept
    {
        return organParameterToNormalised (id, getPlain (id));
    }

    OrganParameterSnapshot OrganParameterStore::snapshot() const noexcept
    {
        OrganParameterSnapshot result;

        for (std::size_t i = 0; i < values_.size(); ++i)
            result.values[i] = bitsFloat (
                values_[i].load (std::memory_order_relaxed));

        return result;
    }

    std::uint32_t OrganParameterStore::revision() const noexcept
    {
        return revision_.load (std::memory_order_acquire);
    }

    std::uint32_t OrganParameterStore::floatBits (float value) noexcept
    {
        std::uint32_t bits = 0;
        static_assert (sizeof (bits) == sizeof (value), "float size mismatch");
        std::memcpy (&bits, &value, sizeof (bits));
        return bits;
    }

    float OrganParameterStore::bitsFloat (std::uint32_t value) noexcept
    {
        float result = 0.0f;
        static_assert (sizeof (result) == sizeof (value), "float size mismatch");
        std::memcpy (&result, &value, sizeof (result));
        return result;
    }
}
