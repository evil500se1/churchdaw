#include "DesktopLeaseSignature.h"
#include "DesktopLicensePublicKey.h"

#if JUCE_WINDOWS

#include <juce_cryptography/juce_cryptography.h>

#ifndef NOMINMAX
 #define NOMINMAX
#endif

#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>

namespace ChurchDAWDesktopLicensing
{
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
    juce::MemoryBlock publicKeyDer (
        decodedKey.getData(), decodedKey.getDataSize());

    CERT_PUBLIC_KEY_INFO* publicKeyInfo = nullptr;
    DWORD publicKeyInfoBytes = 0;
    if (CryptDecodeObjectEx (
            X509_ASN_ENCODING,
            X509_PUBLIC_KEY_INFO,
            static_cast<const BYTE*> (publicKeyDer.getData()),
            static_cast<DWORD> (publicKeyDer.getSize()),
            CRYPT_DECODE_ALLOC_FLAG,
            nullptr,
            &publicKeyInfo,
            &publicKeyInfoBytes) == FALSE)
    {
        error = "Nao foi possivel decodificar a chave publica";
        return false;
    }

    BCRYPT_KEY_HANDLE key = nullptr;
    const auto imported = CryptImportPublicKeyInfoEx2 (
        X509_ASN_ENCODING, publicKeyInfo, 0, nullptr, &key);
    LocalFree (publicKeyInfo);
    if (imported == FALSE || key == nullptr)
    {
        error = "Nao foi possivel importar a chave publica";
        return false;
    }

    const auto digest = juce::SHA256 (message).getRawData();
    BCRYPT_PKCS1_PADDING_INFO padding { BCRYPT_SHA256_ALGORITHM };
    const auto status = BCryptVerifySignature (
        key,
        &padding,
        reinterpret_cast<PUCHAR> (const_cast<void*> (digest.getData())),
        static_cast<ULONG> (digest.getSize()),
        reinterpret_cast<PUCHAR> (const_cast<void*> (signature.getData())),
        static_cast<ULONG> (signature.getSize()),
        BCRYPT_PAD_PKCS1);
    BCryptDestroyKey (key);

    if (status < 0)
    {
        error = "Assinatura de licenca invalida";
        return false;
    }

    return true;
}
}

#endif
