#pragma once

#include <juce_core/juce_core.h>

//==============================================================================
// ATUALIZACAO AUTOMATICA E OBRIGATORIA NO WINDOWS E NO macOS.
//
// POR QUE ESTE ARQUIVO EXISTE
// ---------------------------
// No Android quem instala e a Play Store, e o ChurchDAWUpdateBridge.java fixa a
// regra de ouro do projeto:
//
//     "o app nunca baixa nem encaminha um APK vindo da hospedagem"
//
// A loja confere a assinatura; o app so PEDE. No desktop nao existe loja, entao
// esse trabalho volta para nos -- e junto volta o risco que a regra evitava: se
// a hospedagem for invadida, um instalador falso chega em todas as maquinas dos
// compradores de uma vez. Hospedagem compartilhada e alvo realista.
//
// Por isso aqui a confianca NUNCA vem da origem do download. Vem de duas provas
// independentes, e as duas precisam passar:
//
//   1) Assinatura RS256 nossa sobre o manifesto -- mesmo esquema, mesmo
//      algoritmo e MESMA CHAVE que o Android ja usa
//      (ChurchDAWSignedPolicyVerifier.java, key_id rotacionado
//      "churchdaw-update-2026-08" ou anterior "churchdaw-update-2026-01",
//      RSA-3072, assinatura de 384 bytes sobre os bytes JSON do payload).
//      A chave privada vive fora do projeto e fora da hospedagem, entao editar
//      um JSON no servidor nao basta para inventar uma atualizacao obrigatoria.
//
//   2) Assinatura do SISTEMA OPERACIONAL no instalador baixado -- Authenticode
//      no Windows, avaliacao do Gatekeeper no macOS. Se um dia a nossa chave
//      privada vazar, esta segunda prova ainda segura; e se a cadeia da
//      Microsoft/Apple falhar, a nossa segura. Uma so das duas nunca basta.
//
// O SHA-256 do instalador viaja DENTRO do payload assinado, entao a corrente
// fecha inteira: assinatura RSA -> payload -> SHA-256 -> bytes do instalador.
// Nada e executado antes dos tres elos casarem.
//
//
// A REGRA QUE VALE MAIS QUE AS OUTRAS
// -----------------------------------
// Isto e um INSTRUMENTO, tocado ao vivo. Uma atualizacao obrigatoria que dispara
// no meio do culto e pior do que ficar desatualizado: o tecladista fica sem som
// na frente da igreja e nao tem o que fazer.
//
// Entao o portao obrigatorio so e avaliado no ARRANQUE, antes de existir audio.
// Com o app ja aberto, uma atualizacao e no maximo ANUNCIADA -- nunca imposta.
// Ver mayEnforceNow().
//
//
// A TRAVA ANTI-TIJOLO
// -------------------
// O Android so arma o portao depois que a distribuicao oficial CONFIRMA existir
// uma versao suficiente -- justamente para um numero digitado errado nao
// inutilizar o aparelho de todo mundo. Aqui vale o mesmo: minimum_version_code sozinho NAO
// bloqueia. O portao so arma depois que existe, para ESTA plataforma, um
// instalador anunciado, baixado, com SHA-256 conferido contra a policy assinada
// e de versao suficiente. Faltando qualquer elo, o app abre normalmente e
// apenas avisa. Um erro nosso no servidor jamais pode deixar o comprador sem
// instrumento no domingo de manha.
//
// A assinatura do sistema no instalador e configuravel -- ver
// CHURCHDAW_UPDATE_REQUIRE_OS_SIGNATURE abaixo. O pipeline comercial Windows
// compila com 1; somente builds locais/de teste usam 0. Em ambos os modos uma
// assinatura adulterada continua barrada.
//==============================================================================
namespace ChurchDAWDesktopUpdate
{

#if defined (CHURCHDAW_UPDATE_TEST_STATE_DIR)
inline juce::File updaterTestStateFile()
{
    static const auto stateName = "update-policy-test-" + juce::Uuid().toString() + ".json";
    return juce::File (juce::String::fromUTF8 (CHURCHDAW_UPDATE_TEST_STATE_DIR))
        .getChildFile (stateName);
}
#endif


// Versao desta build como inteiro comparavel. Vem do CMake
// (CHURCHDAW_VERSION_CODE = maior*10000 + menor*100 + correcao), portanto
// 1.2.0 -> 10200. E um contador PROPRIO do desktop: nao misturar com o
// versionCode do Android, que anda em outro ritmo.
#ifdef CHURCHDAW_VERSION_CODE
 inline constexpr int thisVersionCode = CHURCHDAW_VERSION_CODE;
#else
 inline constexpr int thisVersionCode = 0;
#endif

inline constexpr const char* policyKeyId      = "churchdaw-update-2026-01";
inline constexpr const char* policyKeyIdCurrent = "churchdaw-update-2026-08";
inline constexpr const char* policyKeyId202611  = "churchdaw-update-2026-11";
inline constexpr const char* policyAlgorithm  = "SHA256withRSA";
inline constexpr const char* productId        = "com.churchdaw.app";

// Toda URL usada aqui precisa ser HTTPS e terminar neste dominio. A policy e
// assinada, entao a URL ja vem confiavel; esta checagem e o cinto alem do
// suspensorio -- se algum dia um payload for assinado por engano com um link de
// terceiro, ele ainda assim nao baixa.
inline constexpr const char* requiredHostSuffix = "churchdaw.com";

// ASSINATURA DO SISTEMA NO INSTALADOR: EXIGIDA OU APENAS CONFERIDA?
// -----------------------------------------------------------------
// Releases comerciais precisam compilar com 1 e usar certificado publico de
// assinatura de codigo. O CMake deixa 0 como padrao apenas para desenvolvimento
// local, onde nao ha uma identidade comercial disponivel.
//
// Com 0, o elo que continua OBRIGATORIO e o forte: a policy assinada em
// RSA-3072 (chave privada fora desta maquina) carrega o SHA-256 dos bytes
// exatos, e o download so vale se o hash bater. Somem HTTPS e o dominio
// proprio. E o mesmo modelo de updaters consagrados que assinam o manifesto em
// vez de depender da CA do sistema.
//
// ATENCAO -- o que NAO muda: assinatura INVALIDA continua recusada. Tolerar
// "arquivo sem assinatura" e diferente de tolerar "assinatura quebrada,
// certificado revogado ou binario adulterado". Esses seguem barrando a
// instalacao mesmo com este valor em 0.
#ifndef CHURCHDAW_UPDATE_REQUIRE_OS_SIGNATURE
 #define CHURCHDAW_UPDATE_REQUIRE_OS_SIGNATURE 0
#endif

// Endereco da policy. O padrao e producao; o CMake pode apontar para outro
// caminho DO MESMO DOMINIO em builds de teste. Qualquer valor ainda passa por
// isTrustedHttpsUrl(), entao trocar isto nao afrouxa nada.
#ifndef CHURCHDAW_UPDATE_POLICY_URL
 #define CHURCHDAW_UPDATE_POLICY_URL \
     "https://churchdaw.com/app/desktop/update-policy-v1.json"
#endif

inline constexpr const char* policyUrl = CHURCHDAW_UPDATE_POLICY_URL;

// Afrouxa a exigencia de HTTPS + dominio SOMENTE para 127.0.0.1, e somente
// quando o build pede. Desligado por padrao, igual ao
// CHURCHDAW_LICENSE_ALLOW_HTTP_LOCALHOST que ja existe no licenciamento.
// Existe para o fluxo obrigatorio poder ser exercitado inteiro numa maquina de
// desenvolvimento, sem publicar nada em producao para testar.
#ifndef CHURCHDAW_UPDATE_ALLOW_HTTP_LOCALHOST
 #define CHURCHDAW_UPDATE_ALLOW_HTTP_LOCALHOST 0
#endif

inline constexpr int maximumPolicyBytes    = 32 * 1024;
inline constexpr int maximumPayloadBytes   = 24 * 1024;
inline constexpr int signatureBytes        = 384;      // RSA-3072
inline constexpr juce::int64 maximumInstallerBytes = 1024LL * 1024LL * 1024LL;

// A consulta a policy segura o arranque, entao o tempo maximo de espera e
// curto de proposito: um PC de igreja sem internet nao pode ficar 15 s parado
// numa janela "Verificando..." toda vez que o musico abre o programa.
// O download ja acontece com o usuario ciente, e pode esperar mais.
inline constexpr int policyTimeoutMs   = 8000;
inline constexpr int downloadTimeoutMs = 30000;

enum class Decision
{
    upToDate,      // nada a fazer
    optional,      // ha versao nova; avisar sem bloquear
    mandatory,     // versao nova E abaixo do minimo: pode bloquear no arranque
    unavailable    // nao deu para saber (rede/assinatura/servidor) -> abrir normal
};

struct Policy
{
    int schemaVersion       = 0;
    int policySerial        = 0;
    int revokesPolicySerial = 0;      // desfaz uma policy ruim ja publicada
    int latestVersionCode   = 0;
    int minimumVersionCode  = 0;
    juce::int64 issuedAt    = 0;
    juce::int64 expiresAt   = 0;
    juce::String mode;                // "normal" ou "rollback"
    juce::String latestVersionName;
    juce::String updateMessage;

