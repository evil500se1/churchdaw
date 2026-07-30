#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace churchorgan::dsp
{
    inline constexpr float pi =
        3.14159265358979323846264338327950288f;
    inline constexpr float twoPi = 2.0f * pi;

    template <typename Value>
    constexpr Value clamp (
        Value value,
        Value minimum,
        Value maximum) noexcept
    {
        return value < minimum
            ? minimum
            : (value > maximum ? maximum : value);
    }

    inline float finiteOr (float value, float fallback) noexcept
    {
        return std::isfinite (value) ? value : fallback;
    }

    inline double finiteOr (double value, double fallback) noexcept
    {
        return std::isfinite (value) ? value : fallback;
    }

    inline float smoothingStep (
        double sampleRate,
        float milliseconds) noexcept
    {
        const auto safeRate = clamp (
            finiteOr (sampleRate, 48000.0),
            8000.0,
            384000.0);
        const auto safeMilliseconds = clamp (
            finiteOr (milliseconds, 20.0f),
            0.01f,
            10000.0f);

        return static_cast<float> (
            1.0 - std::exp (
                -1.0 / (safeRate
                        * static_cast<double> (safeMilliseconds)
                        * 0.001)));
    }

    inline float onePoleStepForCutoff (
        double sampleRate,
        float cutoffHz) noexcept
    {
        const auto safeRate = clamp (
            finiteOr (sampleRate, 48000.0),
            8000.0,
            384000.0);
        const auto safeCutoff = clamp (
            finiteOr (cutoffHz, 1000.0f),
            1.0f,
            static_cast<float> (safeRate * 0.45));

        return static_cast<float> (
            1.0 - std::exp (
                -static_cast<double> (twoPi)
                * static_cast<double> (safeCutoff)
                / safeRate));
    }

    inline float wrapUnitPhase (float phase) noexcept
    {
        phase -= std::floor (phase);
        return phase < 0.0f ? phase + 1.0f : phase;
    }

    class SmoothedValue
    {
    public:
        void prepare (
            double sampleRate,
            float milliseconds,
            float initialValue) noexcept
        {
            step_ = smoothingStep (sampleRate, milliseconds);
            reset (initialValue);
        }

        void reset (float value) noexcept
        {
            current_ = finiteOr (value, 0.0f);
            target_ = current_;
        }

        void setTarget (float value) noexcept
        {
            target_ = finiteOr (value, target_);
        }

        float next() noexcept
        {
            current_ += step_ * (target_ - current_);
            return current_;
        }

        float current() const noexcept
        {
            return current_;
        }

        float target() const noexcept
        {
            return target_;
        }

    private:
        float current_ = 0.0f;
        float target_ = 0.0f;
        float step_ = 1.0f;
    };

    template <std::size_t Capacity>
    class FixedFractionalDelay
    {
    public:
        static_assert (
            Capacity >= 16
                && (Capacity & (Capacity - 1)) == 0,
            "Fractional delay capacity must be a power of two");

        void reset() noexcept
        {
            data_.fill (0.0f);
            writeIndex_ = 0;
        }

        void push (float value) noexcept
        {
            data_[writeIndex_] = finiteOr (value, 0.0f);
            writeIndex_ = (writeIndex_ + 1) & mask;
        }

        float read (float delaySamples) const noexcept
        {
            const auto safeDelay = clamp (
                finiteOr (delaySamples, 1.0f),
                1.0f,
                static_cast<float> (Capacity - 4));

            const auto wholeDelay =
                static_cast<std::size_t> (safeDelay);
            const auto fraction =
                safeDelay - static_cast<float> (wholeDelay);
            const auto newest = (writeIndex_ - 1) & mask;
            const auto index0 = (newest - wholeDelay) & mask;

            // Increasing delay walks backwards through history. ym1 is the
            // newer neighbour, y1/y2 are progressively older samples.
            const auto ym1 = data_[(index0 + 1) & mask];
            const auto y0 = data_[index0];
            const auto y1 = data_[(index0 - 1) & mask];
            const auto y2 = data_[(index0 - 2) & mask];

            // Four-point Catmull-Rom interpolation. The delay is always at
            // least one sample, so no unread future sample participates.
            const auto c0 = y0;
            const auto c1 = 0.5f * (y1 - ym1);
            const auto c2 =
                ym1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
            const auto c3 =
                0.5f * (y2 - ym1) + 1.5f * (y0 - y1);

            return ((c3 * fraction + c2) * fraction + c1)
                * fraction
                + c0;
        }

        static constexpr std::size_t capacity = Capacity;

    private:
        static constexpr std::size_t mask = Capacity - 1;
        std::array<float, Capacity> data_ {};
        std::size_t writeIndex_ = 0;
    };

    inline float softPeakProtect (
        float value,
        float threshold = 0.78f,
        float ceiling = 0.985f) noexcept
    {
        value = finiteOr (value, 0.0f);
        const auto magnitude = std::abs (value);
        if (magnitude <= threshold)
            return value;

        const auto safeCeiling =
            clamp (ceiling, threshold + 0.001f, 4.0f);
        const auto scaled =
            (magnitude - threshold) / (safeCeiling - threshold);
        const auto protectedMagnitude =
            threshold
            + (safeCeiling - threshold)
                * scaled / (1.0f + scaled);

        return std::copysign (protectedMagnitude, value);
    }
}
