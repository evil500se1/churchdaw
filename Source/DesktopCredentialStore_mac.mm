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

NSMutableDictionary* baseQuery (const juce::String& account,
                                bool useDataProtectionKeychain)
{
    auto* query = [@{
        (__bridge id) kSecClass: (__bridge id) kSecClassGenericPassword,
        (__bridge id) kSecAttrService: serviceName(),
        (__bridge id) kSecAttrAccount: nsString (account),
        (__bridge id) kSecAttrSynchronizable: @NO
    } mutableCopy];

    if (useDataProtectionKeychain)
        query[(__bridge id) kSecUseDataProtectionKeychain] = @YES;

    return query;
}

bool canFallBackToLoginKeychain (OSStatus status)
{
    // Developer ID builds do not necessarily carry an application-identifier
    // entitlement. In that case the Data Protection Keychain returns
    // errSecMissingEntitlement (-34018), even though the normal per-user
    // Keychain is available and encrypted by macOS.
    return status == errSecMissingEntitlement
        || status == errSecNotAvailable
        || status == errSecUnimplemented
        || status == errSecParam;
}

OSStatus addSecret (const juce::String& account,
                    const juce::String& secret,
                    bool useDataProtectionKeychain)
{
    auto* query = baseQuery (account, useDataProtectionKeychain);
    SecItemDelete ((__bridge CFDictionaryRef) query);

    NSData* data = [NSData dataWithBytes: secret.toRawUTF8()
                                 length: secret.getNumBytesAsUTF8()];
    query[(__bridge id) kSecValueData] = data;

    // kSecAttrAccessible belongs to the Data Protection Keychain API. The
    // classic macOS login Keychain is already local/encrypted, and @NO in
    // kSecAttrSynchronizable keeps the item out of iCloud.
    if (useDataProtectionKeychain)
        query[(__bridge id) kSecAttrAccessible] =
            (__bridge id) kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly;

    return SecItemAdd ((__bridge CFDictionaryRef) query, nullptr);
}

OSStatus copySecret (const juce::String& account,
                     bool useDataProtectionKeychain,
                     CFTypeRef* result)
{
    auto* query = baseQuery (account, useDataProtectionKeychain);
    query[(__bridge id) kSecReturnData] = @YES;
    query[(__bridge id) kSecMatchLimit] =
        (__bridge id) kSecMatchLimitOne;
    return SecItemCopyMatching ((__bridge CFDictionaryRef) query, result);
}

OSStatus deleteSecret (const juce::String& account,
                       bool useDataProtectionKeychain)
{
    auto* query = baseQuery (account, useDataProtectionKeychain);
    return SecItemDelete ((__bridge CFDictionaryRef) query);
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
        auto status = addSecret (account, secret, true);

        if (status != errSecSuccess && canFallBackToLoginKeychain (status))
            status = addSecret (account, secret, false);

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
        CFTypeRef result = nullptr;
        auto status = copySecret (account, true, &result);

        if (status == errSecItemNotFound || canFallBackToLoginKeychain (status))
        {
            if (result != nullptr)
            {
                CFRelease (result);
                result = nullptr;
            }

            status = copySecret (account, false, &result);
        }

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
        const auto protectedStatus = deleteSecret (account, true);
        const auto loginStatus = deleteSecret (account, false);
        const auto acceptable = [] (OSStatus status)
        {
            return status == errSecSuccess
                || status == errSecItemNotFound
                || canFallBackToLoginKeychain (status);
        };

        if (! acceptable (protectedStatus) || ! acceptable (loginStatus))
        {
            error = "Nao foi possivel remover a sessao protegida: "
                  + statusError (! acceptable (protectedStatus)
                                    ? protectedStatus : loginStatus);
            return false;
        }
    }

    return true;
}
}

#endif
