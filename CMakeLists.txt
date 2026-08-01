cmake_minimum_required(VERSION 3.22)
# Versao unica do produto: acompanha o ChurchDAW.jucer (Android ja esta
# em 1.5.5). Publicar conteudo novo com numero velho fazia o updater dos
# clientes 1.4.0 achar que nao havia atualizacao.
project(MinhaDAW VERSION 1.5.6)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(CHURCHDAW_ENABLE_DESKTOP_LICENSING
       "Exige conta e licenca validas antes de iniciar o motor desktop"
       ON)
option(CHURCHDAW_BUILD_LICENSE_TESTS
       "Compila testes nativos do verificador de licenca"
       OFF)
option(CHURCHDAW_LICENSE_ALLOW_HTTP_LOCALHOST
       "Permite endpoint HTTP somente em localhost para testes locais"
       OFF)
set(CHURCHDAW_LICENSE_API_BASE
    "https://churchdaw.com/api/v1"
    CACHE STRING
    "Endpoint HTTPS do backend de licenciamento desktop")

set(CHURCHDAW_UPDATE_POLICY_URL
    "https://churchdaw.com/app/desktop/update-policy-v1.json"
    CACHE STRING
    "Endereco da politica de atualizacao obrigatoria do desktop")
option(CHURCHDAW_UPDATE_ALLOW_LOCALHOST
       "Aceita politica de atualizacao vinda de 127.0.0.1 (SO para teste local)"
       OFF)

