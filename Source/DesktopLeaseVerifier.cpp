#include "DesktopLeaseVerifier.h"
#include "DesktopLeaseSignature.h"
#include "DesktopLicensePublicKey.h"

namespace ChurchDAWDesktopLicensing
{
namespace
{
juce::String propertyString (const juce::var& value, const char* name)
{
    if (const auto* object = value.getDynamicObject())
        return object->getProperty (name).toString();
    return {};
}

juce::int64 propertyInt64 (const juce::var& value, const char* name)
{
    if (const auto* object = value.getDynamicObject())
        return static_cast<juce::int64> (object->getProperty (name));
    return 0;
}

bool decodeBase64Url (juce::String value, juce::MemoryBlock& decoded)
{
    if (value.isEmpty()
        || ! value.containsOnly (
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"))
        return false;

    value = value.replaceCharacter ('-', '+').replaceCharacter ('_', '/');
    while (value.length() % 4 != 0)
        value += '=';
    juce::MemoryOutputStream output;
    if (! juce::Base64::convertFromBase64 (output, value))
        return false;
    decoded.replaceWith (output.getData(), output.getDataSize());
    return true;
}

bool parseJsonBlock (const juce::MemoryBlock& data, juce::var& parsed)
{
    if (data.isEmpty() || data.getSize() > 32 * 1024)
        return false;
    const auto json = juce::String::fromUTF8 (
        static_cast<const char*> (data.getData()),
        static_cast<int> (data.getSize()));
    return juce::JSON::parse (json, parsed).wasOk()
        && parsed.getDynamicObject() != nullptr;
}
}

bool verifyLeaseToken (const juce::String& token,
                       const juce::String& expectedInstallationId,
                       juce::int64 currentTime,
                       bool allowOfflineWindow,
                       VerifiedLease& verified,
                       juce::String& error)
{
    verified = {};
    error.clear();

    const auto parts = juce::StringArray::fromTokens (token, ".", "");
    if (parts.size() != 3 || token.length() > 32 * 1024)
    {
        error = "Formato de licenca invalido";
        return false;
    }

    juce::MemoryBlock headerData;
    juce::MemoryBlock payloadData;
    juce::MemoryBlock signature;
    if (! decodeBase64Url (parts[0], headerData)
        || ! decodeBase64Url (parts[1], payloadData)
        || ! decodeBase64Url (parts[2], signature))
    {
        error = "Codificacao de licenca invalida";
        return false;
    }

    const auto signedText = parts[0] + "." + parts[1];
    const juce::MemoryBlock message (
        signedText.toRawUTF8(),
        static_cast<size_t> (signedText.getNumBytesAsUTF8()));
    if (! verifyLeaseRs256Signature (message, signature, error))
        return false;

    juce::var header;
    juce::var payload;
    if (! parseJsonBlock (headerData, header)
        || ! parseJsonBlock (payloadData, payload)
        || propertyString (header, "alg") != "RS256"
        || propertyString (header, "typ") != "churchdaw-lease+jwt"
        || propertyString (header, "kid") != licenseKeyId)
    {
        error = "Cabecalho de licenca invalido";
        return false;
    }

    verified.email = propertyString (payload, "email");
    verified.displayName = propertyString (payload, "display_name");
    verified.activationLabel = propertyString (payload, "activation_label");
    verified.installationId = propertyString (payload, "installation_id");
    verified.licenseId = propertyString (payload, "license_id");
    verified.productCode = propertyString (payload, "product_code");
    verified.issuedAt = propertyInt64 (payload, "iat");
    verified.leaseExpiresAt = propertyInt64 (payload, "lease_expires_at");
    verified.offlineUntil = propertyInt64 (payload, "offline_until");
    const auto notBefore = propertyInt64 (payload, "nbf");

    const auto validUntil = allowOfflineWindow
                              ? verified.offlineUntil
                              : verified.leaseExpiresAt;
    if (propertyString (payload, "iss") != "https://churchdaw.com"
        || propertyString (payload, "aud") != "churchdaw-desktop"
        || (verified.productCode != "churchdaw-perpetual"
            && verified.productCode != "churchdaw-desktop-perpetual")
        || verified.installationId.isEmpty()
        || verified.installationId != expectedInstallationId
        || verified.email.isEmpty()
        || verified.licenseId.isEmpty()
        || verified.issuedAt <= 0
        || notBefore > currentTime + 300
        || verified.issuedAt > currentTime + 300
        || verified.leaseExpiresAt <= verified.issuedAt
        || verified.offlineUntil < verified.leaseExpiresAt
        || validUntil < currentTime)
    {
        error = "Licenca expirada ou vinculada a outro computador";
        verified = {};
        return false;
    }

    return true;
}
}
