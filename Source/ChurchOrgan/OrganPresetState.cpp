#include "OrganPresetState.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace churchorgan
{
    namespace
    {
        constexpr std::array<std::uint8_t, 8> packetMagic {{
            'C', 'H', 'O', 'R', 'G', 'N', '0', '1'
        }};

        void writeU16 (
            std::uint8_t* destination,
            std::uint16_t value) noexcept
        {
            destination[0] = static_cast<std::uint8_t> (value);
            destination[1] = static_cast<std::uint8_t> (value >> 8);
        }

        void writeU32 (
            std::uint8_t* destination,
            std::uint32_t value) noexcept
        {
            destination[0] = static_cast<std::uint8_t> (value);
            destination[1] = static_cast<std::uint8_t> (value >> 8);
            destination[2] = static_cast<std::uint8_t> (value >> 16);
            destination[3] = static_cast<std::uint8_t> (value >> 24);
        }

        void writeU64 (
            std::uint8_t* destination,
            std::uint64_t value) noexcept
        {
            for (std::size_t byte = 0; byte < 8; ++byte)
                destination[byte] = static_cast<std::uint8_t> (
                    value >> (byte * 8));
        }

        std::uint16_t readU16 (const std::uint8_t* source) noexcept
        {
            return static_cast<std::uint16_t> (source[0])
                   | static_cast<std::uint16_t> (
                       static_cast<std::uint16_t> (source[1]) << 8);
        }

        std::uint32_t readU32 (const std::uint8_t* source) noexcept
        {
            return static_cast<std::uint32_t> (source[0])
                   | (static_cast<std::uint32_t> (source[1]) << 8)
                   | (static_cast<std::uint32_t> (source[2]) << 16)
                   | (static_cast<std::uint32_t> (source[3]) << 24);
        }

        std::uint64_t readU64 (const std::uint8_t* source) noexcept
        {
            std::uint64_t result = 0;

            for (std::size_t byte = 0; byte < 8; ++byte)
                result |= static_cast<std::uint64_t> (source[byte])
                          << (byte * 8);

            return result;
        }

        std::uint32_t floatBits (float value) noexcept
        {
            std::uint32_t bits = 0;
            static_assert (sizeof (bits) == sizeof (value),
                           "float must be 32 bit");
            std::memcpy (&bits, &value, sizeof (bits));
            return bits;
        }

        float bitsFloat (std::uint32_t value) noexcept
        {
            float result = 0.0f;
            std::memcpy (&result, &value, sizeof (result));
            return result;
        }

        bool validPresetCharacter (unsigned char character) noexcept
        {
            return character == 0
                   || (character >= 0x20 && character <= 0x7e);
        }
    }

    void OrganPresetState::resetToDefaults() noexcept
    {
        schemaVersion = organStateSchemaVersion;
        engineRevision = organEngineRevision;
        deterministicSeed = 0x4348555243484f52ULL;
        transitionFlags =
            OrganTransitionFlag::keepRotary
            | OrganTransitionFlag::keepExpression
            | OrganTransitionFlag::tailsOn;
        presetId.fill (0);

        const auto& catalog = organParameterCatalog();
        for (std::size_t i = 0; i < catalog.size(); ++i)
            parameters.values[i] = catalog[i].defaultValue;
    }

    void OrganPresetState::setPresetId (
        const char* stableAsciiId) noexcept
    {
        presetId.fill (0);

        if (stableAsciiId == nullptr)
            return;

        for (std::size_t i = 0; i + 1 < presetId.size(); ++i)
        {
            const auto character =
                static_cast<unsigned char> (stableAsciiId[i]);

            if (character == 0)
                break;

            if (character < 0x20 || character > 0x7e)
                continue;

            presetId[i] = static_cast<char> (character);
        }
    }

    OrganStateEncodeResult OrganPresetStateCodec::encode (
        const OrganPresetState& state,
        std::uint8_t* destination,
        std::size_t capacity) noexcept
    {
        OrganStateEncodeResult result;
        const auto packetSize = currentPacketSize();

        if (destination == nullptr || capacity < packetSize)
            return result;

        std::fill (destination, destination + packetSize, std::uint8_t { 0 });
        std::copy (
            packetMagic.begin(), packetMagic.end(), destination);

        writeU16 (destination + 8, organStateSchemaVersion);
        writeU16 (
            destination + 10,
            static_cast<std::uint16_t> (headerBytes));
        writeU32 (
            destination + 12,
            static_cast<std::uint32_t> (packetSize));
        writeU32 (
            destination + 16,
            static_cast<std::uint32_t> (organParameterCount));
        writeU32 (destination + 20, state.transitionFlags);
        writeU64 (
            destination + 24,
            state.deterministicSeed != 0
                ? state.deterministicSeed
                : 0x4348555243484f52ULL);

        auto* presetDestination = destination + headerBytes;
        for (std::size_t i = 0; i < state.presetId.size(); ++i)
        {
            const auto character =
                static_cast<unsigned char> (state.presetId[i]);
            presetDestination[i] =
                validPresetCharacter (character) ? character : 0;
        }
        presetDestination[maximumOrganPresetIdBytes - 1] = 0;

        auto* parameterDestination =
            presetDestination + maximumOrganPresetIdBytes;

        for (std::size_t i = 0; i < organParameterCount; ++i)
        {
            const auto id = static_cast<OrganParameterId> (i);
            writeU32 (
                parameterDestination + i * sizeof (std::uint32_t),
                floatBits (sanitiseOrganParameter (
                    id, state.parameters.values[i])));
        }

        const auto crc = checksum (
            destination, packetSize - checksumBytes);
        writeU32 (destination + packetSize - checksumBytes, crc);

        result.ok = true;
        result.bytesWritten = packetSize;
        return result;
    }

    OrganStateDecodeResult OrganPresetStateCodec::decode (
        const std::uint8_t* source,
        std::size_t size,
        OrganPresetState& destination) noexcept
    {
        if (source == nullptr)
            return OrganStateDecodeResult::invalidArgument;

        if (size < headerBytes + maximumOrganPresetIdBytes + checksumBytes)
            return OrganStateDecodeResult::truncated;

        if (! std::equal (
                packetMagic.begin(), packetMagic.end(), source))
            return OrganStateDecodeResult::invalidMagic;

        const auto schema = readU16 (source + 8);
        if (schema != organStateSchemaVersion)
            return OrganStateDecodeResult::unsupportedSchema;

        const auto encodedHeaderBytes =
            static_cast<std::size_t> (readU16 (source + 10));
        const auto totalBytes =
            static_cast<std::size_t> (readU32 (source + 12));
        const auto encodedParameterCount =
            static_cast<std::size_t> (readU32 (source + 16));

        if (encodedHeaderBytes < headerBytes
             || totalBytes > maximumOrganStatePacketBytes
             || totalBytes > size
             || totalBytes < encodedHeaderBytes
                                + maximumOrganPresetIdBytes
                                + checksumBytes
             || encodedParameterCount
                    > (maximumOrganStatePacketBytes / sizeof (std::uint32_t))
             || encodedHeaderBytes
                    + maximumOrganPresetIdBytes
                    + encodedParameterCount * sizeof (std::uint32_t)
                    + checksumBytes
                    > totalBytes)
            return OrganStateDecodeResult::invalidLayout;

        const auto expectedChecksum =
            readU32 (source + totalBytes - checksumBytes);
        const auto actualChecksum =
            checksum (source, totalBytes - checksumBytes);

        if (expectedChecksum != actualChecksum)
            return OrganStateDecodeResult::checksumMismatch;

        OrganPresetState decoded;
        decoded.resetToDefaults();
        decoded.schemaVersion = schema;
        decoded.engineRevision = organEngineRevision;
        decoded.transitionFlags = readU32 (source + 20)
                                  & (OrganTransitionFlag::keepRotary
                                     | OrganTransitionFlag::keepExpression
                                     | OrganTransitionFlag::tailsOn);
        decoded.deterministicSeed = readU64 (source + 24);
        if (decoded.deterministicSeed == 0)
            decoded.deterministicSeed = 0x4348555243484f52ULL;

        const auto* presetSource = source + encodedHeaderBytes;
        std::size_t presetWrite = 0;

        for (std::size_t i = 0;
             i < maximumOrganPresetIdBytes && presetWrite + 1
                                               < decoded.presetId.size();
             ++i)
        {
            const auto character = presetSource[i];
            if (character == 0)
                break;

            if (character >= 0x20 && character <= 0x7e)
                decoded.presetId[presetWrite++] =
                    static_cast<char> (character);
        }
        decoded.presetId[presetWrite] = 0;

        const auto* parameterSource =
            presetSource + maximumOrganPresetIdBytes;
        const auto parametersToRead = std::min (
            organParameterCount, encodedParameterCount);

        for (std::size_t i = 0; i < parametersToRead; ++i)
        {
            const auto id = static_cast<OrganParameterId> (i);
            decoded.parameters.values[i] =
                sanitiseOrganParameter (
                    id,
                    bitsFloat (readU32 (
                        parameterSource + i * sizeof (std::uint32_t))));
        }

        destination = decoded;
        return OrganStateDecodeResult::ok;
    }

    std::uint32_t OrganPresetStateCodec::checksum (
        const std::uint8_t* data,
        std::size_t size) noexcept
    {
        if (data == nullptr)
            return 0;

        std::uint32_t crc = 0xffffffffu;

        for (std::size_t i = 0; i < size; ++i)
        {
            crc ^= data[i];

            for (int bit = 0; bit < 8; ++bit)
            {
                const auto mask =
                    static_cast<std::uint32_t> (
                        -static_cast<std::int32_t> (crc & 1u));
                crc = (crc >> 1) ^ (0xedb88320u & mask);
            }
        }

        return ~crc;
    }
}
