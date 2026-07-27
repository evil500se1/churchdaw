cmake_minimum_required(VERSION 3.22)
project(MinhaDAW VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# runtime do C++ embutido no exe: roda em qualquer PC sem instalar redistribuivel
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Apple Silicon + Intel no mesmo binario — precisa valer para TODOS os alvos
# (app, assets, juceaide), senao o linker mistura arquiteturas e falha.
if(APPLE)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
endif()

add_subdirectory(JUCE)

juce_add_gui_app(MinhaDAW
    PRODUCT_NAME "ChurchDAW"
    COMPANY_NAME "ChurchDAW"
    VERSION "1.1.0"
    BUNDLE_ID "com.churchdaw.app"
    HARDENED_RUNTIME_ENABLED TRUE
    HARDENED_RUNTIME_OPTIONS "com.apple.security.cs.disable-library-validation"
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

target_sources(MinhaDAW PRIVATE
    Source/Main.cpp
    Source/MainComponent.cpp)

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
    JUCE_PLUGINHOST_VST3=${CHURCHDAW_PLUGINS}
    JUCE_WEB_BROWSER=0
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
endif()

target_link_libraries(MinhaDAW
    PRIVATE
        ChurchAssets
        juce::juce_dsp
        juce::juce_audio_utils
        juce::juce_audio_processors
        juce::juce_audio_devices
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags)
