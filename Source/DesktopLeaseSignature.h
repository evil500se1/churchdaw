#pragma once

#include <juce_core/juce_core.h>

namespace ChurchDAWDesktopLicensing
{
bool verifyLeaseRs256Signature (const juce::MemoryBlock& message,
                                const juce::MemoryBlock& signature,
                                juce::String& error);
}
