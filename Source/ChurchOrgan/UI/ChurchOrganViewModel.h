#pragma once

#include <algorithm>
#include <array>
#include <utility>
#include <vector>

#include <juce_core/juce_core.h>

namespace churchdaw::organ
{
enum class ChurchOrganRotaryMode
{
    stop,
    slow,
    fast,
    brake
};

enum class ChurchOrganQuality
{
    eco,
    standard,
    ultra
};

enum class ChurchOrganParameter
{
    drive,
    expression,
    reverb,
    worshipIntensity,

    tonewheelAge,
    keyClick,
    leakage,
    crosstalk,
    hum,
    contactSpread,
    transformer,
    amplifier,
    cabinet,
    tone,

    hornLevel,
    drumLevel,
    rotaryDistance,
    rotaryAngle,
    rotaryWidth,
    rotaryRoom,
    acceleration,
    deceleration,

    worshipPad,
    worshipAir,
    worshipStrings,
    worshipShimmer,
    worshipTexture,
    worshipDucking,
    worshipAttack,
    worshipRelease,

    count
};

inline constexpr int churchOrganParameterCount =
    static_cast<int> (ChurchOrganParameter::count);

struct ChurchOrganPresetSummary
{
    juce::String id;
    juce::String name;
    juce::String category;
    juce::String tags;
    std::array<int, 9> drawbars {};
    ChurchOrganRotaryMode rotary = ChurchOrganRotaryMode::slow;
    float worshipIntensity = 0.0f;
    bool favourite = false;
};

struct ChurchOrganUiSnapshot
{
    std::array<int, 9> drawbars { 7, 3, 8, 6, 2, 3, 1, 0, 1 };
    std::array<float, churchOrganParameterCount> parameters {};

    ChurchOrganRotaryMode rotaryMode = ChurchOrganRotaryMode::slow;
    ChurchOrganQuality quality = ChurchOrganQuality::standard;

    bool engineEnabled = true;
    bool percussionEnabled = true;
    int percussionHarmonic = 3;
    int chorusMode = 5; // Off, V1, V2, V3, C1, C2, C3.
    int tonewheelProfile = 1; // Clean, Aged, Road, Custom.
    bool autoMixEnabled = true;

    bool midiConnected = true;
    bool midiActivity = false;
    int midiDrawbarIndex = -1;
    bool midiPickupWaiting = false;
    float cpuPercent = 14.0f;

    bool seamlessSwitching = false;
    float seamlessProgress = 0.0f;
    juce::String seamlessMessage { "Ready to play" };

    juce::String presetId { "soft-worship" };
    juce::String presetName { "Soft Worship" };
    juce::String presetCategory { "Worship" };

    float value (ChurchOrganParameter parameter) const noexcept
    {
        return parameters[static_cast<size_t> (parameter)];
    }

    void setValue (ChurchOrganParameter parameter, float newValue) noexcept
    {
        parameters[static_cast<size_t> (parameter)] = juce::jlimit (0.0f, 1.0f, newValue);
    }
};

// Message-thread boundary used by the standalone UI phase. A future processor
// adapter can implement this interface with an atomic/POD snapshot. No DSP,
// AudioProcessor or host class is referenced here.
class ChurchOrganParameterSink
{
public:
    virtual ~ChurchOrganParameterSink() = default;

    virtual ChurchOrganUiSnapshot getUiSnapshot() const = 0;
    virtual std::vector<ChurchOrganPresetSummary> getPresetSummaries() const = 0;

