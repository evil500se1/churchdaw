#pragma once

#include <juce_core/juce_core.h>

// Verificacao estritamente local da policy no iOS/iPadOS. A implementacao
// fica fora do updater desktop para o alvo movel nao herdar UI, download de
// instalador ou qualquer outra responsabilidade de desktop.
namespace ChurchDAWIOSPolicy
{
bool verifySignedPolicyEnvelope (const juce::String& payloadBase64,
                                 const juce::String& signatureBase64,
                                 const juce::String& keyId);
}
