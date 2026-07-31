#include "DesktopCredentialStore.h"

#if JUCE_MAC

#import <Foundation/Foundation.h>
#import <Security/Security.h>
#include <CommonCrypto/CommonCrypto.h>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

// POR QUE ARQUIVO, E NAO O COFRE (KEYCHAIN)?
//
// O ChurchDAW para macOS e distribuido com assinatura ad-hoc, e ad-hoc
// significa identidade NOVA a cada build. O cofre amarra cada item ao
// app que o criou: depois de qualquer atualizacao, o item de sessao
// vira "de outro app" e a leitura dispara o dialogo "digite a senha
// das chaves login" — que o cliente nao tem como responder. Em
// 30/07/2026 dois clientes ficaram presos nesse ciclo: a cada reinicio
// o app pedia senha do cofre, o cliente negava e a licenca "sumia".
//
// A sessao agora vive num arquivo proprio, cifrado por maquina+usuario
// (AES-256-CBC + HMAC-SHA256 via CommonCrypto, chave derivada do
// identificador unico do Mac + uid, mesmo espirito do DPAPI usado no
// Windows). Sobrevive a reassinatura, atualizacao e reinicio, e nunca
// mostra dialogo nenhum. O cofre so e tocado para APAGAR itens de
// versoes antigas — apagar nao exige autorizacao e nao abre dialogo.

