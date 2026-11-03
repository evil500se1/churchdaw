#pragma once

#include <juce_core/juce_core.h>
#include "DistributionFailure.h"

namespace ChurchDAWLibraryStatus
{
    struct Presentation
    {
        juce::String button;
        juce::String message;
        juce::String empty;
    };

    inline Presentation unavailable (ChurchDAWDistribution::Failure failure, bool installed)
    {
        using Failure = ChurchDAWDistribution::Failure;
        Presentation result;
        switch (failure)
        {
            case Failure::connection:
                result.button = juce::String::fromUTF8 ("Sem conexão");
                result.message = installed
                    ? juce::String::fromUTF8 ("Sem conexão. Bibliotecas instaladas continuam disponíveis.")
                    : juce::String::fromUTF8 ("Sem conexão com o catálogo. Tente atualizar.");
                result.empty = juce::String::fromUTF8 ("Catálogo indisponível.\nConecte-se e toque em Atualizar catálogo.");
                break;
            case Failure::httpResponse:
                result.button = juce::String::fromUTF8 ("Indisponível");
                result.message = installed
                    ? juce::String::fromUTF8 ("Servidor indisponível. Bibliotecas instaladas continuam disponíveis.")
                    : juce::String::fromUTF8 ("Catálogo indisponível no servidor. Tente novamente.");
                result.empty = juce::String::fromUTF8 ("Catálogo temporariamente indisponível.\nToque em Atualizar catálogo para tentar novamente.");
                break;
            case Failure::validation:
                result.button = juce::String::fromUTF8 ("Não validado");
                result.message = installed
                    ? juce::String::fromUTF8 ("Catálogo não validado. Bibliotecas instaladas continuam disponíveis.")
                    : juce::String::fromUTF8 ("Não foi possível validar o catálogo. Tente atualizar.");
                result.empty = juce::String::fromUTF8 ("Não foi possível validar o catálogo.\nToque em Atualizar catálogo para tentar novamente.");
                break;
            case Failure::none:
                result.button = juce::String::fromUTF8 ("Indisponível");
                result.message = installed
                    ? juce::String::fromUTF8 ("Catálogo indisponível. Bibliotecas instaladas continuam disponíveis.")
                    : juce::String::fromUTF8 ("Catálogo indisponível. Tente atualizar.");
                result.empty = juce::String::fromUTF8 ("Catálogo indisponível.\nToque em Atualizar catálogo para tentar novamente.");
                break;
        }
        return result;
    }
}
