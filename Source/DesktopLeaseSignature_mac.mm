#include "DesktopLeaseSignature.h"
#include "DesktopLicensePublicKey.h"

#if JUCE_MAC

#import <Foundation/Foundation.h>
#import <Security/Security.h>

#include <cstdint>

namespace ChurchDAWDesktopLicensing
{
namespace
{
bool readDerLength (const uint8_t* data,
                    size_t size,
                    size_t& offset,
                    size_t& length) noexcept
{
    if (offset >= size)
        return false;

    const auto first = data[offset++];
    if ((first & 0x80u) == 0)
    {
        length = first;
        return length <= size - offset;
    }

    const auto lengthBytes = static_cast<size_t> (first & 0x7fu);
    if (lengthBytes == 0 || lengthBytes > sizeof (size_t)
        || lengthBytes > size - offset)
        return false;

    length = 0;
    for (size_t index = 0; index < lengthBytes; ++index)
        length = (length << 8u) | data[offset++];

    return length <= size - offset;
}

bool readDerElement (const uint8_t* data,
                     size_t size,
                     size_t& offset,
                     uint8_t expectedTag,
                     size_t& contentOffset,
                     size_t& contentLength) noexcept
{
    if (offset >= size || data[offset++] != expectedTag
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
    size_t rootOffset = 0;
    size_t rootContent = 0;
    size_t rootLength = 0;
    if (! readDerElement (
            data, size, rootOffset, 0x30, rootContent, rootLength)
        || rootOffset != size)
        return false;

    const auto rootEnd = rootContent + rootLength;
    size_t childOffset = rootContent;
    size_t algorithmContent = 0;
    size_t algorithmLength = 0;
    if (! readDerElement (
            data, rootEnd, childOffset, 0x30, algorithmContent, algorithmLength))
        return false;

    size_t bitStringContent = 0;
    size_t bitStringLength = 0;
    if (! readDerElement (
            data, rootEnd, childOffset, 0x03, bitStringContent, bitStringLength)
        || childOffset != rootEnd
        || bitStringLength < 2
        || data[bitStringContent] != 0)
        return false;

    const auto keyOffset = bitStringContent + 1;
    const auto keySize = bitStringLength - 1;
    size_t keyEnd = keyOffset;
    size_t keyContent = 0;
    size_t keyContentLength = 0;
    if (! readDerElement (
            data, keyOffset + keySize, keyEnd, 0x30, keyContent, keyContentLength)
        || keyEnd != keyOffset + keySize)
        return false;

    pkcs1.replaceWith (data + keyOffset, keySize);
    return true;
}
}

bool verifyLeaseRs256Signature (const juce::MemoryBlock& message,
                                const juce::MemoryBlock& signature,
                                juce::String& error)
{
    error.clear();
    juce::MemoryOutputStream decodedKey;
    if (! juce::Base64::convertFromBase64 (
            decodedKey, licensePublicKeyDerBase64))
    {
        error = "Chave publica embutida invalida";
        return false;
    }
    juce::MemoryBlock publicKeySpki (
        decodedKey.getData(), decodedKey.getDataSize());
    juce::MemoryBlock publicKeyDer;
    if (! extractPkcs1PublicKey (publicKeySpki, publicKeyDer))
    {
        error = "Formato da chave publica embutida invalido";
        return false;
    }

    @autoreleasepool
    {
        NSData* keyData = [NSData dataWithBytes: publicKeyDer.getData()
                                         length: publicKeyDer.getSize()];
        NSDictionary* attributes = @{
            (__bridge id) kSecAttrKeyType: (__bridge id) kSecAttrKeyTypeRSA,
            (__bridge id) kSecAttrKeyClass: (__bridge id) kSecAttrKeyClassPublic,
            (__bridge id) kSecAttrKeySizeInBits: @3072
        };
        CFErrorRef creationError = nullptr;
        SecKeyRef key = SecKeyCreateWithData (
            (__bridge CFDataRef) keyData,
            (__bridge CFDictionaryRef) attributes,
            &creationError);
        if (key == nullptr)
        {
            if (creationError != nullptr)
                CFRelease (creationError);
            error = "Nao foi possivel importar a chave publica";
            return false;
        }

        NSData* messageData = [NSData dataWithBytes: message.getData()
                                             length: message.getSize()];
        NSData* signatureData = [NSData dataWithBytes: signature.getData()
                                               length: signature.getSize()];
        CFErrorRef verificationError = nullptr;
        const bool valid = SecKeyVerifySignature (
            key,
            kSecKeyAlgorithmRSASignatureMessagePKCS1v15SHA256,
            (__bridge CFDataRef) messageData,
            (__bridge CFDataRef) signatureData,
            &verificationError);
        CFRelease (key);
        if (verificationError != nullptr)
            CFRelease (verificationError);

        if (! valid)
        {
            error = "Assinatura de licenca invalida";
            return false;
        }
    }

    return true;
}
}

#endif
