#include "DesktopCredentialStore.h"

#if JUCE_WINDOWS

#include <juce_cryptography/juce_cryptography.h>

#ifndef NOMINMAX
 #define NOMINMAX
#endif

#include <windows.h>
#include <dpapi.h>

namespace ChurchDAWDesktopLicensing
{
namespace
{
juce::File credentialFolder()
{
    return juce::File::getSpecialLocation (
               juce::File::userApplicationDataDirectory)
        .getChildFile ("ChurchDAW")
        .getChildFile ("Licensing");
}

juce::File credentialFile (const juce::String& account)
{
    const auto safeName = juce::SHA256 (account.toUTF8()).toHexString();
    return credentialFolder().getChildFile (safeName + ".credential");
}

DATA_BLOB makeEntropy()
{
    static constexpr char entropyText[] =
        "ChurchDAW.DesktopLicensing.CredentialStore.v1";

    DATA_BLOB entropy {};
    entropy.pbData = reinterpret_cast<BYTE*> (
        const_cast<char*> (entropyText));
    entropy.cbData = static_cast<DWORD> (sizeof (entropyText) - 1);
    return entropy;
}

juce::String windowsError (const juce::String& operation)
{
    return operation + " (Windows " + juce::String ((int) GetLastError()) + ")";
}
}

bool CredentialStore::saveSecret (const juce::String& account,
                                  const juce::String& secret,
                                  juce::String& error)
{
    error.clear();

    if (account.isEmpty() || secret.isEmpty())
    {
        error = "Credencial vazia";
        return false;
    }

    juce::MemoryBlock plain (secret.toRawUTF8(),
                             static_cast<size_t> (secret.getNumBytesAsUTF8()));
    DATA_BLOB input {
        static_cast<DWORD> (plain.getSize()),
        static_cast<BYTE*> (plain.getData())
    };
    auto entropy = makeEntropy();
    DATA_BLOB encrypted {};

    const auto ok = CryptProtectData (&input,
                                      L"ChurchDAW desktop session",
                                      &entropy,
                                      nullptr,
                                      nullptr,
                                      CRYPTPROTECT_UI_FORBIDDEN,
                                      &encrypted) != FALSE;

    juce::zeromem (plain.getData(), plain.getSize());

    if (! ok)
    {
        error = windowsError ("Nao foi possivel proteger a sessao");
        return false;
    }

    const juce::MemoryBlock protectedData (encrypted.pbData,
                                            encrypted.cbData);
    SecureZeroMemory (encrypted.pbData, encrypted.cbData);
    LocalFree (encrypted.pbData);

    auto folder = credentialFolder();
    if (folder.createDirectory().failed())
    {
        error = "Nao foi possivel criar o armazenamento seguro";
        return false;
    }

    if (! credentialFile (account).replaceWithData (
            protectedData.getData(), protectedData.getSize()))
    {
        error = "Nao foi possivel gravar a sessao protegida";
        return false;
    }

    return true;
}

juce::String CredentialStore::loadSecret (const juce::String& account,
                                          juce::String& error)
{
    error.clear();
    const auto file = credentialFile (account);

    if (! file.existsAsFile())
        return {};

    juce::MemoryBlock encrypted;
    if (! file.loadFileAsData (encrypted) || encrypted.isEmpty())
    {
        error = "Sessao protegida corrompida";
        return {};
    }

    DATA_BLOB input {
        static_cast<DWORD> (encrypted.getSize()),
        static_cast<BYTE*> (encrypted.getData())
    };
    auto entropy = makeEntropy();
    DATA_BLOB plain {};

    if (CryptUnprotectData (&input,
                            nullptr,
                            &entropy,
                            nullptr,
                            nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN,
                            &plain) == FALSE)
    {
        error = windowsError ("Nao foi possivel abrir a sessao protegida");
        return {};
    }

    const auto result = juce::String::fromUTF8 (
        reinterpret_cast<const char*> (plain.pbData),
        static_cast<int> (plain.cbData));
    SecureZeroMemory (plain.pbData, plain.cbData);
    LocalFree (plain.pbData);
    return result;
}

bool CredentialStore::removeSecret (const juce::String& account,
                                    juce::String& error)
{
    error.clear();
    const auto file = credentialFile (account);

    if (! file.exists())
        return true;

    if (! file.deleteFile())
    {
        error = "Nao foi possivel remover a sessao protegida";
        return false;
    }

    return true;
}
}

#endif