# runtime do C++ embutido no exe: roda em qualquer PC sem instalar redistribuivel
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Apple Silicon + Intel no mesmo binario — precisa valer para TODOS os alvos
# (app, assets, juceaide), senao o linker mistura arquiteturas e falha.
if(APPLE)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)

    # PISO DE COMPATIBILIDADE DO macOS.
    #
    # Sem esta linha o piso vira o SDK da maquina que compilou. Medido em
    # 28/07/2026 no Mac de build (Xcode 26.5): o binario saiu com
    # "minos 26.0" -- ou seja, recusaria abrir em qualquer macOS anterior ao
    # 26. Praticamente nenhum comprador teria essa versao.
    #
    # E uma falha silenciosa: compila, roda na maquina de build, e so quebra
    # no computador do cliente. E piora sozinha a cada atualizacao do Xcode.
    #
    # 11.0 (Big Sur, 2020) e o primeiro macOS com Apple Silicon, entao cobre
    # as duas arquiteturas do binario universal sem excluir maquinas Intel
    # ainda em uso nas igrejas.
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "" FORCE)
endif()

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

    juce_add_console_app(ChurchDAWChannelToneTests
        PRODUCT_NAME "ChurchDAWChannelToneTests")

    juce_add_console_app(ChurchDAWMidiMappingSafetyTests
        PRODUCT_NAME "ChurchDAWMidiMappingSafetyTests")

    juce_add_console_app(ChurchDAWChannelRotaryTests
        PRODUCT_NAME "ChurchDAWChannelRotaryTests")

    juce_add_console_app(ChurchDAWPadProcessorTests
        PRODUCT_NAME "ChurchDAWPadProcessorTests")

    juce_add_console_app(ChurchDAWLibraryPackPathTests
        PRODUCT_NAME "ChurchDAWLibraryPackPathTests")

    juce_add_console_app(ChurchDAWLiveScenesTests
        PRODUCT_NAME "ChurchDAWLiveScenesTests")

    juce_add_console_app(ChurchDAWSceneFileWriteProbe
        PRODUCT_NAME "ChurchDAWSceneFileWriteProbe")

    target_sources(ChurchDAWMasterLimiterTests PRIVATE
        Tests/Audio/StereoLookaheadLimiterTests.cpp)

    target_sources(ChurchDAWChannelToneTests PRIVATE
        Tests/Audio/ChannelToneShaperTests.cpp)

    target_sources(ChurchDAWMidiMappingSafetyTests PRIVATE
        Tests/Audio/MidiMappingSafetyTests.cpp)

    target_sources(ChurchDAWChannelRotaryTests PRIVATE
        Tests/Audio/ChannelRotarySpeakerTests.cpp)

    target_sources(ChurchDAWPadProcessorTests PRIVATE
        Tests/Audio/PadProcessorTests.cpp)

    target_sources(ChurchDAWLibraryPackPathTests PRIVATE
        Tests/Library/LibraryPackPathTests.cpp)

    target_sources(ChurchDAWLiveScenesTests PRIVATE
        Tests/Live/LiveScenesTests.cpp)

    target_sources(ChurchDAWSceneFileWriteProbe PRIVATE
        Tests/Live/SceneFileWriteProbe.cpp)

    target_include_directories(ChurchDAWMasterLimiterTests PRIVATE Source)
    target_include_directories(ChurchDAWChannelToneTests PRIVATE Source)
    target_include_directories(ChurchDAWMidiMappingSafetyTests PRIVATE Source)
    target_include_directories(ChurchDAWChannelRotaryTests PRIVATE Source)
    target_include_directories(ChurchDAWPadProcessorTests PRIVATE Source)
    target_include_directories(ChurchDAWLibraryPackPathTests PRIVATE Source)
    target_include_directories(ChurchDAWLiveScenesTests PRIVATE Source)
    target_include_directories(ChurchDAWSceneFileWriteProbe PRIVATE Source)
    target_compile_definitions(ChurchDAWMasterLimiterTests PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_PUSH_NOTIFICATIONS=0
        JUCE_USE_CURL=0)
    target_compile_definitions(ChurchDAWChannelToneTests PRIVATE
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
    target_compile_definitions(ChurchDAWLibraryPackPathTests PRIVATE
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

    target_link_libraries(ChurchDAWMasterLimiterTests
        PRIVATE
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

    target_link_libraries(ChurchDAWMidiMappingSafetyTests
        PRIVATE
            juce::juce_audio_processors
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

    target_link_libraries(ChurchDAWLibraryPackPathTests
        PRIVATE
            juce::juce_cryptography
            juce::juce_gui_extra
        PUBLIC
            juce::juce_recommended_config_flags)

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

    if(MSVC)
        target_compile_options(ChurchDAWMasterLimiterTests PRIVATE /utf-8)
        target_compile_options(ChurchDAWChannelToneTests PRIVATE /utf-8)
        target_compile_options(ChurchDAWMidiMappingSafetyTests PRIVATE /utf-8)
        target_compile_options(ChurchDAWChannelRotaryTests PRIVATE /utf-8)
        target_compile_options(ChurchDAWPadProcessorTests PRIVATE /utf-8)
        target_compile_options(ChurchDAWLibraryPackPathTests PRIVATE /utf-8)
        target_compile_options(ChurchDAWLiveScenesTests PRIVATE /utf-8)
        target_compile_options(ChurchDAWSceneFileWriteProbe PRIVATE /utf-8)
    endif()

    add_test(
        NAME ChurchDAWReverbPitch
        COMMAND ChurchDAWReverbPitchTests --all)
    add_test(
        NAME ChurchDAWMasterLimiter
        COMMAND ChurchDAWMasterLimiterTests)
    add_test(
        NAME ChurchDAWChannelTone
        COMMAND ChurchDAWChannelToneTests)
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
        NAME ChurchDAWLibraryPackPath
        COMMAND ChurchDAWLibraryPackPathTests)
    add_test(
        NAME ChurchDAWLiveScenes
        COMMAND ChurchDAWLiveScenesTests)
    add_test(
        NAME ChurchDAWSceneFileWrite
        COMMAND ChurchDAWSceneFileWriteProbe)
    set_tests_properties(
        ChurchDAWReverbPitch
        ChurchDAWMasterLimiter
        ChurchDAWChannelTone
        ChurchDAWMidiMappingSafety
        ChurchDAWChannelRotary
        ChurchDAWPadProcessor
        ChurchDAWLibraryPackPath
        PROPERTIES TIMEOUT 240)
endif()

juce_add_gui_app(MinhaDAW
    PRODUCT_NAME "ChurchDAW"
    COMPANY_NAME "ChurchDAW"
    VERSION "${PROJECT_VERSION}"
    BUNDLE_ID "com.churchdaw.app"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "O ChurchDAW usa a entrada de audio somente quando voce ativa guitarra/microfone em Audio/MIDI."

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

    HARDENED_RUNTIME_ENABLED TRUE
    HARDENED_RUNTIME_OPTIONS
        "com.apple.security.device.audio-input"
        "com.apple.security.cs.disable-library-validation"
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/Assets/icon.png"
    ICON_SMALL "${CMAKE_CURRENT_SOURCE_DIR}/Assets/icon.png")

juce_add_binary_data(ChurchAssets SOURCES
    Assets/icon.png
    Assets/logo_round.png
    Assets/glyph_piano.png
    Assets/glyph_pad.png
    Assets/glyph_organ.png
    Assets/glyph_strings.png
    Assets/glyph_ambience.png
    Assets/glyph_bells.png
    Assets/glyph_lead.png)

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

target_sources(MinhaDAW PRIVATE
    Source/Main.cpp
    Source/MainComponent.cpp
    ${CHURCHDAW_ORGAN_SOURCES})

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
    target_compile_definitions(MinhaDAW PRIVATE
        CHURCHDAW_VERSION_CODE=${CHURCHDAW_VERSION_CODE}
        CHURCHDAW_UPDATE_POLICY_URL="${CHURCHDAW_UPDATE_POLICY_URL}"
        CHURCHDAW_UPDATE_ALLOW_HTTP_LOCALHOST=$<BOOL:${CHURCHDAW_UPDATE_ALLOW_LOCALHOST}>)
    message(STATUS "ChurchDAW version code: ${CHURCHDAW_VERSION_CODE}")
    message(STATUS "ChurchDAW update policy: ${CHURCHDAW_UPDATE_POLICY_URL}")

    if(CHURCHDAW_UPDATE_ALLOW_LOCALHOST)
        message(WARNING
            "CHURCHDAW_UPDATE_ALLOW_LOCALHOST esta LIGADO: este binario aceita "
            "politica de atualizacao vinda de 127.0.0.1. Nunca distribuir.")
    endif()

    if(WIN32)
        # wintrust = Authenticode (WinVerifyTrust); crypt32/bcrypt = RS256 da policy
        target_link_libraries(MinhaDAW PRIVATE crypt32 bcrypt wintrust)
    elseif(APPLE)
        target_link_libraries(MinhaDAW PRIVATE "-framework Security")
    endif()
endif()

# Hospedar plugins (Kontakt e afins) e recurso de COMPUTADOR. No Android e no
# iOS nao existe plugin nativo carregavel -- nao e limitacao do JUCE, e da
# plataforma. Ali o ChurchDAW roda so com os timbres proprios.
#
# O codigo do host NAO e apagado: fica atras de CHURCHDAW_HOSTS_PLUGINS, para
# haver UM codigo-fonte so. Apagar criaria duas versoes do motor de samples que
# divergiriam na primeira correcao.
if(ANDROID OR IOS)
    set(CHURCHDAW_PLUGINS 0)
else()
    set(CHURCHDAW_PLUGINS 1)
endif()

target_compile_definitions(MinhaDAW PRIVATE
    CHURCHDAW_HOSTS_PLUGINS=${CHURCHDAW_PLUGINS}
    CHURCHDAW_DESKTOP_LICENSING=$<BOOL:${CHURCHDAW_ENABLE_DESKTOP_LICENSING}>
    CHURCHDAW_APP_VERSION="${PROJECT_VERSION}"
    CHURCHDAW_LICENSE_ALLOW_HTTP_LOCALHOST=$<BOOL:${CHURCHDAW_LICENSE_ALLOW_HTTP_LOCALHOST}>
    CHURCHDAW_LICENSE_API_BASE="${CHURCHDAW_LICENSE_API_BASE}"
    JUCE_PLUGINHOST_VST3=${CHURCHDAW_PLUGINS}
    JUCE_WEB_BROWSER=0
    JUCE_PUSH_NOTIFICATIONS=0
    JUCE_USE_CURL=0)

# macOS: o Kontakt e a maioria dos plugins existem tambem como Audio Unit, que e
# o formato nativo do sistema. Sem isto o usuario de Mac so enxergaria os VST3.
if(APPLE AND NOT IOS)
    target_compile_definitions(MinhaDAW PRIVATE
        JUCE_PLUGINHOST_AU=1)

    set_target_properties(MinhaDAW PROPERTIES
        XCODE_ATTRIBUTE_CLANG_CXX_LIBRARY "libc++"
        OSX_ARCHITECTURES "arm64;x86_64")   # Apple Silicon + Intel no mesmo binario

    target_link_libraries(MinhaDAW PRIVATE
        "-framework AudioUnit"
        "-framework AudioToolbox"
        "-framework CoreAudioKit")
endif()

# Habilita ASIO se o SDK da Steinberg estiver na pasta asiosdk/
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/asiosdk/common/iasiodrv.h")
    message(STATUS "ASIO SDK encontrado - habilitando suporte ASIO")
    target_compile_definitions(MinhaDAW PRIVATE JUCE_ASIO=1)
    target_include_directories(MinhaDAW PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/asiosdk/common")
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
        ${CHURCHDAW_ORGAN_SOURCES})

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
        ${CHURCHDAW_ORGAN_SOURCES})

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

    target_sources(ChurchDAWUpdatePolicyTests PRIVATE
        Tests/DesktopUpdate/UpdatePolicyTests.cpp
        Source/DesktopUpdater.cpp)

    target_sources(ChurchDAWAndroidDistributionProbe PRIVATE
        Tests/DesktopUpdate/AndroidDistributionProbe.cpp
        Source/DesktopUpdater.cpp)

    target_include_directories(ChurchDAWAndroidDistributionProbe PRIVATE Source)

    target_compile_definitions(ChurchDAWUpdatePolicyTests PRIVATE
        CHURCHDAW_VERSION_CODE=${CHURCHDAW_VERSION_CODE}
        JUCE_WEB_BROWSER=0
        JUCE_PUSH_NOTIFICATIONS=0
        JUCE_USE_CURL=0)

    target_compile_definitions(ChurchDAWAndroidDistributionProbe PRIVATE
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

    if(WIN32)
        target_link_libraries(ChurchDAWUpdatePolicyTests PRIVATE crypt32 bcrypt wintrust)
        target_link_libraries(ChurchDAWAndroidDistributionProbe PRIVATE crypt32 bcrypt wintrust)
    elseif(APPLE)
        target_link_libraries(ChurchDAWUpdatePolicyTests PRIVATE "-framework Security")
        target_link_libraries(ChurchDAWAndroidDistributionProbe PRIVATE "-framework Security")
    endif()

    if(MSVC)
        target_compile_options(ChurchDAWUpdatePolicyTests PRIVATE /utf-8)
        target_compile_options(ChurchDAWAndroidDistributionProbe PRIVATE /utf-8)
    endif()
endif()
