#pragma once

#include <cmath>
#include <cstddef>

namespace churchorgan::worshipdsp
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

    inline float onePoleStep (
        double sampleRate,
        float milliseconds) noexcept
    {
        const auto safeRate = clamp (
            finiteOr (sampleRate, 48000.0),
            8000.0,
            384000.0);
        const auto safeTime = clamp (
            finiteOr (milliseconds, 20.0f),
            0.05f,
            30000.0f);
        return static_cast<float> (
            1.0 - std::exp (
                -1.0
                / (safeRate
                   * static_cast<double> (safeTime)
                   * 0.001)));
    }

    inline float filterStep (
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

    inline float smoothStep (
        float edge0,
        float edge1,
        float value) noexcept
    {
        if (edge1 <= edge0)
            return value >= edge1 ? 1.0f : 0.0f;
        const auto normalised = clamp (
            (value - edge0) / (edge1 - edge0),
            0.0f,
            1.0f);
        return normalised
            * normalised
            * (3.0f - 2.0f * normalised);
    }

    inline float decibelsToGain (float decibels) noexcept
    {
        return std::pow (
            10.0f,
            finiteOr (decibels, 0.0f) / 20.0f);
    }

    inline float protectedSample (
        float value,
        float threshold = 0.48f,
        float ceiling = 0.68f) noexcept
    {
        value = finiteOr (value, 0.0f);
        const auto magnitude = std::abs (value);
        if (magnitude <= threshold)
            return value;

        const auto safeCeiling =
            clamp (ceiling, threshold + 0.001f, 1.0f);
        const auto scaled =
            (magnitude - threshold) / (safeCeiling - threshold);
        const auto limited =
            threshold
            + (safeCeiling - threshold)
                * scaled / (1.0f + scaled);
        return std::copysign (limited, value);
    }

    class SmoothedValue
    {
    public:
        void prepare (
            double sampleRate,
            float milliseconds,
            float initial) noexcept
        {
            step_ = onePoleStep (sampleRate, milliseconds);
            reset (initial);
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
            if (std::abs (target_ - current_) < 1.0e-8f)
                current_ = target_;
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
}
