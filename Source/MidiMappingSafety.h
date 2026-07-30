#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace ChurchDAWMidiMapping
{
    struct ControllerKey
    {
        juce::String sourceId;
        int channel = 0;
        int cc = -1;

        bool isValid() const noexcept
        {
            return sourceId.isNotEmpty()
                    && channel >= 1 && channel <= 16
                    && cc >= 0 && cc <= 127;
        }

        bool operator< (const ControllerKey& other) const noexcept
        {
            const int sourceOrder = sourceId.compare (other.sourceId);

            if (sourceOrder != 0)
                return sourceOrder < 0;

            return channel != other.channel ? channel < other.channel
                                            : cc < other.cc;
        }
    };

    enum class LaunchkeyEncoderTarget
    {
        none,
        reverbReturn,
        padVolume,
        channelComp
    };

    struct LaunchkeyEncoderRoute
    {
        LaunchkeyEncoderTarget target = LaunchkeyEncoderTarget::none;
        int layer = -1;
    };

    inline LaunchkeyEncoderRoute launchkeyEncoderRoute (int index) noexcept
    {
        if (index == 0)
            return { LaunchkeyEncoderTarget::reverbReturn, -1 };

        if (index == 1)
            return { LaunchkeyEncoderTarget::padVolume, -1 };

        if (index >= 2 && index <= 6)
            return { LaunchkeyEncoderTarget::channelComp, index - 2 };

        // Knob 8 fica deliberadamente sem alvo fixo. Em particular, ele nunca
        // volta a ser Transpose global por uma refatoracao acidental.
        return {};
    }

    inline juce::String stableParameterId (const juce::AudioProcessorParameter* parameter)
    {
        if (const auto* withId =
                dynamic_cast<const juce::AudioProcessorParameterWithID*> (parameter))
            return withId->getParameterID();

        return {};
    }

    inline bool isProtectedPitchTarget (
        const juce::String& stableId,
        const juce::String& legacyName) noexcept
    {
        // Transpose de canal e global sao controles deliberados da interface,
        // nao destinos seguros de MIDI Learn. Um CC espurio ou um mapa legado
        // para estes parametros muda o tom de todo o culto e fica persistido.
        return stableId.equalsIgnoreCase ("gtransp")
            || stableId.startsWithIgnoreCase ("transp")
            || legacyName.containsIgnoreCase ("transpose")
            || legacyName.containsIgnoreCase ("transpor")
            || legacyName.containsIgnoreCase (
                juce::String::fromUTF8 ("transposi\xc3\xa7"));
    }

    // Nunca resolve um mapa persistido apenas pelo indice. A ordem dos
    // parametros muda quando o ChurchDAW ganha um controle novo; reutilizar o
    // indice antigo pode transformar, por exemplo, um fader de volume em
    // Transpose. ID e a ancora primaria; nome existe somente para migrar mapas
    // criados antes de o ID ser persistido.
    inline int resolveParameterIndex (
        const juce::Array<juce::AudioProcessorParameter*>& parameters,
        const juce::String& stableId,
        const juce::String& legacyName)
    {
        // Vale tambem para um mapa aprendido na sessao atual: ele pode ate
        // existir temporariamente no arquivo antigo, mas jamais sera aplicado.
        // Na proxima restauracao isSafeStoredTarget() o remove definitivamente.
        if (isProtectedPitchTarget (stableId, legacyName))
            return -1;

        if (stableId.isNotEmpty())
        {
            for (int i = 0; i < parameters.size(); ++i)
                if (stableParameterId (parameters[i]) == stableId)
                    return i;

            // ID presente, mas ausente no processador atual: o plugin/schema
            // mudou. Nao cair para um nome generico e arriscar outro alvo.
            return -1;
        }

        if (legacyName.isNotEmpty())
            for (int i = 0; i < parameters.size(); ++i)
                if (parameters[i]->getName (128) == legacyName
                     || parameters[i]->getName (32) == legacyName)
                    return i;

        return -1;
    }

    inline bool isSafeStoredTarget (int slot,
                                    const juce::String& stableId,
                                    const juce::String& legacyName) noexcept
    {
        if (isProtectedPitchTarget (stableId, legacyName))
            return false;

        // Master nao aponta para a lista mutavel de parametros do instrumento.
        return slot == -1 || stableId.isNotEmpty() || legacyName.isNotEmpty();
    }

    inline bool isMappableController (int cc) noexcept
    {
        // Pedais, Bank Select, Data Entry e RPN/NRPN aparecem no boot e na
        // configuracao de pitch-bend de muitos controladores. Channel Mode
        // (120..127) tambem nunca pode controlar parametro salvo.
        if (cc < 0 || cc >= 120)
            return false;

        switch (cc)
        {
            case 0:
            case 6:
            case 32:
            case 38:
            case 64:
            case 66:
            case 67:
            case 96:
            case 97:
            case 98:
            case 99:
            case 100:
            case 101:
                return false;
            default:
                return true;
        }
    }
}
