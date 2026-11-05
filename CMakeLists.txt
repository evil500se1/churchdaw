cmake_minimum_required(VERSION 3.22)

# Configure o target Apple ANTES de project()/enable_language(). Caso contrario,
# CMake pode inicializar o compilador com o SDK atual e gravar um minos novo
# demais nos objetos antes de CMAKE_OSX_DEPLOYMENT_TARGET ser alterado.
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "" FORCE)
elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin"
       AND (NOT CMAKE_SYSTEM_NAME OR CMAKE_SYSTEM_NAME STREQUAL "Darwin"))
    # O piso geral e do Info.plist e Catalina para o slice Intel. O toolchain
    # Apple eleva automaticamente o slice arm64 ao piso real da arquitetura,
    # macOS 11.0; os workflows verificam cada slice com lipo + vtool.
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "" FORCE)
endif()

# Versao do produto desktop. O Android mantem versionName/versionCode no
# ChurchDAW.jucer e pode ser publicado em um ciclo independente.
# 1.6.2 — remocao completa do Sabrina VST2; correcao do filtro de portas MIDI
#         e do laco infinito no wrap de loop (ver HANDOFF_SABRINA.md).
# 1.6.3 — refinamentos visuais do painel Master e cabecalho (publicada em
#         05/08/2026 com version code 10610, fora da formula abaixo).
# 1.6.4 — medidor L/R nao acende mais em silencio (o piso da escala era tratado
#         como sinal) e clip que fica travado ate o musico ver; grade de 12 tons
#         no Pad Continuo com config por tom; "ENV / FX" nao invade mais os
#         botoes M/S na tira de canal; guarda de NaN no master; roubo de voz nao
#         rouba mais notas do proprio bloco; handshake de vida entre a thread de
#         MIDI e a message thread (crash ao trocar o som do slot com o teclado
#         mandando notas); shelf de agudo do master relativo a Nyquist.
# 1.6.5 — MESMO codigo-fonte da 1.6.4, agora tambem para macOS (universal
#         Intel + Apple Silicon). O numero subiu porque a 1.6.4 ja tinha sido
#         publicada so para Windows: uma politica normal precisa AUMENTAR o
#         latest_version_code, e nao existe modo "mesma versao, plataforma
#         nova". Preferimos subir a versao a afrouxar essa trava.
# 1.6.6 — RUIDO METALICO EM TODOS OS TIMBRES: a interpolacao Hermite estava com
#         o coeficiente cubico errado (faltava um termo 'w' em TimbresProcessor.h).
#         O erro valia exatamente zero em f=0 e f=1, entao a curva ainda passava
#         por cima de cada amostra e nada estourava -- so a tangente ENTRE elas
#         saia errada, e o defeito passou anos invisivel. A biblioteca e 44,1 kHz
#         e o host roda a 48 kHz, entao TODA nota de TODO timbre passava pela
#         formula quebrada. Medido na reamostragem 44,1 -> 48 kHz: SNR de 37 para
#         89 dB a 1 kHz; a versao quebrada era 17 dB PIOR que a interpolacao
#         linear que ela substituiu.
# 1.6.7 — CORRECOES DE AUDITORIA (07/08/2026): cena com timbre ausente nao deixa
#         mais o instrumento anterior tocando; guarda de nivel finito atenua em
#         vez de zerar a mistura; piano com loop gravado fica em streaming (nao
#         carrega GB na RAM); Pad Continuo segue o Transpose Global e troca de
#         tom sem buraco; eco do delay decai naturalmente; PUNCH sem "tec";
#         ROTARY nao some ao trocar timbre; seletor de timbres rapido; layout
#         monotono em 1100-1440px; medidores com balistica consistente; Android
#         (tela nao apaga, volta ao app sem esconder a DAW, seletor atualiza,
#         faders sem salto); MIDI hot-plug reconecta sozinho; MIDI Learn
#         sobrevive ao replug; Launchkey recupera auto-mapeamento no replug.
# 1.6.8 — REFORMULACAO VISUAL E RELEASE DESKTOP (08/08/2026): interface premium
#         unificada, mixer/canais compactos, seletor de timbres reformulado,
#         paineis Live/Pad/MIDI/Bibliotecas revisados e auto-update publicado em
#         conjunto para Windows e macOS.
# 1.6.9 - MIDI, CENAS E PAD DE SAMPLES (08/08/2026): MIDI Learn direto nos
#         controles visiveis, isolamento seguro do CC11, Pad sem sintetizador,
#         reset completo de cenas vazias e parser de update compativel com as
#         policies publicadas.
# 1.6.10 - CENAS E PAD LP (08/08/2026): nova cena abre realmente vazia, Salvar
#          superior captura a cena preparada/ativa, reset geral persistente sem
#          apagar cenas salvas, Pad Continuo novamente visivel e Pad LP publicado.
# 1.6.11 - FILTROS NORD E BIBLIOTECA NSMP4 (09/08/2026): editor de pads com
#          seis modos de filtro, envelope, drive e Amp/EQ; perfil fiel do Pad LP;
#          extracao/catalogacao dos presets Nord; setas ate o canal 14 e numeros
#          de dois digitos legiveis no mixer.
# 1.6.12 - DOWNLOADS E VST3 (09/08/2026): downloads de bibliotecas e timbres
#          encerram no tamanho oficial, retomam arquivos parciais e exibem
#          progresso real de extracao; o scanner sempre inclui as pastas VST3
#          padrao e reclassifica instrumentos pela instancia carregada quando o
#          fabricante omite a categoria, mantendo efeitos somente como efeitos.
# 1.6.13 - LICENCIAMENTO E CATALOGO (09/08/2026): sessao desktop fail-closed,
#          logout resistente a corrida de refresh e validacao estrita de
#          respostas HTTP; catalogos de timbres reconciliados para publicacao.
# 1.6.14 - COMPATIBILIDADE DE LICENCA (10/08/2026): aceita o contrato legado
#          schema 014 somente depois de validar integralmente o lease RS256,
#          mantendo respostas modernas e hibridas em modo fail-closed.
# 1.6.15 - INSTALACAO DE TIMBRES (10/08/2026): commit transacional com rollback,
#          retomada que contabiliza somente bytes restantes, recibo exato por
#          arquivo e menor uso de disco para os pianos grandes.
# 1.6.35 - MOTOR RT-SAFE, DESIGN E TOUCH (03/11/2026): DSP blindado contra
#          denormais/NaN, roubo de voz suave no orgao, Transpose Global em tempo
#          real no Pad Continuo, polimento visual PT-BR e modo touch iOS/Android.
project(MinhaDAW VERSION 1.6.35)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options("/utf-8")
endif()

option(CHURCHDAW_ENABLE_DESKTOP_LICENSING
       "Exige conta e licenca validas antes de iniciar o motor desktop"
       ON)
option(CHURCHDAW_ALLOW_UNLICENSED_DESKTOP_BUILD
       "Permite build desktop sem licenca SOMENTE para self-tests locais"
       OFF)
option(CHURCHDAW_BUILD_LICENSE_TESTS
       "Compila testes nativos do verificador de licenca"
       OFF)
option(CHURCHDAW_BUILD_IOS_UI_TESTS
       "Compila a sonda XCTest de interface para iPhone/iPad fisicos"
       OFF)
option(CHURCHDAW_LICENSE_ALLOW_HTTP_LOCALHOST
       "Permite endpoint HTTP somente em localhost para testes locais"
       OFF)
set(CHURCHDAW_LICENSE_API_BASE
    "https://churchdaw.com/api/v1"
    CACHE STRING
    "Endpoint HTTPS do backend de licenciamento desktop")

# Um cache CMake antigo com CHURCHDAW_ENABLE_DESKTOP_LICENSING=OFF gerava um
# aplicativo comercial completamente aberto. Exigir uma segunda chave,
# explicitamente nomeada como self-test, transforma esse erro silencioso em
# falha de configuracao. Android e iOS possuem gates proprios.
if((WIN32 OR (APPLE AND NOT IOS))
   AND NOT CHURCHDAW_ENABLE_DESKTOP_LICENSING
   AND NOT CHURCHDAW_ALLOW_UNLICENSED_DESKTOP_BUILD)
    message(FATAL_ERROR
        "Build desktop sem licenciamento recusado. Para self-test local use "
        "-DCHURCHDAW_ALLOW_UNLICENSED_DESKTOP_BUILD=ON; nunca publique esse build.")
endif()

set(CHURCHDAW_UPDATE_POLICY_URL
    "https://churchdaw.com/app/desktop/update-policy-v1.json"
    CACHE STRING
    "Endereco da politica de atualizacao obrigatoria do desktop")
option(CHURCHDAW_UPDATE_ALLOW_LOCALHOST
       "Aceita politica de atualizacao vinda de 127.0.0.1 (SO para teste local)"
       OFF)
option(CHURCHDAW_UPDATE_REQUIRE_OS_SIGNATURE
       "Exige Authenticode/Gatekeeper confiavel no instalador do auto-update"
       OFF)

# VST2 e um formato legado cuja distribuicao exige que o fabricante do host
# tenha uma autorizacao Steinberg assinada antes de outubro de 2018. Por isso o
# ChurchDAW nunca baixa nem incorpora esses headers automaticamente. Quem ja
# possui essa autorizacao pode apontar para uma copia licenciada do SDK e gerar
# o host Windows com suporte aos plugins .dll de 64 bits.
option(CHURCHDAW_ENABLE_VST2_HOSTING
       "Habilita hospedagem VST2 legada no Windows (requer licenca e SDK validos)"
       OFF)
set(CHURCHDAW_VST2_SDK_PATH ""
    CACHE PATH
    "Pasta licenciada que contem pluginterfaces/vst2.x/aeffect.h e aeffectx.h")

# runtime do C++ embutido no exe: roda em qualquer PC sem instalar redistribuivel
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_subdirectory(JUCE)

# Testes offline do motor de reverb. Ficam fora das builds normais e verificam
# afinacao, cauda, troca de preset e estabilidade numerica antes do APK/AAB.
option(CHURCHDAW_BUILD_AUDIO_TESTS
       "Compila os testes DSP offline do reverb"
       OFF)

