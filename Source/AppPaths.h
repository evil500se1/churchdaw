#pragma once

#include <juce_core/juce_core.h>

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
//==============================================================================
struct AppPaths
{
    // Pasta "onde o aplicativo esta", do ponto de vista do usuario.
    static juce::File appFolder()
    {
        auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile);

       #if JUCE_MAC
        // sobe de Contents/MacOS/exe ate a pasta que contem o .app
        auto bundle = exe.getParentDirectory().getParentDirectory().getParentDirectory();

        if (bundle.getFileExtension() == ".app")
            return bundle.getParentDirectory();

        return exe.getParentDirectory();
       #else
        return exe.getParentDirectory();
       #endif
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
    static juce::File sharedDataFolder()
    {
        return juce::File::getSpecialLocation (juce::File::commonApplicationDataDirectory)
                   .getChildFile ("ChurchDAW");
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