    virtual void setDrawbar (int index, int value) = 0;
    virtual void setParameter (ChurchOrganParameter parameter, float normalisedValue) = 0;
    virtual void setRotaryMode (ChurchOrganRotaryMode mode) = 0;
    virtual void setPercussionEnabled (bool enabled) = 0;
    virtual void setPercussionHarmonic (int harmonic) = 0;
    virtual void setChorusMode (int mode) = 0;
    virtual void setTonewheelProfile (int profile) = 0;
    virtual void setAutoMixEnabled (bool enabled) = 0;
    virtual void setQuality (ChurchOrganQuality quality) = 0;
    virtual void setEngineEnabled (bool enabled) = 0;
    virtual void selectPreset (const juce::String& presetId) = 0;
    virtual void setPresetFavourite (const juce::String& presetId, bool favourite) = 0;
};

class ChurchOrganMockViewModel final : public ChurchOrganParameterSink
{
public:
    ChurchOrganMockViewModel()
    {
        state.setValue (ChurchOrganParameter::drive, 0.31f);
        state.setValue (ChurchOrganParameter::expression, 0.82f);
        state.setValue (ChurchOrganParameter::reverb, 0.48f);
        state.setValue (ChurchOrganParameter::worshipIntensity, 0.42f);
        state.setValue (ChurchOrganParameter::tonewheelAge, 0.42f);
        state.setValue (ChurchOrganParameter::keyClick, 0.36f);
        state.setValue (ChurchOrganParameter::leakage, 0.24f);
        state.setValue (ChurchOrganParameter::crosstalk, 0.18f);
        state.setValue (ChurchOrganParameter::hum, 0.12f);
        state.setValue (ChurchOrganParameter::contactSpread, 0.48f);
        state.setValue (ChurchOrganParameter::transformer, 0.44f);
        state.setValue (ChurchOrganParameter::amplifier, 0.58f);
        state.setValue (ChurchOrganParameter::cabinet, 0.46f);
        state.setValue (ChurchOrganParameter::tone, 0.54f);
        state.setValue (ChurchOrganParameter::hornLevel, 0.68f);
        state.setValue (ChurchOrganParameter::drumLevel, 0.61f);
        state.setValue (ChurchOrganParameter::rotaryDistance, 0.38f);
        state.setValue (ChurchOrganParameter::rotaryAngle, 0.52f);
        state.setValue (ChurchOrganParameter::rotaryWidth, 0.76f);
        state.setValue (ChurchOrganParameter::rotaryRoom, 0.34f);
        state.setValue (ChurchOrganParameter::acceleration, 0.46f);
        state.setValue (ChurchOrganParameter::deceleration, 0.62f);
        state.setValue (ChurchOrganParameter::worshipPad, 0.58f);
        state.setValue (ChurchOrganParameter::worshipAir, 0.46f);
        state.setValue (ChurchOrganParameter::worshipStrings, 0.38f);
        state.setValue (ChurchOrganParameter::worshipShimmer, 0.24f);
        state.setValue (ChurchOrganParameter::worshipTexture, 0.44f);
        state.setValue (ChurchOrganParameter::worshipDucking, 0.52f);
        state.setValue (ChurchOrganParameter::worshipAttack, 0.42f);
        state.setValue (ChurchOrganParameter::worshipRelease, 0.68f);

        presets =
        {
            makePreset ("soft-worship", "Soft Worship", "Worship",
                        "Warm - Pad - Wide", { 7, 3, 8, 6, 2, 3, 1, 0, 1 },
                        ChurchOrganRotaryMode::slow, 0.42f, true),
            makePreset ("gospel-fire", "Gospel Fire", "Gospel",
                        "Bright - Fast - Drive", { 8, 6, 8, 7, 5, 6, 4, 3, 3 },
                        ChurchOrganRotaryMode::fast, 0.18f, false),
            makePreset ("velvet-chapel", "Velvet Chapel", "Classic",
                        "Clean - Slow - Soft", { 6, 2, 7, 4, 1, 2, 0, 0, 0 },
                        ChurchOrganRotaryMode::slow, 0.12f, false),
            makePreset ("open-heavens", "Open Heavens", "Worship",
                        "Shimmer - Air - Cinematic", { 7, 4, 8, 5, 3, 4, 2, 1, 2 },
                        ChurchOrganRotaryMode::slow, 0.88f, true),
            makePreset ("cathedral-principal", "Cathedral Principal", "Pipe",
                        "Pipe - Grand - Room", { 8, 5, 8, 7, 6, 5, 4, 3, 4 },
                        ChurchOrganRotaryMode::stop, 0.30f, false),
            makePreset ("transistor-sun", "Transistor Sun", "Transistor",
                        "Bite - Combo - Dry", { 4, 1, 8, 7, 4, 5, 2, 2, 2 },
                        ChurchOrganRotaryMode::fast, 0.06f, false)
        };
    }

