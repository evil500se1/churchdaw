#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace ChurchDAWPerformanceMidiSafety
{
    template <std::size_t NumLayers>
    class NoteTransposeLedger
    {
    public:
        static constexpr std::size_t maxOverlappingNoteOns = 8;

        bool recordNoteOn (int layer,
                           int midiChannel,
                           int inputNote,
                           int outputNote) noexcept
        {
            if (! isValid (layer, midiChannel, inputNote))
                return false;

            auto& entry = entries[(std::size_t) layer]
                                 [(std::size_t) (midiChannel - 1)]
                                 [(std::size_t) inputNote];
            const auto offset = (juce::int8) juce::jlimit (
                -127, 127, outputNote - inputNote);

            if (entry.count >= maxOverlappingNoteOns)
                return false;

            const auto tail =
                (entry.head + entry.count) % maxOverlappingNoteOns;
            entry.offsets[tail] = offset;
            ++entry.count;
            return true;
        }

        int translatedNoteOff (int layer,
                               int midiChannel,
                               int inputNote) noexcept
        {
            if (! isValid (layer, midiChannel, inputNote))
                return juce::jlimit (0, 127, inputNote);

            auto& exactEntry = entries[(std::size_t) layer]
                                 [(std::size_t) (midiChannel - 1)]
                                 [(std::size_t) inputNote];
            int offset = 0;

            if (exactEntry.count > 0)
            {
                offset = (int) exactEntry.offsets[exactEntry.head];
                exactEntry.head = (exactEntry.head + 1) % maxOverlappingNoteOns;
                --exactEntry.count;

                if (exactEntry.count == 0)
                    exactEntry.head = 0;

                return juce::jlimit (0, 127, inputNote + offset);
            }

            // Fallback robusto para rotas host/mídia em que o Note-Off chega
            // em canal diferente. Mantém comportamento por camada, sem perder
            // transposição da nota quando o estado legado ficou “fora do canal”.
            for (std::size_t ch = 1; ch <= 16; ++ch)
            {
                auto& candidate = entries[(std::size_t) layer][ch - 1]
                                  [(std::size_t) inputNote];

                if (candidate.count <= 0)
                    continue;

                offset = (int) candidate.offsets[candidate.head];
                candidate.head = (candidate.head + 1) % maxOverlappingNoteOns;
                --candidate.count;

                if (candidate.count == 0)
                    candidate.head = 0;

                return juce::jlimit (0, 127, inputNote + offset);
            }

            return juce::jlimit (0, 127, inputNote);
        }

        void clear() noexcept
        {
            for (auto& layer : entries)
                for (auto& channel : layer)
                    for (auto& entry : channel)
                    {
                        entry.offsets.fill (0);
                        entry.head = 0;
                        entry.count = 0;
                    }
        }

        // Limpa somente a camada que está sendo substituída. O storage é
        // inteiramente fixo/POD: nenhum lock ou alocação entra na fronteira de
        // swap suspensa do processador.
        void clearLayer (int layer) noexcept
        {
            if (layer < 0 || layer >= (int) NumLayers)
                return;

            for (auto& channel :
                 entries[(std::size_t) layer])
                for (auto& entry : channel)
                {
                    entry.offsets.fill (0);
                    entry.head = 0;
                    entry.count = 0;
                }
        }

    private:
        struct Entry
        {
            std::array<juce::int8, maxOverlappingNoteOns> offsets {};
            std::size_t head = 0;
            std::size_t count = 0;
        };

        static bool isValid (int layer,
                             int midiChannel,
                             int note) noexcept
        {
            return layer >= 0 && layer < (int) NumLayers
                && midiChannel >= 1 && midiChannel <= 16
                && note >= 0 && note < 128;
        }

        std::array<
            std::array<std::array<Entry, 128>, 16>,
            NumLayers> entries {};
    };

    // Protege instrumentos hospedados contra um Pitch Wheel que ficou fora do
    // centro porque o evento de retorno se perdeu numa troca de rota/plugin.
    //
    // A regra e deliberadamente conservadora:
    //   * nunca interfere enquanto ainda ha tecla ou sustain pressionado;
    //   * um pitch bend recebido sem teclas e considerado pre-bend intencional;
    //   * somente um bend herdado da frase ANTERIOR e recentrado, imediatamente
    //     antes do primeiro note-on da frase seguinte;
    //   * note-off + note-on no mesmo sample continuam sendo legato e preservam
    //     o bend.
    //
    // Todo o estado tem tamanho fixo. process() apenas escreve no MidiBuffer de
    // scratch que o chamador ja reservou em prepareToPlay().
    class PitchBendGuard
    {
    public:
        static constexpr int centreValue = 8192;

        void reset() noexcept
        {
            for (auto& channel : channels)
                channel = {};

            physicalSustainDown = false;
            emitPitchCentresOnNextBlock = false;
        }

        // prepareToPlay/reabertura de rota pode manter a mesma instancia AU/VST
        // viva. O primeiro bloco precisa portanto enviar o centro de verdade,
        // nao apenas zerar o espelho interno.
        void requestPitchCentresOnNextBlock() noexcept
        {
            reset();
            emitPitchCentresOnNextBlock = true;
        }

        void process (const juce::MidiBuffer& source,
                      juce::MidiBuffer& destination,
                      bool forceGlobalSustain = false,
                      int physicalSustainAtBlockStart = -1)
        {
            jassert (&source != &destination);
            destination.clear();

            // O host conhece o estado real do pedal no início do callback.
            // Após load/reprepare o guard foi resetado, mas o teclado pode
            // continuar com CC64 abaixado sem retransmiti-lo. -1 mantém a
            // compatibilidade dos testes/chamadores que deixam o próprio guard
            // carregar o estado entre blocos.
            if (physicalSustainAtBlockStart >= 0)
                physicalSustainDown =
                    physicalSustainAtBlockStart != 0;

            if (emitPitchCentresOnNextBlock)
            {
                for (int channel = 1; channel <= 16; ++channel)
                    destination.addEvent (
                        juce::MidiMessage::pitchWheel (channel, centreValue), 0);

                emitPitchCentresOnNextBlock = false;
            }

            std::array<bool, 16> phraseEndedThisBlock {};
            std::array<int, 16> phraseEndSample {};
            std::uint16_t pendingAllSoundOffChannels = 0;
            std::uint16_t pendingAllNotesOffChannels = 0;
            int pendingAllOffSample = 0;
            bool hasPendingAllOff = false;

            // No ChurchDAW, tanto o pedal físico quanto o toggle SUSTAIN são
            // globais: sampler, Church Organ e VST/AU recebem o mesmo estado
            // em todos os canais. O guard precisa espelhar essa semântica ou
            // encerraria a frase do canal 1 enquanto uma voz ainda está
            // legitimamente sustentada por CC64 vindo do canal 4/da UI.
            const bool sustainAtBlockStart =
                forceGlobalSustain || physicalSustainDown;

            for (std::size_t channelIndex = 0;
                 channelIndex < channels.size();
                 ++channelIndex)
            {
                auto& state = channels[channelIndex];
                const bool wasDown = state.sustainDown;
                state.sustainDown = sustainAtBlockStart;

                if (wasDown && ! sustainAtBlockStart)
                    markPhraseEndIfNeeded (
                        state, channelIndex, -1,
                        phraseEndedThisBlock,
                        phraseEndSample);
            }

            const auto flushGlobalAllOffBoundary = [&]
            {
                if (! hasPendingAllOff)
                    return;

                // CC120/123 e uma fronteira global no host: qualquer canal
                // pode ser a origem do PANIC. Todos os instrumentos recebem
                // um Pitch Wheel real no centro antes dos all-off adiados.
                for (int channel = 1; channel <= 16; ++channel)
                    destination.addEvent (
                        juce::MidiMessage::pitchWheel (
                            channel, centreValue),
                        pendingAllOffSample);

                // Os all-off ficam por ultimo no sample. Duplicatas do mesmo
                // controlador/canal sao coalescidas por bitmasks fixos.
                for (int channel = 1; channel <= 16; ++channel)
                {
                    const auto channelBit = static_cast<std::uint16_t> (
                        1u << (channel - 1));

                    if ((pendingAllSoundOffChannels & channelBit) != 0)
                        destination.addEvent (
                            juce::MidiMessage::allSoundOff (channel),
                            pendingAllOffSample);

                    if ((pendingAllNotesOffChannels & channelBit) != 0)
                        destination.addEvent (
                            juce::MidiMessage::allNotesOff (channel),
                            pendingAllOffSample);
                }

                physicalSustainDown = false;

                for (auto& channel : channels)
                {
                    resetChannel (channel);
                    channel.sustainDown =
                        forceGlobalSustain;
                }

                phraseEndedThisBlock.fill (false);
                phraseEndSample.fill (0);
                pendingAllSoundOffChannels = 0;
                pendingAllNotesOffChannels = 0;
                pendingAllOffSample = 0;
                hasPendingAllOff = false;
            };

            for (const auto meta : source)
            {
                if (hasPendingAllOff
                     && meta.samplePosition != pendingAllOffSample)
                    flushGlobalAllOffBoundary();

                const auto message = meta.getMessage();
                const int midiChannel = message.getChannel();

                if (midiChannel < 1 || midiChannel > 16)
                {
                    destination.addEvent (message, meta.samplePosition);
                    continue;
                }

                const auto channelIndex = (std::size_t) (midiChannel - 1);
                auto& state = channels[channelIndex];

                if (message.isPitchWheel())
                {
                    state.pitchWheel = message.getPitchWheelValue();

                    // Qualquer Pitch Wheel novo enquanto o canal esta ocioso e
                    // uma intencao explicita (inclusive um pre-bend). Nao o
                    // sobrescrever antes da proxima nota.
                    if (! hasPhysicalKeysDown (state) && ! state.sustainDown)
                    {
                        state.staleFromPreviousPhrase = false;
                        phraseEndedThisBlock[channelIndex] = false;
                    }
                }
                else if (message.isNoteOn())
                {
                    const bool sameSampleLegato =
                        state.staleFromPreviousPhrase
                        && phraseEndedThisBlock[channelIndex]
                        && phraseEndSample[channelIndex] == meta.samplePosition;

                    if (state.staleFromPreviousPhrase && ! sameSampleLegato)
                    {
                        // Inserido antes do note-on no mesmo sample: o AU/VST
                        // calcula o ataque ja afinado, sem degrau audivel depois.
                        destination.addEvent (
                            juce::MidiMessage::pitchWheel (
                                midiChannel, centreValue),
                            meta.samplePosition);
                        state.pitchWheel = centreValue;
                    }

                    state.staleFromPreviousPhrase = false;
                    phraseEndedThisBlock[channelIndex] = false;
                    recordNoteOn (state, message.getNoteNumber());
                }
                else if (message.isNoteOff())
                {
                    recordNoteOff (state, message.getNoteNumber());
                    markPhraseEndIfNeeded (
                        state, channelIndex, meta.samplePosition,
                        phraseEndedThisBlock, phraseEndSample);
                }
                else if (message.isController())
                {
                    const int controller = message.getControllerNumber();

                    if (controller == 64)
                    {
                        physicalSustainDown =
                            message.getControllerValue() >= 64;

                        const bool effectiveSustain =
                            forceGlobalSustain
                            || physicalSustainDown;

                        for (std::size_t globalChannel = 0;
                             globalChannel < channels.size();
                             ++globalChannel)
                        {
                            auto& globalState =
                                channels[globalChannel];
                            globalState.sustainDown =
                                effectiveSustain;

                            if (! effectiveSustain)
                                markPhraseEndIfNeeded (
                                    globalState,
                                    globalChannel,
                                    meta.samplePosition,
                                    phraseEndedThisBlock,
                                    phraseEndSample);
                        }
                    }
                    else if (controller == 121)
                    {
                        // O host replica CC121 para todos os 16 canais. Faça o
                        // mesmo no espelho do guard e emita centros reais para
                        // que nenhum AU/VST preserve bend ou sustain de outro
                        // canal. O toggle global é reaplicado pelo roteador.
                        physicalSustainDown = false;

                        for (auto& globalState : channels)
                        {
                            // Reset All Controllers NÃO encerra notas. Apagar
                            // noteDepth/physicalKeysDown aqui fazia o guard
                            // declarar fim de frase enquanto uma tecla ainda
                            // soava; um note-off de outra tecla então armava um
                            // centro espúrio e desafinava a nota sustentada.
                            globalState.pitchWheel =
                                centreValue;
                            globalState.sustainDown =
                                forceGlobalSustain;
                            globalState.staleFromPreviousPhrase =
                                false;
                        }

                        phraseEndedThisBlock.fill (false);
                        phraseEndSample.fill (0);
                        destination.addEvent (message, meta.samplePosition);

                        for (int channel = 1;
                             channel <= 16;
                             ++channel)
                        {
                            destination.addEvent (
                                juce::MidiMessage::pitchWheel (
                                    channel, centreValue),
                                meta.samplePosition);
                        }

                        continue;
                    }
                    else if (controller == 120 || controller == 123)
                    {
                        if (! hasPendingAllOff)
                        {
                            pendingAllOffSample = meta.samplePosition;
                            hasPendingAllOff = true;
                        }

                        const auto channelBit =
                            static_cast<std::uint16_t> (
                                1u << (midiChannel - 1));

                        if (controller == 120)
                            pendingAllSoundOffChannels |= channelBit;
                        else
                            pendingAllNotesOffChannels |= channelBit;

                        // Todo evento que nao seja all-off continua na ordem
                        // original deste sample. O boundary sera emitido por
                        // ultimo, depois dos centros dos 16 canais.
                        continue;
                    }
                }

                destination.addEvent (message, meta.samplePosition);
            }

            flushGlobalAllOffBoundary();
        }

    private:
        struct ChannelState
        {
            std::array<juce::uint8, 128> noteDepth {};
            int physicalKeysDown = 0;
            int pitchWheel = centreValue;
            bool sustainDown = false;
            bool staleFromPreviousPhrase = false;
        };

        static bool hasPhysicalKeysDown (
            const ChannelState& state) noexcept
        {
            return state.physicalKeysDown > 0;
        }

        static void recordNoteOn (ChannelState& state, int note) noexcept
        {
            if (! juce::isPositiveAndBelow (note, 128))
                return;

            auto& depth = state.noteDepth[(std::size_t) note];

            if (depth < std::numeric_limits<juce::uint8>::max())
            {
                ++depth;
                ++state.physicalKeysDown;
            }
        }

        static void recordNoteOff (ChannelState& state, int note) noexcept
        {
            if (! juce::isPositiveAndBelow (note, 128))
                return;

            auto& depth = state.noteDepth[(std::size_t) note];

            if (depth > 0)
            {
                --depth;
                state.physicalKeysDown =
                    juce::jmax (0, state.physicalKeysDown - 1);
            }
        }

        static void resetChannel (ChannelState& state) noexcept
        {
            state = {};
        }

        static void markPhraseEndIfNeeded (
            ChannelState& state,
            std::size_t channelIndex,
            int samplePosition,
            std::array<bool, 16>& phraseEndedThisBlock,
            std::array<int, 16>& phraseEndSample) noexcept
        {
            if (hasPhysicalKeysDown (state) || state.sustainDown)
                return;

            state.staleFromPreviousPhrase =
                state.pitchWheel != centreValue;
            phraseEndedThisBlock[channelIndex] =
                state.staleFromPreviousPhrase;
            phraseEndSample[channelIndex] = samplePosition;
        }

        std::array<ChannelState, 16> channels {};
        bool physicalSustainDown = false;
        bool emitPitchCentresOnNextBlock = false;
    };

    inline void appendFullPanic (juce::MidiBuffer& destination, int samplePosition)
    {
        for (int channel = 1; channel <= 16; ++channel)
        {
            destination.addEvent (
                juce::MidiMessage::controllerEvent (channel, 64, 0), samplePosition);
            destination.addEvent (
                juce::MidiMessage::controllerEvent (channel, 121, 0), samplePosition);
            destination.addEvent (
                juce::MidiMessage::pitchWheel (channel, 8192), samplePosition);
            destination.addEvent (
                juce::MidiMessage::allNotesOff (channel), samplePosition);
            destination.addEvent (
                juce::MidiMessage::allSoundOff (channel), samplePosition);
        }
    }
}
