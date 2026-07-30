#include "OrganPresetState.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <new>

namespace
{
    std::atomic<std::uint64_t> allocationCount { 0 };
    std::size_t checks = 0;

    void require (bool condition, const char* message)
    {
        ++checks;
        if (! condition)
        {
            std::cerr << "FAIL: " << message << '\n';
            std::exit (1);
        }
    }

    bool approximatelyEqual (
        float a, float b, float tolerance = 1.0e-6f)
    {
        return std::abs (a - b) <= tolerance;
    }

    void writeU32LittleEndian (
        std::uint8_t* destination,
        std::uint32_t value) noexcept
    {
        destination[0] = static_cast<std::uint8_t> (value);
        destination[1] = static_cast<std::uint8_t> (value >> 8);
        destination[2] = static_cast<std::uint8_t> (value >> 16);
        destination[3] = static_cast<std::uint8_t> (value >> 24);
    }
}

void* operator new (std::size_t size)
{
    allocationCount.fetch_add (1, std::memory_order_relaxed);
    if (auto* result = std::malloc (size))
        return result;
    throw std::bad_alloc();
}

void* operator new[] (std::size_t size)
{
    return ::operator new (size);
}

void operator delete (void* memory) noexcept
{
    std::free (memory);
}

void operator delete[] (void* memory) noexcept
{
    std::free (memory);
}

void operator delete (void* memory, std::size_t) noexcept
{
    std::free (memory);
}

void operator delete[] (void* memory, std::size_t) noexcept
{
    std::free (memory);
}

