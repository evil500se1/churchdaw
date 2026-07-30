#pragma once

#include "OrganParameters.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace churchorgan
{
    inline constexpr std::uint16_t organStateSchemaVersion = 1;
    inline constexpr std::uint16_t organEngineRevision = 1;
    inline constexpr std::size_t maximumOrganPresetIdBytes = 64;
    inline constexpr std::size_t maximumOrganStatePacketBytes = 4096;

    enum OrganTransitionFlag : std::uint32_t
    {
        keepRotary = 1u << 0,
        keepExpression = 1u << 1,
        tailsOn = 1u << 2
    };

    struct OrganPresetState
    {
        std::uint16_t schemaVersion = organStateSchemaVersion;
        std::uint16_t engineRevision = organEngineRevision;
        std::uint64_t deterministicSeed = 0x4348555243484f52ULL;
        std::uint32_t transitionFlags =
            OrganTransitionFlag::keepRotary
            | OrganTransitionFlag::keepExpression
            | OrganTransitionFlag::tailsOn;
        std::array<char, maximumOrganPresetIdBytes> presetId {};
        OrganParameterSnapshot parameters {};

        void resetToDefaults() noexcept;
        void setPresetId (const char* stableAsciiId) noexcept;
    };

    enum class OrganStateDecodeResult : std::uint8_t
    {
        ok,
        invalidArgument,
        truncated,
        invalidMagic,
        unsupportedSchema,
        invalidLayout,
        checksumMismatch
    };

    struct OrganStateEncodeResult
    {
        bool ok = false;
        std::size_t bytesWritten = 0;
    };

    class OrganPresetStateCodec
    {
    public:
        static constexpr std::size_t currentPacketSize() noexcept
        {
            return headerBytes
                   + maximumOrganPresetIdBytes
                   + organParameterCount * sizeof (std::uint32_t)
                   + checksumBytes;
        }

        static OrganStateEncodeResult encode (
            const OrganPresetState& state,
            std::uint8_t* destination,
            std::size_t capacity) noexcept;

        static OrganStateDecodeResult decode (
            const std::uint8_t* source,
            std::size_t size,
            OrganPresetState& destination) noexcept;

        static std::uint32_t checksum (
            const std::uint8_t* data,
            std::size_t size) noexcept;

    private:
        static constexpr std::size_t headerBytes = 32;
        static constexpr std::size_t checksumBytes = 4;
    };
}
