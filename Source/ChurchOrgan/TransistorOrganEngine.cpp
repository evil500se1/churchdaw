#include "TransistorOrganEngine.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace churchorgan
{
    namespace
    {
        constexpr double twoPi =
            6.283185307179586476925286766559005768;

        struct RankComponent
        {
            int semitoneOffset = 0;
            float gain = 0.0f;
        };

        struct RankDefinition
        {
            std::array<RankComponent, 2> components {};
        };

        constexpr RankDefinition makeRank (
            int firstOffset,
            float firstGain,
            int secondOffset,
            float secondGain) noexcept
        {
            RankDefinition result {};
            result.components[0].semitoneOffset = firstOffset;
            result.components[0].gain = firstGain;
            result.components[1].semitoneOffset = secondOffset;
            result.components[1].gain = secondGain;
            return result;
        }

        struct TabColour
        {
            float square = 0.0f;
            float pulse = 0.0f;
            float ramp = 0.0f;
            float gain = 0.0f;
        };

        struct ProfileDefinition
        {
            std::array<TabColour, transistorTabCount> colours {};
            std::array<float, transistorTabCount> defaultTabs {};
            float cabinetCutoffHz = 7200.0f;
            float vibratoRateHz = 6.5f;
            float vibratoDepthCents = 14.0f;
            float outputGain = 1.0f;
            float allpassCoefficient = 0.42f;
        };

        constexpr std::array<RankDefinition, transistorTabCount>
            rankDefinitions {{
                makeRank (-12, 1.00f,  0, 0.00f),
                makeRank (  0, 1.00f,  0, 0.00f),
                makeRank (  0, 0.82f, 12, 0.18f),
                makeRank ( 12, 1.00f,  0, 0.00f),
                makeRank ( 12, 0.80f, 24, 0.20f),
                makeRank ( 24, 1.00f,  0, 0.00f),
                makeRank ( 19, 0.62f, 28, 0.38f),
                makeRank ( 31, 0.58f, 36, 0.42f)
            }};

        constexpr std::array<int, transistorTabCount>
            uniqueRankOffsets {{
                -12, 0, 12, 24, 19, 28, 31, 36
            }};

        constexpr std::array<std::array<std::uint8_t, 2>,
                             transistorTabCount>
            rankOffsetIndices {{
                {{ 0, 0 }},
                {{ 1, 1 }},
                {{ 1, 2 }},
                {{ 2, 2 }},
                {{ 2, 3 }},
                {{ 3, 3 }},
                {{ 4, 5 }},
                {{ 6, 7 }}
            }};

        constexpr std::array<ProfileDefinition, 3> profiles {{
            {
                {{
                    { 0.82f, 0.05f, 0.13f, 0.78f },
                    { 0.72f, 0.10f, 0.18f, 0.82f },
                    { 0.08f, 0.76f, 0.16f, 0.74f },
                    { 0.74f, 0.03f, 0.23f, 0.54f },
                    { 0.18f, 0.67f, 0.15f, 0.57f },
                    { 0.52f, 0.20f, 0.28f, 0.45f },
                    { 0.40f, 0.26f, 0.34f, 0.35f },
                    { 0.22f, 0.42f, 0.36f, 0.26f }
                }},
                {{ 0.00f, 0.88f, 0.62f, 0.50f,
                   0.70f, 0.25f, 0.35f, 0.15f }},
                7600.0f, 7.1f, 13.5f, 1.00f, 0.38f
            },
            {
                {{
                    { 0.56f, 0.18f, 0.26f, 0.82f },
                    { 0.42f, 0.24f, 0.34f, 0.84f },
                    { 0.04f, 0.76f, 0.20f, 0.80f },
                    { 0.34f, 0.14f, 0.52f, 0.58f },
                    { 0.09f, 0.55f, 0.36f, 0.62f },
                    { 0.26f, 0.34f, 0.40f, 0.52f },
                    { 0.18f, 0.38f, 0.44f, 0.43f },
                    { 0.08f, 0.48f, 0.44f, 0.33f }
                }},
                {{ 0.20f, 0.75f, 0.85f, 0.40f,
                   0.65f, 0.42f, 0.50f, 0.28f }},
                8900.0f, 6.3f, 17.5f, 0.96f, 0.46f
            },
            {
                {{
                    { 0.76f, 0.04f, 0.20f, 0.78f },
                    { 0.63f, 0.05f, 0.32f, 0.80f },
                    { 0.24f, 0.50f, 0.26f, 0.68f },
                    { 0.50f, 0.01f, 0.49f, 0.56f },
                    { 0.32f, 0.42f, 0.26f, 0.52f },
                    { 0.48f, 0.10f, 0.42f, 0.40f },
                    { 0.36f, 0.16f, 0.48f, 0.31f },
                    { 0.20f, 0.22f, 0.58f, 0.22f }
                }},
                {{ 0.35f, 0.82f, 0.45f, 0.62f,
                   0.40f, 0.30f, 0.28f, 0.12f }},
                6800.0f, 5.7f, 10.5f, 0.92f, 0.33f
            }
        }};

        constexpr std::array<const char*, transistorTabCount>
            tabNames {{
                "Bass 16",
                "Principal 8",
                "Reed 8",
                "Flute 4",
                "String 4",
                "Bright 2",
                "Mixture",
                "Brilliance"
            }};

        const ProfileDefinition& profileDefinition (
            TransistorOrganProfile profile) noexcept
        {
            auto index = static_cast<std::size_t> (profile);

            if (index >= profiles.size())
                index = 0;

            return profiles[index];
        }

    }

    TransistorOrganEngine::TransistorOrganEngine() noexcept
    {
        activeSlots_.fill (-1);
    }

    bool TransistorOrganEngine::prepare (
        const TransistorOrganPrepareSpec& spec) noexcept
    {
        if (! validSpec (spec)
             || ! dividerBank_.prepare (
                 spec.sampleRate, spec.deterministicSeed))
            return false;

        spec_ = spec;
        buildRankRoutes();
        gateAttackCoefficient_ = smoothingCoefficient (
            spec_.sampleRate, 0.38f);
        gateReleaseCoefficient_ = smoothingCoefficient (
            spec_.sampleRate, 1.45f);
        tabSmoothingCoefficient_ = smoothingCoefficient (
            spec_.sampleRate, 12.0f);
        clickLowCoefficient_ = onePoleCoefficient (
            spec_.sampleRate, 720.0f);
        clickHighCoefficient_ = onePoleCoefficient (
            spec_.sampleRate, 7200.0f);
        clickOnDecayCoefficient_ = envelopeDecayCoefficient (
            spec_.sampleRate, 5.8f);
        clickOffDecayCoefficient_ = envelopeDecayCoefficient (
            spec_.sampleRate, 3.6f);
        dcBlockCoefficient_ = static_cast<float> (
            std::exp (-twoPi * 18.0 / spec_.sampleRate));
        refreshProfileCoefficients();
        prepared_ = true;
        loadProfileDefaultTabs();
        reset();
        return true;
    }

    void TransistorOrganEngine::reset() noexcept
    {
        dividerBank_.resetPhases();
        notes_ = {};
        activeNotes_ = {};
        activeSlots_.fill (-1);
        activeNoteCount_ = 0;
        activeLowerTapMask_ = 0;
        activeUpperTapMask_ = 0;
        dividerBank_.setActiveTapMask (0, 0);
        events_ = {};
        eventCount_ = 0;
        droppedEvents_ = 0;
        tabCurrents_ = tabTargets_;
        vibratoSin_ = 0.0f;
        vibratoCos_ = 1.0f;
        vibratoNormaliseCounter_ = 0;
        preDcInput_ = 0.0f;
        preDcOutput_ = 0.0f;
        postDcInput_ = 0.0f;
        postDcOutput_ = 0.0f;
        cabinetStateLeft_ = 0.0f;
        cabinetStateRight_ = 0.0f;
        allpassState_ = 0.0f;
        renderedSamples_ = 0;
        invalidSamples_ = 0;
        diagnostics_ = {};
    }

    bool TransistorOrganEngine::render (
        TransistorAudioBlockView output) noexcept
    {
        if (! prepared_
             || output.numSamples > spec_.maximumBlockSize
             || (output.numSamples > 0 && output.left == nullptr))
            return false;

        for (std::size_t event = 0; event < eventCount_; ++event)
            if (events_[event].sampleOffset >= output.numSamples)
            {
                eventCount_ = 0;
                return false;
            }

        std::size_t eventIndex = 0;
        float peak = 0.0f;
        double sum = 0.0;
        double sumSquares = 0.0;
        std::uint64_t blockInvalidSamples = 0;

        for (std::size_t sample = 0;
             sample < output.numSamples;
             ++sample)
        {
            while (eventIndex < eventCount_
                   && events_[eventIndex].sampleOffset == sample)
            {
                applyEvent (events_[eventIndex]);
                ++eventIndex;
            }

            for (std::size_t tab = 0;
                 tab < transistorTabCount;
                 ++tab)
                tabCurrents_[tab] +=
                    (tabTargets_[tab] - tabCurrents_[tab])
                    * tabSmoothingCoefficient_;

            dividerBank_.advanceActive (
                vibratoPitchMultiplier());
            const float raw = renderVoices();
            const float driven = processDriveAndDc (raw);
            float left = 0.0f;
            float right = 0.0f;
            processCabinet (driven, left, right);

            if (! std::isfinite (left)
                 || ! std::isfinite (right))
            {
                left = 0.0f;
                right = 0.0f;
                ++blockInvalidSamples;
            }

            left = std::max (-0.999f, std::min (0.999f, left));
            right = std::max (-0.999f, std::min (0.999f, right));
            output.left[sample] = left;

            if (output.right != nullptr)
                output.right[sample] = right;

            const float samplePeak = output.right != nullptr
                ? std::max (std::abs (left), std::abs (right))
                : std::abs (left);
            peak = std::max (peak, samplePeak);
            const double measured = output.right != nullptr
                ? 0.5 * (static_cast<double> (left)
                         + static_cast<double> (right))
                : static_cast<double> (left);
            sum += measured;
            sumSquares += measured * measured;
        }

        eventCount_ = 0;
        renderedSamples_ += output.numSamples;
        invalidSamples_ += blockInvalidSamples;
        publishDiagnostics (
            peak,
            sum,
            sumSquares,
            output.numSamples,
            blockInvalidSamples);
        return true;
    }

    bool TransistorOrganEngine::queueUpperNoteOn (
        int note,
        int velocity,
        std::uint32_t sampleOffset) noexcept
    {
        if (note < 0
             || note >= static_cast<int> (transistorMidiNoteCount)
             || velocity <= 0
             || velocity > 127)
            return false;

        return queueEvent ({
            TransistorOrganEventType::noteOn,
            TransistorOrganManual::upper,
            static_cast<std::uint8_t> (note),
            static_cast<std::uint8_t> (velocity),
            sampleOffset
        });
    }

    bool TransistorOrganEngine::queueUpperNoteOff (
        int note,
        std::uint32_t sampleOffset) noexcept
    {
        if (note < 0
             || note >= static_cast<int> (transistorMidiNoteCount))
            return false;

        return queueEvent ({
            TransistorOrganEventType::noteOff,
            TransistorOrganManual::upper,
            static_cast<std::uint8_t> (note),
            0,
            sampleOffset
        });
    }

    bool TransistorOrganEngine::queueUpperAllNotesOff (
        std::uint32_t sampleOffset) noexcept
    {
        return queueEvent ({
            TransistorOrganEventType::allNotesOff,
            TransistorOrganManual::upper,
            0,
            0,
            sampleOffset
        });
    }

    void TransistorOrganEngine::setProfile (
        TransistorOrganProfile profile,
        bool loadDefaultTabs) noexcept
    {
        const auto index = static_cast<std::size_t> (profile);

        if (index >= profiles.size())
            return;

        spec_.profile = profile;
        refreshProfileCoefficients();

        if (loadDefaultTabs)
            loadProfileDefaultTabs();
    }

    TransistorOrganProfile
    TransistorOrganEngine::profile() const noexcept
    {
        return spec_.profile;
    }

    void TransistorOrganEngine::loadProfileDefaultTabs() noexcept
    {
        const auto& defaults =
            profileDefinition (spec_.profile).defaultTabs;
        tabTargets_ = defaults;

        if (! prepared_)
            tabCurrents_ = tabTargets_;
    }

    void TransistorOrganEngine::setTabLevel (
        TransistorOrganTab tab,
        float levelZeroToOne) noexcept
    {
        const auto index = tabIndex (tab);

        if (index < tabTargets_.size())
            tabTargets_[index] = clampUnit (levelZeroToOne);
    }

    float TransistorOrganEngine::targetTabLevel (
        TransistorOrganTab tab) const noexcept
    {
        const auto index = tabIndex (tab);
        return index < tabTargets_.size()
                   ? tabTargets_[index]
                   : 0.0f;
    }

    float TransistorOrganEngine::currentTabLevel (
        TransistorOrganTab tab) const noexcept
    {
        const auto index = tabIndex (tab);
        return index < tabCurrents_.size()
                   ? tabCurrents_[index]
                   : 0.0f;
    }

    void TransistorOrganEngine::setVibratoEnabled (
        bool enabled) noexcept
    {
        spec_.vibratoEnabled = enabled;
    }

    void TransistorOrganEngine::setVibratoDepth (
        float depthZeroToOne) noexcept
    {
        spec_.vibratoDepth = clampUnit (depthZeroToOne);
    }

    void TransistorOrganEngine::setDrive (
        float driveZeroToOne) noexcept
    {
        spec_.drive = clampUnit (driveZeroToOne);
    }

    void TransistorOrganEngine::setOutputTrim (float trim) noexcept
    {
        if (std::isfinite (trim))
            spec_.outputTrim = std::max (
                0.0f, std::min (0.25f, trim));
    }

    void TransistorOrganEngine::setStereoWidth (
        float widthZeroToOne) noexcept
    {
        spec_.stereoWidth = clampUnit (widthZeroToOne);
    }

    void TransistorOrganEngine::setPerformanceEnvelopeMilliseconds (
        float attackMilliseconds,
        float releaseMilliseconds) noexcept
    {
        if (! prepared_)
            return;

        const auto attack = std::isfinite (attackMilliseconds)
                                && attackMilliseconds > 0.0f
            ? std::max (0.02f,
                std::min (2000.0f, attackMilliseconds))
            : 0.38f;
        const auto release = std::isfinite (releaseMilliseconds)
                                 && releaseMilliseconds > 0.0f
            ? std::max (0.02f,
                std::min (12000.0f, releaseMilliseconds))
            : 1.45f;

        gateAttackCoefficient_ = smoothingCoefficient (
            spec_.sampleRate, attack);
        gateReleaseCoefficient_ = smoothingCoefficient (
            spec_.sampleRate, release);
    }

    bool TransistorOrganEngine::vibratoEnabled() const noexcept
    {
        return spec_.vibratoEnabled;
    }

    float TransistorOrganEngine::vibratoDepth() const noexcept
    {
        return spec_.vibratoDepth;
    }

    float TransistorOrganEngine::drive() const noexcept
    {
        return spec_.drive;
    }

    float TransistorOrganEngine::outputTrim() const noexcept
    {
        return spec_.outputTrim;
    }

    float TransistorOrganEngine::stereoWidth() const noexcept
    {
        return spec_.stereoWidth;
    }

    std::size_t
    TransistorOrganEngine::activeUpperNoteCount() const noexcept
    {
        return activeNoteCount_;
    }

    TransistorOrganDiagnostics
    TransistorOrganEngine::diagnostics() const noexcept
    {
        return diagnostics_;
    }

    const char* TransistorOrganEngine::tabName (
        TransistorOrganTab tab) noexcept
    {
        const auto index = tabIndex (tab);
        return index < tabNames.size()
                   ? tabNames[index]
                   : "";
    }

    bool TransistorOrganEngine::validSpec (
        const TransistorOrganPrepareSpec& spec) noexcept
    {
        const auto profileIndex =
            static_cast<std::size_t> (spec.profile);

        return std::isfinite (spec.sampleRate)
               && spec.sampleRate >= 32000.0
               && spec.sampleRate <= 384000.0
               && spec.maximumBlockSize > 0
               && spec.maximumBlockSize <= 8192
               && profileIndex < profiles.size()
               && std::isfinite (spec.outputTrim)
               && spec.outputTrim >= 0.0f
               && spec.outputTrim <= 0.25f
               && std::isfinite (spec.drive)
               && spec.drive >= 0.0f
               && spec.drive <= 1.0f
               && std::isfinite (spec.stereoWidth)
               && spec.stereoWidth >= 0.0f
               && spec.stereoWidth <= 1.0f
               && std::isfinite (spec.vibratoDepth)
               && spec.vibratoDepth >= 0.0f
               && spec.vibratoDepth <= 1.0f;
    }

    std::size_t TransistorOrganEngine::tabIndex (
        TransistorOrganTab tab) noexcept
    {
        return static_cast<std::size_t> (tab);
    }

    float TransistorOrganEngine::clampUnit (float value) noexcept
    {
        if (! std::isfinite (value))
            return 0.0f;

        return std::max (0.0f, std::min (1.0f, value));
    }

    float TransistorOrganEngine::onePoleCoefficient (
        double sampleRate,
        float frequencyHz) noexcept
    {
        return static_cast<float> (
            1.0 - std::exp (
                -twoPi * static_cast<double> (frequencyHz)
                / sampleRate));
    }

    float TransistorOrganEngine::smoothingCoefficient (
        double sampleRate,
        float milliseconds) noexcept
    {
        return static_cast<float> (
            1.0 - std::exp (
                -1.0 / (sampleRate
                        * static_cast<double> (milliseconds)
                        * 0.001)));
    }

    float TransistorOrganEngine::envelopeDecayCoefficient (
        double sampleRate,
        float milliseconds) noexcept
    {
        return static_cast<float> (
            std::exp (
                std::log (0.001)
                / (sampleRate
                   * static_cast<double> (milliseconds)
                   * 0.001)));
    }

    std::uint64_t TransistorOrganEngine::mixedHash (
        std::uint64_t value) noexcept
    {
        value += 0x9e3779b97f4a7c15ULL;
        value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
        return value ^ (value >> 31);
    }

    float TransistorOrganEngine::nextNoise (
        std::uint64_t& state) noexcept
    {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        const auto bits = state * 0x2545f4914f6cdd1dULL;
        const auto mantissa =
            static_cast<std::uint32_t> (bits >> 40);
        return static_cast<float> (mantissa)
                   * (2.0f / 16777215.0f)
               - 1.0f;
    }

    int TransistorOrganEngine::foldToDividerRange (
        int midiNote) noexcept
    {
        while (midiNote < TransistorDividerBank::lowestMidi)
            midiNote += 12;

        while (midiNote > TransistorDividerBank::highestMidi)
            midiNote -= 12;

        return midiNote;
    }

    bool TransistorOrganEngine::queueEvent (
        const TransistorOrganEvent& event) noexcept
    {
        if (! prepared_
             || event.sampleOffset >= spec_.maximumBlockSize
             || eventCount_ >= events_.size())
        {
            ++droppedEvents_;
            return false;
        }

        std::size_t insertion = eventCount_;

        while (insertion > 0
               && events_[insertion - 1].sampleOffset
                   > event.sampleOffset)
        {
            events_[insertion] = events_[insertion - 1];
            --insertion;
        }

        events_[insertion] = event;
        ++eventCount_;
        return true;
    }

    void TransistorOrganEngine::applyEvent (
        const TransistorOrganEvent& event) noexcept
    {
        if (event.manual != TransistorOrganManual::upper)
            return;

        switch (event.type)
        {
            case TransistorOrganEventType::noteOn:
                activateNote (event.note, event.velocity);
                break;

            case TransistorOrganEventType::noteOff:
                releaseNote (event.note);
                break;

            case TransistorOrganEventType::allNotesOff:
                releaseAllNotes();
                break;
        }
    }

    void TransistorOrganEngine::activateNote (
        int note,
        int velocity) noexcept
    {
        ensureActive (note);
        auto& state = notes_[static_cast<std::size_t> (note)];
        state.held = true;
        state.velocity = static_cast<std::uint8_t> (velocity);
        state.clickEnvelope =
            0.050f
            * (0.90f
               + 0.10f
                 * static_cast<float> (velocity)
                 * (1.0f / 127.0f));
        state.clickLowState = 0.0f;
        state.clickHighState = 0.0f;
        ++state.generation;
        state.randomState = mixedHash (
            spec_.deterministicSeed
            ^ (static_cast<std::uint64_t> (note + 1) << 32)
            ^ static_cast<std::uint64_t> (state.generation));

        if (state.randomState == 0)
            state.randomState = 1;
    }

    void TransistorOrganEngine::releaseNote (int note) noexcept
    {
        auto& state = notes_[static_cast<std::size_t> (note)];

        if (! state.held)
            return;

        state.held = false;
        state.clickEnvelope = 0.020f;
        state.clickLowState = 0.0f;
        state.clickHighState = 0.0f;
        ++state.generation;
        state.randomState = mixedHash (
            spec_.deterministicSeed
            ^ 0xd1b54a32d192ed03ULL
            ^ (static_cast<std::uint64_t> (note + 1) << 31)
            ^ static_cast<std::uint64_t> (state.generation));

        if (state.randomState == 0)
            state.randomState = 1;
    }

    void TransistorOrganEngine::releaseAllNotes() noexcept
    {
        for (int note = 0;
             note < static_cast<int> (transistorMidiNoteCount);
             ++note)
            if (notes_[static_cast<std::size_t> (note)].held)
                releaseNote (note);
    }

    void TransistorOrganEngine::ensureActive (int note) noexcept
    {
        const auto noteIndex = static_cast<std::size_t> (note);

        if (activeSlots_[noteIndex] >= 0)
            return;

        activeNotes_[activeNoteCount_] =
            static_cast<std::uint8_t> (note);
        activeSlots_[noteIndex] =
            static_cast<std::int16_t> (activeNoteCount_);
        ++activeNoteCount_;
        rebuildActiveTapMask();
    }

    void TransistorOrganEngine::removeActiveAt (
        std::size_t activeIndex) noexcept
    {
        const auto removed =
            static_cast<std::size_t> (activeNotes_[activeIndex]);
        --activeNoteCount_;

        if (activeIndex != activeNoteCount_)
        {
            const auto moved = activeNotes_[activeNoteCount_];
            activeNotes_[activeIndex] = moved;
            activeSlots_[static_cast<std::size_t> (moved)] =
                static_cast<std::int16_t> (activeIndex);
        }

        activeSlots_[removed] = -1;
        rebuildActiveTapMask();
    }

    void TransistorOrganEngine::rebuildActiveTapMask() noexcept
    {
        activeLowerTapMask_ = 0;
        activeUpperTapMask_ = 0;

        for (std::size_t active = 0;
             active < activeNoteCount_;
             ++active)
        {
            const auto note =
                static_cast<std::size_t> (activeNotes_[active]);

            for (std::size_t tab = 0;
                 tab < transistorTabCount;
                 ++tab)
                for (std::size_t component = 0;
                     component < 2;
                     ++component)
                {
                    const auto wheel =
                        routes_[note][tab].wheelMidi[component];

                    if (wheel < 0)
                        continue;

                    const auto tapIndex =
                        static_cast<std::size_t> (
                            wheel
                            - TransistorDividerBank::lowestMidi);

                    if (tapIndex < 64)
                        activeLowerTapMask_ |=
                            std::uint64_t { 1 } << tapIndex;
                    else
                        activeUpperTapMask_ |=
                            std::uint32_t { 1 } << (tapIndex - 64);
                }
        }

        dividerBank_.setActiveTapMask (
            activeLowerTapMask_,
            activeUpperTapMask_);
    }

    void TransistorOrganEngine::buildRankRoutes() noexcept
    {
        for (std::size_t note = 0;
             note < transistorMidiNoteCount;
             ++note)
            for (std::size_t tab = 0;
                 tab < transistorTabCount;
                 ++tab)
                for (std::size_t component = 0;
                     component < 2;
                     ++component)
                {
                    const auto& definition =
                        rankDefinitions[tab].components[component];
                    routes_[note][tab].wheelMidi[component] =
                        definition.gain > 0.0f
                            ? static_cast<std::int16_t> (
                                foldToDividerRange (
                                    static_cast<int> (note)
                                    + definition.semitoneOffset))
                            : static_cast<std::int16_t> (-1);
                }

        for (std::size_t offset = 0;
             offset < uniqueRankOffsets.size();
             ++offset)
            for (std::size_t note = 0;
                 note < transistorMidiNoteCount;
                 ++note)
                offsetTapIndices_[offset][note] =
                    static_cast<std::uint8_t> (
                        foldToDividerRange (
                            static_cast<int> (note)
                            + uniqueRankOffsets[offset])
                        - TransistorDividerBank::lowestMidi);
    }

    void TransistorOrganEngine::refreshProfileCoefficients() noexcept
    {
        const auto& definition = profileDefinition (spec_.profile);
        cabinetCoefficientLeft_ = onePoleCoefficient (
            spec_.sampleRate, definition.cabinetCutoffHz);
        cabinetCoefficientRight_ = onePoleCoefficient (
            spec_.sampleRate,
            definition.cabinetCutoffHz * 0.94f);
        allpassCoefficient_ = definition.allpassCoefficient;
        const double vibratoStep =
            twoPi
            * static_cast<double> (definition.vibratoRateHz)
            / spec_.sampleRate;
        vibratoSinStep_ =
            static_cast<float> (std::sin (vibratoStep));
        vibratoCosStep_ =
            static_cast<float> (std::cos (vibratoStep));
    }

    float TransistorOrganEngine::renderVoices() noexcept
    {
        if (activeNoteCount_ == 0)
            return 0.0f;

        if (activeNoteCount_ >= 48)
            return renderVoicesAggregated();

        const auto& profile = profileDefinition (spec_.profile);
        std::array<std::array<float, 2>, transistorTabCount>
            squareCoefficients {};
        std::array<std::array<float, 2>, transistorTabCount>
            pulseCoefficients {};
        std::array<std::array<float, 2>, transistorTabCount>
            rampCoefficients {};

        for (std::size_t tab = 0;
             tab < transistorTabCount;
             ++tab)
        {
            const auto& colour = profile.colours[tab];
            const float tabScale =
                tabCurrents_[tab] * colour.gain;

            for (std::size_t component = 0;
                 component < 2;
                 ++component)
            {
                const float scale =
                    tabScale
                    * rankDefinitions[tab]
                        .components[component].gain;
                squareCoefficients[tab][component] =
                    colour.square * scale;
                pulseCoefficients[tab][component] =
                    colour.pulse * scale;
                rampCoefficients[tab][component] =
                    colour.ramp * scale;
            }
        }

        float sum = 0.0f;
        std::size_t activeIndex = 0;

        while (activeIndex < activeNoteCount_)
        {
            const auto noteIndex =
                static_cast<std::size_t> (activeNotes_[activeIndex]);
            auto& note = notes_[noteIndex];
            const float target = note.held ? 1.0f : 0.0f;
            const float gateCoefficient =
                note.held
                    ? gateAttackCoefficient_
                    : gateReleaseCoefficient_;
            note.gate +=
                (target - note.gate) * gateCoefficient;

            float noteSignal = 0.0f;

            for (std::size_t tab = 0;
                 tab < transistorTabCount;
                 ++tab)
            {
                const float tabLevel = tabCurrents_[tab];

                if (tabLevel < 0.000001f)
                    continue;

                for (std::size_t component = 0;
                     component < 2;
                     ++component)
                {
                    const auto wheel =
                        routes_[noteIndex][tab]
                            .wheelMidi[component];

                    if (wheel < 0)
                        continue;

                    const auto tapIndex =
                        static_cast<std::size_t> (
                            wheel
                            - TransistorDividerBank::lowestMidi);
                    const auto& tap =
                        dividerBank_.tapAt (tapIndex);
                    noteSignal +=
                        squareCoefficients[tab][component]
                            * tap.square
                        + pulseCoefficients[tab][component]
                            * tap.pulse
                        + rampCoefficients[tab][component]
                            * tap.ramp;
                }
            }

            sum += noteSignal * note.gate;
            sum += renderKeyTransient (note);

            if (! note.held
                 && note.gate < 0.000001f
                 && note.clickEnvelope < 0.000001f)
            {
                note = {};
                removeActiveAt (activeIndex);
                continue;
            }

            ++activeIndex;
        }

        return sum * profile.outputGain;
    }

    float TransistorOrganEngine::renderVoicesAggregated() noexcept
    {
        const auto& profile = profileDefinition (spec_.profile);

        for (auto& offset : offsetGateSums_)
            offset.fill (0.0f);

        float sum = 0.0f;
        std::size_t activeIndex = 0;

        while (activeIndex < activeNoteCount_)
        {
            const auto noteIndex =
                static_cast<std::size_t> (activeNotes_[activeIndex]);
            auto& note = notes_[noteIndex];
            const float target = note.held ? 1.0f : 0.0f;
            const float gateCoefficient =
                note.held
                    ? gateAttackCoefficient_
                    : gateReleaseCoefficient_;
            note.gate +=
                (target - note.gate) * gateCoefficient;
            sum += renderKeyTransient (note);

            if (! note.held
                 && note.gate < 0.000001f
                 && note.clickEnvelope < 0.000001f)
            {
                note = {};
                removeActiveAt (activeIndex);
                continue;
            }

            for (std::size_t offset = 0;
                 offset < uniqueRankOffsets.size();
                 ++offset)
                offsetGateSums_[offset][
                    offsetTapIndices_[offset][noteIndex]]
                    += note.gate;

            ++activeIndex;
        }

        for (std::size_t tab = 0;
             tab < transistorTabCount;
             ++tab)
        {
            const float tabLevel = tabCurrents_[tab];

            if (tabLevel < 0.000001f)
                continue;

            const auto& colour = profile.colours[tab];
            const auto& definition = rankDefinitions[tab];
            const auto firstOffset =
                static_cast<std::size_t> (
                    rankOffsetIndices[tab][0]);
            const auto secondOffset =
                static_cast<std::size_t> (
                    rankOffsetIndices[tab][1]);
            const float squareCoefficient =
                colour.square * tabLevel * colour.gain;
            const float pulseCoefficient =
                colour.pulse * tabLevel * colour.gain;
            const float rampCoefficient =
                colour.ramp * tabLevel * colour.gain;
            float tabSum = 0.0f;

            if (definition.components[1].gain > 0.0f)
            {
                const float firstGain =
                    definition.components[0].gain;
                const float secondGain =
                    definition.components[1].gain;

                for (std::size_t tapIndex = 0;
                     tapIndex < TransistorDividerBank::tapCount;
                     ++tapIndex)
                {
                    const auto& tap =
                        dividerBank_.tapAt (tapIndex);
                    const float waveform =
                        squareCoefficient * tap.square
                        + pulseCoefficient * tap.pulse
                        + rampCoefficient * tap.ramp;
                    const float gate =
                        firstGain
                            * offsetGateSums_[firstOffset][tapIndex]
                        + secondGain
                            * offsetGateSums_[secondOffset][tapIndex];
                    tabSum += gate * waveform;
                }
            }
            else
            {
                const auto& gates =
                    offsetGateSums_[firstOffset];

                for (std::size_t tapIndex = 0;
                     tapIndex < TransistorDividerBank::tapCount;
                     ++tapIndex)
                {
                    const auto& tap =
                        dividerBank_.tapAt (tapIndex);
                    const float waveform =
                        squareCoefficient * tap.square
                        + pulseCoefficient * tap.pulse
                        + rampCoefficient * tap.ramp;
                    tabSum += gates[tapIndex] * waveform;
                }
            }

            sum += tabSum;
        }

        return sum * profile.outputGain;
    }

    float TransistorOrganEngine::renderKeyTransient (
        NoteState& note) noexcept
    {
        if (note.clickEnvelope < 0.000001f)
        {
            note.clickEnvelope = 0.0f;
            return 0.0f;
        }

        const float noise = nextNoise (note.randomState);
        note.clickLowState +=
            clickLowCoefficient_
            * (noise - note.clickLowState);
        const float highPassed = noise - note.clickLowState;
        note.clickHighState +=
            clickHighCoefficient_
            * (highPassed - note.clickHighState);
        const float output =
            note.clickHighState * note.clickEnvelope;
        note.clickEnvelope *=
            note.held
                ? clickOnDecayCoefficient_
                : clickOffDecayCoefficient_;
        return output;
    }

    float TransistorOrganEngine::processDriveAndDc (
        float input) noexcept
    {
        const float coupled =
            input - preDcInput_
            + dcBlockCoefficient_ * preDcOutput_;
        preDcInput_ = input;
        preDcOutput_ = coupled;

        const float driveGain = 1.0f + 3.2f * spec_.drive;
        const float scaled =
            coupled * spec_.outputTrim * driveGain;
        const float saturated = std::tanh (scaled);
        const float postCoupled =
            saturated - postDcInput_
            + dcBlockCoefficient_ * postDcOutput_;
        postDcInput_ = saturated;
        postDcOutput_ = postCoupled;
        return postCoupled;
    }

    void TransistorOrganEngine::processCabinet (
        float input,
        float& left,
        float& right) noexcept
    {
        cabinetStateLeft_ +=
            cabinetCoefficientLeft_
            * (input - cabinetStateLeft_);

        const float allpass =
            -allpassCoefficient_ * input + allpassState_;
        allpassState_ =
            input + allpassCoefficient_ * allpass;
        const float rightInput =
            input
            + spec_.stereoWidth * (allpass - input);
        cabinetStateRight_ +=
            cabinetCoefficientRight_
            * (rightInput - cabinetStateRight_);
        constexpr float cabinetHeadroom = 0.92f;
        const float leftScaled =
            cabinetStateLeft_ * cabinetHeadroom;
        const float rightScaled =
            cabinetStateRight_ * cabinetHeadroom;
        left = 0.98f * leftScaled
               / std::sqrt (1.0f + leftScaled * leftScaled);
        right = 0.98f * rightScaled
                / std::sqrt (1.0f + rightScaled * rightScaled);
    }

    float TransistorOrganEngine::vibratoPitchMultiplier() noexcept
    {
        const auto& definition = profileDefinition (spec_.profile);
        const float lfo = vibratoSin_;
        const float nextSin =
            vibratoSin_ * vibratoCosStep_
            + vibratoCos_ * vibratoSinStep_;
        const float nextCos =
            vibratoCos_ * vibratoCosStep_
            - vibratoSin_ * vibratoSinStep_;
        vibratoSin_ = nextSin;
        vibratoCos_ = nextCos;

        ++vibratoNormaliseCounter_;

        if (vibratoNormaliseCounter_ >= 4096)
        {
            const float magnitudeSquared =
                vibratoSin_ * vibratoSin_
                + vibratoCos_ * vibratoCos_;
            const float inverseMagnitude =
                1.0f / std::sqrt (
                    std::max (magnitudeSquared, 0.000001f));
            vibratoSin_ *= inverseMagnitude;
            vibratoCos_ *= inverseMagnitude;
            vibratoNormaliseCounter_ = 0;
        }

        if (! spec_.vibratoEnabled
             || spec_.vibratoDepth <= 0.0f)
            return 1.0f;

        const float octaves =
            lfo
            * definition.vibratoDepthCents
            * spec_.vibratoDepth
            * (1.0f / 1200.0f);
        const float exponent =
            octaves * 0.6931471805599453f;

        // Expansao suficiente para |exponent| < 0.011.
        return 1.0f
               + exponent
               + 0.5f * exponent * exponent
               + (1.0f / 6.0f)
                 * exponent * exponent * exponent;
    }

    void TransistorOrganEngine::publishDiagnostics (
        float peak,
        double sum,
        double sumSquares,
        std::size_t samples,
        std::uint64_t) noexcept
    {
        diagnostics_.renderedSamples = renderedSamples_;
        diagnostics_.droppedEvents = droppedEvents_;
        diagnostics_.invalidSamples = invalidSamples_;
        diagnostics_.activeUpperNotes =
            static_cast<std::uint32_t> (activeNoteCount_);
        diagnostics_.lastBlockPeak = peak;

        if (samples > 0)
        {
            const double inverse =
                1.0 / static_cast<double> (samples);
            diagnostics_.lastBlockMean =
                static_cast<float> (sum * inverse);
            diagnostics_.lastBlockRms =
                static_cast<float> (
                    std::sqrt (sumSquares * inverse));
        }
        else
        {
            diagnostics_.lastBlockMean = 0.0f;
            diagnostics_.lastBlockRms = 0.0f;
        }
    }
}
