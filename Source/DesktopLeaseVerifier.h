#pragma once

#include <juce_core/juce_core.h>

namespace ChurchDAWDesktopLicensing
{
struct VerifiedLease
{
    juce::String email;
    juce::String displayName;
    juce::String activationLabel;
    juce::String installationId;
    juce::String licenseId;
    juce::String productCode;
    juce::int64 issuedAt = 0;
    juce::int64 leaseExpiresAt = 0;
    juce::int64 offlineUntil = 0;
};

bool verifyLeaseToken (const juce::String& token,
                       const juce::String& expectedInstallationId,
                       juce::int64 currentTime,
                       bool allowOfflineWindow,
                       VerifiedLease& verified,
                       juce::String& error);
}
