// Testes do portao de atualizacao obrigatoria do desktop.
//
// Nao sao testes de fumaca: o grupo A usa a politica Android REAL, assinada com
// a chave privada de verdade, como vetor conhecido. Se o algoritmo, a
// codificacao da mensagem ou a chave embutida estiverem errados, A1 reprova --
// e um "compilou" nunca teria percebido.
//
// Uso: UpdatePolicyTests <raiz-do-repo> <policy-serial-1.json> <policy-serial-2.json>

#include "../../Source/DesktopUpdater.h"

#include <juce_core/juce_core.h>
#include <juce_cryptography/juce_cryptography.h>

#include <iostream>

namespace
{
    int failures = 0;
    int checks = 0;

    void check (bool condition, const juce::String& name, const juce::String& detail = {})
    {
        ++checks;

        if (condition)
        {
            std::cout << "  ok   " << name << std::endl;
        }
        else
        {
            ++failures;
            std::cout << "  FALHOU " << name;
            if (detail.isNotEmpty())
                std::cout << "  <- " << detail;
            std::cout << std::endl;
        }
    }

    juce::String makeEnvelope (int schemaVersion,
                               const juce::String& algorithm,
                               const juce::String& keyId,
                               const juce::MemoryBlock& payload,
                               const juce::String& signatureBase64)
    {
        auto* object = new juce::DynamicObject();
        object->setProperty ("schema_version", schemaVersion);
        object->setProperty ("algorithm", algorithm);
        object->setProperty ("key_id", keyId);
        object->setProperty ("payload",
                             juce::Base64::toBase64 (payload.getData(), payload.getSize()));
        object->setProperty ("signature", signatureBase64);
        return juce::JSON::toString (juce::var (object), true);
    }

    bool readEnvelopeParts (const juce::File& file,
                            juce::MemoryBlock& payload,
                            juce::String& signatureBase64,
                            juce::String& keyId)
    {
        juce::var parsed;
        if (juce::JSON::parse (file.loadFileAsString(), parsed).failed())
            return false;

        auto* object = parsed.getDynamicObject();
        if (object == nullptr)
            return false;

        signatureBase64 = object->getProperty ("signature").toString();
        keyId = object->getProperty ("key_id").toString();

        juce::MemoryOutputStream decoded;
        if (! juce::Base64::convertFromBase64 (decoded, object->getProperty ("payload").toString()))
            return false;

        payload = juce::MemoryBlock (decoded.getData(), decoded.getDataSize());
        return payload.getSize() > 0 && signatureBase64.isNotEmpty();
    }

    juce::File stateFile()
    {
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("ChurchDAW")
                   .getChildFile ("update-state.json");
    }

    ChurchDAWDesktopUpdate::Policy makePolicy (int latest, int minimum, bool withInstaller)
    {
        ChurchDAWDesktopUpdate::Policy policy;
        policy.valid              = true;
        policy.schemaVersion      = 1;
        policy.policySerial       = 1;
        policy.latestVersionCode  = latest;
        policy.minimumVersionCode = minimum;
        policy.issuedAt           = 1700000000;
        policy.expiresAt          = 2000000000;
        policy.mode               = "normal";
        policy.latestVersionName  = "9.9.9";

        if (withInstaller)
        {
            policy.installerUrl    = "https://churchdaw.com/app/desktop/ChurchDAW-Setup.exe";
            policy.installerSha256 = juce::String::repeatedString ("ab", 32);
            policy.installerBytes  = 1234;
        }

        return policy;
    }
}

