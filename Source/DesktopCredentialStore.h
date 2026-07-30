#pragma once

#include <juce_core/juce_core.h>

namespace ChurchDAWDesktopLicensing
{
// Armazena exclusivamente segredos de sessao/dispositivo no cofre nativo.
// Metadados nao secretos (lease assinado, e-mail exibido e timestamps) ficam
// em um cache separado e nunca passam por esta interface.
class CredentialStore final
{
public:
    static bool saveSecret (const juce::String& account,
                            const juce::String& secret,
                            juce::String& error);

    static juce::String loadSecret (const juce::String& account,
                                    juce::String& error);

    static bool removeSecret (const juce::String& account,
                              juce::String& error);

private:
    CredentialStore() = delete;
};
}