namespace ChurchDAWDesktopLicensing
{
namespace
{
constexpr char fileMagic[8] = { 'C','D','W','S','E','S','1','\n' };

NSString* keychainServiceName()
{
    return @"com.churchdaw.app.desktop-licensing";
}

// Limpeza silenciosa dos itens da era-cofre. Roda em todo save/load/
// remove: garante que nenhum item velho sobra para assombrar uma
// leitura futura. SecItemDelete nunca abre dialogo.
void deleteLegacyKeychainItem (const juce::String& account)
{
    @autoreleasepool
    {
        for (const bool dataProtection : { true, false })
        {
            auto* query = [@{
                (__bridge id) kSecClass:
                    (__bridge id) kSecClassGenericPassword,
                (__bridge id) kSecAttrService: keychainServiceName(),
                (__bridge id) kSecAttrAccount:
                    [NSString stringWithUTF8String: account.toRawUTF8()],
                (__bridge id) kSecAttrSynchronizable: @NO
            } mutableCopy];

            if (dataProtection)
                query[(__bridge id) kSecUseDataProtectionKeychain] = @YES;

            SecItemDelete ((__bridge CFDictionaryRef) query);
        }
    }
}

juce::File sessionFolder()
{
    return juce::File::getSpecialLocation (
               juce::File::userApplicationDataDirectory)
        .getChildFile ("Application Support")
        .getChildFile ("ChurchDAW")
        .getChildFile ("Sessao");
}

// SHA-256 via CommonCrypto: nao depende do modulo juce_cryptography.
juce::MemoryBlock sha256Of (const juce::String& text)
{
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256 (text.toRawUTF8(),
               (CC_LONG) text.getNumBytesAsUTF8(), digest);
    return { digest, sizeof (digest) };
}

juce::File sessionFile (const juce::String& account)
{
    // O nome da conta e interno ("desktop-device-v1"...), mas passa por
    // um hash mesmo assim: nome de arquivo nunca vaza conteudo e nunca
    // esbarra em caracteres invalidos.
    return sessionFolder().getChildFile (
        juce::String::toHexString (
            (const unsigned char*) sha256Of (account).getData(), 16, 0)
        + ".sessao");
}

// Chave por maquina+usuario. getUniqueDeviceID e estavel entre
// reinicios e reinstalacoes; uid separa contas do mesmo Mac.
juce::MemoryBlock deriveKey (const char* purpose)
{
    return sha256Of (juce::SystemStats::getUniqueDeviceID()
                     + ":" + juce::String ((int) getuid())
                     + ":" + purpose
                     + ":churchdaw-sessao-v1");
}

bool computeHmac (const juce::MemoryBlock& key,
                  const void* data, size_t size,
                  unsigned char out[CC_SHA256_DIGEST_LENGTH])
{
    if (key.getSize() == 0)
        return false;

    CCHmac (kCCHmacAlgSHA256, key.getData(), key.getSize(),
            data, size, out);
    return true;
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

    deleteLegacyKeychainItem (account);

    const auto aesKey  = deriveKey ("aes");
    const auto macKey  = deriveKey ("hmac");

    unsigned char iv[kCCBlockSizeAES128];
    if (SecRandomCopyBytes (kSecRandomDefault, sizeof (iv), iv)
            != errSecSuccess)
    {
        error = "Nao foi possivel gerar o vetor de cifra";
        return false;
    }

    const auto plainSize = secret.getNumBytesAsUTF8();
    juce::MemoryBlock cipher (plainSize + kCCBlockSizeAES128, true);
    size_t cipherSize = 0;

    const auto cryptStatus = CCCrypt (
        kCCEncrypt, kCCAlgorithmAES, kCCOptionPKCS7Padding,
        aesKey.getData(), kCCKeySizeAES256, iv,
        secret.toRawUTF8(), plainSize,
        cipher.getData(), cipher.getSize(), &cipherSize);

    if (cryptStatus != kCCSuccess)
    {
        error = "Falha ao cifrar a sessao ("
              + juce::String ((int) cryptStatus) + ")";
        return false;
    }

    juce::MemoryBlock payload;
    payload.append (fileMagic, sizeof (fileMagic));
    payload.append (iv, sizeof (iv));

    unsigned char hmac[CC_SHA256_DIGEST_LENGTH];
    juce::MemoryBlock authed;
    authed.append (iv, sizeof (iv));
    authed.append (cipher.getData(), cipherSize);
    if (! computeHmac (macKey, authed.getData(), authed.getSize(), hmac))
    {
        error = "Falha ao autenticar a sessao";
        return false;
    }

    payload.append (hmac, sizeof (hmac));
    payload.append (cipher.getData(), cipherSize);

    auto folder = sessionFolder();
    if (! folder.createDirectory() && ! folder.isDirectory())
    {
        error = "Nao foi possivel criar a pasta de sessao";
        return false;
    }

    const auto file = sessionFile (account);
    if (! file.replaceWithData (payload.getData(), payload.getSize()))
    {
        error = "Nao foi possivel gravar a sessao";
        return false;
    }

    // arquivo legivel so pelo dono, como uma chave ssh
    chmod (file.getFullPathName().toRawUTF8(), S_IRUSR | S_IWUSR);
    return true;
}

juce::String CredentialStore::loadSecret (const juce::String& account,
                                          juce::String& error)
{
    error.clear();

    const auto file = sessionFile (account);
    if (! file.existsAsFile())
    {
        // Sem arquivo = sem sessao. Inclui clientes vindos da era-cofre:
        // ler o cofre dispararia o dialogo de senha, entao NUNCA lemos —
        // apagamos o item velho e deixamos o app pedir o login uma vez.
        deleteLegacyKeychainItem (account);
        return {};
    }

    juce::MemoryBlock payload;
    if (! file.loadFileAsData (payload)
         || payload.getSize() < sizeof (fileMagic)
                               + kCCBlockSizeAES128
                               + CC_SHA256_DIGEST_LENGTH
         || std::memcmp (payload.getData(), fileMagic,
                         sizeof (fileMagic)) != 0)
    {
        // Corrompido: descartar em silencio e pedir login de novo.
        file.deleteFile();
        return {};
    }

    const auto* bytes =
        static_cast<const unsigned char*> (payload.getData());
    const auto* iv = bytes + sizeof (fileMagic);
    const auto* storedHmac = iv + kCCBlockSizeAES128;
    const auto* cipher = storedHmac + CC_SHA256_DIGEST_LENGTH;
    const auto cipherSize = payload.getSize() - sizeof (fileMagic)
                          - kCCBlockSizeAES128 - CC_SHA256_DIGEST_LENGTH;

    const auto macKey = deriveKey ("hmac");
    unsigned char expected[CC_SHA256_DIGEST_LENGTH];
    juce::MemoryBlock authed;
    authed.append (iv, kCCBlockSizeAES128);
    authed.append (cipher, cipherSize);

    if (! computeHmac (macKey, authed.getData(), authed.getSize(),
                       expected)
         || std::memcmp (expected, storedHmac,
                         CC_SHA256_DIGEST_LENGTH) != 0)
    {
        file.deleteFile();
        return {};
    }

    const auto aesKey = deriveKey ("aes");
    juce::MemoryBlock plain (cipherSize + kCCBlockSizeAES128, true);
    size_t plainSize = 0;

    if (CCCrypt (kCCDecrypt, kCCAlgorithmAES, kCCOptionPKCS7Padding,
                 aesKey.getData(), kCCKeySizeAES256, iv,
                 cipher, cipherSize,
                 plain.getData(), plain.getSize(), &plainSize)
            != kCCSuccess)
    {
        file.deleteFile();
        return {};
    }

    return juce::String::fromUTF8 (
        static_cast<const char*> (plain.getData()),
        static_cast<int> (plainSize));
}

bool CredentialStore::removeSecret (const juce::String& account,
                                    juce::String& error)
{
    error.clear();
    deleteLegacyKeychainItem (account);

    const auto file = sessionFile (account);
    if (file.existsAsFile() && ! file.deleteFile())
    {
        error = "Nao foi possivel remover a sessao";
        return false;
    }

    return true;
}
}

#endif
