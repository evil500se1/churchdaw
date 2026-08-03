#pragma once

#include <juce_core/juce_core.h>

//==============================================================================
// Hospedar plugins (Kontakt e afins) e recurso de COMPUTADOR.
//
// Android e iOS nao tem plugin nativo carregavel -- e limitacao da PLATAFORMA,
// nao do JUCE nem nossa. No celular o ChurchDAW roda so com os timbres proprios.
//
// O CMake ja define este simbolo; a rede de seguranca abaixo existe porque o
// build de Android nao passa por CMake (o proprio JUCE avisa em docs/CMake
// API.md linha 6: "Android targets are not currently supported"), entao ele vem
// do Projucer -- e um projeto do Projucer mal configurado silenciosamente
// ligaria o host de plugins num alvo onde ele nao compila.
#ifndef CHURCHDAW_HOSTS_PLUGINS
 #if JUCE_ANDROID || JUCE_IOS
  #define CHURCHDAW_HOSTS_PLUGINS 0
 #else
  #define CHURCHDAW_HOSTS_PLUGINS 1
 #endif
#endif

//==============================================================================
// Caminhos da aplicacao, resolvidos por plataforma.
//
// A ARMADILHA DO MAC: no Windows o executavel fica solto numa pasta, entao
// "biblioteca ao lado do .exe" funciona. No macOS o app e um BUNDLE --
//
//     ChurchDAW.app/Contents/MacOS/ChurchDAW
//
// entao getParentDirectory() devolve "Contents/MacOS", nao a pasta onde o
// usuario ve o aplicativo. Procurar a biblioteca ali nunca acharia nada, e o
// app abriria vazio no Mac sem dar erro nenhum -- o pior tipo de bug.
//
// A ARMADILHA DO ANDROID: e a mesma armadilha, so que pior, e vale a pena
// registrar porque o codigo abaixo parece errado ate voce saber disto.
// Conferido em juce_Files_android.cpp:809-870 (JUCE 9.0.0):
//
//   currentExecutableFile          -> publicSourceDir, que e o CAMINHO DO .APK
//                                     (/data/app/<pkg>/base.apk). Nao e o
//                                     executavel numa pasta: getParentDirectory()
//                                     cai em /data/app/<pkg>, pasta do SISTEMA,
//                                     somente leitura, onde biblioteca nenhuma
//                                     jamais vai estar. "appFolder" nao existe
//                                     como conceito no Android.
//
//   commonApplicationDataDirectory -> dataDir (/data/user/0/<pkg>), e o mesmo
//   userApplicationDataDirectory      valor para os DOIS. Alem de coincidirem,
//                                     e armazenamento PRIVADO: nao aparece
//                                     quando o usuario liga o cabo USB. Mandar
//                                     ele copiar 29 GB para la e impossivel,
//                                     nao dificil.
//
// O primeiro Android do ChurchDAW usava Music/ChurchDAW para permitir copia
// manual por USB. A versao de loja nao pode depender disso: Scoped Storage
// exige permissao ampla, o instalador nao consegue ser atomico e uma copia pela
// metade parece uma biblioteca valida. A partir da 1.2 o proprio app baixa para
// seu dataDir privado. Atualizacoes preservam esse diretorio; desinstalar o app
// o remove, como o usuario espera de conteudo gerenciado pelo aplicativo.
//
// Music/ChurchDAW continua sendo consultada como caminho legado, sem virar o
// destino de novos downloads. Assim uma atualizacao nao abandona quem ja copiou
// samples manualmente, mas uma instalacao limpa nao pede acesso a toda a musica
// do aparelho.
//==============================================================================
struct AppPaths
{
    // Pasta "onde o aplicativo esta", do ponto de vista do usuario.
    static juce::File appFolder()
    {
       #if JUCE_ANDROID
        // ver nota acima: derivar do "executavel" apontaria para /data/app.
        return managedDataFolder();
       #else
        auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile);

       #if JUCE_MAC
        // sobe de Contents/MacOS/exe ate a pasta que contem o .app
        auto bundle = exe.getParentDirectory().getParentDirectory().getParentDirectory();

        if (bundle.getFileExtension() == ".app")
            return bundle.getParentDirectory();