    ChurchOrganUiSnapshot getUiSnapshot() const override { return state; }
    std::vector<ChurchOrganPresetSummary> getPresetSummaries() const override { return presets; }

    void setDrawbar (int index, int value) override
    {
        if (juce::isPositiveAndBelow (index, static_cast<int> (state.drawbars.size())))
            state.drawbars[static_cast<size_t> (index)] = juce::jlimit (0, 8, value);
    }

    void setParameter (ChurchOrganParameter parameter, float normalisedValue) override
    {
        state.setValue (parameter, normalisedValue);
    }

    void setRotaryMode (ChurchOrganRotaryMode mode) override { state.rotaryMode = mode; }
    void setPercussionEnabled (bool enabled) override { state.percussionEnabled = enabled; }
    void setPercussionHarmonic (int harmonic) override
    {
        state.percussionHarmonic = harmonic == 2 ? 2 : 3;
    }
    void setChorusMode (int mode) override { state.chorusMode = juce::jlimit (0, 6, mode); }
    void setTonewheelProfile (int profile) override
    {
        state.tonewheelProfile = juce::jlimit (0, 3, profile);
    }
    void setAutoMixEnabled (bool enabled) override { state.autoMixEnabled = enabled; }
    void setQuality (ChurchOrganQuality quality) override { state.quality = quality; }
    void setEngineEnabled (bool enabled) override { state.engineEnabled = enabled; }

    void selectPreset (const juce::String& presetId) override
    {
        const auto iterator = std::find_if (presets.begin(), presets.end(),
            [&presetId] (const auto& preset) { return preset.id == presetId; });

        if (iterator == presets.end())
            return;

        state.seamlessSwitching = true;
        state.seamlessProgress = 1.0f;
        state.seamlessMessage = "Seamless preset staged";
        state.presetId = iterator->id;
        state.presetName = iterator->name;
        state.presetCategory = iterator->category;
        state.drawbars = iterator->drawbars;
        state.rotaryMode = iterator->rotary;
        state.setValue (ChurchOrganParameter::worshipIntensity, iterator->worshipIntensity);
    }

    void setPresetFavourite (const juce::String& presetId, bool favourite) override
    {
        const auto iterator = std::find_if (presets.begin(), presets.end(),
            [&presetId] (const auto& preset) { return preset.id == presetId; });

        if (iterator != presets.end())
            iterator->favourite = favourite;
    }

    ChurchOrganUiSnapshot& mutableStateForHarness() noexcept { return state; }

private:
    static ChurchOrganPresetSummary makePreset (
        juce::String id,
        juce::String name,
        juce::String category,
        juce::String tags,
        std::array<int, 9> drawbars,
        ChurchOrganRotaryMode rotary,
        float worship,
        bool favourite)
    {
        ChurchOrganPresetSummary preset;
        preset.id = std::move (id);
        preset.name = std::move (name);
        preset.category = std::move (category);
        preset.tags = std::move (tags);
        preset.drawbars = drawbars;
        preset.rotary = rotary;
        preset.worshipIntensity = worship;
        preset.favourite = favourite;
        return preset;
    }

    ChurchOrganUiSnapshot state;
    std::vector<ChurchOrganPresetSummary> presets;
};
} // namespace churchdaw::organ
