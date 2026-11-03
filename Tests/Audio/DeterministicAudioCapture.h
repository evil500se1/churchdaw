#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#if defined (_WIN32)
 #include <fcntl.h>
 #include <io.h>
 #include <sys/stat.h>
#else
 #include <fcntl.h>
 #include <sys/stat.h>
 #include <unistd.h>
#endif

static_assert (sizeof (float) == 4,
               "ChurchDAW audio captures require 32-bit float");
static_assert (std::numeric_limits<float>::is_iec559,
               "ChurchDAW audio captures require IEC-559 float");
static_assert (sizeof (double) == 8
                   && std::numeric_limits<double>::is_iec559,
               "ChurchDAW audio captures require IEC-559 double metadata");

namespace ChurchDAWTest
{
struct AudioCapture
{
    std::uint32_t scenarioVersion = 0;
    double sampleRate = 0.0;
    std::uint32_t blockSize = 0;
    std::uint32_t channels = 0;
    std::uint64_t frames = 0;
    std::vector<float> interleaved;
};

struct AudioDiagnostics
{
    std::uint64_t sampleCount = 0;
    std::uint64_t nonFiniteCount = 0;
    double peak = 0.0;
    double rms = 0.0;
    long double energy = 0.0;
    std::uint64_t fingerprint = 0;