        return exe.getParentDirectory();
       #else
        return exe.getParentDirectory();
       #endif // JUCE_MAC
       #endif // JUCE_ANDROID
    }

    // Recursos embarcados. No Mac vivem dentro do bundle; no Windows, ao lado.
    static juce::File resourcesFolder()
    {
       #if JUCE_MAC
        auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
        auto res = exe.getParentDirectory().getSiblingFile ("Resources");

        if (res.isDirectory())
            return res;
       #endif

        return appFolder();
    }

    // Onde o instalador coloca as bibliotecas compartilhadas.
    //   Windows: C:\ProgramData\ChurchDAW
    //   macOS:   /Library/Application Support/ChurchDAW
    //   Android/iOS: sandbox/ChurchDAW (gerenciado pelo proprio app)
    static juce::File sharedDataFolder()
    {
       #if JUCE_ANDROID || JUCE_IOS
        return managedDataFolder();
       #endif

        return juce::File::getSpecialLocation (juce::File::commonApplicationDataDirectory)
                   .getChildFile ("ChurchDAW");
    }

    // Raiz privada e gravavel usada por downloads, cache e instalacoes atomicas.
    // No Android e /data/user/0/com.churchdaw.app/ChurchDAW; no iOS fica na
    // Library gravavel do sandbox, nunca na /Library global nem no bundle.
    static juce::File managedDataFolder()
    {
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("ChurchDAW");
    }

    static juce::File managedLibrariesFolder()
    {
        return managedDataFolder().getChildFile ("Libraries");
    }

    // Caminho da versao 1.1. Ele e somente uma fonte de compatibilidade; novos
    // packs nunca sao instalados aqui.
    static juce::File legacySharedDataFolder()
    {
       #if JUCE_ANDROID
        if (auto music = juce::File::getSpecialLocation (juce::File::userMusicDirectory);
            music != juce::File())
            return music.getChildFile ("ChurchDAW");
       #endif

        return {};
    }

    // Pasta de bibliotecas escolhida pelo usuario (fica no arquivo de config).
    // E o caminho que substitui os caminhos fixos de desenvolvimento: o usuario
    // aponta onde estao os samples dele e o app passa a achar em qualquer maquina.
    // VARIAS pastas, uma por linha. Ninguem guarda todas as bibliotecas de sample
    // num lugar so -- uma veio num pack, outra num instalador, outra num HD
    // externo. Exigir uma raiz unica obrigaria o usuario a reorganizar o disco.
    static juce::Array<juce::File> userLibraryRoots()
    {
        juce::Array<juce::File> out;

       #if JUCE_ANDROID
        // As versoes antigas permitiam escolher/copiar bibliotecas em
        // Music/ChurchDAW. Sem a permissao de audio o Android ainda deixa
        // enxergar parte da arvore, mas nega a abertura dos FLACs; tratar essa
        // pasta como valida faria o app anunciar um timbre com zero zonas.
        if (! juce::RuntimePermissions::isGranted (
                juce::RuntimePermissions::readMediaAudio))
            return out;
       #endif

        auto f = settingsFile();

        if (! f.existsAsFile())
            return out;

        juce::StringArray lines;
        lines.addLines (f.loadFileAsString());

        for (auto& l : lines)
            if (auto t = l.trim(); t.isNotEmpty() && ! t.startsWith ("#"))
                if (juce::File dir (t); dir.isDirectory())
                    out.add (dir);

        return out;
    }

    static juce::File userLibraryRoot()
    {
        auto roots = userLibraryRoots();
        return roots.isEmpty() ? juce::File() : roots.getFirst();
    }

    static void setUserLibraryRoots (const juce::Array<juce::File>& folders)
    {
        juce::StringArray lines;

        for (const auto& f : folders)
            lines.add (f.getFullPathName());

        auto f = settingsFile();
        f.getParentDirectory().createDirectory();
        f.replaceWithText (lines.joinIntoString ("\n"));
    }

    static void addUserLibraryRoot (const juce::File& folder)
    {
        auto roots = userLibraryRoots();

        if (folder.isDirectory() && ! roots.contains (folder))
        {
            roots.add (folder);
            setUserLibraryRoots (roots);
        }
    }

    // Procura uma biblioteca por nome, na ordem que faz sentido:
    //   1) sob a pasta escolhida pelo usuario
    //   2) ao lado do aplicativo (modo portatil / pendrive)
    //   3) onde o instalador colocou
    // Devolve File() se nao achar -- e o chamador decide o que fazer.
    static juce::File findLibrary (std::initializer_list<const char*> names)
    {
        for (const auto& root : userLibraryRoots())
        {
            // a propria pasta apontada pode SER a biblioteca
            for (const char* n : names)
                if (root.getFileName().equalsIgnoreCase (n))
                    return root;

            for (const char* n : names)
                if (auto f = root.getChildFile (n); f.isDirectory())
                    return f;
        }

        const auto app = appFolder();

        for (const char* n : names)
            if (auto f = app.getChildFile (n); f.isDirectory())
                return f;

        const auto shared = sharedDataFolder();

        for (const char* n : names)
            if (auto f = shared.getChildFile (n); f.isDirectory())
                return f;

       #if JUCE_ANDROID
        if (! juce::RuntimePermissions::isGranted (
                juce::RuntimePermissions::readMediaAudio))
            return {};
       #endif

        const auto legacy = legacySharedDataFolder();

        if (legacy.isDirectory())
            for (const char* n : names)
                if (auto f = legacy.getChildFile (n); f.isDirectory())
                    return f;

        return {};
    }

    // Busca em PROFUNDIDADE sob a raiz do usuario. Bibliotecas comerciais vem em
    // arvores fundas e com nomes proprios ("Live Pianos 4.0/Live Pianos 4.0/
    // SAMPLES"), entao exigir que a pasta esteja na raiz condenaria o usuario a
    // reorganizar o disco antes de usar o app.
    static juce::File findLibraryDeep (std::initializer_list<const char*> names, int maxDepth = 3)
    {
        // primeiro o caminho barato
        if (auto direct = findLibrary (names); direct.isDirectory())
            return direct;

        juce::Array<juce::File> level = userLibraryRoots();

        if (level.isEmpty())
            return {};

        for (int depth = 0; depth < maxDepth; ++depth)
        {
            juce::Array<juce::File> next;

            for (const auto& dir : level)
                for (const auto& child : dir.findChildFiles (juce::File::findDirectories, false))
                {
                    for (const char* n : names)
                        if (child.getFileName().equalsIgnoreCase (n))
                            return child;

                    next.add (child);
                }

            if (next.isEmpty())
                break;

            level = std::move (next);
        }

        return {};
    }

    static bool hasUserLibraryRoot()
    {
        return userLibraryRoot().isDirectory();
    }

private:
    static juce::File settingsFile()
    {
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("ChurchDAW")
                   .getChildFile ("library_root.txt");
    }
};
