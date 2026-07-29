#include <iostream>
#include <array>
#include <algorithm>

#include <juce_core/juce_core.h>

#include "DesktopLeaseVerifier.h"
#include "DesktopSecureRandom.h"

namespace
{
int fail (const char* message)
{
    std::cerr << "FAIL: " << message << std::endl;
    return 1;
}
}

int main (int argc, char** argv)
{
    if (argc != 4)
        return fail ("uso: teste <token-file> <installation-id> <unix-time>");

    const auto token = juce::File (juce::String::fromUTF8 (argv[1]))
                           .loadFileAsString()
                           .trim();
    const auto installationId = juce::String::fromUTF8 (argv[2]);
    const auto now = juce::String::fromUTF8 (argv[3]).getLargeIntValue();
    ChurchDAWDesktopLicensing::VerifiedLease lease;
    juce::String error;

    std::array<unsigned char, 64> randomA {};
    std::array<unsigned char, 64> randomB {};
    if (! ChurchDAWDesktopLicensing::fillSecureRandom (
            randomA.data(), randomA.size())
        || ! ChurchDAWDesktopLicensing::fillSecureRandom (
            randomB.data(), randomB.size())
        || std::all_of (randomA.begin(), randomA.end(),
                        [] (auto value) { return value == 0; })
        || randomA == randomB)
        return fail ("CSPRNG nativo indisponivel ou repetido");

    if (! ChurchDAWDesktopLicensing::verifyLeaseToken (
            token, installationId, now, false, lease, error))
    {
        std::cerr << error << std::endl;
        return fail ("lease valido foi recusado");
    }

    if (lease.installationId != installationId
        || lease.email.isEmpty()
        || (lease.productCode != "churchdaw-perpetual"
            && lease.productCode != "churchdaw-desktop-perpetual"))
        return fail ("claims comerciais validados sao inconsistentes");

    auto altered = token;
    const auto firstDot = altered.indexOfChar ('.');
    const auto secondDot = altered.indexOfChar (firstDot + 1, '.');
    if (firstDot <= 0 || secondDot <= firstDot + 4)
        return fail ("fixture malformado");
    const auto position = firstDot + 3;
    altered = altered.replaceSection (
        position, 1, altered[position] == 'A' ? "B" : "A");

    if (ChurchDAWDesktopLicensing::verifyLeaseToken (
            altered, installationId, now, false, lease, error))
        return fail ("lease adulterado foi aceito");

    if (ChurchDAWDesktopLicensing::verifyLeaseToken (
            token, "outro-computador", now, false, lease, error))
        return fail ("lease foi aceito em outro computador");

    if (! ChurchDAWDesktopLicensing::verifyLeaseToken (
            token, installationId, now, true, lease, error))
        return fail ("janela offline valida foi recusada");

    std::cout << "PASS: CSPRNG, RS256, claims, adulteracao e vinculo de dispositivo"
              << std::endl;
    return 0;
}