int main()
{
    using namespace churchorgan;

    static_assert (
        OrganPresetStateCodec::currentPacketSize()
            <= maximumOrganStatePacketBytes,
        "state packet exceeds hard limit");

    OrganPresetState source;
    source.resetToDefaults();
    source.setPresetId ("churchdaw.factory.open-worship");
    source.deterministicSeed = 0x123456789abcdef0ULL;
    source.transitionFlags = OrganTransitionFlag::keepRotary
                             | OrganTransitionFlag::tailsOn;

    for (std::size_t i = 0; i < organParameterCount; ++i)
    {
        const auto id = static_cast<OrganParameterId> (i);
        source.parameters.values[i] =
            organParameterFromNormalised (
                id,
                static_cast<float> ((i * 37) % 101) / 100.0f);
    }

    std::array<std::uint8_t, maximumOrganStatePacketBytes> packet {};
    const auto encoded = OrganPresetStateCodec::encode (
        source, packet.data(), packet.size());
    require (encoded.ok, "encode succeeds");
    require (
        encoded.bytesWritten
            == OrganPresetStateCodec::currentPacketSize(),
        "packet size");

    OrganPresetState restored;
    const auto decoded = OrganPresetStateCodec::decode (
        packet.data(), encoded.bytesWritten, restored);
    require (decoded == OrganStateDecodeResult::ok, "decode succeeds");
    require (restored.deterministicSeed == source.deterministicSeed,
             "seed round trip");
    require (restored.transitionFlags == source.transitionFlags,
             "flags round trip");
    require (
        std::strcmp (restored.presetId.data(), source.presetId.data()) == 0,
        "preset id round trip");

    for (std::size_t i = 0; i < organParameterCount; ++i)
        require (
            approximatelyEqual (
                restored.parameters.values[i],
                sanitiseOrganParameter (
                    static_cast<OrganParameterId> (i),
                    source.parameters.values[i])),
            "parameter round trip");

    // Simulate the exact shorter packet layout emitted before the two shared
    // performance-envelope parameters were appended. Decode must retain every
    // old value and deterministically initialise the appended fields.
    constexpr auto appendedEnvelopeParameterCount = std::size_t { 2 };
    static_assert (
        organParameterCount > appendedEnvelopeParameterCount,
        "legacy packet requires a non-empty old catalog");
    const auto legacyParameterCount =
        organParameterCount - appendedEnvelopeParameterCount;
    const auto legacyPacketBytes =
        std::size_t { 32 }
        + maximumOrganPresetIdBytes
        + legacyParameterCount * sizeof (std::uint32_t)
        + sizeof (std::uint32_t);
    auto legacyPacket = packet;
    writeU32LittleEndian (
        legacyPacket.data() + 12,
        static_cast<std::uint32_t> (legacyPacketBytes));
    writeU32LittleEndian (
        legacyPacket.data() + 16,
        static_cast<std::uint32_t> (legacyParameterCount));
    writeU32LittleEndian (
        legacyPacket.data() + legacyPacketBytes
            - sizeof (std::uint32_t),
        OrganPresetStateCodec::checksum (
            legacyPacket.data(),
            legacyPacketBytes - sizeof (std::uint32_t)));

    restored.parameters.values[
        static_cast<std::size_t> (
            OrganParameterId::performanceAttackMs)] = 777.0f;
    restored.parameters.values[
        static_cast<std::size_t> (
            OrganParameterId::performanceReleaseMs)] = 7777.0f;
    require (
        OrganPresetStateCodec::decode (
            legacyPacket.data(), legacyPacketBytes, restored)
            == OrganStateDecodeResult::ok,
        "shorter legacy packet decodes");
    require (
        approximatelyEqual (
            restored.parameters.get (
                OrganParameterId::performanceAttackMs),
            descriptor (
                OrganParameterId::performanceAttackMs).defaultValue),
        "legacy packet migrates attack to native");
    require (
        approximatelyEqual (
            restored.parameters.get (
                OrganParameterId::performanceReleaseMs),
            descriptor (
                OrganParameterId::performanceReleaseMs).defaultValue),
        "legacy packet migrates release to native");

    auto corrupted = packet;
    corrupted[encoded.bytesWritten / 2] ^= 0x40;
    require (
        OrganPresetStateCodec::decode (
            corrupted.data(), encoded.bytesWritten, restored)
            == OrganStateDecodeResult::checksumMismatch,
        "corruption detected");

    auto wrongMagic = packet;
    wrongMagic[0] = 'X';
    require (
        OrganPresetStateCodec::decode (
            wrongMagic.data(), encoded.bytesWritten, restored)
            == OrganStateDecodeResult::invalidMagic,
        "magic validated");

    auto wrongSchema = packet;
    wrongSchema[8] = 99;
    require (
        OrganPresetStateCodec::decode (
            wrongSchema.data(), encoded.bytesWritten, restored)
            == OrganStateDecodeResult::unsupportedSchema,
        "schema validated");

    require (
        OrganPresetStateCodec::decode (
            packet.data(), 12, restored)
            == OrganStateDecodeResult::truncated,
        "truncation detected");

    std::array<std::uint8_t, 16> tooSmall {};
    require (
        ! OrganPresetStateCodec::encode (
              source, tooSmall.data(), tooSmall.size()).ok,
        "small destination rejected");

    source.parameters.values[
        static_cast<std::size_t> (OrganParameterId::drive)] =
            std::numeric_limits<float>::quiet_NaN();
    source.parameters.values[
        static_cast<std::size_t> (OrganParameterId::rotaryMode)] = 99.0f;
    source.deterministicSeed = 0;
    source.presetId.fill (static_cast<char> (0xff));

    const auto sanitisedEncode = OrganPresetStateCodec::encode (
        source, packet.data(), packet.size());
    require (sanitisedEncode.ok, "sanitised encode");
    require (
        OrganPresetStateCodec::decode (
            packet.data(), sanitisedEncode.bytesWritten, restored)
            == OrganStateDecodeResult::ok,
        "sanitised decode");
    require (
        approximatelyEqual (
            restored.parameters.get (OrganParameterId::drive),
            descriptor (OrganParameterId::drive).defaultValue),
        "NaN parameter becomes default");
    require (
        approximatelyEqual (
            restored.parameters.get (OrganParameterId::rotaryMode),
            descriptor (OrganParameterId::rotaryMode).maximum),
        "parameter is clamped");
    require (restored.deterministicSeed != 0, "zero seed replaced");
    require (restored.presetId[0] == 0, "non-ASCII id removed");

    source.resetToDefaults();
    source.setPresetId ("churchdaw.user.allocation-proof");
    const auto allocationsBefore =
        allocationCount.load (std::memory_order_relaxed);

    std::uint64_t checksumAccumulator = 0;
    for (int pass = 0; pass < 10000; ++pass)
    {
        source.parameters.values[
            static_cast<std::size_t> (
                OrganParameterId::worshipIntensity)] =
                    static_cast<float> (pass % 100) / 99.0f;

        const auto write = OrganPresetStateCodec::encode (
            source, packet.data(), packet.size());
        require (write.ok, "repeated encode");
        require (
            OrganPresetStateCodec::decode (
                packet.data(), write.bytesWritten, restored)
                == OrganStateDecodeResult::ok,
            "repeated decode");
        checksumAccumulator += OrganPresetStateCodec::checksum (
            packet.data(), write.bytesWritten);
    }

    const auto allocationsAfter =
        allocationCount.load (std::memory_order_relaxed);
    require (allocationsAfter == allocationsBefore,
             "codec allocates nothing");
    require (checksumAccumulator != 0, "checksum exercised");

    std::cout << "CHURCH_ORGAN_STATE_TESTS_OK checks="
              << checks
              << " allocations="
              << (allocationsAfter - allocationsBefore)
              << " bytes="
              << OrganPresetStateCodec::currentPacketSize()
              << '\n';
    return 0;
}
