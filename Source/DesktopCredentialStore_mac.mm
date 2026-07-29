#include "DesktopCredentialStore.h"

#if JUCE_MAC

#import <Foundation/Foundation.h>
#import <Security/Security.h>

namespace ChurchDAWDesktopLicensing
{
namespace
{
NSString* serviceName()
{
    return @"com.churchdaw.app.desktop-licensing";
}

NSString* nsString (const juce::String& text)
{
    return [NSString stringWithUTF8String: text.toRawUTF8()];
}

juce::String statusError (OSStatus status)
{
    CFStringRef message = SecCopyErrorMessageString (status, nullptr);
    if (message == nullptr)
        return "Keychain " + juce::String ((int) status);

    const auto result = juce::String::fromCFString (message);
    CFRelease (message);
    return result;
}

NSMutableDictionary* baseQuery (const juce::String& account)
{
    return [@{
        (__bridge id) kSecClass: (__bridge id) kSecClassGenericPassword,
        (__bridge id) kSecAttrService: serviceName(),
        (__bridge id) kSecAttrAccount: nsString (account),
        (__bridge id) kSecAttrSynchronizable: @NO,
        (__bridge id) kSecUseDataProtectionKeychain: @YES
    } mutableCopy];
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

    @autoreleasepool
    {
        auto* query = baseQuery (account);
        SecItemDelete ((__bridge CFDictionaryRef) query);

        NSData* data = [NSData dataWithBytes: secret.toRawUTF8()
                                     length: secret.getNumBytesAsUTF8()];
        query[(__bridge id) kSecValueData] = data;
        query[(__bridge id) kSecAttrAccessible] =
            (__bridge id) kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly;

        const auto status = SecItemAdd (
            (__bridge CFDictionaryRef) query, nullptr);

        if (status != errSecSuccess)
        {
            error = "Nao foi possivel proteger a sessao: "
                  + statusError (status);
            return false;
        }
    }

    return true;
}

juce::String CredentialStore::loadSecret (const juce::String& account,
                                          juce::String& error)
{
    error.clear();

    @autoreleasepool
    {
        auto* query = baseQuery (account);
        query[(__bridge id) kSecReturnData] = @YES;
        query[(__bridge id) kSecMatchLimit] =
            (__bridge id) kSecMatchLimitOne;

        CFTypeRef result = nullptr;
        const auto status = SecItemCopyMatching (
            (__bridge CFDictionaryRef) query, &result);

        if (status == errSecItemNotFound)
            return {};

        if (status != errSecSuccess || result == nullptr)
        {
            error = "Nao foi possivel abrir a sessao protegida: "
                  + statusError (status);
            return {};
        }

        NSData* data = (__bridge_transfer NSData*) result;
        return juce::String::fromUTF8 (
            static_cast<const char*> (data.bytes),
            static_cast<int> (data.length));
    }
}

bool CredentialStore::removeSecret (const juce::String& account,
                                    juce::String& error)
{
    error.clear();

    @autoreleasepool
    {
        auto* query = baseQuery (account);
        const auto status = SecItemDelete (
            (__bridge CFDictionaryRef) query);

        if (status != errSecSuccess && status != errSecItemNotFound)
        {
            error = "Nao foi possivel remover a sessao protegida: "
                  + statusError (status);
            return false;
        }
    }

    return true;
}
}

#endif