    // ja resolvido para a plataforma que compilou este binario
    juce::String installerUrl;
    juce::String installerSha256;
    juce::int64  installerBytes = 0;

    bool valid = false;               // so true depois da assinatura conferida

    bool hasInstaller() const noexcept
    {
        return installerUrl.isNotEmpty()
            && installerSha256.length() == 64
            && installerBytes > 0;
    }
};

// Confere a assinatura RS256 da policy com a chave publica de atualizacao.
// message = bytes JSON do payload (NAO o Base64 dele) -- e o que o
// SignAndroidUpdatePolicy.ps1 assina e o que o verificador Java confere.
bool verifyUpdateRs256Signature (const juce::MemoryBlock& message,
                                 const juce::MemoryBlock& signature,
                                 juce::String& error);

bool verifyUpdateRs256Signature (const juce::MemoryBlock& message,
                                 const juce::MemoryBlock& signature,
                                 const juce::String& keyId,
                                 juce::String& error);

// Verifica um envelope assinado no formato Base64 usado pelo Android
// (payload + assinatura + key_id).
//
// Existe porque no desktop nao ha JNI para chamar o
// ChurchDAWSignedPolicyVerifier.java. Sem isto o ChurchDAWServices nao consegue
// validar o catalogo de bibliotecas no Windows/Mac, e o download dos timbres
// fica bloqueado -- ver o comentario em verifyAndroidSignedPolicyEnvelope.
bool verifySignedPolicyEnvelope (const juce::String& payloadBase64,
                                 const juce::String& signatureBase64,
                                 const juce::String& keyId);

// Le o envelope assinado e devolve a policy ja validada. Nao toca na rede.
bool parseSignedPolicy (const juce::String& envelopeJson,
                        Policy& policy,
                        juce::String& error);

// Converte o formato IMF-fixdate usado pelo cabecalho HTTP Date, por exemplo
// "Sat, 15 Aug 2026 12:34:56 GMT". Zero significa formato/valor rejeitado.
juce::int64 parseHttpDateEpochSeconds (const juce::String& value);

// Decide o que fazer com uma policy JA validada. Funcao pura -- sem rede, sem
// relogio, sem disco -- para a regra anti-tijolo poder ser testada direto.
Decision decideFor (const Policy& policy,
                    juce::int64 nowEpochSeconds,
                    int installedVersionCode,
                    juce::String& reason);

// Baixa a policy e valida. Falha vira Decision::unavailable -- nunca bloqueia
// por erro de rede. Chamar de thread de fundo; jamais da thread de audio.
Decision fetchPolicy (Policy& policy, juce::String& error);

// Baixa o instalador e confere o SHA-256 contra o payload ASSINADO.
// false = nao confie no que baixou (o arquivo e apagado).
bool downloadAndVerifyInstaller (const Policy& policy,
                                 juce::File& destination,
                                 juce::String& error,
                                 std::function<void (double)> onProgress = {});

// Segunda prova, independente da nossa: o sistema confia neste instalador?
bool operatingSystemTrustsInstaller (const juce::File& installer,
                                     juce::String& error);

// Registra que este instalador (pelo SHA-256) ja foi executado.
//
// Existe para quebrar o loop: se a policy exigir uma versao que o instalador
// publicado nao entrega, sem isto o app instalaria, reabriria ainda abaixo do
// minimo e instalaria de novo, sem fim.
void rememberInstallerWasRun (const juce::String& installerSha256);

// Entrega o instalador ao sistema. NAO instalamos por conta propria: quem
// escreve em Program Files ou em /Applications e o instalador assinado, usando
// a elevacao de privilegio do proprio sistema.
juce::StringArray macGatekeeperAssessmentCommand (const juce::File& installer);

bool launchInstaller (const juce::File& installer, juce::String& error);

// O portao obrigatorio pode ser IMPOSTO agora? Falso durante apresentacao.
bool mayEnforceNow (bool audioDeviceOpen,
                    int activeVoices,
                    bool startupPhase) noexcept;

// Orquestra tudo no arranque e chama onFinished quando termina.
//
// shouldQuit == true  -> o instalador foi iniciado; o app precisa sair.
// shouldQuit == false -> siga o arranque normal.
//
// E assincrono porque este JUCE compila com JUCE_MODAL_LOOPS_PERMITTED=0: nao
// existe runModalLoop() para bloquear esperando o usuario. O fluxo inteiro vive
// em callbacks do MessageManager.
//
// Seguro chamar sempre: sem rede, sem policy ou com qualquer falha, onFinished
// recebe false e o ChurchDAW abre normalmente.
void runStartupUpdateGate (std::function<void (bool shouldQuit)> onFinished);

}   // namespace ChurchDAWDesktopUpdate