if(CHURCHDAW_BUILD_AUDIO_TESTS AND NOT ANDROID AND NOT IOS)
    enable_testing()

    juce_add_console_app(ChurchDAWReverbPitchTests
        PRODUCT_NAME "ChurchDAWReverbPitchTests")

    target_sources(ChurchDAWReverbPitchTests PRIVATE
        Tests/Audio/WorshipReverbPitchTests.cpp)

    target_include_directories(ChurchDAWReverbPitchTests PRIVATE Source)
    target_compile_definitions(ChurchDAWReverbPitchTests PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_PUSH_NOTIFICATIONS=0
        JUCE_USE_CURL=0)

    target_link_libraries(ChurchDAWReverbPitchTests
        PRIVATE
            juce::juce_dsp
            juce::juce_audio_basics
        PUBLIC
            juce::juce_recommended_config_flags)

    if(MSVC)
        target_compile_options(ChurchDAWReverbPitchTests PRIVATE /utf-8)
    endif()

    juce_add_console_app(ChurchDAWMasterLimiterTests
        PRODUCT_NAME "ChurchDAWMasterLimiterTests")

    juce_add_console_app(ChurchDAWSabrinaTests
        PRODUCT_NAME "ChurchDAWSabrinaTests")

    juce_add_console_app(ChurchDAWMixerStripCollapseTests
        PRODUCT_NAME "ChurchDAWMixerStripCollapseTests")

    juce_add_console_app(ChurchDAWChannelToneTests
        PRODUCT_NAME "ChurchDAWChannelToneTests")

    juce_add_console_app(ChurchDAWNordSampleToneTests
        PRODUCT_NAME "ChurchDAWNordSampleToneTests")

    juce_add_console_app(ChurchDAWMidiMappingSafetyTests
        PRODUCT_NAME "ChurchDAWMidiMappingSafetyTests")

    juce_add_console_app(ChurchDAWChannelRotaryTests
        PRODUCT_NAME "ChurchDAWChannelRotaryTests")

    juce_add_console_app(ChurchDAWPadProcessorTests
        PRODUCT_NAME "ChurchDAWPadProcessorTests")

    juce_add_console_app(ChurchDAWPadSynthProcessorTests
        PRODUCT_NAME "ChurchDAWPadSynthProcessorTests")

    juce_add_console_app(ChurchDAWPadSynthRenderProbe
        PRODUCT_NAME "ChurchDAWPadSynthRenderProbe")

    juce_add_console_app(ChurchDAWLibraryPackPathTests
        PRODUCT_NAME "ChurchDAWLibraryPackPathTests")

    juce_add_console_app(ChurchDAWTimbreDownloadTests
        PRODUCT_NAME "ChurchDAWTimbreDownloadTests")

    juce_add_console_app(ChurchDAWLiveScenesTests
        PRODUCT_NAME "ChurchDAWLiveScenesTests")

    juce_add_console_app(ChurchDAWSceneFileWriteProbe
        PRODUCT_NAME "ChurchDAWSceneFileWriteProbe")

    juce_add_console_app(ChurchDAWPluginScanPolicyTests
        PRODUCT_NAME "ChurchDAWPluginScanPolicyTests")

    juce_add_console_app(ChurchDAWChildProcessReapTests
        PRODUCT_NAME "ChurchDAWChildProcessReapTests")

    add_executable(ChurchDAWAndroidStageLayoutTests
        Tests/UI/AndroidStageLayoutTests.cpp)

    add_executable(ChurchDAWMobileBufferPolicyTests
        Tests/Audio/MobileAudioBufferPolicyTests.cpp)

    target_sources(ChurchDAWMasterLimiterTests PRIVATE
        Tests/Audio/StereoLookaheadLimiterTests.cpp)

    target_sources(ChurchDAWSabrinaTests PRIVATE
        Tests/Audio/SabrinaProcessorTests.cpp
        Source/Plugins/Sabrina/SabrinaProcessor.cpp)

    target_sources(ChurchDAWMixerStripCollapseTests PRIVATE
        Tests/UI/MixerStripCollapseTests.cpp
        Tests/UI/MidiLearnOverlayTests.cpp
        Tests/UI/TimbrePickerLayoutTests.cpp
        Tests/Plugin/PluginPreviewLifecycleTests.cpp
        Tests/Plugin/PluginCandidateSearchTests.cpp
        Tests/UI/FullUpgradeTests.cpp)

    target_sources(ChurchDAWChannelToneTests PRIVATE
        Tests/Audio/ChannelToneShaperTests.cpp)

    target_sources(ChurchDAWNordSampleToneTests PRIVATE
        Tests/Audio/NordSampleToneTests.cpp)

    target_sources(ChurchDAWMidiMappingSafetyTests PRIVATE
        Tests/Audio/MidiMappingSafetyTests.cpp)

    target_sources(ChurchDAWChannelRotaryTests PRIVATE
        Tests/Audio/ChannelRotarySpeakerTests.cpp)

    target_sources(ChurchDAWPadProcessorTests PRIVATE
        Tests/Audio/PadProcessorTests.cpp)

    target_sources(ChurchDAWPadSynthProcessorTests PRIVATE
        Tests/Audio/PadSynthProcessorTests.cpp)

    target_sources(ChurchDAWPadSynthRenderProbe PRIVATE
        Tests/Audio/PadSynthRenderProbe.cpp)

    target_sources(ChurchDAWLibraryPackPathTests PRIVATE
        Tests/Library/LibraryPackPathTests.cpp)

    target_sources(ChurchDAWTimbreDownloadTests PRIVATE
        Tests/Library/TimbreDownloadTests.cpp
        Source/DesktopUpdater.cpp)

    target_sources(ChurchDAWLiveScenesTests PRIVATE
        Tests/Live/LiveScenesTests.cpp)

    target_sources(ChurchDAWSceneFileWriteProbe PRIVATE
        Tests/Live/SceneFileWriteProbe.cpp)

    target_sources(ChurchDAWPluginScanPolicyTests PRIVATE
        Tests/Plugin/PluginScanPolicyTests.cpp)

    target_sources(ChurchDAWChildProcessReapTests PRIVATE
        Tests/Plugin/ConnectedChildProcessReapTests.cpp)

    target_include_directories(ChurchDAWMasterLimiterTests PRIVATE Source)
    target_include_directories(ChurchDAWSabrinaTests PRIVATE Source)
    target_include_directories(ChurchDAWMixerStripCollapseTests PRIVATE Source)
    target_include_directories(ChurchDAWChannelToneTests PRIVATE Source)
    target_include_directories(ChurchDAWNordSampleToneTests PRIVATE Source)
    target_include_directories(ChurchDAWMidiMappingSafetyTests PRIVATE Source)
    target_include_directories(ChurchDAWChannelRotaryTests PRIVATE Source)
    target_include_directories(ChurchDAWPadProcessorTests PRIVATE Source)
    target_include_directories(ChurchDAWPadSynthProcessorTests PRIVATE Source)
    target_include_directories(ChurchDAWPadSynthRenderProbe PRIVATE Source)
    target_include_directories(ChurchDAWLibraryPackPathTests PRIVATE Source)
    target_include_directories(ChurchDAWTimbreDownloadTests PRIVATE Source)
    target_include_directories(ChurchDAWLiveScenesTests PRIVATE Source)
    target_include_directories(ChurchDAWSceneFileWriteProbe PRIVATE Source)
    target_include_directories(ChurchDAWPluginScanPolicyTests PRIVATE Source)
    target_include_directories(ChurchDAWAndroidStageLayoutTests PRIVATE Source)
    target_include_directories(ChurchDAWMobileBufferPolicyTests PRIVATE Source)
    target_compile_definitions(ChurchDAWMasterLimiterTests PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_PUSH_NOTIFICATIONS=0
        JUCE_USE_CURL=0)
    target_compile_definitions(ChurchDAWSabrinaTests PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_PUSH_NOTIFICATIONS=0
        JUCE_USE_CURL=0)
    target_compile_definitions(ChurchDAWMixerStripCollapseTests PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_PUSH_NOTIFICATIONS=0
        JUCE_USE_CURL=0)
    target_compile_definitions(ChurchDAWChannelToneTests PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_PUSH_NOTIFICATIONS=0
        JUCE_USE_CURL=0)
    target_compile_definitions(ChurchDAWNordSampleToneTests PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_PUSH_NOTIFICATIONS=0
        JUCE_USE_CURL=0)
    target_compile_definitions(ChurchDAWMidiMappingSafetyTests PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_PUSH_NOTIFICATIONS=0
        JUCE_USE_CURL=0)
    target_compile_definitions(ChurchDAWChannelRotaryTests PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_PUSH_NOTIFICATIONS=0
        JUCE_USE_CURL=0)
    target_compile_definitions(ChurchDAWPadProcessorTests PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_PUSH_NOTIFICATIONS=0
        JUCE_USE_CURL=0)
    target_compile_definitions(ChurchDAWPadSynthProcessorTests PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_PUSH_NOTIFICATIONS=0
        JUCE_USE_CURL=0)
    target_compile_definitions(ChurchDAWPadSynthRenderProbe PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_PUSH_NOTIFICATIONS=0
        JUCE_USE_CURL=0)
    target_compile_definitions(ChurchDAWLibraryPackPathTests PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_PUSH_NOTIFICATIONS=0
        JUCE_USE_CURL=0)
    target_compile_definitions(ChurchDAWTimbreDownloadTests PRIVATE
        JUCE_MODAL_LOOPS_PERMITTED=1
        JUCE_WEB_BROWSER=0
        JUCE_PUSH_NOTIFICATIONS=0
        JUCE_USE_CURL=0)
    target_compile_definitions(ChurchDAWLiveScenesTests PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_PUSH_NOTIFICATIONS=0
        JUCE_USE_CURL=0)
    target_compile_definitions(ChurchDAWSceneFileWriteProbe PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_PUSH_NOTIFICATIONS=0
        JUCE_USE_CURL=0)
    target_compile_definitions(ChurchDAWPluginScanPolicyTests PRIVATE
        JUCE_PLUGINHOST_VST3=1
        JUCE_WEB_BROWSER=0
        JUCE_PUSH_NOTIFICATIONS=0
        JUCE_USE_CURL=0)
    target_compile_definitions(ChurchDAWChildProcessReapTests PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_PUSH_NOTIFICATIONS=0
        JUCE_USE_CURL=0)
    target_link_libraries(ChurchDAWMasterLimiterTests
        PRIVATE
            juce::juce_dsp
            juce::juce_audio_basics
            juce::juce_audio_processors
        PUBLIC
            juce::juce_recommended_config_flags)

    target_link_libraries(ChurchDAWSabrinaTests
        PRIVATE
            juce::juce_dsp
            juce::juce_audio_basics
            juce::juce_audio_processors
        PUBLIC
            juce::juce_recommended_config_flags)

    target_link_libraries(ChurchDAWMixerStripCollapseTests
        PRIVATE
            ChurchAssets   # NordLookAndFeel.h inclui ChurchLiveFonts.h (BinaryData) com o visual Church Live ligado
            juce::juce_dsp
            juce::juce_audio_basics
            juce::juce_audio_processors
        PUBLIC
            juce::juce_recommended_config_flags)

    target_link_libraries(ChurchDAWChannelToneTests
        PRIVATE
            juce::juce_dsp
            juce::juce_audio_basics
        PUBLIC
            juce::juce_recommended_config_flags)

    target_link_libraries(ChurchDAWNordSampleToneTests
        PRIVATE
            juce::juce_dsp
            juce::juce_audio_basics
        PUBLIC
            juce::juce_recommended_config_flags)

    target_link_libraries(ChurchDAWMidiMappingSafetyTests
        PRIVATE
            juce::juce_audio_processors
            juce::juce_cryptography
        PUBLIC
            juce::juce_recommended_config_flags)

    target_link_libraries(ChurchDAWChannelRotaryTests
        PRIVATE
            juce::juce_audio_basics
        PUBLIC
            juce::juce_recommended_config_flags)

    target_link_libraries(ChurchDAWPadProcessorTests
        PRIVATE
            juce::juce_audio_formats
            juce::juce_audio_processors
        PUBLIC
            juce::juce_recommended_config_flags)

    target_link_libraries(ChurchDAWPadSynthProcessorTests
        PRIVATE
            juce::juce_dsp
            juce::juce_audio_processors
        PUBLIC
            juce::juce_recommended_config_flags)

    target_link_libraries(ChurchDAWPadSynthRenderProbe
        PRIVATE
            juce::juce_dsp
            juce::juce_cryptography
            juce::juce_audio_processors
        PUBLIC
            juce::juce_recommended_config_flags)

    target_link_libraries(ChurchDAWLibraryPackPathTests
        PRIVATE
            juce::juce_cryptography
            juce::juce_gui_extra
        PUBLIC
            juce::juce_recommended_config_flags)

    target_link_libraries(ChurchDAWTimbreDownloadTests
        PRIVATE
            juce::juce_cryptography
            juce::juce_gui_extra
        PUBLIC
            juce::juce_recommended_config_flags)

    if(WIN32)
        target_link_libraries(ChurchDAWTimbreDownloadTests PRIVATE
            crypt32 bcrypt wintrust)
    elseif(APPLE)
        target_link_libraries(ChurchDAWTimbreDownloadTests PRIVATE
            "-framework Security")
    endif()

    # LiveScenes.h so precisa de juce_core (XmlElement, String, jlimit) — e
    # isso e proposital: a serializacao das cenas nao conhece audio nem UI.
    target_link_libraries(ChurchDAWLiveScenesTests
        PRIVATE
            juce::juce_core
        PUBLIC
            juce::juce_recommended_config_flags)

    target_link_libraries(ChurchDAWSceneFileWriteProbe
        PRIVATE
            juce::juce_core
        PUBLIC
            juce::juce_recommended_config_flags)

    target_link_libraries(ChurchDAWPluginScanPolicyTests
        PRIVATE
            juce::juce_audio_processors
        PUBLIC
            juce::juce_recommended_config_flags)

    target_link_libraries(ChurchDAWChildProcessReapTests
        PRIVATE
            juce::juce_events
        PUBLIC
            juce::juce_recommended_config_flags)


    if(APPLE OR WIN32)
        # Smoke real do transporte V2. O mesmo executavel funciona como
        # coordenador e como processo filho descartavel. No macOS usa o
        # DLSMusicDevice; no Windows exige um VST3 indicado pelo ambiente.
        # No Windows, uma fixture sintetica adicional prova em processo real
        # que uma classe valida sobrevive ao crash da classe irma no mesmo DLL.
        if(WIN32)
            set(churchdaw_vst3_sdk_dir
                "${CMAKE_CURRENT_SOURCE_DIR}/JUCE/modules/juce_audio_processors_headless/format_types/VST3_SDK")

            add_library(ChurchDAWVST3MultiClassCrashFixture MODULE
                Tests/Plugin/Fixtures/VST3MultiClassCrashFixture.cpp
                "${churchdaw_vst3_sdk_dir}/pluginterfaces/base/funknown.cpp"
                "${churchdaw_vst3_sdk_dir}/pluginterfaces/base/coreiids.cpp"
                "${churchdaw_vst3_sdk_dir}/public.sdk/source/vst/vstinitiids.cpp")
            target_include_directories(
                ChurchDAWVST3MultiClassCrashFixture PRIVATE
                "${churchdaw_vst3_sdk_dir}")
            target_link_libraries(
                ChurchDAWVST3MultiClassCrashFixture PRIVATE ole32)
            set_target_properties(
                ChurchDAWVST3MultiClassCrashFixture PROPERTIES
                PREFIX ""
                OUTPUT_NAME "ChurchDAWVST3MultiClassCrashFixture"
                SUFFIX ".vst3")
        endif()

        juce_add_console_app(ChurchDAWPluginScanIntegrationTests
            PRODUCT_NAME "ChurchDAWPluginScanIntegrationTests")

        target_sources(ChurchDAWPluginScanIntegrationTests PRIVATE
            Tests/Plugin/PluginScanSubprocessV2IntegrationTests.cpp)
        target_include_directories(
            ChurchDAWPluginScanIntegrationTests PRIVATE Source)
        target_compile_definitions(
            ChurchDAWPluginScanIntegrationTests PRIVATE
            JUCE_PLUGINHOST_VST3=1
            JUCE_WEB_BROWSER=0
            JUCE_PUSH_NOTIFICATIONS=0
            JUCE_USE_CURL=0)
        target_link_libraries(
            ChurchDAWPluginScanIntegrationTests
            PRIVATE
                juce::juce_audio_processors
            PUBLIC
                juce::juce_recommended_config_flags)

        if(APPLE)
            target_compile_definitions(
                ChurchDAWPluginScanIntegrationTests PRIVATE
                JUCE_PLUGINHOST_AU=1)
            target_compile_options(
                ChurchDAWPluginScanIntegrationTests PRIVATE
                -Werror=unguarded-availability-new)
            target_link_libraries(
                ChurchDAWPluginScanIntegrationTests PRIVATE
                "-framework AudioUnit"
                "-framework AudioToolbox"
                "-framework CoreAudioKit")

            add_test(
                NAME ChurchDAWPluginScanIntegration
                COMMAND ChurchDAWPluginScanIntegrationTests)
            set_tests_properties(
                ChurchDAWPluginScanIntegration
                PROPERTIES TIMEOUT 300)
        elseif(DEFINED ENV{CHURCHDAW_PLUGIN_SCAN_FIXTURE}
               AND NOT "$ENV{CHURCHDAW_PLUGIN_SCAN_FIXTURE}" STREQUAL "")
            # A fixture e o diretorio isolado de relatorios sao impostos
            # externamente ao executavel. Sem fixture, o alvo continua
            # compilavel, mas nao vira um CTest que falharia por configuracao
            # ausente.
            set(churchdaw_plugin_scan_test_appdata
                "${CMAKE_CURRENT_BINARY_DIR}/plugin-scan-integration-appdata")
            file(MAKE_DIRECTORY "${churchdaw_plugin_scan_test_appdata}")
            add_test(
                NAME ChurchDAWPluginScanIntegration
                COMMAND ChurchDAWPluginScanIntegrationTests)
            set_tests_properties(
                ChurchDAWPluginScanIntegration
                PROPERTIES
                    TIMEOUT 300
                    ENVIRONMENT
                        "CHURCHDAW_PLUGIN_SCAN_FIXTURE=$ENV{CHURCHDAW_PLUGIN_SCAN_FIXTURE};CHURCHDAW_PLUGIN_SCAN_REPORT_DIR=${churchdaw_plugin_scan_test_appdata}/reports")
        endif()

        if(WIN32)
            add_dependencies(
                ChurchDAWPluginScanIntegrationTests
                ChurchDAWVST3MultiClassCrashFixture)

            set(churchdaw_plugin_scan_synthetic_appdata
                "${CMAKE_CURRENT_BINARY_DIR}/plugin-scan-synthetic-appdata")
            file(MAKE_DIRECTORY
                "${churchdaw_plugin_scan_synthetic_appdata}")

            add_test(
                NAME ChurchDAWPluginScanSyntheticShell
                COMMAND "${CMAKE_COMMAND}" -E env
                    "CHURCHDAW_PLUGIN_SCAN_REPORT_DIR=${churchdaw_plugin_scan_synthetic_appdata}/reports"
                    "CHURCHDAW_PLUGIN_SCAN_FIXTURE=$<TARGET_FILE:ChurchDAWVST3MultiClassCrashFixture>"
                    "CHURCHDAW_PLUGIN_SCAN_MIN_LOADED=1"
                    "CHURCHDAW_PLUGIN_SCAN_MIN_FACTORY_CLASSES=2"
                    "CHURCHDAW_PLUGIN_SCAN_MIN_FAILED_CLASSES=1"
                    "CHURCHDAW_PLUGIN_SCAN_REQUIRED_FAILURE_STATUS=crash"
                    "CHURCHDAW_PLUGIN_SCAN_STRICT_SYNTHETIC=1"
                    "$<TARGET_FILE:ChurchDAWPluginScanIntegrationTests>")
            set_tests_properties(
                ChurchDAWPluginScanSyntheticShell PROPERTIES
                TIMEOUT 120
                WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
        endif()
    endif()

    if(MSVC)
        target_compile_options(ChurchDAWMasterLimiterTests PRIVATE /utf-8)
        target_compile_options(ChurchDAWSabrinaTests PRIVATE /utf-8)
        target_compile_options(ChurchDAWMixerStripCollapseTests PRIVATE /utf-8)
        target_compile_options(ChurchDAWChannelToneTests PRIVATE /utf-8)
        target_compile_options(ChurchDAWNordSampleToneTests PRIVATE /utf-8)
        target_compile_options(ChurchDAWMidiMappingSafetyTests PRIVATE /utf-8)
        target_compile_options(ChurchDAWChannelRotaryTests PRIVATE /utf-8)
        target_compile_options(ChurchDAWPadProcessorTests PRIVATE /utf-8)
        target_compile_options(ChurchDAWPadSynthProcessorTests PRIVATE /utf-8)
        target_compile_options(ChurchDAWPadSynthRenderProbe PRIVATE /utf-8)
        target_compile_options(ChurchDAWLibraryPackPathTests PRIVATE /utf-8)
        target_compile_options(ChurchDAWTimbreDownloadTests PRIVATE /utf-8)
        target_compile_options(ChurchDAWLiveScenesTests PRIVATE /utf-8)
        target_compile_options(ChurchDAWSceneFileWriteProbe PRIVATE /utf-8)
        target_compile_options(ChurchDAWPluginScanPolicyTests PRIVATE /utf-8)
        target_compile_options(ChurchDAWChildProcessReapTests PRIVATE /utf-8)
        target_compile_options(ChurchDAWAndroidStageLayoutTests PRIVATE /utf-8)
        target_compile_options(ChurchDAWMobileBufferPolicyTests PRIVATE /utf-8)
        target_compile_options(ChurchDAWPluginScanIntegrationTests PRIVATE /utf-8)
        if(TARGET ChurchDAWVST3MultiClassCrashFixture)
            target_compile_options(
                ChurchDAWVST3MultiClassCrashFixture PRIVATE /utf-8)
        endif()
    endif()

    add_test(
        NAME ChurchDAWReverbPitch
        COMMAND ChurchDAWReverbPitchTests --all)
    add_test(
        NAME ChurchDAWMasterLimiter
        COMMAND ChurchDAWMasterLimiterTests)
    add_test(
        NAME ChurchDAWSabrina
        COMMAND ChurchDAWSabrinaTests)
    add_test(
        NAME ChurchDAWMixerStripCollapse
        COMMAND ChurchDAWMixerStripCollapseTests)
    add_test(NAME ChurchDAWMidiLearnOverlay
        COMMAND ChurchDAWMixerStripCollapseTests --midi-learn)
    add_test(NAME ChurchDAWTimbrePickerLayout
        COMMAND ChurchDAWMixerStripCollapseTests --timbre-picker)
    add_test(NAME ChurchDAWFullUpgrade
        COMMAND ChurchDAWMixerStripCollapseTests --full-upgrade)
    add_test(NAME ChurchDAWPluginPreviewLifecycle
        COMMAND ChurchDAWMixerStripCollapseTests --plugin-preview)
    add_test(NAME ChurchDAWPluginCandidateSearch
        COMMAND ChurchDAWMixerStripCollapseTests --plugin-candidate-search)
    set_tests_properties(ChurchDAWPluginCandidateSearch PROPERTIES TIMEOUT 15)
    add_test(NAME ChurchDAWPluginGalleryLayout
        COMMAND ChurchDAWMixerStripCollapseTests --plugin-gallery)
    add_test(
        NAME ChurchDAWChannelTone
        COMMAND ChurchDAWChannelToneTests)
    add_test(
        NAME ChurchDAWNordSampleTone
        COMMAND ChurchDAWNordSampleToneTests)
    add_test(
        NAME ChurchDAWMidiMappingSafety
        COMMAND ChurchDAWMidiMappingSafetyTests)
    add_test(
        NAME ChurchDAWChannelRotary
        COMMAND ChurchDAWChannelRotaryTests)
    add_test(
        NAME ChurchDAWPadProcessor
        COMMAND ChurchDAWPadProcessorTests)
    add_test(
        NAME ChurchDAWMobileBufferPolicy
        COMMAND ChurchDAWMobileBufferPolicyTests)
    add_test(
        NAME ChurchDAWPadSynthProcessor
        COMMAND ChurchDAWPadSynthProcessorTests)
    set(churchdaw_pad_synth_baseline
        "${CMAKE_CURRENT_SOURCE_DIR}/Tests/Audio/Baselines/padsynth-v1-48k.cdpcap")
    add_test(
        NAME ChurchDAWPadSynthRenderEquivalence
        COMMAND ChurchDAWPadSynthRenderProbe
            --self-test
            --repetitions 5
            --baseline "${churchdaw_pad_synth_baseline}")
    add_test(
        NAME ChurchDAWPadSynthCaptureInfrastructure
        COMMAND ChurchDAWPadSynthRenderProbe --infrastructure-self-test)
    add_test(
        NAME ChurchDAWPadSynthCaptureCompareCli
        COMMAND ChurchDAWPadSynthRenderProbe
            --compare
            "${churchdaw_pad_synth_baseline}"
            "${churchdaw_pad_synth_baseline}")
    add_test(
        NAME ChurchDAWLibraryPackPath
        COMMAND ChurchDAWLibraryPackPathTests)
    add_test(
        NAME ChurchDAWTimbreDownload
        COMMAND ChurchDAWTimbreDownloadTests)
    add_test(
        NAME ChurchDAWLiveScenes
        COMMAND ChurchDAWLiveScenesTests)
    add_test(
        NAME ChurchDAWSceneFileWrite
        COMMAND ChurchDAWSceneFileWriteProbe)
    add_test(
        NAME ChurchDAWPluginScanPolicy
        COMMAND ChurchDAWPluginScanPolicyTests)
    add_test(
        NAME ChurchDAWChildProcessReap
        COMMAND ChurchDAWChildProcessReapTests)
    add_test(
        NAME ChurchDAWAndroidStageLayout
        COMMAND ChurchDAWAndroidStageLayoutTests)
    set_tests_properties(
        ChurchDAWReverbPitch
        ChurchDAWMasterLimiter
        ChurchDAWChannelTone
        ChurchDAWNordSampleTone
        ChurchDAWMidiMappingSafety
        ChurchDAWChannelRotary
        ChurchDAWPadProcessor
        ChurchDAWPadSynthProcessor
        ChurchDAWPadSynthRenderEquivalence
        ChurchDAWPadSynthCaptureInfrastructure
        ChurchDAWPadSynthCaptureCompareCli
        ChurchDAWLibraryPackPath
        ChurchDAWPluginScanPolicy
        ChurchDAWChildProcessReap
        ChurchDAWAndroidStageLayout
        PROPERTIES TIMEOUT 240)
endif()

# As propriedades JUCE precisam ser passadas no momento de criar o alvo. Separe
# capacidades de macOS das de iOS para nao injetar entitlements desktop no
# bundle movel nem pedir microfone quando o app abre 0 canais de entrada.
if(IOS)
    # O controller do JUCE consulta prefersStatusBarHidden quando esta chave e
    # true, mas a implementacao padrao so esconde a barra em kiosk mode. Isso
    # fazia a barra do iPad reaparecer por cima da titlebar do app apesar de
    # STATUS_BAR_HIDDEN. Com o modo legado, UIStatusBarHidden e respeitado desde
    # o primeiro frame, sem transicao/flash durante a abertura.
    file(READ
        "${CMAKE_CURRENT_SOURCE_DIR}/Packaging/ChurchDAW-iOS-Info.plist"
        CHURCHDAW_IOS_PLIST_TO_MERGE)
    set(CHURCHDAW_PLATFORM_APP_OPTIONS
        # Precisa ser maior que o build 2026.8.16 ja enviado ao App Store
        # Connect. O formato por data facilita garantir a monotonicidade.
        BUILD_VERSION "2026.11.3"
        PLIST_TO_MERGE
            "${CHURCHDAW_IOS_PLIST_TO_MERGE}"
        CAMERA_PERMISSION_ENABLED TRUE
        CAMERA_PERMISSION_TEXT "O ChurchCam usa a camera somente quando voce a ativa para gravar seu video tocando."
        MICROPHONE_PERMISSION_ENABLED TRUE
        MICROPHONE_PERMISSION_TEXT "O ChurchCam usa o microfone somente quando voce ativa a gravacao da sua voz."
        TARGETED_DEVICE_FAMILY "1,2"
        STATUS_BAR_HIDDEN TRUE
        REQUIRES_FULL_SCREEN TRUE
        BACKGROUND_AUDIO_ENABLED TRUE
        # NAO usar PLUGINHOST_AU aqui: a opcao do JUCE injeta a entitlement
        # inter-app-audio (IAA, morta) e o perfil da App Store nao a inclui —
        # o exportArchive recusa. O host AUv3 ja e ligado pelas compile
        # definitions JUCE_PLUGINHOST_AU=1 do bloco if(IOS) mais abaixo.
        FILE_SHARING_ENABLED TRUE
        IPHONE_SCREEN_ORIENTATIONS
            UIInterfaceOrientationLandscapeLeft
            UIInterfaceOrientationLandscapeRight
        IPAD_SCREEN_ORIENTATIONS
            UIInterfaceOrientationLandscapeLeft
            UIInterfaceOrientationLandscapeRight)
else()
    set(CHURCHDAW_PLATFORM_APP_OPTIONS
        CAMERA_PERMISSION_ENABLED TRUE
        CAMERA_PERMISSION_TEXT "O ChurchDAW usa a webcam somente quando voce a liga no Studio para incluir seu rosto no video."
        MICROPHONE_PERMISSION_ENABLED TRUE
        MICROPHONE_PERMISSION_TEXT
            "O ChurchDAW usa a entrada de audio somente quando voce ativa guitarra/microfone em Audio/MIDI ou a gravacao no Studio.")

    if(APPLE)
        file(READ
            "${CMAKE_CURRENT_SOURCE_DIR}/Packaging/ChurchDAW-macOS-Info.plist"
            CHURCHDAW_MACOS_PLIST_TO_MERGE)
        list(APPEND CHURCHDAW_PLATFORM_APP_OPTIONS
            PLIST_TO_MERGE
                "${CHURCHDAW_MACOS_PLIST_TO_MERGE}"
            HARDENED_RUNTIME_ENABLED TRUE
            HARDENED_RUNTIME_OPTIONS
                "com.apple.security.device.audio-input"
                "com.apple.security.device.camera"
                "com.apple.security.cs.disable-library-validation")
    endif()
endif()

juce_add_gui_app(MinhaDAW
    PRODUCT_NAME "ChurchDAW"
    COMPANY_NAME "ChurchDAW"
    VERSION "${PROJECT_VERSION}"
    BUNDLE_ID "com.churchdaw.app"
    ${CHURCHDAW_PLATFORM_APP_OPTIONS}

    # SEM ESTA CHAVE O macOS MATA O APP.
    #
    # O ChurchDAW nao tem uma linha de codigo de Bluetooth. Mas o CoreMIDI
    # toca no Bluetooth EM NOME DO APP quando existe (ou aparece) um
    # dispositivo MIDI Bluetooth por perto, e o TCC nao pergunta quem pediu:
    # sem NSBluetoothAlwaysUsageDescription no Info.plist ele encerra o
    # processo na hora.
    #
    # Comprovado pelo relatorio de falha do cliente em 01/08/2026, ChurchDAW
    # 1.5.5, macOS 26.6:
    #   Termination Reason: Namespace TCC
    #   "...must contain an NSBluetoothAlwaysUsageDescription key"
    #   Thread 6: __TCC_CRASHING_DUE_TO_PRIVACY_VIOLATION__
    # Ele foi parear um pedal nas configuracoes de Bluetooth do macOS e o app
    # fechou sozinho. Nao era bug de MIDI nem de audio: era permissao.
    BLUETOOTH_PERMISSION_ENABLED TRUE
    BLUETOOTH_PERMISSION_TEXT "O ChurchDAW usa o Bluetooth apenas para receber notas de teclados e pedais MIDI sem fio."

    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/Assets/logo_round.png"
    ICON_SMALL "${CMAKE_CURRENT_SOURCE_DIR}/Assets/logo_round.png")

juce_add_binary_data(ChurchAssets SOURCES
    Assets/3D/concert_grand.mesh
    Assets/icon.png
    Assets/logo_round.png
    Assets/churchdaw_sidebar_texture.png
    Assets/glyph_piano.png
    Assets/glyph_pad.png
    Assets/glyph_organ.png
    Assets/glyph_strings.png
    Assets/glyph_ambience.png
    Assets/glyph_bells.png
    Assets/glyph_lead.png
    Assets/Premium/premium_grand_piano.png
        Assets/Premium/premium_electric_piano.png
        Assets/Premium/premium_tonewheel_organ.png
        Assets/Premium/premium_synth.png
        Assets/Premium/premium_strings.png
        Assets/Premium/premium_bells.png
    Assets/Premium/premium_graphite.png
    Assets/Premium/premium_bigsky.png
    Assets/Premium/premium_pianoteq.png
    # Barlow / Barlow Condensed / JetBrains Mono (SIL OFL 1.1, licencas em
    # Assets/Fonts): tipografia do shell Church Live, empacotada para nao
    # depender de fonte do sistema nem de rede.
    Assets/Fonts/Barlow-Regular.ttf
    Assets/Fonts/Barlow-SemiBold.ttf
    Assets/Fonts/Barlow-Bold.ttf
    Assets/Fonts/BarlowCondensed-Bold.ttf
    Assets/Fonts/BarlowCondensed-ExtraBold.ttf
    Assets/Fonts/BarlowCondensed-ExtraBoldItalic.ttf
    Assets/Fonts/JetBrainsMono-SemiBold.ttf)

if(IOS)
    # Archive relocates installable targets. This static resource library must
    # stay at the build path used by CMake's linker, outside the app archive.
    set_target_properties(ChurchAssets PROPERTIES
        XCODE_ATTRIBUTE_DEPLOYMENT_LOCATION NO
        XCODE_ATTRIBUTE_SKIP_INSTALL YES)
endif()

#
# Church Organ Engine: os quatro motores e a UI sao nativos e comuns a
# Windows, macOS e Android. A lista unica impede que o app e as sondas do
# TimbresProcessor acabem compilando inventarios diferentes.
#
set(CHURCHDAW_ORGAN_SOURCES
    Source/ChurchOrgan/AutoMixContext.cpp
    Source/ChurchOrgan/ChurchOrganCore.cpp
    Source/ChurchOrgan/ChurchOrganEditor.cpp
    Source/ChurchOrgan/ChurchOrganLayerProcessor.cpp
    Source/ChurchOrgan/ChurchOrganLayout.cpp
    Source/ChurchOrgan/HarmonicFoldback.cpp
    Source/ChurchOrgan/HarmonicPercussion.cpp
    Source/ChurchOrgan/KeyClickGenerator.cpp
    Source/ChurchOrgan/MultiContactKeying.cpp
    Source/ChurchOrgan/OrganAmpCab.cpp
    Source/ChurchOrgan/OrganParameters.cpp
    Source/ChurchOrgan/OrganPresetState.cpp
    Source/ChurchOrgan/PipeOrganEngine.cpp
    Source/ChurchOrgan/RotaryCabinet.cpp
    Source/ChurchOrgan/ScannerVibrato.cpp
    Source/ChurchOrgan/TonewheelBank.cpp
    Source/ChurchOrgan/TonewheelCharacter.cpp
    Source/ChurchOrgan/TonewheelOrganEngine.cpp
    Source/ChurchOrgan/TransistorDividerBank.cpp
    Source/ChurchOrgan/TransistorOrganEngine.cpp
    Source/ChurchOrgan/UI/ChurchOrganControls.cpp
    Source/ChurchOrgan/WorshipHybridEngine.cpp
    Source/ChurchOrgan/WorshipLayerEngine.cpp)

set(CHURCHDAW_SF2_SOURCES
    Source/SoundFontEngine.cpp)

set(CHURCHDAW_SABRINA_SOURCES
    Source/Plugins/Sabrina/SabrinaProcessor.cpp)

target_sources(MinhaDAW PRIVATE
    Source/Main.cpp
    Source/MainComponent.cpp
    ${CHURCHDAW_ORGAN_SOURCES}
    ${CHURCHDAW_SF2_SOURCES}
    ${CHURCHDAW_SABRINA_SOURCES})

if(IOS)
    # O catalogo usa a mesma policy RSA do Android/desktop. No iOS nao ha JNI
    # e o updater desktop e deliberadamente excluido, portanto um verificador
    # pequeno baseado no Security.framework fecha esta lacuna.
    target_sources(MinhaDAW PRIVATE
        Source/IOSPolicySignatureVerifier.cpp)
    target_compile_definitions(MinhaDAW PRIVATE
        JUCE_DISABLE_AUDIO_MIXING_WITH_OTHER_APPS=1)
    target_link_libraries(MinhaDAW PRIVATE
        "-framework Security"
        juce::juce_opengl)

    # Archives enviados para a App Store precisam carregar os simbolos de
    # diagnostico da mesma build otimizada. O gerador Xcode do CMake desativa
    # GCC_GENERATE_DEBUGGING_SYMBOLS em Release por padrao; sem este override o
    # archive funciona, mas nao inclui ChurchDAW.app.dSYM para simbolicar
    # eventuais crashes vistos pela Apple ou por clientes.
    set_target_properties(MinhaDAW PROPERTIES
        XCODE_ATTRIBUTE_GCC_GENERATE_DEBUGGING_SYMBOLS "YES"
        XCODE_ATTRIBUTE_DEBUG_INFORMATION_FORMAT "dwarf-with-dsym")

    # A sonda de interface fica desligada em todas as builds de entrega. Quando
    # habilitada num diretorio de QA separado, o Xcode executa o aplicativo num
    # aparelho fisico e preserva capturas + arvore de acessibilidade no xcresult.
    # Assim validamos layout e navegacao sem adicionar XCTest ao ChurchDAW.app.
    if(CHURCHDAW_BUILD_IOS_UI_TESTS)
        enable_testing()
        find_package(XCTest REQUIRED)
        set_target_properties(MinhaDAW PROPERTIES
            XCODE_GENERATE_SCHEME TRUE
            XCODE_SCHEME_TEST_CONFIGURATION "Debug")
        xctest_add_bundle(ChurchDAWUITests MinhaDAW
            Tests/IOSUI/ChurchDAWUITests.m)

        set_target_properties(ChurchDAWUITests PROPERTIES
            BUNDLE TRUE
            BUNDLE_EXTENSION "xctest"
            XCODE_PRODUCT_TYPE "com.apple.product-type.bundle.ui-testing"
            XCODE_GENERATE_SCHEME TRUE
            XCODE_ATTRIBUTE_CLANG_ENABLE_MODULES "YES"
            XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic"
            XCODE_ATTRIBUTE_CURRENT_PROJECT_VERSION "1"
            XCODE_ATTRIBUTE_GENERATE_INFOPLIST_FILE "YES"
            XCODE_ATTRIBUTE_MARKETING_VERSION "1.0"
            XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER
                "com.churchdaw.app.uitests"
            XCODE_ATTRIBUTE_BUNDLE_LOADER ""
            XCODE_ATTRIBUTE_TEST_HOST ""
            XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
            XCODE_ATTRIBUTE_TEST_TARGET_NAME "MinhaDAW")

        target_link_libraries(ChurchDAWUITests PRIVATE
            "-framework XCTest"
            "-framework XCUIAutomation")
        add_dependencies(ChurchDAWUITests MinhaDAW)
        xctest_add_test(ChurchDAWPhysicalUI ChurchDAWUITests)
    endif()
endif()

if(CHURCHDAW_ENABLE_DESKTOP_LICENSING AND NOT ANDROID AND NOT IOS)
    target_sources(MinhaDAW PRIVATE
        Source/DesktopLicenseClient.cpp
        Source/DesktopLoginPanel.cpp
        Source/DesktopLeaseVerifier.cpp)

    if(WIN32)
        target_sources(MinhaDAW PRIVATE
            Source/DesktopCredentialStore_windows.cpp
            Source/DesktopLeaseSignature_windows.cpp
            Source/DesktopSecureRandom_windows.cpp)
        target_link_libraries(MinhaDAW PRIVATE crypt32 bcrypt)
    elseif(APPLE)
        target_sources(MinhaDAW PRIVATE
            Source/DesktopCredentialStore_mac.mm
            Source/DesktopLeaseSignature_mac.mm
            Source/DesktopSecureRandom_mac.mm)
        target_link_libraries(MinhaDAW PRIVATE "-framework Security")
    else()
        message(FATAL_ERROR
            "O licenciamento desktop suporta somente Windows e macOS")
    endif()
endif()

# Atualizacao automatica e OBRIGATORIA no desktop -- ver Source/DesktopUpdater.h.
#
# Fica de proposito FORA do bloco de licenciamento: a atualizacao obrigatoria
# nao pode depender de uma opcao de build. Um binario compilado sem
# CHURCHDAW_ENABLE_DESKTOP_LICENSING ainda precisa obedecer a policy assinada.
if(NOT ANDROID AND NOT IOS)
    target_sources(MinhaDAW PRIVATE Source/DesktopUpdater.cpp)

    # Numero de versao comparavel: 1.2.0 -> 10200. Derivado do project() acima
    # para nunca divergir da versao real do binario.
    math(EXPR CHURCHDAW_VERSION_CODE
         "${PROJECT_VERSION_MAJOR}*10000 + ${PROJECT_VERSION_MINOR}*100 + ${PROJECT_VERSION_PATCH}")

    # PISO HISTORICO. A 1.6.3 foi publicada em 05/08/2026 com version code 10610,
    # fora da formula acima (que daria 10603). O updater compara CODIGOS, nao
    # nomes: sem um piso a 1.6.4 sairia com 10604 -- MENOR que a 1.6.3 no ar --
    # e quem ja atualizou nunca receberia a correcao, ou receberia um downgrade.
    #
    # O piso sobe a cada release enquanto a formula nao alcanca a numeracao
    # publicada. Historico ate agora:
    #   1.6.3 (Windows)          -> 10610  publicada fora da formula
    #   1.6.4 (Windows)          -> 10611  piso
    #   1.6.5 (Windows + macOS)  -> 10612  piso; mesmo fonte da 1.6.4, mas uma
    #                                      politica normal precisa AUMENTAR o
    #                                      codigo e nao ha modo "so acrescentar
    #                                      plataforma"
    # Some ate a formula ultrapassar naturalmente o piso (1.7.0 -> 10700).
    #   1.6.6 (Windows + macOS)  -> 10613  piso; a formula daria 10606
    #   1.6.7 (Windows + macOS)  -> 10614  piso; a formula daria 10607
    #   1.6.8 (Windows + macOS)  -> 10615  piso; a formula daria 10608
    #   1.6.9 (Windows + macOS)  -> 10616  piso; a formula daria 10609
    #   1.6.10 (Windows + macOS) -> 10617  piso; a formula daria 10610
    #   1.6.11 (Windows + macOS) -> 10618  piso; a formula daria 10611
    #   1.6.12 (Windows + macOS) -> 10619  piso; a formula daria 10612
	    #   1.6.13 (Windows + macOS) -> 10620  piso; a formula daria 10613
	    #   1.6.14 (Windows + macOS) -> 10621  piso; a formula daria 10614
	    #   1.6.15 (Windows + macOS) -> 10622  piso; a formula daria 10615
	    #   1.6.16 (Windows + macOS) -> 10623  piso
	    #   1.6.17 (Windows + macOS) -> 10624  piso
	    #   1.6.18 (Windows + macOS) -> 10625  piso; a formula daria 10618
	    #   1.6.24 (Windows + macOS) -> 10631  piso; release retirada por gate
	    #                                      de licenca desativado no cache
	    #   1.6.25 (Windows + macOS) -> 10632  correcao obrigatoria do gate
	    #   1.6.26 (Windows + macOS) -> 10633  desempenho e UI mobile
	    #   1.6.27 (Windows + macOS) -> 10634  hardening de mapeamento MIDI
	    #   1.6.28 (Windows + macOS) -> 10635  correcao ASIO ausente na 1.6.27
	    #                                      + canal MIDI/split por camada
	    #   1.6.29 (Windows + macOS) -> 10636  guard de crash-recovery:
	    #                                      app em crash-loop no boot
	    #                                      (ASIO podre restaurado)
 	    #   1.6.30 (Windows + macOS) -> 10637  hotfix: guard two-strikes (strike 1 preserva ASIO4ALL)
	    #                                      volta a abrir em MODO SEGURO
	    #   1.6.31 (Windows + macOS) -> 10638  MIDI, visual, motor e update notarizado
	    #   1.6.34 (Windows + macOS) -> 10641  ChurchCam com gravacao MP4 no Windows
	    #   1.6.35 (Windows + macOS) -> 10642  DSP RT-safe, polimento visual e modo touch iOS/Android
	    set(CHURCHDAW_VERSION_CODE_FLOOR 10642)
    if(CHURCHDAW_VERSION_CODE LESS ${CHURCHDAW_VERSION_CODE_FLOOR})
        set(CHURCHDAW_VERSION_CODE ${CHURCHDAW_VERSION_CODE_FLOOR})
        message(STATUS "ChurchDAW: version code elevado ao piso historico "
                       "${CHURCHDAW_VERSION_CODE_FLOOR} "
                       "(a formula daria menos que o ja publicado)")
    endif()
    target_compile_definitions(MinhaDAW PRIVATE
        CHURCHDAW_VERSION_CODE=${CHURCHDAW_VERSION_CODE}
        CHURCHDAW_UPDATE_POLICY_URL="${CHURCHDAW_UPDATE_POLICY_URL}"
        CHURCHDAW_UPDATE_ALLOW_HTTP_LOCALHOST=$<BOOL:${CHURCHDAW_UPDATE_ALLOW_LOCALHOST}>
        CHURCHDAW_UPDATE_REQUIRE_OS_SIGNATURE=$<BOOL:${CHURCHDAW_UPDATE_REQUIRE_OS_SIGNATURE}>)
    message(STATUS "ChurchDAW version code: ${CHURCHDAW_VERSION_CODE}")
    message(STATUS "ChurchDAW update policy: ${CHURCHDAW_UPDATE_POLICY_URL}")

    if(CHURCHDAW_UPDATE_ALLOW_LOCALHOST)
        message(WARNING
            "CHURCHDAW_UPDATE_ALLOW_LOCALHOST esta LIGADO: este binario aceita "
            "politica de atualizacao vinda de 127.0.0.1. Nunca distribuir.")
    endif()

    if(NOT CHURCHDAW_UPDATE_REQUIRE_OS_SIGNATURE)
        message(WARNING
            "CHURCHDAW_UPDATE_REQUIRE_OS_SIGNATURE esta DESLIGADO: permitido "
            "somente em desenvolvimento/teste; release comercial deve exigir "
            "a assinatura confiavel do sistema.")
    endif()

    if(WIN32)
        # wintrust = Authenticode (WinVerifyTrust); crypt32/bcrypt = RS256 da policy
        target_link_libraries(MinhaDAW PRIVATE crypt32 bcrypt wintrust)
    elseif(APPLE)
        target_link_libraries(MinhaDAW PRIVATE "-framework Security")
    endif()
endif()

# Hospedar plugins: no COMPUTADOR sao VST3/AU (Kontakt e afins); no iOS sao
# os AUv3 da App Store (bs-16i, Ravenscroft...) — pedido dos clientes de
# iPad/iPhone (ago/2026). So o Android fica sem: la nao existe formato de
# plugin carregavel.
#
# O codigo do host NAO e apagado onde nao ha plugin: fica atras de
# CHURCHDAW_HOSTS_PLUGINS, para haver UM codigo-fonte so.
if(ANDROID)
    set(CHURCHDAW_PLUGINS 0)
else()
    set(CHURCHDAW_PLUGINS 1)
endif()

# VST3 e formato de desktop; no iOS o unico formato real e o AUv3.
if(IOS)
    set(CHURCHDAW_VST3_HOSTING 0)
else()
    set(CHURCHDAW_VST3_HOSTING ${CHURCHDAW_PLUGINS})
endif()

set(CHURCHDAW_VST2_HOSTING 0)
if(CHURCHDAW_ENABLE_VST2_HOSTING)
    if(NOT WIN32)
        message(FATAL_ERROR
            "CHURCHDAW_ENABLE_VST2_HOSTING aceita somente builds Windows; "
            "o pedido do produto e para plugins VST2 .dll.")
    endif()

    if(NOT CHURCHDAW_VST2_SDK_PATH
       AND DEFINED ENV{CHURCHDAW_VST2_SDK_PATH})
        file(TO_CMAKE_PATH "$ENV{CHURCHDAW_VST2_SDK_PATH}"
             CHURCHDAW_VST2_SDK_PATH)
    endif()

    set(CHURCHDAW_VST2_AEFFECT
        "${CHURCHDAW_VST2_SDK_PATH}/pluginterfaces/vst2.x/aeffect.h")
    set(CHURCHDAW_VST2_AEFFECTX
        "${CHURCHDAW_VST2_SDK_PATH}/pluginterfaces/vst2.x/aeffectx.h")

    if(NOT EXISTS "${CHURCHDAW_VST2_AEFFECT}"
       OR NOT EXISTS "${CHURCHDAW_VST2_AEFFECTX}")
        message(FATAL_ERROR
            "VST2 solicitado, mas os headers licenciados nao foram encontrados. "
            "Defina CHURCHDAW_VST2_SDK_PATH para a pasta que contem "
            "pluginterfaces/vst2.x/aeffect.h e aeffectx.h. O ChurchDAW nao "
            "distribui nem baixa o SDK legado automaticamente.")
    endif()

    set(CHURCHDAW_VST2_HOSTING 1)
    target_include_directories(MinhaDAW PRIVATE
        "${CHURCHDAW_VST2_SDK_PATH}")
    message(STATUS
        "ChurchDAW: hospedagem VST2 .dll habilitada com SDK externo licenciado")
endif()

target_compile_definitions(MinhaDAW PRIVATE
    CHURCHDAW_HOSTS_PLUGINS=${CHURCHDAW_PLUGINS}
    CHURCHDAW_DESKTOP_LICENSING=$<BOOL:${CHURCHDAW_ENABLE_DESKTOP_LICENSING}>
    CHURCHDAW_APP_VERSION="${PROJECT_VERSION}"
    CHURCHDAW_LICENSE_ALLOW_HTTP_LOCALHOST=$<BOOL:${CHURCHDAW_LICENSE_ALLOW_HTTP_LOCALHOST}>
    CHURCHDAW_LICENSE_API_BASE="${CHURCHDAW_LICENSE_API_BASE}"
    JUCE_PLUGINHOST_VST=${CHURCHDAW_VST2_HOSTING}
    JUCE_PLUGINHOST_VST3=${CHURCHDAW_VST3_HOSTING}
    JUCE_WEB_BROWSER=0
    JUCE_PUSH_NOTIFICATIONS=0
    JUCE_USE_CURL=0)

# iOS: hospedagem de AUv3 (extensoes instaladas da App Store). O scan e
# in-process — cada AUv3 ja roda em processo proprio por desenho da Apple,
# entao um plugin quebrado nao derruba o app (o isolamento que o desktop
# obtem com o subprocesso vem de graca aqui).
if(IOS)
    # ATENCAO (aprendido em campo, 18/ago/2026): NAO exigir PLUGINHOST_AU nas
    # app options. A opcao faz o JUCE injetar a entitlement inter-app-audio
    # (IAA) — tecnologia MORTA que o perfil da App Store nao inclui, e o
    # exportArchive RECUSA o binario. AUv3 de terceiros NAO precisa dela:
    # validado no iPad do dono (bs-16i enumerado e carregado) com o app
    # assinado apenas com as entitlements basicas. O formato AU e compilado
    # pelas definitions JUCE_PLUGINHOST_AU=1 abaixo, e a enumeracao usa o
    # AVAudioUnitComponentManager (patch no JUCE headless vendorado).
    target_compile_definitions(MinhaDAW PRIVATE
        JUCE_PLUGINHOST_AU=1)
    target_link_libraries(MinhaDAW PRIVATE
        "-framework AudioToolbox"
        "-framework CoreAudioKit"
        "-framework AVFoundation")
endif()

# macOS: o Kontakt e a maioria dos plugins existem tambem como Audio Unit, que e
# o formato nativo do sistema. Sem isto o usuario de Mac so enxergaria os VST3.
if(APPLE AND NOT IOS)
    target_compile_definitions(MinhaDAW PRIVATE
        JUCE_PLUGINHOST_AU=1)

    # Catalina e o contrato do slice Intel. Qualquer API mais nova usada sem
    # @available deve quebrar a compilacao, nao somente falhar no Mac do cliente.
    target_compile_options(MinhaDAW PRIVATE
        -Werror=unguarded-availability-new)

    set_target_properties(MinhaDAW PROPERTIES
        XCODE_ATTRIBUTE_CLANG_CXX_LIBRARY "libc++"
        OSX_ARCHITECTURES "arm64;x86_64")   # Apple Silicon + Intel no mesmo binario

    target_compile_definitions(MinhaDAW PRIVATE
        JUCE_USE_VDSP_FRAMEWORK=1)

    target_link_libraries(MinhaDAW PRIVATE
        "-framework AudioUnit"
        "-framework AudioToolbox"
        "-framework CoreAudioKit"
        "-framework Accelerate")
endif()

# Habilita ASIO se o SDK da Steinberg estiver na pasta asiosdk/
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/asiosdk/common/iasiodrv.h")
    message(STATUS "ASIO SDK encontrado - habilitando suporte ASIO")
    target_compile_definitions(MinhaDAW PRIVATE JUCE_ASIO=1)
    target_include_directories(MinhaDAW PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/asiosdk/common")
elseif(WIN32 AND NOT CHURCHDAW_ALLOW_NO_ASIO)
    # A 1.6.27 foi publicada SEM ASIO porque a arvore de release (clone do
    # bundle git) nao continha asiosdk/ (gitignored) e este if silenciava a
    # ausencia — os clientes com ASIO4ALL perderam o driver no auto-update.
    # Release Windows sem ASIO nunca mais sai calado: falhe o configure e
    # deixe explicito de onde copiar o SDK.
    message(FATAL_ERROR
        "ASIO SDK ausente em ${CMAKE_CURRENT_SOURCE_DIR}/asiosdk/. Build "
        "Windows sem ASIO quebra ASIO4ALL nos clientes. Copie a pasta "
        "asiosdk/ (Steinberg) para a raiz da arvore antes de compilar; "
        "para um build de teste sem ASIO, defina -DCHURCHDAW_ALLOW_NO_ASIO=ON.")
endif()
if(WIN32 AND CHURCHDAW_ALLOW_NO_ASIO)
    message(WARNING "Build Windows SEM ASIO autorizado explicitamente (teste).")
endif()

if(MSVC)
    target_compile_options(MinhaDAW PRIVATE /utf-8)
    # Os quatro motores do Church Organ são preparados fora do callback, mas
    # a construção das tabelas Pipe/Hybrid usa uma frame temporária maior que
    # o reserve padrão de 1 MiB do PE. Reserva virtual maior (commit continua
    # sob demanda) evita 0xC00000FD no Windows sem tocar no caminho RT.
    target_link_options(MinhaDAW PRIVATE /STACK:33554432)
endif()

target_link_libraries(MinhaDAW
    PRIVATE
        juce::juce_opengl
        ChurchAssets
        juce::juce_cryptography
        juce::juce_dsp
        juce::juce_audio_utils
        juce::juce_audio_processors
        juce::juce_audio_devices
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags)

# Sonda MANUAL de troca de timbres com biblioteca real. Nao entra no CTest
# porque precisa receber a pasta/prefixo de um piano instalado. Ela compila o
# mesmo TimbresProcessor, mas nunca gera nem substitui o executavel ChurchDAW.
if(CHURCHDAW_BUILD_AUDIO_TESTS AND NOT ANDROID AND NOT IOS)
    juce_add_console_app(ChurchDAWTimbreLoadStressTests
        PRODUCT_NAME "ChurchDAWTimbreLoadStressTests")

    target_sources(ChurchDAWTimbreLoadStressTests PRIVATE
        Tests/Audio/TimbreLoadStressTests.cpp
        ${CHURCHDAW_ORGAN_SOURCES}
        ${CHURCHDAW_SF2_SOURCES})

    target_include_directories(ChurchDAWTimbreLoadStressTests PRIVATE Source)
    target_compile_definitions(ChurchDAWTimbreLoadStressTests PRIVATE
        CHURCHDAW_HOSTS_PLUGINS=0
        CHURCHDAW_STREAM_SAMPLES=1
        CHURCHDAW_INTERNAL_CATALOG_TEST_HOOKS=1
        JUCE_PLUGINHOST_VST3=0
        JUCE_MODAL_LOOPS_PERMITTED=1
        JUCE_WEB_BROWSER=0
        JUCE_PUSH_NOTIFICATIONS=0
        JUCE_USE_CURL=0)

    target_link_libraries(ChurchDAWTimbreLoadStressTests
        PRIVATE
            ChurchAssets
            juce::juce_cryptography
            juce::juce_dsp
            juce::juce_audio_utils
            juce::juce_audio_processors
            juce::juce_audio_devices
            juce::juce_gui_extra
        PUBLIC
            juce::juce_recommended_config_flags)

    if(WIN32)
        target_link_libraries(ChurchDAWTimbreLoadStressTests PRIVATE psapi)
    endif()

    if(MSVC)
        target_compile_options(ChurchDAWTimbreLoadStressTests PRIVATE /utf-8 /bigobj)
        target_link_options(ChurchDAWTimbreLoadStressTests
            PRIVATE /STACK:33554432)
    endif()

    add_test(
        NAME ChurchDAWChannelControlState
        COMMAND ChurchDAWTimbreLoadStressTests --controls-only)
    set_tests_properties(
        ChurchDAWChannelControlState
        PROPERTIES TIMEOUT 60)

    # Integração MIDI real do ramo de instrumento hospedado. O capturador deriva
    # de AudioPluginInstance, então asserta os bytes que Kontakt/AU/VST recebem
    # depois de sustain global, CC121, all-off, PANIC e ledger de transposição.
    juce_add_console_app(ChurchDAWHostedInstrumentMidiRoutingTests
        PRODUCT_NAME "ChurchDAWHostedInstrumentMidiRoutingTests")

    target_sources(ChurchDAWHostedInstrumentMidiRoutingTests PRIVATE
        Tests/Audio/HostedInstrumentMidiRoutingTests.cpp
        ${CHURCHDAW_ORGAN_SOURCES}
        ${CHURCHDAW_SF2_SOURCES})

    target_include_directories(
        ChurchDAWHostedInstrumentMidiRoutingTests PRIVATE Source)
    target_compile_definitions(
        ChurchDAWHostedInstrumentMidiRoutingTests PRIVATE
        CHURCHDAW_HOSTS_PLUGINS=0
        CHURCHDAW_STREAM_SAMPLES=1
        CHURCHDAW_INTERNAL_AUDIO_TEST_HOOKS=1
        JUCE_PLUGINHOST_VST3=0
        JUCE_MODAL_LOOPS_PERMITTED=1
        JUCE_WEB_BROWSER=0
        JUCE_PUSH_NOTIFICATIONS=0
        JUCE_USE_CURL=0)

    target_link_libraries(
        ChurchDAWHostedInstrumentMidiRoutingTests
        PRIVATE
            ChurchAssets
            juce::juce_cryptography
            juce::juce_dsp
            juce::juce_audio_utils
            juce::juce_audio_processors
            juce::juce_audio_devices
            juce::juce_gui_extra
        PUBLIC
            juce::juce_recommended_config_flags)

    if(MSVC)
        target_compile_options(
            ChurchDAWHostedInstrumentMidiRoutingTests
            PRIVATE /utf-8 /bigobj)
        target_link_options(
            ChurchDAWHostedInstrumentMidiRoutingTests
            PRIVATE /STACK:33554432)
    endif()

    add_test(
        NAME ChurchDAWHostedInstrumentMidiRouting
        COMMAND ChurchDAWHostedInstrumentMidiRoutingTests)
    set_tests_properties(
        ChurchDAWHostedInstrumentMidiRouting
        PROPERTIES TIMEOUT 120)

    # Sonda MANUAL do roteamento do "plugin no master" (MASTER FX do Church
    # Live). Instala um insert real derivado de AudioPluginInstance e mede
    # quanto do mix dos canais chega ate ele. NAO entra em add_test: depende da
    # biblioteca de timbres instalada na maquina, como a TimbreRenderProbe.
    juce_add_console_app(ChurchDAWMasterInsertRoutingProbe
        PRODUCT_NAME "ChurchDAWMasterInsertRoutingProbe")

    target_sources(ChurchDAWMasterInsertRoutingProbe PRIVATE
        Tests/Audio/MasterInsertRoutingProbe.cpp
        ${CHURCHDAW_ORGAN_SOURCES}
        ${CHURCHDAW_SF2_SOURCES})

    target_include_directories(
        ChurchDAWMasterInsertRoutingProbe PRIVATE Source)
    target_compile_definitions(
        ChurchDAWMasterInsertRoutingProbe PRIVATE
        CHURCHDAW_HOSTS_PLUGINS=0
        CHURCHDAW_STREAM_SAMPLES=1
        CHURCHDAW_INTERNAL_AUDIO_TEST_HOOKS=1
        JUCE_PLUGINHOST_VST3=0
        JUCE_MODAL_LOOPS_PERMITTED=1
        JUCE_WEB_BROWSER=0
        JUCE_PUSH_NOTIFICATIONS=0
        JUCE_USE_CURL=0)

    target_link_libraries(
        ChurchDAWMasterInsertRoutingProbe
        PRIVATE
            ChurchAssets
            juce::juce_cryptography
            juce::juce_dsp
            juce::juce_audio_utils
            juce::juce_audio_processors
            juce::juce_audio_devices
            juce::juce_gui_extra
        PUBLIC
            juce::juce_recommended_config_flags)

    if(MSVC)
        target_compile_options(
            ChurchDAWMasterInsertRoutingProbe
            PRIVATE /utf-8 /bigobj)
        target_link_options(
            ChurchDAWMasterInsertRoutingProbe
            PRIVATE /STACK:33554432)
    endif()

    # Sonda ADVERSARIAL do mesmo roteamento: ataca o que a sonda acima nao
    # cobre (vazamento de rota entre projetos, bloco 0, troca de sample rate,
    # plugin que falha ao preparar, plugin removido tocando). E autocontida e
    # por isso integra o gate automatizado de regressao.
    juce_add_console_app(ChurchDAWMasterRouteAdversarialProbe
        PRODUCT_NAME "ChurchDAWMasterRouteAdversarialProbe")

    target_sources(ChurchDAWMasterRouteAdversarialProbe PRIVATE
        Tests/Audio/MasterRouteAdversarialProbe.cpp
        ${CHURCHDAW_ORGAN_SOURCES}
        ${CHURCHDAW_SF2_SOURCES})

    target_include_directories(
        ChurchDAWMasterRouteAdversarialProbe PRIVATE Source)
    target_compile_definitions(
        ChurchDAWMasterRouteAdversarialProbe PRIVATE
        CHURCHDAW_HOSTS_PLUGINS=0
        CHURCHDAW_STREAM_SAMPLES=1
        CHURCHDAW_INTERNAL_AUDIO_TEST_HOOKS=1
        JUCE_PLUGINHOST_VST3=0
        JUCE_MODAL_LOOPS_PERMITTED=1
        JUCE_WEB_BROWSER=0
        JUCE_PUSH_NOTIFICATIONS=0
        JUCE_USE_CURL=0)

    target_link_libraries(
        ChurchDAWMasterRouteAdversarialProbe
        PRIVATE
            ChurchAssets
            juce::juce_cryptography
            juce::juce_dsp
            juce::juce_audio_utils
            juce::juce_audio_processors
            juce::juce_audio_devices
            juce::juce_gui_extra
        PUBLIC
            juce::juce_recommended_config_flags)

    if(MSVC)
        target_compile_options(
            ChurchDAWMasterRouteAdversarialProbe
            PRIVATE /utf-8 /bigobj)
        target_link_options(
            ChurchDAWMasterRouteAdversarialProbe
            PRIVATE /STACK:33554432)
    endif()

    add_test(
        NAME ChurchDAWMasterRouteAdversarial
        COMMAND ChurchDAWMasterRouteAdversarialProbe)
    set_tests_properties(
        ChurchDAWMasterRouteAdversarial
        PROPERTIES TIMEOUT 120)
endif()

if(CHURCHDAW_BUILD_LICENSE_TESTS AND NOT ANDROID AND NOT IOS)
    add_executable(ChurchDAWLicenseVerifierTests
        Tests/DesktopLicensing/LeaseVerifierTests.cpp
        Source/DesktopLeaseVerifier.cpp)
    target_include_directories(ChurchDAWLicenseVerifierTests PRIVATE Source)
    target_link_libraries(ChurchDAWLicenseVerifierTests PRIVATE
        juce::juce_cryptography
        juce::juce_core)
    target_compile_definitions(ChurchDAWLicenseVerifierTests PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0)

    if(WIN32)
        target_sources(ChurchDAWLicenseVerifierTests PRIVATE
            Source/DesktopLeaseSignature_windows.cpp
            Source/DesktopSecureRandom_windows.cpp)
        target_link_libraries(ChurchDAWLicenseVerifierTests PRIVATE crypt32 bcrypt)
    elseif(APPLE)
        target_sources(ChurchDAWLicenseVerifierTests PRIVATE
            Source/DesktopLeaseSignature_mac.mm
            Source/DesktopSecureRandom_mac.mm)
        target_link_libraries(ChurchDAWLicenseVerifierTests PRIVATE "-framework Security")
    endif()

    add_test(
        NAME ChurchDAWLicenseVerifier
        COMMAND ChurchDAWLicenseVerifierTests
            "${CMAKE_SOURCE_DIR}/Tests/DesktopLicensing/lease-fixture-ci.token"
            "fixture-installation-id"
            "1800000000")
    set_tests_properties(
        ChurchDAWLicenseVerifier
        PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endif()

# Sonda do "ZIP nao pode ser liberado" -- ver Tests/Library/ZipDeleteProbe.cpp.
# Prova que apagar um .zip com juce::ZipFile aberto falha no Windows e passa no
# POSIX -- a razao de o erro aparecer so no Windows, uma vez por parte.
if(CHURCHDAW_BUILD_AUDIO_TESTS AND NOT ANDROID AND NOT IOS)
    juce_add_console_app(ChurchDAWZipDeleteProbe
        PRODUCT_NAME "ChurchDAWZipDeleteProbe")

    target_sources(ChurchDAWZipDeleteProbe PRIVATE
        Tests/Library/ZipDeleteProbe.cpp)

    target_compile_definitions(ChurchDAWZipDeleteProbe PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_PUSH_NOTIFICATIONS=0
        JUCE_USE_CURL=0)

    target_link_libraries(ChurchDAWZipDeleteProbe
        PRIVATE
            juce::juce_core
        PUBLIC
            juce::juce_recommended_config_flags)

    if(MSVC)
        target_compile_options(ChurchDAWZipDeleteProbe PRIVATE /utf-8)
    endif()

    add_test(
        NAME ChurchDAWZipDelete
        COMMAND ChurchDAWZipDeleteProbe)
    set_tests_properties(
        ChurchDAWZipDelete
        PROPERTIES TIMEOUT 30)
endif()

# Regressao de limites do reverb -- ver Tests/Audio/ReverbBoundsProbe.cpp.
# Debug usa a checagem de indice do STL; RelWithDebInfo usa AddressSanitizer.
if(CHURCHDAW_BUILD_AUDIO_TESTS AND NOT ANDROID AND NOT IOS)
    juce_add_console_app(ChurchDAWReverbBoundsProbe
        PRODUCT_NAME "ChurchDAWReverbBoundsProbe")

    target_sources(ChurchDAWReverbBoundsProbe PRIVATE
        Tests/Audio/ReverbBoundsProbe.cpp)

    target_include_directories(ChurchDAWReverbBoundsProbe PRIVATE Source)
    target_compile_definitions(ChurchDAWReverbBoundsProbe PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_PUSH_NOTIFICATIONS=0
        JUCE_USE_CURL=0)

    target_link_libraries(ChurchDAWReverbBoundsProbe
        PRIVATE
            juce::juce_dsp
            juce::juce_audio_basics
        PUBLIC
            juce::juce_recommended_config_flags)

    if(MSVC)
        target_compile_options(ChurchDAWReverbBoundsProbe PRIVATE /utf-8)

        # Em RelWithDebInfo a sonda roda sob AddressSanitizer: ele diz o
        # ENDERECO e o tamanho da regiao violada, que e o que distingue um
        # estouro de um indice selvagem. O Debug do MSVC nao aceita ASan
        # junto do /RTC1, por isso a separacao por configuracao.
        target_compile_options(ChurchDAWReverbBoundsProbe PRIVATE
            $<$<CONFIG:RelWithDebInfo>:/fsanitize=address>)
        target_link_options(ChurchDAWReverbBoundsProbe PRIVATE
            $<$<CONFIG:RelWithDebInfo>:/INCREMENTAL:NO>)
    endif()

    add_test(
        NAME ChurchDAWReverbBounds
        COMMAND ChurchDAWReverbBoundsProbe)
    set_tests_properties(
        ChurchDAWReverbBounds
        PROPERTIES TIMEOUT 180)
endif()

# Sonda MANUAL de render offline do motor de samples -- ver
# Tests/Audio/TimbreRenderProbe.cpp. Ela instancia o MESMO TimbresProcessor do
# produto, carrega um timbre real da biblioteca instalada, dispara uma nota e
# grava o resultado em WAV, sem interface e sem placa de som.
#
# NAO entra em nenhum add_test: depende da biblioteca da maquina e serve para
# diagnostico manual, como a ChurchDAWTimbreLoadStressTests. Ela compila o
# mesmo motor, mas nunca gera nem substitui o executavel ChurchDAW.
if(CHURCHDAW_BUILD_AUDIO_TESTS AND NOT ANDROID AND NOT IOS)
    juce_add_console_app(ChurchDAWTimbreRenderProbe
        PRODUCT_NAME "ChurchDAWTimbreRenderProbe")

    target_sources(ChurchDAWTimbreRenderProbe PRIVATE
        Tests/Audio/TimbreRenderProbe.cpp
        ${CHURCHDAW_ORGAN_SOURCES}
        ${CHURCHDAW_SF2_SOURCES})

    target_include_directories(ChurchDAWTimbreRenderProbe PRIVATE Source)

    # As mesmas definicoes da ChurchDAWTimbreLoadStressTests: sem host de
    # plugin (a sonda nao carrega VST) e COM streaming de samples, que e o
    # padrao do produto (TimbresProcessor.h:58).
    target_compile_definitions(ChurchDAWTimbreRenderProbe PRIVATE
        CHURCHDAW_HOSTS_PLUGINS=0
        CHURCHDAW_STREAM_SAMPLES=1
        JUCE_PLUGINHOST_VST3=0
        JUCE_MODAL_LOOPS_PERMITTED=1
        JUCE_WEB_BROWSER=0
        JUCE_PUSH_NOTIFICATIONS=0
        JUCE_USE_CURL=0)

    target_link_libraries(ChurchDAWTimbreRenderProbe
        PRIVATE
            ChurchAssets
            juce::juce_cryptography
            juce::juce_dsp
            juce::juce_audio_utils
            juce::juce_audio_processors
            juce::juce_audio_devices
            juce::juce_gui_extra
        PUBLIC
            juce::juce_recommended_config_flags)

    if(MSVC)
        target_compile_options(ChurchDAWTimbreRenderProbe PRIVATE /utf-8 /bigobj)
        target_link_options(ChurchDAWTimbreRenderProbe
            PRIVATE /STACK:33554432)
    endif()
endif()

# Sonda MANUAL da EMENDA DO PAD CONTINUO -- ver Tests/Audio/PadLoopProbe.cpp.
#
# A ChurchDAWTimbreRenderProbe nao serve aqui: ela renderiza o TimbresProcessor,
# e o Pad Continuo e outro no do grafo, com laco proprio. Esta sonda instancia o
# MESMO PadProcessor do produto e renderiza os ~5 min ate a volta do laco, para
# medir clique e buraco na emenda com o codigo que vai para o palco.
#
# Igual a sonda de timbres: NAO entra em add_test (depende da biblioteca de pads
# instalada na maquina) e nunca gera nem substitui o executavel ChurchDAW.
if(CHURCHDAW_BUILD_AUDIO_TESTS AND NOT ANDROID AND NOT IOS)
    juce_add_console_app(ChurchDAWPadLoopProbe
        PRODUCT_NAME "ChurchDAWPadLoopProbe")

    # O PadProcessor e header-only e so depende do AppPaths.h: nao precisa dos
    # fontes do orgao, ao contrario da sonda de timbres.
    target_sources(ChurchDAWPadLoopProbe PRIVATE
        Tests/Audio/PadLoopProbe.cpp)

    target_include_directories(ChurchDAWPadLoopProbe PRIVATE Source)

    target_compile_definitions(ChurchDAWPadLoopProbe PRIVATE
        CHURCHDAW_HOSTS_PLUGINS=0
        JUCE_PLUGINHOST_VST3=0
        JUCE_MODAL_LOOPS_PERMITTED=1
        JUCE_WEB_BROWSER=0
        JUCE_PUSH_NOTIFICATIONS=0
        JUCE_USE_CURL=0)

    target_link_libraries(ChurchDAWPadLoopProbe
        PRIVATE
            juce::juce_audio_utils
            juce::juce_audio_processors
            juce::juce_audio_formats
        PUBLIC
            juce::juce_recommended_config_flags)

    if(MSVC)
        target_compile_options(ChurchDAWPadLoopProbe PRIVATE /utf-8 /bigobj)
    endif()
endif()

# Instalador do Windows -- ver Installer/ChurchDAWSetup.cpp.
#
# Ele embute o ChurchDAW.exe como recurso RCDATA (dentro da imagem PE, para a
# assinatura Authenticode cobrir tambem o payload) e instala no perfil do
# usuario, sem UAC. Sem isso, a atualizacao obrigatoria pediria senha de
# administrador que o musico normalmente nao tem.
option(CHURCHDAW_BUILD_INSTALLER
       "Compila o instalador do Windows (embute o app)"
       OFF)

if(CHURCHDAW_BUILD_INSTALLER AND WIN32 AND NOT ANDROID AND NOT IOS)
    set(CHURCHDAW_PAYLOAD_DIR "${CMAKE_CURRENT_BINARY_DIR}/installer-payload")
    set(CHURCHDAW_PAYLOAD_PATH "${CHURCHDAW_PAYLOAD_DIR}/ChurchDAW.exe")

    add_custom_command(
        OUTPUT "${CHURCHDAW_PAYLOAD_PATH}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CHURCHDAW_PAYLOAD_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:MinhaDAW>" "${CHURCHDAW_PAYLOAD_PATH}"
        DEPENDS MinhaDAW
        COMMENT "Preparando o payload do instalador")

    # O compilador de recursos nao entende barra normal em caminho.
    string(REPLACE "/" "\\\\" CHURCHDAW_PAYLOAD_PATH_RC "${CHURCHDAW_PAYLOAD_PATH}")
    string(REPLACE "/" "\\\\" CHURCHDAW_ICON_PATH_RC
           "${CMAKE_CURRENT_SOURCE_DIR}/Installer/ChurchDAW.ico")

    configure_file(Installer/ChurchDAWSetup.rc.in
                   "${CMAKE_CURRENT_BINARY_DIR}/ChurchDAWSetup.rc" @ONLY)

    add_executable(ChurchDAWSetup WIN32
        Installer/ChurchDAWSetup.cpp
        "${CMAKE_CURRENT_BINARY_DIR}/ChurchDAWSetup.rc"
        "${CHURCHDAW_PAYLOAD_PATH}")

    # Entra so para criar a dependencia de build; nao e compilado.
    set_source_files_properties("${CHURCHDAW_PAYLOAD_PATH}"
                                PROPERTIES HEADER_FILE_ONLY TRUE)

    add_dependencies(ChurchDAWSetup MinhaDAW)

    target_compile_definitions(ChurchDAWSetup PRIVATE
        CHURCHDAW_VERSION_NAME=L"${PROJECT_VERSION}"
        UNICODE _UNICODE)

    target_link_libraries(ChurchDAWSetup PRIVATE shlwapi ole32 uuid shell32)

    set_target_properties(ChurchDAWSetup PROPERTIES
        OUTPUT_NAME "ChurchDAW-${PROJECT_VERSION}-Setup")
endif()

# Testes do portao de atualizacao obrigatoria. Usam a politica Android REAL
# (assinada com a chave privada de verdade) como vetor conhecido -- ver
# Tests/DesktopUpdate/UpdatePolicyTests.cpp.
option(CHURCHDAW_BUILD_UPDATE_TESTS
       "Compila os testes do portao de atualizacao obrigatoria"
       OFF)

if(CHURCHDAW_BUILD_UPDATE_TESTS AND NOT ANDROID AND NOT IOS)
    juce_add_console_app(ChurchDAWUpdatePolicyTests
        PRODUCT_NAME "ChurchDAWUpdatePolicyTests")

    juce_add_console_app(ChurchDAWAndroidDistributionProbe
        PRODUCT_NAME "ChurchDAWAndroidDistributionProbe")

    juce_add_console_app(ChurchDAWParseManifestParityTests
        PRODUCT_NAME "ChurchDAWParseManifestParityTests")

    target_sources(ChurchDAWUpdatePolicyTests PRIVATE
        Tests/DesktopUpdate/UpdatePolicyTests.cpp
        Source/DesktopUpdater.cpp)

    target_sources(ChurchDAWAndroidDistributionProbe PRIVATE
        Tests/DesktopUpdate/AndroidDistributionProbe.cpp
        Source/DesktopUpdater.cpp)

    target_sources(ChurchDAWParseManifestParityTests PRIVATE
        Tests/DesktopUpdate/ParseManifestParityTest.cpp
        Source/DesktopUpdater.cpp)

    target_include_directories(ChurchDAWUpdatePolicyTests PRIVATE Source)
    target_include_directories(ChurchDAWAndroidDistributionProbe PRIVATE Source)
    target_include_directories(ChurchDAWParseManifestParityTests PRIVATE Source)

    target_compile_definitions(ChurchDAWUpdatePolicyTests PRIVATE
        CHURCHDAW_UPDATE_TEST_STATE_DIR="${CMAKE_CURRENT_BINARY_DIR}"
        CHURCHDAW_VERSION_CODE=${CHURCHDAW_VERSION_CODE}
        CHURCHDAW_UPDATE_REQUIRE_OS_SIGNATURE=$<BOOL:${CHURCHDAW_UPDATE_REQUIRE_OS_SIGNATURE}>
        JUCE_WEB_BROWSER=0
        JUCE_PUSH_NOTIFICATIONS=0
        JUCE_USE_CURL=0)

    target_compile_definitions(ChurchDAWAndroidDistributionProbe PRIVATE
        CHURCHDAW_VERSION_CODE=${CHURCHDAW_VERSION_CODE}
        JUCE_WEB_BROWSER=0
        JUCE_PUSH_NOTIFICATIONS=0
        JUCE_USE_CURL=0)

    target_compile_definitions(ChurchDAWParseManifestParityTests PRIVATE
        CHURCHDAW_VERSION_CODE=${CHURCHDAW_VERSION_CODE}
        JUCE_WEB_BROWSER=0
        JUCE_PUSH_NOTIFICATIONS=0
        JUCE_USE_CURL=0)

    target_link_libraries(ChurchDAWUpdatePolicyTests
        PRIVATE
            juce::juce_cryptography
            juce::juce_gui_extra
        PUBLIC
            juce::juce_recommended_config_flags)

    target_link_libraries(ChurchDAWAndroidDistributionProbe
        PRIVATE
            juce::juce_cryptography
            juce::juce_gui_extra
        PUBLIC
            juce::juce_recommended_config_flags)

    target_link_libraries(ChurchDAWParseManifestParityTests
        PRIVATE
            juce::juce_cryptography
            juce::juce_gui_extra
        PUBLIC
            juce::juce_recommended_config_flags)

    if(WIN32)
        target_link_libraries(ChurchDAWUpdatePolicyTests PRIVATE crypt32 bcrypt wintrust)
        target_link_libraries(ChurchDAWAndroidDistributionProbe PRIVATE crypt32 bcrypt wintrust)
        target_link_libraries(ChurchDAWParseManifestParityTests PRIVATE crypt32 bcrypt wintrust)
    elseif(APPLE)
        target_link_libraries(ChurchDAWUpdatePolicyTests PRIVATE "-framework Security")
        target_link_libraries(ChurchDAWAndroidDistributionProbe PRIVATE "-framework Security")
        target_link_libraries(ChurchDAWParseManifestParityTests PRIVATE "-framework Security")
    endif()

    if(MSVC)
        target_compile_options(ChurchDAWUpdatePolicyTests PRIVATE /utf-8)
        target_compile_options(ChurchDAWAndroidDistributionProbe PRIVATE /utf-8)
        target_compile_options(ChurchDAWParseManifestParityTests PRIVATE /utf-8)
    endif()
endif()

#==============================================================================
# ChurchDAW Instrument -- VST3 (e AU no macOS) do motor "Church Live".
#
# ALVO NOVO E SEPARADO. O aplicativo MinhaDAW acima nao muda em NADA: mesmas
# fontes, mesmas definicoes, mesmo link. Isso e deliberado -- o ChurchDAW toca
# ao vivo em culto, e a unica forma honesta de responder "da para virar VST3?"
# sem arriscar o domingo e construir um segundo binario a partir do mesmo motor.
#
# O que entra: TimbresProcessor (o instrumento inteiro, com editor) + os 24
# .cpp do Church Organ (a MESMA lista CHURCHDAW_ORGAN_SOURCES do app, de
# proposito: duas listas divergiriam na primeira correcao) + dez linhas de cola
# em Source/Plugin/ChurchDAWPluginEntry.cpp.
#
# O que NAO entra, e por que, esta escrito no cabecalho daquele arquivo:
# MainComponent (dono do dispositivo de audio), host de VST3 aninhado (o
# scanner relanca o executavel do host), DesktopUpdater e licenciamento.
#
# Desligado por padrao. Ligue com -DCHURCHDAW_BUILD_PLUGIN=ON: assim nenhum
# build de release existente passa a gerar um artefato que ninguem pediu.
#==============================================================================
option(CHURCHDAW_BUILD_PLUGIN
       "Compila o ChurchDAW Instrument (VST3/AU) alem do aplicativo" OFF)

if(CHURCHDAW_BUILD_PLUGIN AND NOT ANDROID AND NOT IOS)
    if(APPLE)
        set(CHURCHDAW_PLUGIN_FORMATS VST3 AU)
    else()
        set(CHURCHDAW_PLUGIN_FORMATS VST3)
    endif()

    juce_add_plugin(ChurchDAWInstrument
        PRODUCT_NAME "ChurchDAW Church Live"
        COMPANY_NAME "ChurchDAW"
        BUNDLE_ID "com.churchdaw.instrument"
        VERSION "${PROJECT_VERSION}"

        # IS_SYNTH + NEEDS_MIDI_INPUT: e um instrumento. O TimbresProcessor ja
        # declara BusesProperties().withOutput(estereo) e acceptsMidi()==true,
        # entao o contrato do wrapper bate com o que o motor sempre foi.
        IS_SYNTH TRUE
        NEEDS_MIDI_INPUT TRUE
        NEEDS_MIDI_OUTPUT FALSE
        IS_MIDI_EFFECT FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS TRUE

        # Quatro letras, primeira maiuscula -- exigencia do AU, herdada pelo
        # VST3 do JUCE. Cdaw/Cliv: se um dia sair um segundo plugin da casa,
        # o MANUFACTURER_CODE tem que continuar Cdaw.
        PLUGIN_MANUFACTURER_CODE Cdaw
        PLUGIN_CODE Cliv

        VST3_CATEGORIES "Instrument" "Sampler"
        AU_MAIN_TYPE "kAudioUnitType_MusicDevice"

        FORMATS ${CHURCHDAW_PLUGIN_FORMATS}

        COPY_PLUGIN_AFTER_BUILD FALSE)

    target_sources(ChurchDAWInstrument PRIVATE
        Source/Plugin/ChurchDAWPluginEntry.cpp
        ${CHURCHDAW_ORGAN_SOURCES}
        ${CHURCHDAW_SF2_SOURCES})

    target_include_directories(ChurchDAWInstrument PRIVATE Source)

    target_compile_definitions(ChurchDAWInstrument PUBLIC
        # ESTAS DUAS SAO O ALVO INTEIRO, e nao sao opcionais:
        #
        # CHURCHDAW_IS_PLUGIN=1 conserta AppPaths::appFolder(). Dentro de um
        # plugin, currentExecutableFile e o reaper.exe -- sem isto a busca de
        # biblioteca acontece dentro da instalacao do Reaper e o instrumento
        # abre com zero timbres, sem uma unica mensagem de erro.
        #
        # CHURCHDAW_HOSTS_PLUGINS=0: o simbolo so aparece em Main.cpp,
        # MainComponent.h e MainComponent.cpp -- nenhum dos tres entra neste
        # alvo, entao o SCANNER (PluginScanSubprocessV2, que relanca
        # File::currentExecutableFile como processo filho e dentro de um plugin
        # relancaria o HOST) ja fica de fora por construcao, nao por macro.
        # Definir 0 aqui e cinto e suspensorio: se um dia alguem acrescentar
        # MainComponent a este alvo, o rack nao vai junto por acidente.
        #
        # NAO CONFUNDA: o TimbresProcessor continua contendo o codigo de
        # inserts do MASTER FX, porque ele nao esta atras dessa macro. Sem
        # AudioPluginFormatManager (setPluginResources nunca e chamado neste
        # alvo) os combos de insert simplesmente ficam vazios -- os ponteiros
        # sao testados por nullptr. Nenhum processo filho e criado.
        CHURCHDAW_IS_PLUGIN=1
        CHURCHDAW_HOSTS_PLUGINS=0
        JUCE_PLUGINHOST_VST3=0
        JUCE_PLUGINHOST_AU=0

        # Nunca existiu VST2 do ChurchDAW. Sem isto o wrapper do JUCE aborta a
        # compilacao alertando sobre IDs de parametro incompativeis entre VST2
        # e VST3 -- um problema que so tem quem ja publicou um VST2.
        JUCE_VST3_CAN_REPLACE_VST2=0

        CHURCHDAW_STREAM_SAMPLES=1
        CHURCHDAW_APP_VERSION="${PROJECT_VERSION}"
        CHURCHDAW_DESKTOP_LICENSING=0
        CHURCHDAW_LICENSE_API_BASE="${CHURCHDAW_LICENSE_API_BASE}"
        CHURCHDAW_LICENSE_ALLOW_HTTP_LOCALHOST=0
        JUCE_WEB_BROWSER=0
        JUCE_PUSH_NOTIFICATIONS=0
        JUCE_USE_CURL=0

        # Num plugin nao existe "sair do aplicativo", e um modal loop rodando
        # dentro do loop de mensagens do host trava a sessao.
        JUCE_MODAL_LOOPS_PERMITTED=0

        # O host e dono do dispositivo. Sem isto o wrapper ainda arrastaria o
        # painel de selecao de dispositivo do juce_audio_devices para dentro do
        # Reaper.
        JUCE_DISPLAY_SPLASH_SCREEN=0)

    target_link_libraries(ChurchDAWInstrument
        PRIVATE
            ChurchAssets
            juce::juce_cryptography
            juce::juce_dsp
            juce::juce_audio_utils
            juce::juce_audio_processors
            juce::juce_audio_devices
            juce::juce_gui_extra
        PUBLIC
            juce::juce_recommended_config_flags)

    if(MSVC)
        target_compile_options(ChurchDAWInstrument PRIVATE /utf-8 /bigobj)

        # ATENCAO -- LEIA ANTES DE COPIAR O /STACK DO MinhaDAW PARA CA:
        #
        # /STACK numa DLL NAO TEM EFEITO. Quem manda no stack reserve das
        # threads e o cabecalho PE do EXECUTAVEL hospedeiro (reaper.exe), nao o
        # da DLL. O MinhaDAW usa /STACK:33554432 porque a construcao das
        # tabelas Pipe/Hybrid do Church Organ estoura 1 MiB. Dentro do host
        # esse mesmo codigo roda com o stack que o host reservou.
        #
        # Por isso a mitigacao aqui NAO e de link: e o trampolim de
        # prepareToPlay em TimbresProcessor.h, que sob CHURCHDAW_IS_PLUGIN roda
        # o prepare dos orgaos numa thread com stack explicito.
    endif()

    if(APPLE)
        # Universal por padrao, como o aplicativo. Mas ao contrario do MinhaDAW
        # isto NAO e imposto: passar -DCMAKE_OSX_ARCHITECTURES=arm64 corta o
        # tempo de compilacao pela metade, e num alvo que ainda esta sendo
        # provado essa diferenca importa mais do que a fatia Intel.
        if(NOT CMAKE_OSX_ARCHITECTURES)
            set_target_properties(ChurchDAWInstrument PROPERTIES
                OSX_ARCHITECTURES "arm64;x86_64")
        endif()

        target_compile_options(ChurchDAWInstrument PRIVATE
            -Werror=unguarded-availability-new)
    endif()
endif()

#==============================================================================
# Sonda MANUAL de CARGA do ChurchDAW Instrument -- ver
# Tests/Audio/PluginHostLoadProbe.cpp.
#
# E um HOST de linha de comando minusculo. Nao compila uma linha do ChurchDAW:
# carrega o .vst3 ja construido pelo juce::VST3PluginFormat, do mesmo jeito que
# o Reaper faria, e mede carga/prepare/process/estado/editor. Serve para
# responder "abre num host?" sem depender de GUI nem de DAW instalada.
#
# NAO entra em add_test (depende de um artefato ja compilado e da biblioteca de
# samples da maquina) e nunca gera nem substitui o executavel ChurchDAW.
#==============================================================================
if(CHURCHDAW_BUILD_AUDIO_TESTS AND NOT ANDROID AND NOT IOS)
    juce_add_console_app(ChurchDAWPluginHostLoadProbe
        PRODUCT_NAME "ChurchDAWPluginHostLoadProbe")

    target_sources(ChurchDAWPluginHostLoadProbe PRIVATE
        Tests/Audio/PluginHostLoadProbe.cpp)

    if(APPLE)
        target_compile_definitions(ChurchDAWPluginHostLoadProbe PRIVATE JUCE_PLUGINHOST_AU=1)
    endif()

    # Este alvo e o unico do repositorio que precisa hospedar VST3 SEM ser o
    # aplicativo: e justamente o que ele testa.
    target_compile_definitions(ChurchDAWPluginHostLoadProbe PRIVATE
        JUCE_PLUGINHOST_VST3=1
        JUCE_MODAL_LOOPS_PERMITTED=1
        JUCE_WEB_BROWSER=0
        JUCE_PUSH_NOTIFICATIONS=0
        JUCE_USE_CURL=0)

    target_link_libraries(ChurchDAWPluginHostLoadProbe
        PRIVATE
            juce::juce_audio_utils
            juce::juce_audio_processors
            juce::juce_gui_extra
        PUBLIC
            juce::juce_recommended_config_flags)

    if(MSVC)
        target_compile_options(ChurchDAWPluginHostLoadProbe PRIVATE /utf-8)
    endif()
endif()

# Deterministic real-time engine regressions and optional offline benchmark.
if(CHURCHDAW_BUILD_AUDIO_TESTS)
    juce_add_console_app(ChurchDAWEngineRealtimeTests PRODUCT_NAME "ChurchDAWEngineRealtimeTests")
    target_sources(ChurchDAWEngineRealtimeTests PRIVATE
        Tests/Audio/EngineRealtimeTests.cpp
        Tests/Audio/FinalOutputGuardTests.cpp
        Tests/Audio/DiagnosticTestToneTests.cpp)
    target_include_directories(ChurchDAWEngineRealtimeTests PRIVATE Source)
    target_compile_definitions(ChurchDAWEngineRealtimeTests PRIVATE CHURCHDAW_INTERNAL_AUDIO_TEST_HOOKS=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
    target_link_libraries(ChurchDAWEngineRealtimeTests PRIVATE juce::juce_audio_utils juce::juce_dsp PUBLIC juce::juce_recommended_config_flags)
    add_test(NAME ChurchDAWEngineRealtime COMMAND ChurchDAWEngineRealtimeTests)
    set_tests_properties(ChurchDAWEngineRealtime PROPERTIES TIMEOUT 60)
endif()

# Manual GPU/MIDI preview with no audio device or application settings access.
# Excluded from normal builds and release packaging.
if(NOT ANDROID AND NOT IOS)
    juce_add_gui_app(ChurchDAWPiano3DPreview PRODUCT_NAME "ChurchDAW Piano 3D Preview")
    set_target_properties(ChurchDAWPiano3DPreview PROPERTIES EXCLUDE_FROM_ALL TRUE)
    target_sources(ChurchDAWPiano3DPreview PRIVATE Tests/UI/Piano3DPreview.cpp)
    target_include_directories(ChurchDAWPiano3DPreview PRIVATE Source)
    target_compile_definitions(ChurchDAWPiano3DPreview PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
    target_link_libraries(ChurchDAWPiano3DPreview PRIVATE ChurchAssets
        juce::juce_opengl juce::juce_audio_devices juce::juce_recommended_config_flags)
endif()

# ChurchCam uses the same compositor and audio tap on desktop and iOS.
# Camera permission is requested only from the explicit camera button.
if(NOT ANDROID)
    target_compile_definitions(MinhaDAW PRIVATE JUCE_USE_CAMERA=1)
    target_link_libraries(MinhaDAW PRIVATE juce::juce_video juce::juce_opengl)
    if(APPLE)
        target_sources(MinhaDAW PRIVATE Source/Studio/StudioMovieWriter_mac.mm)
        target_link_libraries(MinhaDAW PRIVATE "-framework AVFoundation" "-framework CoreVideo" "-framework CoreMedia")
        if(IOS)
            target_sources(MinhaDAW PRIVATE Source/Studio/StudioCamera_ios.mm)
            target_compile_definitions(MinhaDAW PRIVATE JUCE_CONTENT_SHARING=1)
        endif()
    elseif(WIN32)
        target_sources(MinhaDAW PRIVATE Source/Studio/StudioMovieWriter_windows.cpp)
        target_link_libraries(MinhaDAW PRIVATE mfplat mfreadwrite mfuuid ole32)
    else()
        target_sources(MinhaDAW PRIVATE Source/Studio/StudioMovieWriter_stub.cpp)
    endif()
endif()
if(NOT ANDROID AND NOT IOS)
    juce_add_gui_app(ChurchDAWStudioPreview PRODUCT_NAME "ChurchDAW Studio Preview"
        BUNDLE_ID "com.churchdaw.studiopreview"
        CAMERA_PERMISSION_ENABLED TRUE CAMERA_PERMISSION_TEXT "Mostrar seu rosto no video do ChurchDAW Studio, somente quando ativado."
        MICROPHONE_PERMISSION_ENABLED TRUE MICROPHONE_PERMISSION_TEXT "Gravar sua voz no video, somente quando o microfone for ativado.")
    set_target_properties(ChurchDAWStudioPreview PROPERTIES EXCLUDE_FROM_ALL TRUE)
    target_sources(ChurchDAWStudioPreview PRIVATE Tests/Studio/StudioPreview.cpp)
    target_include_directories(ChurchDAWStudioPreview PRIVATE Source)
    target_compile_definitions(ChurchDAWStudioPreview PRIVATE JUCE_USE_CAMERA=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
    target_link_libraries(ChurchDAWStudioPreview PRIVATE ChurchAssets juce::juce_opengl
        juce::juce_video juce::juce_audio_devices juce::juce_audio_formats juce::juce_recommended_config_flags)
    if(APPLE)
        target_sources(ChurchDAWStudioPreview PRIVATE Source/Studio/StudioMovieWriter_mac.mm)
        target_link_libraries(ChurchDAWStudioPreview PRIVATE "-framework AVFoundation" "-framework CoreVideo" "-framework CoreMedia")
    elseif(WIN32)
        target_sources(ChurchDAWStudioPreview PRIVATE Source/Studio/StudioMovieWriter_windows.cpp)
        target_link_libraries(ChurchDAWStudioPreview PRIVATE mfplat mfreadwrite mfuuid ole32)
    else()
        target_sources(ChurchDAWStudioPreview PRIVATE Source/Studio/StudioMovieWriter_stub.cpp)
    endif()
endif()

# Manual desktop design review using the real MainComponent. This is a separate
# application, excluded from default builds and every release/installer target.
# Explicit sources are intentional: Main.cpp, DesktopUpdater and the desktop
# licence/credential implementation must never be inherited into this harness.
if(CHURCHDAW_BUILD_AUDIO_TESTS AND NOT ANDROID AND NOT IOS)
    juce_add_gui_app(ChurchDAWDesktopDesignPreview
        PRODUCT_NAME "ChurchDAW Design Preview"
        BUNDLE_ID "com.churchdaw.design-preview")
    set_target_properties(ChurchDAWDesktopDesignPreview PROPERTIES
        EXCLUDE_FROM_ALL TRUE)
    target_sources(ChurchDAWDesktopDesignPreview PRIVATE
        Tests/UI/DesktopDesignPreview.cpp
        Tests/UI/DesktopShellTests.cpp
        Source/MainComponent.cpp
        ${CHURCHDAW_ORGAN_SOURCES}
        ${CHURCHDAW_SF2_SOURCES}
        ${CHURCHDAW_SABRINA_SOURCES})
    target_include_directories(ChurchDAWDesktopDesignPreview PRIVATE Source)
    target_compile_definitions(ChurchDAWDesktopDesignPreview PRIVATE
        CHURCHDAW_UI_PREVIEW=1
        CHURCHDAW_REMOTE_CONTROL_ENABLED=0
        CHURCHDAW_HOSTS_PLUGINS=1
        CHURCHDAW_APP_VERSION="${PROJECT_VERSION}"
        JUCE_PLUGINHOST_VST=0
        JUCE_PLUGINHOST_VST3=1
        JUCE_USE_CAMERA=1
        JUCE_WEB_BROWSER=0
        JUCE_PUSH_NOTIFICATIONS=0
        JUCE_USE_CURL=0)
    target_link_libraries(ChurchDAWDesktopDesignPreview PRIVATE
        ChurchAssets
        juce::juce_opengl
        juce::juce_video
        juce::juce_cryptography
        juce::juce_dsp
        juce::juce_audio_utils
        juce::juce_audio_processors
        juce::juce_audio_devices
        juce::juce_gui_extra
        juce::juce_recommended_config_flags)
    if(APPLE)
        # This manual preview runs on the developer's Mac, never in a release.
        # Keep it native: running an Intel slice under Rosetta on Apple Silicon
        # raises a system compatibility warning attributed to the preview app.
        # Production and the dedicated compatibility tests remain universal.
        execute_process(COMMAND /usr/sbin/sysctl -n hw.optional.arm64
            OUTPUT_VARIABLE churchdaw_preview_host_arm64
            OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(churchdaw_preview_host_arm64 STREQUAL "1")
            set_target_properties(ChurchDAWDesktopDesignPreview PROPERTIES
                OSX_ARCHITECTURES "arm64")
        else()
            set_target_properties(ChurchDAWDesktopDesignPreview PROPERTIES
                OSX_ARCHITECTURES "${CMAKE_HOST_SYSTEM_PROCESSOR}")
        endif()
        target_sources(ChurchDAWDesktopDesignPreview PRIVATE
            Source/Studio/StudioMovieWriter_mac.mm)
        target_compile_definitions(ChurchDAWDesktopDesignPreview PRIVATE
            JUCE_PLUGINHOST_AU=1 JUCE_USE_VDSP_FRAMEWORK=1)
        target_link_libraries(ChurchDAWDesktopDesignPreview PRIVATE
            "-framework AudioUnit" "-framework AudioToolbox"
            "-framework CoreAudioKit" "-framework Accelerate"
            "-framework AVFoundation" "-framework CoreVideo"
            "-framework CoreMedia" "-framework Security")
        target_compile_options(ChurchDAWDesktopDesignPreview PRIVATE
            -Werror=unguarded-availability-new)
    elseif(WIN32)
        target_sources(ChurchDAWDesktopDesignPreview PRIVATE
            Source/Studio/StudioMovieWriter_windows.cpp)
        target_link_libraries(ChurchDAWDesktopDesignPreview PRIVATE
            mfplat mfreadwrite mfuuid ole32)
    else()
        target_sources(ChurchDAWDesktopDesignPreview PRIVATE
            Source/Studio/StudioMovieWriter_stub.cpp)
    endif()
    if(MSVC)
        target_compile_options(ChurchDAWDesktopDesignPreview PRIVATE /utf-8 /bigobj)
        target_link_options(ChurchDAWDesktopDesignPreview PRIVATE /STACK:33554432)
    endif()

    juce_add_gui_app(ChurchDAWMobileDesignPreview
        PRODUCT_NAME "ChurchDAW Mobile Preview"
        BUNDLE_ID "com.churchdaw.mobile-preview")
    set_target_properties(ChurchDAWMobileDesignPreview PROPERTIES
        EXCLUDE_FROM_ALL TRUE)
    target_sources(ChurchDAWMobileDesignPreview PRIVATE
        Tests/UI/MobileDesignPreview.cpp
        Source/MainComponent.cpp
        ${CHURCHDAW_ORGAN_SOURCES}
        ${CHURCHDAW_SF2_SOURCES}
        ${CHURCHDAW_SABRINA_SOURCES})
    target_include_directories(ChurchDAWMobileDesignPreview PRIVATE Source)
    target_compile_definitions(ChurchDAWMobileDesignPreview PRIVATE
        CHURCHDAW_UI_PREVIEW=1
        CHURCHDAW_MOBILE_UI_PREVIEW=1
        CHURCHDAW_REMOTE_CONTROL_ENABLED=0
        CHURCHDAW_HOSTS_PLUGINS=0
        CHURCHDAW_APP_VERSION="${PROJECT_VERSION}"
        JUCE_PLUGINHOST_VST=0
        JUCE_PLUGINHOST_VST3=0
        JUCE_USE_CAMERA=1
        JUCE_WEB_BROWSER=0
        JUCE_PUSH_NOTIFICATIONS=0
        JUCE_USE_CURL=0)
    target_link_libraries(ChurchDAWMobileDesignPreview PRIVATE
        ChurchAssets
        juce::juce_opengl
        juce::juce_video
        juce::juce_cryptography
        juce::juce_dsp
        juce::juce_audio_utils
        juce::juce_audio_processors
        juce::juce_audio_devices
        juce::juce_gui_extra
        juce::juce_recommended_config_flags)
    if(APPLE)
        target_sources(ChurchDAWMobileDesignPreview PRIVATE
            Source/Studio/StudioMovieWriter_mac.mm)
        target_compile_definitions(ChurchDAWMobileDesignPreview PRIVATE
            JUCE_USE_VDSP_FRAMEWORK=1)
        target_link_libraries(ChurchDAWMobileDesignPreview PRIVATE
            "-framework AudioUnit" "-framework AudioToolbox"
            "-framework CoreAudioKit" "-framework Accelerate"
            "-framework AVFoundation" "-framework CoreVideo"
            "-framework CoreMedia" "-framework Security")
    elseif(WIN32)
        target_sources(ChurchDAWMobileDesignPreview PRIVATE
            Source/Studio/StudioMovieWriter_windows.cpp)
        target_link_libraries(ChurchDAWMobileDesignPreview PRIVATE
            mfplat mfreadwrite mfuuid ole32 bcrypt crypt32)
    else()
        target_sources(ChurchDAWMobileDesignPreview PRIVATE
            Source/Studio/StudioMovieWriter_stub.cpp)
    endif()
    if(MSVC)
        target_compile_options(ChurchDAWMobileDesignPreview PRIVATE /utf-8 /bigobj)
        target_link_options(ChurchDAWMobileDesignPreview PRIVATE /STACK:33554432)
    endif()
endif()