int main (int argc, char* argv[])
{
    if (argc < 4)
    {
        std::cerr << "uso: UpdatePolicyTests <raiz> <policy-serial-1> <policy-serial-2>"
                  << std::endl;
        return 2;
    }

    // Chaves, nao parenteses: com parenteses o C++ le isto como declaracao de
    // funcao (most vexing parse) e nada compila.
    const juce::File repoRoot       { juce::String (juce::CharPointer_UTF8 (argv[1])) };
    const juce::File desktopPolicy4 { juce::String (juce::CharPointer_UTF8 (argv[2])) };
    const juce::File desktopPolicy5 { juce::String (juce::CharPointer_UTF8 (argv[3])) };

    // O verificador guarda o maior serial ja aceito no perfil do usuario.
    // Comecar do zero e devolver o estado real no fim.
    const auto state = stateFile();
    const bool hadState = state.existsAsFile();
    const auto savedState = hadState ? state.loadFileAsString() : juce::String();
    state.deleteFile();

    using namespace ChurchDAWDesktopUpdate;

    //==========================================================================
    std::cout << "A. assinatura RS256 contra a politica Android REAL" << std::endl;

    const auto androidPolicy = repoRoot.getChildFile (
        "Packaging/AndroidHosting/app/android/update-policy-v2.json");

    juce::MemoryBlock payload;
    juce::String signatureBase64, keyId;

    if (! androidPolicy.existsAsFile()
         || ! readEnvelopeParts (androidPolicy, payload, signatureBase64, keyId))
    {
        std::cerr << "nao consegui ler " << androidPolicy.getFullPathName() << std::endl;
        return 2;
    }

    juce::MemoryOutputStream decodedSignature;
    juce::Base64::convertFromBase64 (decodedSignature, signatureBase64);
    const juce::MemoryBlock signature (decodedSignature.getData(), decodedSignature.getDataSize());

    check (keyId == "churchdaw-update-2026-01", "A0 key_id do vetor confere", keyId);
    check (signature.getSize() == 384, "A0 assinatura tem 384 bytes (RSA-3072)",
           juce::String ((int) signature.getSize()));

    juce::String error;
    check (verifyUpdateRs256Signature (payload, signature, error),
           "A1 assinatura legitima ACEITA", error);

    {
        auto tampered = payload;
        static_cast<char*> (tampered.getData())[10] ^= 0x01;   // um bit
        juce::String e;
        check (! verifyUpdateRs256Signature (tampered, signature, e),
               "A2 payload adulterado RECUSADO");
    }

    {
        auto tampered = signature;
        static_cast<char*> (tampered.getData())[100] ^= 0x01;
        juce::String e;
        check (! verifyUpdateRs256Signature (payload, tampered, e),
               "A3 assinatura adulterada RECUSADA");
    }

    //==========================================================================
    // Este e o caminho que o ChurchDAWServices usa para validar o CATALOGO de
    // bibliotecas no desktop. Se F1 reprovar, ninguem no Windows/Mac consegue
    // baixar timbre nenhum -- checkRemoteManifest() aborta antes de sequer
    // pedir o manifest.json.
    std::cout << "F. envelope Base64 (validacao do catalogo no desktop)" << std::endl;

    {
        juce::var parsed;
        juce::JSON::parse (androidPolicy.loadFileAsString(), parsed);
        auto* object = parsed.getDynamicObject();
        const auto payloadB64   = object->getProperty ("payload").toString();
        const auto signatureB64 = object->getProperty ("signature").toString();

        check (verifySignedPolicyEnvelope (payloadB64, signatureB64,
                                           "churchdaw-update-2026-01"),
               "F1 envelope Android real ACEITO no desktop");

        check (! verifySignedPolicyEnvelope (payloadB64, signatureB64,
                                             "churchdaw-2026-01"),
               "F2 key_id da chave de licenca RECUSADO");

        auto broken = payloadB64;
        broken = broken.replaceSection (12, 1, broken[12] == 'A' ? "B" : "A");
        check (! verifySignedPolicyEnvelope (broken, signatureB64,
                                             "churchdaw-update-2026-01"),
               "F3 payload alterado RECUSADO");

        check (! verifySignedPolicyEnvelope (payloadB64, signatureB64.dropLastCharacters (4),
                                             "churchdaw-update-2026-01"),
               "F4 assinatura de tamanho errado RECUSADA");
    }

    //==========================================================================
    std::cout << "B. leitura do envelope de desktop assinado" << std::endl;

    juce::MemoryBlock payload4;
    juce::String signature4, keyId4;

    if (! desktopPolicy4.existsAsFile()
         || ! readEnvelopeParts (desktopPolicy4, payload4, signature4, keyId4))
    {
        std::cerr << "nao consegui ler " << desktopPolicy4.getFullPathName() << std::endl;
        return 2;
    }

    // Os numeros esperados saem do PROPRIO payload, nao de constantes. Uma
    // politica publicada muda de serial e de versao a cada release: com valor
    // fixo aqui, o teste quebra sozinho na proxima publicacao e vira teste
    // morto -- alguem passa a ignorar a falha em vez de investigar. O que
    // interessa provar e que o parser le os campos certos do envelope assinado.
    const auto payload4Text = juce::String::fromUTF8 (
        static_cast<const char*> (payload4.getData()), (int) payload4.getSize());
    const auto payload4Json = juce::JSON::parse (payload4Text);

    const int esperadoSerial = (int) payload4Json.getProperty ("policy_serial", -1);
    const int esperadoLatest = (int) payload4Json.getProperty ("latest_version_code", -1);
    const int esperadoMinimo = (int) payload4Json.getProperty ("minimum_version_code", -1);

    {
        Policy policy;
        juce::String e;
        const bool ok = parseSignedPolicy (desktopPolicy4.loadFileAsString(), policy, e);

        check (ok, "B1 envelope legitimo ACEITO", e);
        check (esperadoSerial > 0 && esperadoMinimo > 0,
               "B1 payload declara serial e minimo",
               "serial=" + juce::String (esperadoSerial)
                   + " minimo=" + juce::String (esperadoMinimo));
        check (ok && policy.policySerial == esperadoSerial, "B1 policy_serial lido",
               juce::String (policy.policySerial) + " != " + juce::String (esperadoSerial));
        check (ok && policy.latestVersionCode == esperadoLatest, "B1 latest_version_code lido",
               juce::String (policy.latestVersionCode) + " != " + juce::String (esperadoLatest));
        check (ok && policy.minimumVersionCode == esperadoMinimo, "B1 minimum_version_code lido",
               juce::String (policy.minimumVersionCode) + " != " + juce::String (esperadoMinimo));
       #if JUCE_WINDOWS
        check (ok && policy.hasInstaller(), "B1 instalador do Windows resolvido",
               policy.installerUrl);
        check (ok && policy.installerSha256.length() == 64, "B1 SHA-256 do instalador presente");
       #endif
    }

    {
        // Trocar um digito do payload sem reassinar. O valor alvo vem do
        // proprio payload pelo mesmo motivo do bloco B1.
        const auto& text = payload4Text;

        juce::String alvo = "\"minimum_version_code\":" + juce::String (esperadoMinimo);
        auto broken = text.replace (alvo, "\"minimum_version_code\":99999");

        if (broken == text)   // payload gravado com espaco depois do dois-pontos
        {
            alvo = "\"minimum_version_code\": " + juce::String (esperadoMinimo);
            broken = text.replace (alvo, "\"minimum_version_code\": 99999");
        }

        check (broken != text, "B2 preparacao: campo encontrado para adulterar", alvo);

        const juce::MemoryBlock brokenBytes (broken.toRawUTF8(), broken.getNumBytesAsUTF8());
        Policy policy;
        juce::String e;
        check (! parseSignedPolicy (
                   makeEnvelope (1, "SHA256withRSA", "churchdaw-update-2026-01",
                                 brokenBytes, signature4),
                   policy, e),
               "B2 payload adulterado RECUSADO", e);
    }

    {
        Policy policy;
        juce::String e;
        check (! parseSignedPolicy (
                   makeEnvelope (1, "SHA256withRSA", "churchdaw-2026-01",  // chave de LICENCA
                                 payload4, signature4),
                   policy, e),
               "B3 key_id de outra chave RECUSADO", e);
    }

    {
        Policy policy;
        juce::String e;
        check (! parseSignedPolicy (
                   makeEnvelope (1, "RS256", "churchdaw-update-2026-01",
                                 payload4, signature4),
                   policy, e),
               "B4 algoritmo diferente RECUSADO", e);
    }

    //==========================================================================
    std::cout << "C. decisao (funcao pura) -- inclui a trava anti-tijolo" << std::endl;

    {
        juce::String reason;

        auto expired = makePolicy (10300, 10200, true);
        expired.expiresAt = 1700000000;
        check (decideFor (expired, 1800000000, 10100, reason) == Decision::unavailable,
               "C1 politica vencida NAO bloqueia", reason);

        check (decideFor (makePolicy (10300, 10200, true), 1750000000, 0, reason)
                   == Decision::unavailable,
               "C2 build sem version code NAO bloqueia", reason);

        check (decideFor (makePolicy (10300, 10200, true), 1750000000, 10300, reason)
                   == Decision::upToDate,
               "C3 versao atual -> upToDate", reason);

        check (decideFor (makePolicy (10300, 10200, true), 1750000000, 10100, reason)
                   == Decision::mandatory,
               "C4 abaixo do minimo COM instalador -> mandatory", reason);

        // O coracao da trava anti-tijolo.
        check (decideFor (makePolicy (10300, 10200, false), 1750000000, 10100, reason)
                   == Decision::optional,
               "C5 abaixo do minimo SEM instalador -> so avisa", reason);

        check (decideFor (makePolicy (10300, 10200, true), 1750000000, 10250, reason)
                   == Decision::optional,
               "C6 acima do minimo e abaixo do ultimo -> optional", reason);

        Policy invalid;
        check (decideFor (invalid, 1750000000, 10100, reason) == Decision::unavailable,
               "C7 politica nao validada NAO bloqueia", reason);
    }

    //==========================================================================
    std::cout << "D. reapresentacao de politica antiga" << std::endl;

    {
        Policy policy;
        juce::String e;
        check (parseSignedPolicy (desktopPolicy5.loadFileAsString(), policy, e),
               "D1 serial 2 aceito depois do 1", e);

        Policy older;
        juce::String e2;
        check (! parseSignedPolicy (desktopPolicy4.loadFileAsString(), older, e2),
               "D2 serial 1 RECUSADO depois do 2 (anti-replay)", e2);
    }

    //==========================================================================
    // A segunda prova. O modo de falha PERIGOSO aqui e o silencioso: se
    // WinVerifyTrust/spctl estivesse mal configurado e aprovasse tudo, o app
    // continuaria "funcionando" e nos teriamos perdido metade da protecao sem
    // nunca perceber. Por isso testamos os DOIS lados.
    std::cout << "E. assinatura do sistema operacional no instalador" << std::endl;

    {
        juce::String e;

        // Precisa ser um binario com assinatura EMBUTIDA. Nao serve
        // notepad.exe nem outro componente do Windows: eles sao assinados por
        // CATALOGO, e WinVerifyTrust com WTD_CHOICE_FILE devolve
        // TRUST_E_NOSIGNATURE (0x800b0100) para eles. Nosso instalador vai ser
        // assinado com signtool, que produz assinatura embutida -- exigir
        // embutida e a regra certa e a mais estrita.
       #if JUCE_WINDOWS
        const juce::StringArray candidates {
            "C:\\Program Files\\CMake\\bin\\cmake.exe",
            "C:\\Program Files\\Git\\bin\\git.exe"
        };
       #else
        // No macOS nao existe equivalente honesto: "spctl --assess --type
        // install" so responde de forma util sobre um instalador/imagem de
        // disco de verdade. Este caso precisa ser rodado la com o .dmg
        // assinado e notarizado -- ver PENDENCIAS no ATUALIZACAO_DESKTOP.md.
        const juce::StringArray candidates;
       #endif

        juce::File reference;
        for (const auto& path : candidates)
        {
            const juce::File candidate { path };
            if (candidate.exists())
            {
                reference = candidate;
                break;
            }
        }

        if (reference != juce::File())
            check (operatingSystemTrustsInstaller (reference, e),
                   "E1 binario com assinatura embutida ACEITO",
                   reference.getFileName() + ": " + e);
        else
            std::cout << "  (pulado) E1 sem binario de referencia assinado" << std::endl;

        // O nosso proprio executavel nao e assinado. Com
        // CHURCHDAW_UPDATE_REQUIRE_OS_SIGNATURE em 0 ele passa a ser ACEITO --
        // sustentado pelo SHA-256 da policy RSA, nao pela CA do sistema. Este
        // teste existe para que a mudanca seja uma decisao registrada e nao um
        // afrouxamento silencioso: se alguem devolver a exigencia para 1, e
        // aqui que a expectativa tem de ser revertida junto.
        const auto unsignedBinary = repoRoot.getChildFile (
            "build/MinhaDAW_artefacts/Release/ChurchDAW.exe");

        if (unsignedBinary.existsAsFile())
        {
            juce::String e2;
            const bool trusted = operatingSystemTrustsInstaller (unsignedBinary, e2);

           #if CHURCHDAW_UPDATE_REQUIRE_OS_SIGNATURE
            check (! trusted, "E2 binario SEM assinatura recusado (exigencia ligada)",
                   "aceitou indevidamente");
           #else
            check (trusted, "E2 binario SEM assinatura ACEITO (exigencia desligada)", e2);
           #endif
        }
        else
        {
            std::cout << "  (pulado) E2 sem binario nao assinado" << std::endl;
        }

        // O elo que NAO pode afrouxar: um PE com assinatura presente porem
        // quebrada continua recusado mesmo com a exigencia desligada. Forjamos
        // isso adulterando um byte de um binario assinado de verdade.
        if (reference != juce::File())
        {
            const auto tampered = juce::File::getSpecialLocation (
                juce::File::tempDirectory).getChildFile ("churchdaw-adulterado.exe");
            tampered.deleteFile();

            if (reference.copyFileTo (tampered))
            {
                juce::MemoryBlock bytes;
                tampered.loadFileAsData (bytes);

                if (bytes.getSize() > 4096)
                {
                    // Mexe no meio do codigo, longe do cabecalho, para o arquivo
                    // continuar sendo um PE valido com assinatura que nao confere.
                    auto* raw = static_cast<juce::uint8*> (bytes.getData());
                    raw[bytes.getSize() / 2] = (juce::uint8) (raw[bytes.getSize() / 2] ^ 0xFF);
                    tampered.replaceWithData (bytes.getData(), bytes.getSize());

                    juce::String e4;
                    check (! operatingSystemTrustsInstaller (tampered, e4),
                           "E4 binario assinado porem ADULTERADO continua recusado",
                           "aceitou um arquivo com assinatura quebrada");
                }
            }
            tampered.deleteFile();
        }

        juce::String e3;
        check (! operatingSystemTrustsInstaller (
                   repoRoot.getChildFile ("nao-existe-mesmo.exe"), e3),
               "E3 arquivo inexistente recusado");
    }

    //==========================================================================
    if (hadState)
        state.replaceWithText (savedState);
    else
        state.deleteFile();

    std::cout << std::endl
              << (failures == 0 ? "TODOS OS TESTES PASSARAM" : "TESTES REPROVARAM")
              << ": " << (checks - failures) << "/" << checks << std::endl;

    return failures == 0 ? 0 : 1;
}