    bool isFinite() const noexcept { return nonFiniteCount == 0; }
};

struct AudioComparison
{
    bool compatible = false;
    bool finite = false;
    bool bitIdentical = false;
    std::uint64_t sampleCount = 0;
    std::uint64_t firstBitMismatch = std::numeric_limits<std::uint64_t>::max();
    double maxAbsoluteError = std::numeric_limits<double>::infinity();
    double rmsNull = std::numeric_limits<double>::infinity();
    double rmsNullDbfs = std::numeric_limits<double>::infinity();
    std::string error;
};

inline std::uint32_t floatBits (float value) noexcept
{
    std::uint32_t bits = 0;
    std::memcpy (&bits, &value, sizeof (bits));
    return bits;
}

inline float floatFromBits (std::uint32_t bits) noexcept
{
    float value = 0.0f;
    std::memcpy (&value, &bits, sizeof (value));
    return value;
}

inline std::uint64_t doubleBits (double value) noexcept
{
    std::uint64_t bits = 0;
    std::memcpy (&bits, &value, sizeof (bits));
    return bits;
}

inline double doubleFromBits (std::uint64_t bits) noexcept
{
    double value = 0.0;
    std::memcpy (&value, &bits, sizeof (value));
    return value;
}

inline void hashByte (std::uint64_t& hash, std::uint8_t value) noexcept
{
    hash ^= value;
    hash *= UINT64_C (1099511628211);
}

inline void hashU32 (std::uint64_t& hash, std::uint32_t value) noexcept
{
    for (unsigned int shift = 0; shift < 32; shift += 8)
        hashByte (hash, static_cast<std::uint8_t> (value >> shift));
}

inline void hashU64 (std::uint64_t& hash, std::uint64_t value) noexcept
{
    for (unsigned int shift = 0; shift < 64; shift += 8)
        hashByte (hash, static_cast<std::uint8_t> (value >> shift));
}

inline std::uint64_t stableFingerprint (const AudioCapture& capture) noexcept
{
    // blockSize is deliberately excluded. Identical streams rendered with
    // different callback sizes must have the same content fingerprint.
    std::uint64_t hash = UINT64_C (14695981039346656037);
    hashU32 (hash, capture.scenarioVersion);
    hashU64 (hash, doubleBits (capture.sampleRate));
    hashU32 (hash, capture.channels);
    hashU64 (hash, capture.frames);
    for (const float sample : capture.interleaved)
        hashU32 (hash, floatBits (sample));
    return hash;
}

inline AudioDiagnostics diagnose (const AudioCapture& capture) noexcept
{
    AudioDiagnostics result;
    result.sampleCount = static_cast<std::uint64_t> (
        capture.interleaved.size());
    result.fingerprint = stableFingerprint (capture);

    long double sumSquares = 0.0;
    for (const float sample : capture.interleaved)
    {
        if (! std::isfinite (sample))
        {
            ++result.nonFiniteCount;
            continue;
        }

        const auto absolute = std::abs (static_cast<double> (sample));
        result.peak = std::max (result.peak, absolute);
        const auto wide = static_cast<long double> (sample);
        sumSquares += wide * wide;
    }

    result.energy = sumSquares;
    if (result.sampleCount != 0 && result.nonFiniteCount == 0)
        result.rms = std::sqrt (static_cast<double> (
            sumSquares / static_cast<long double> (result.sampleCount)));
    else if (result.nonFiniteCount != 0)
        result.rms = std::numeric_limits<double>::quiet_NaN();

    return result;
}

inline AudioComparison compare (const AudioCapture& reference,
                                const AudioCapture& candidate)
{
    AudioComparison result;
    if (reference.scenarioVersion != candidate.scenarioVersion
         || doubleBits (reference.sampleRate) != doubleBits (candidate.sampleRate)
         || reference.channels != candidate.channels
         || reference.frames != candidate.frames
         || reference.interleaved.size() != candidate.interleaved.size())
    {
        result.error = "capture metadata/sample counts differ";
        return result;
    }

    result.compatible = true;
    result.bitIdentical = true;
    result.sampleCount = static_cast<std::uint64_t> (
        reference.interleaved.size());
    long double sumSquares = 0.0;
    double maximum = 0.0;

    for (std::size_t i = 0; i < reference.interleaved.size(); ++i)
    {
        const float a = reference.interleaved[i];
        const float b = candidate.interleaved[i];
        if (! std::isfinite (a) || ! std::isfinite (b))
        {
            result.bitIdentical = false;
            result.error = "capture contains NaN or Inf";
            return result;
        }

        if (floatBits (a) != floatBits (b))
        {
            if (result.bitIdentical)
                result.firstBitMismatch = static_cast<std::uint64_t> (i);
            result.bitIdentical = false;
        }

        const double difference = static_cast<double> (b)
                                - static_cast<double> (a);
        maximum = std::max (maximum, std::abs (difference));
        const auto wide = static_cast<long double> (difference);
        sumSquares += wide * wide;
    }

    result.finite = true;
    result.maxAbsoluteError = maximum;
    result.rmsNull = result.sampleCount == 0 ? 0.0
        : std::sqrt (static_cast<double> (
            sumSquares / static_cast<long double> (result.sampleCount)));
    result.rmsNullDbfs = result.rmsNull > 0.0
        ? 20.0 * std::log10 (result.rmsNull)
        : -std::numeric_limits<double>::infinity();
    return result;
}

inline std::string fingerprintHex (std::uint64_t fingerprint)
{
    std::ostringstream text;
    text << std::hex << std::setfill ('0') << std::setw (16) << fingerprint;
    return text.str();
}

namespace CaptureFileDetail
{
inline constexpr char magic[8] = { 'C', 'D', 'P', 'A', 'D', '0', '2', '\n' };
inline constexpr std::uint32_t formatVersion = 2;
inline constexpr std::uint64_t maxPayloadBytes = UINT64_C (64) * 1024 * 1024;
inline constexpr std::uint64_t formatOffset = 8;
inline constexpr std::uint64_t scenarioOffset = 12;
inline constexpr std::uint64_t sampleRateOffset = 16;
inline constexpr std::uint64_t blockSizeOffset = 24;
inline constexpr std::uint64_t channelsOffset = 28;
inline constexpr std::uint64_t framesOffset = 32;
inline constexpr std::uint64_t integrityOffset = 40;
inline constexpr std::uint64_t headerSize = 48;

inline bool checkedMultiply (std::uint64_t a, std::uint64_t b,
                             std::uint64_t& result) noexcept
{
    if (a != 0 && b > std::numeric_limits<std::uint64_t>::max() / a)
        return false;
    result = a * b;
    return true;
}

inline bool checkedAdd (std::uint64_t a, std::uint64_t b,
                        std::uint64_t& result) noexcept
{
    if (b > std::numeric_limits<std::uint64_t>::max() - a)
        return false;
    result = a + b;
    return true;
}

inline bool validateLayout (const AudioCapture& capture,
                            std::uint64_t& sampleCount,
                            std::uint64_t& payloadBytes,
                            std::string& error,
                            bool requireStoredSamples)
{
    if (capture.scenarioVersion == 0)
    {
        error = "capture scenario version must be positive";
        return false;
    }
    if (! std::isfinite (capture.sampleRate)
         || capture.sampleRate < 8000.0 || capture.sampleRate > 768000.0)
    {
        error = "capture sample rate is invalid";
        return false;
    }
    if (capture.blockSize == 0 || capture.blockSize > 65536)
    {
        error = "capture block size is out of bounds";
        return false;
    }
    if (capture.channels == 0 || capture.channels > 64 || capture.frames == 0)
    {
        error = "capture dimensions are out of bounds";
        return false;
    }
    if (! checkedMultiply (capture.frames, capture.channels, sampleCount)
         || ! checkedMultiply (sampleCount, sizeof (float), payloadBytes))
    {
        error = "capture dimensions overflow";
        return false;
    }
    if (payloadBytes > maxPayloadBytes)
    {
        error = "capture payload exceeds safety limit";
        return false;
    }
    if (sampleCount > static_cast<std::uint64_t> (
                          std::numeric_limits<std::size_t>::max()))
    {
        error = "capture is too large for this process";
        return false;
    }

    const std::vector<float> empty;
    if (sampleCount > static_cast<std::uint64_t> (empty.max_size()))
    {
        error = "capture exceeds vector max_size";
        return false;
    }
    if (requireStoredSamples
         && sampleCount != static_cast<std::uint64_t> (
                               capture.interleaved.size()))
    {
        error = "capture dimensions do not match sample storage";
        return false;
    }
    return true;
}

inline std::uint64_t fileIntegrityFingerprint (
    const AudioCapture& capture) noexcept
{
    std::uint64_t hash = UINT64_C (14695981039346656037);
    for (const char byte : magic)
        hashByte (hash, static_cast<std::uint8_t> (byte));
    hashU32 (hash, formatVersion);
    hashU32 (hash, capture.scenarioVersion);
    hashU64 (hash, doubleBits (capture.sampleRate));
    hashU32 (hash, capture.blockSize);
    hashU32 (hash, capture.channels);
    hashU64 (hash, capture.frames);
    for (const float sample : capture.interleaved)
        hashU32 (hash, floatBits (sample));
    return hash;
}

inline void writeU32 (std::ostream& output, std::uint32_t value)
{
    const std::uint8_t bytes[4] = {
        static_cast<std::uint8_t> (value),
        static_cast<std::uint8_t> (value >> 8),
        static_cast<std::uint8_t> (value >> 16),
        static_cast<std::uint8_t> (value >> 24)
    };
    output.write (reinterpret_cast<const char*> (bytes), sizeof (bytes));
}

inline void writeU64 (std::ostream& output, std::uint64_t value)
{
    std::uint8_t bytes[8] {};
    for (unsigned int i = 0; i < 8; ++i)
        bytes[i] = static_cast<std::uint8_t> (value >> (8 * i));
    output.write (reinterpret_cast<const char*> (bytes), sizeof (bytes));
}

inline bool readU32 (std::istream& input, std::uint32_t& value)
{
    std::uint8_t bytes[4] {};
    input.read (reinterpret_cast<char*> (bytes), sizeof (bytes));
    if (! input)
        return false;
    value = static_cast<std::uint32_t> (bytes[0])
          | (static_cast<std::uint32_t> (bytes[1]) << 8)
          | (static_cast<std::uint32_t> (bytes[2]) << 16)
          | (static_cast<std::uint32_t> (bytes[3]) << 24);
    return true;
}

inline bool readU64 (std::istream& input, std::uint64_t& value)
{
    std::uint8_t bytes[8] {};
    input.read (reinterpret_cast<char*> (bytes), sizeof (bytes));
    if (! input)
        return false;
    value = 0;
    for (unsigned int i = 0; i < 8; ++i)
        value |= static_cast<std::uint64_t> (bytes[i]) << (8 * i);
    return true;
}
}

inline bool writeCaptureFile (const std::filesystem::path& path,
                              const AudioCapture& capture,
                              std::string& error)
{
    std::uint64_t sampleCount = 0;
    std::uint64_t payloadBytes = 0;
    if (! CaptureFileDetail::validateLayout (
            capture, sampleCount, payloadBytes, error, true))
        return false;
    for (const float sample : capture.interleaved)
        if (! std::isfinite (sample))
        {
            error = "capture contains NaN or Inf";
            return false;
        }

    std::ofstream output (path, std::ios::binary | std::ios::trunc);
    if (! output)
    {
        error = "cannot open capture for writing: " + path.string();
        return false;
    }

    output.write (CaptureFileDetail::magic,
                  sizeof (CaptureFileDetail::magic));
    CaptureFileDetail::writeU32 (output, CaptureFileDetail::formatVersion);
    CaptureFileDetail::writeU32 (output, capture.scenarioVersion);
    CaptureFileDetail::writeU64 (output, doubleBits (capture.sampleRate));
    CaptureFileDetail::writeU32 (output, capture.blockSize);
    CaptureFileDetail::writeU32 (output, capture.channels);
    CaptureFileDetail::writeU64 (output, capture.frames);
    CaptureFileDetail::writeU64 (
        output, CaptureFileDetail::fileIntegrityFingerprint (capture));
    for (const float sample : capture.interleaved)
        CaptureFileDetail::writeU32 (output, floatBits (sample));
    output.flush();

    if (! output)
    {
        error = "failed while writing capture: " + path.string();
        return false;
    }
    output.close();
    if (output.fail())
    {
        error = "failed while closing capture: " + path.string();
        return false;
    }

    std::uint64_t expectedSize = 0;
    if (! CaptureFileDetail::checkedAdd (
            CaptureFileDetail::headerSize, payloadBytes, expectedSize))
    {
        error = "capture file size overflow";
        return false;
    }
    std::error_code fileError;
    const auto actualSize = std::filesystem::file_size (path, fileError);
    if (fileError || actualSize != expectedSize)
    {
        error = "capture write size verification failed";
        return false;
    }
    return true;
}

// Baseline generation must not use exists()+ofstream(trunc): that sequence has
// a TOCTOU window in which a concurrent creator can be silently overwritten.
// Keep ordinary writeCaptureFile() available for deliberately replaceable
// temporary fixtures, but make the explicit baseline path no-replace at the
// operating-system create boundary on both supported desktop families.
inline bool writeCaptureFileExclusive (const std::filesystem::path& path,
                                        const AudioCapture& capture,
                                        std::string& error)
{
    std::uint64_t sampleCount = 0;
    std::uint64_t payloadBytes = 0;
    if (! CaptureFileDetail::validateLayout (
            capture, sampleCount, payloadBytes, error, true))
        return false;
    for (const float sample : capture.interleaved)
        if (! std::isfinite (sample))
        {
            error = "capture contains NaN or Inf";
            return false;
        }

    std::ostringstream serialized (std::ios::binary);
    serialized.write (CaptureFileDetail::magic,
                      sizeof (CaptureFileDetail::magic));
    CaptureFileDetail::writeU32 (serialized, CaptureFileDetail::formatVersion);
    CaptureFileDetail::writeU32 (serialized, capture.scenarioVersion);
    CaptureFileDetail::writeU64 (serialized, doubleBits (capture.sampleRate));
    CaptureFileDetail::writeU32 (serialized, capture.blockSize);
    CaptureFileDetail::writeU32 (serialized, capture.channels);
    CaptureFileDetail::writeU64 (serialized, capture.frames);
    CaptureFileDetail::writeU64 (
        serialized, CaptureFileDetail::fileIntegrityFingerprint (capture));
    for (const float sample : capture.interleaved)
        CaptureFileDetail::writeU32 (serialized, floatBits (sample));
    if (! serialized)
    {
        error = "failed while serializing exclusive capture";
        return false;
    }
    const std::string bytes = serialized.str();

#if defined (_WIN32)
    const int descriptor = _wopen (
        path.c_str(), _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY,
        _S_IREAD | _S_IWRITE);
    if (descriptor < 0)
#else
    const int descriptor = ::open (path.c_str(), O_WRONLY | O_CREAT | O_EXCL,
                                   0666);
    if (descriptor < 0)
#endif
    {
        error = "cannot create capture exclusively: " + path.string();
        return false;
    }

#if defined (_WIN32)
    std::FILE* output = _fdopen (descriptor, "wb");
#else
    std::FILE* output = ::fdopen (descriptor, "wb");
#endif
    if (output == nullptr)
    {
#if defined (_WIN32)
        _close (descriptor);
#else
        ::close (descriptor);
#endif
        std::error_code ignored;
        std::filesystem::remove (path, ignored);
        error = "cannot open exclusive capture stream: " + path.string();
        return false;
    }

    const bool written = bytes.empty()
        || std::fwrite (bytes.data(), 1, bytes.size(), output) == bytes.size();
    const bool flushed = written && std::fflush (output) == 0;
    const bool closed = std::fclose (output) == 0;
    if (! written || ! flushed || ! closed)
    {
        error = "failed while writing exclusive capture: " + path.string();
        std::error_code ignored;
        std::filesystem::remove (path, ignored);
        return false;
    }
    return true;
}

inline bool readCaptureFile (const std::filesystem::path& path,
                             AudioCapture& capture,
                             std::string& error)
{
    std::ifstream input (path, std::ios::binary);
    if (! input)
    {
        error = "cannot open capture for reading: " + path.string();
        return false;
    }

    // Measure the already-open stream. This keeps the size check and all
    // subsequent reads on the same file handle, avoiding a stat/open TOCTOU.
    input.seekg (0, std::ios::end);
    const auto end = input.tellg();
    if (end < 0)
    {
        error = "cannot measure capture for reading: " + path.string();
        return false;
    }
    const auto physicalSize = static_cast<std::uint64_t> (end);
    if (physicalSize < CaptureFileDetail::headerSize)
    {
        error = "truncated capture header";
        return false;
    }
    input.seekg (0, std::ios::beg);
    if (! input)
    {
        error = "cannot seek capture for reading: " + path.string();
        return false;
    }

    char foundMagic[sizeof (CaptureFileDetail::magic)] {};
    input.read (foundMagic, sizeof (foundMagic));
    if (! input
         || std::memcmp (foundMagic, CaptureFileDetail::magic,
                         sizeof (foundMagic)) != 0)
    {
        error = "invalid capture magic";
        return false;
    }

    AudioCapture parsed;
    std::uint32_t format = 0;
    std::uint64_t sampleRateBits = 0;
    std::uint64_t storedIntegrity = 0;
    if (! CaptureFileDetail::readU32 (input, format)
         || ! CaptureFileDetail::readU32 (input, parsed.scenarioVersion)
         || ! CaptureFileDetail::readU64 (input, sampleRateBits)
         || ! CaptureFileDetail::readU32 (input, parsed.blockSize)
         || ! CaptureFileDetail::readU32 (input, parsed.channels)
         || ! CaptureFileDetail::readU64 (input, parsed.frames)
         || ! CaptureFileDetail::readU64 (input, storedIntegrity))
    {
        error = "truncated capture header";
        return false;
    }

    if (format != CaptureFileDetail::formatVersion)
    {
        error = "unsupported capture format version";
        return false;
    }
    parsed.sampleRate = doubleFromBits (sampleRateBits);

    std::uint64_t sampleCount = 0;
    std::uint64_t payloadBytes = 0;
    if (! CaptureFileDetail::validateLayout (
            parsed, sampleCount, payloadBytes, error, false))
        return false;

    std::uint64_t expectedSize = 0;
    if (! CaptureFileDetail::checkedAdd (
            CaptureFileDetail::headerSize, payloadBytes, expectedSize))
    {
        error = "capture file size overflow";
        return false;
    }
    if (physicalSize != expectedSize)
    {
        error = physicalSize < expectedSize
            ? "truncated capture sample data"
            : "capture has unexpected trailing data";
        return false;
    }

    try
    {
        parsed.interleaved.resize (static_cast<std::size_t> (sampleCount));
    }
    catch (const std::exception& allocationError)
    {
        error = "capture allocation failed: "
              + std::string (allocationError.what());
        return false;
    }

    for (auto& sample : parsed.interleaved)
    {
        std::uint32_t bits = 0;
        if (! CaptureFileDetail::readU32 (input, bits))
        {
            error = "truncated capture sample data";
            return false;
        }
        sample = floatFromBits (bits);
        if (! std::isfinite (sample))
        {
            error = "capture contains NaN or Inf";
            return false;
        }
    }

    if (input.peek() != std::char_traits<char>::eof())
    {
        error = "capture has unexpected trailing data";
        return false;
    }
    if (CaptureFileDetail::fileIntegrityFingerprint (parsed)
         != storedIntegrity)
    {
        error = "capture integrity mismatch (header or audio was modified)";
        return false;
    }

    capture = std::move (parsed);
    return true;
}
}
