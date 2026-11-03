#include "IOSPolicySignatureVerifier.h"
#include "DesktopUpdatePublicKey.h"

#include <Security/Security.h>
#include <cstdint>

namespace ChurchDAWIOSPolicy
{
namespace
{
constexpr const char* policyKeyId202601 = "churchdaw-update-2026-01";
constexpr const char* policyKeyId202608 = "churchdaw-update-2026-08";
constexpr const char* policyKeyId202611 = "churchdaw-update-2026-11";
constexpr int maximumPayloadBytes = 24 * 1024;
constexpr int signatureBytes = 384; // RSA-3072

bool readDerLength (const uint8_t* data, size_t size,
                    size_t& offset, size_t& length) noexcept
{
    if (offset >= size)
        return false;

    const auto first = data[offset++];
    if ((first & 0x80u) == 0)
    {
        length = first;
        return length <= size - offset;
    }

    const auto bytes = static_cast<size_t> (first & 0x7fu);
    if (bytes == 0 || bytes > sizeof (size_t) || bytes > size - offset)
        return false;

    length = 0;
    for (size_t index = 0; index < bytes; ++index)
        length = (length << 8u) | data[offset++];

    return length <= size - offset;
}

bool readDerElement (const uint8_t* data, size_t size, size_t& offset,
                     uint8_t tag, size_t& contentOffset,
                     size_t& contentLength) noexcept
{
    if (offset >= size || data[offset++] != tag
         || ! readDerLength (data, size, offset, contentLength))
        return false;

    contentOffset = offset;
    offset += contentLength;
    return offset <= size;
}

bool extractPkcs1PublicKey (const juce::MemoryBlock& spki,
                            juce::MemoryBlock& pkcs1) noexcept
{
    const auto* data = static_cast<const uint8_t*> (spki.getData());
    const auto size = spki.getSize();
    size_t rootOffset = 0, rootContent = 0, rootLength = 0;
    if (! readDerElement (data, size, rootOffset, 0x30,
                          rootContent, rootLength)
         || rootOffset != size)
        return false;

    const auto rootEnd = rootContent + rootLength;
    size_t childOffset = rootContent;
    size_t algorithmContent = 0, algorithmLength = 0;
    if (! readDerElement (data, rootEnd, childOffset, 0x30,
                          algorithmContent, algorithmLength))
        return false;

    size_t bitStringContent = 0, bitStringLength = 0;
    if (! readDerElement (data, rootEnd, childOffset, 0x03,
                          bitStringContent, bitStringLength)
         || childOffset != rootEnd || bitStringLength < 2
         || data[bitStringContent] != 0)
        return false;

    const auto keyOffset = bitStringContent + 1;
    const auto keySize = bitStringLength - 1;
    size_t keyEnd = keyOffset, keyContent = 0, keyContentLength = 0;
    if (! readDerElement (data, keyOffset + keySize, keyEnd, 0x30,
                          keyContent, keyContentLength)
         || keyEnd != keyOffset + keySize)
        return false;

    pkcs1.replaceWith (data + keyOffset, keySize);
    return true;
}

void releaseCF (CFTypeRef object) noexcept
{
    if (object != nullptr)
        CFRelease (object);
}

bool verifyRs256 (const juce::MemoryBlock& message,
                  const juce::MemoryBlock& signature,
                  const char* publicKeyBase64)
{
    juce::MemoryOutputStream decodedKey;
    if (! juce::Base64::convertFromBase64 (
            decodedKey,
            publicKeyBase64))
        return false;

    const juce::MemoryBlock publicKeySpki (
        decodedKey.getData(), decodedKey.getDataSize());
    juce::MemoryBlock publicKeyPkcs1;
    if (! extractPkcs1PublicKey (publicKeySpki, publicKeyPkcs1))
        return false;

    auto* keyData = CFDataCreate (
        nullptr,
        static_cast<const UInt8*> (publicKeyPkcs1.getData()),
        static_cast<CFIndex> (publicKeyPkcs1.getSize()));
    if (keyData == nullptr)
        return false;

    const int keyBits = 3072;
    auto* bits = CFNumberCreate (nullptr, kCFNumberIntType, &keyBits);
    const void* attributeKeys[] {
        kSecAttrKeyType, kSecAttrKeyClass, kSecAttrKeySizeInBits
    };
    const void* attributeValues[] {
        kSecAttrKeyTypeRSA, kSecAttrKeyClassPublic, bits
    };
    auto* attributes = bits != nullptr
        ? CFDictionaryCreate (
            nullptr, attributeKeys, attributeValues, 3,
            &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks)
        : nullptr;

    CFErrorRef creationError = nullptr;
    auto* key = attributes != nullptr
        ? SecKeyCreateWithData (keyData, attributes, &creationError)
        : nullptr;

    releaseCF (creationError);
    releaseCF (attributes);
    releaseCF (bits);
    releaseCF (keyData);

    if (key == nullptr)
        return false;

    auto* messageData = CFDataCreate (
        nullptr,
        static_cast<const UInt8*> (message.getData()),
        static_cast<CFIndex> (message.getSize()));
    auto* signatureData = CFDataCreate (
        nullptr,
        static_cast<const UInt8*> (signature.getData()),
        static_cast<CFIndex> (signature.getSize()));

    CFErrorRef verificationError = nullptr;
    const bool valid = messageData != nullptr && signatureData != nullptr
        && SecKeyVerifySignature (
            key,
            kSecKeyAlgorithmRSASignatureMessagePKCS1v15SHA256,
            messageData,
            signatureData,
            &verificationError);

    releaseCF (verificationError);
    releaseCF (signatureData);
    releaseCF (messageData);
    releaseCF (key);
    return valid;
}
}

bool verifySignedPolicyEnvelope (const juce::String& payloadBase64,
                                 const juce::String& signatureBase64,
                                 const juce::String& keyId)
{
    const char* publicKeyBase64 = nullptr;

    if (keyId == policyKeyId202601)
        publicKeyBase64 =
            ChurchDAWDesktopUpdate::updatePolicyPublicKeyDerBase64;
    else if (keyId == policyKeyId202608)
        publicKeyBase64 =
            ChurchDAWDesktopUpdate::updatePolicyPublicKeyDerBase64_2026_08;
    else if (keyId == policyKeyId202611)
        publicKeyBase64 =
            ChurchDAWDesktopUpdate::updatePolicyPublicKeyDerBase64_2026_11;

    if (publicKeyBase64 == nullptr
         || payloadBase64.isEmpty()
         || payloadBase64.length() > maximumPayloadBytes * 2
         || signatureBase64.length() != 512)
        return false;

    juce::MemoryOutputStream payload;
    juce::MemoryOutputStream signature;
    if (! juce::Base64::convertFromBase64 (payload, payloadBase64)
         || payload.getDataSize() == 0
         || payload.getDataSize() > static_cast<size_t> (maximumPayloadBytes)
         || ! juce::Base64::convertFromBase64 (
                signature, signatureBase64)
         || signature.getDataSize() != static_cast<size_t> (signatureBytes))
        return false;

    const juce::MemoryBlock payloadBytes (
        payload.getData(), payload.getDataSize());
    const juce::MemoryBlock signatureBytesBlock (
        signature.getData(), signature.getDataSize());

    if (! verifyRs256 (payloadBytes, signatureBytesBlock,
                       publicKeyBase64))
        return false;

    // O mesmo vetor recebido e validado fornece uma regressao negativa sem
    // embutir outra policy no app: altere um bit e exija que o Security.framework
    // o recuse. Isso exercita a importacao da chave e os dois resultados no
    // simulador/aparelho sempre que uma policy real e aceita.
    auto tamperedPayload = payloadBytes;
    static_cast<uint8_t*> (tamperedPayload.getData())[0] ^= 0x01u;
    const bool tamperedRejected =
        ! verifyRs256 (tamperedPayload, signatureBytesBlock,
                       publicKeyBase64);

    juce::Logger::writeToLog (
        "IOS_POLICY_RS256 valid=1 tampered_rejected="
        + juce::String (tamperedRejected ? 1 : 0));
    return tamperedRejected;
}
}
