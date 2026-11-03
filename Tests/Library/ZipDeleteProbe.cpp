// Regressao do erro "o ZIP nao pode ser liberado" no instalador de
// bibliotecas do Windows.
//
// O instalador extraia a parte com um juce::ZipFile e, logo depois, chamava
// partFile.deleteFile() com esse ZipFile AINDA VIVO. Versoes/implementacoes do
// JUCE podem manter o FileInputStream aberto ou podem materializar o ZIP e
// liberar o handle depois da leitura.
//
// A garantia do produto independe desse detalhe interno: depois de destruir o
// ZipFile, a parte baixada TEM de ser apagavel. O bloco A registra o
// comportamento observado; o bloco B e o gate de regressao da correcao.

#include <juce_core/juce_core.h>

#include <iostream>
#include <memory>

namespace
{
    int failures = 0;

    void check (bool condition, const juce::String& name, const juce::String& detail = {})
    {
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

    // Monta um .zip pequeno com algumas entradas, como uma parte do pack.
    bool buildSampleZip (const juce::File& target)
    {
        juce::ZipFile::Builder builder;
        juce::OwnedArray<juce::MemoryBlock> blocks;

        for (int i = 0; i < 4; ++i)
        {
            auto* block = blocks.add (new juce::MemoryBlock());
            block->setSize (4096, true);

            builder.addEntry (
                new juce::MemoryInputStream (block->getData(), block->getSize(), false),
                9, "amostra-" + juce::String (i) + ".bin",
                juce::Time::getCurrentTime());
        }

        target.deleteFile();
        juce::FileOutputStream output (target);

        if (! output.openedOk())
            return false;

        double progress = 0.0;
        return builder.writeToStream (output, &progress);
    }
}

int main()
{
    const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                            .getChildFile ("churchdaw-zip-probe");
    folder.deleteRecursively();
    folder.createDirectory();

    const auto zip = folder.getChildFile ("parte-01.zip");
    const auto staging = folder.getChildFile ("staging");
    staging.createDirectory();

    if (! buildSampleZip (zip))
    {
        std::cerr << "nao consegui montar o zip de teste" << std::endl;
        return 2;
    }

    std::cout << "zip de teste: " << zip.getSize() << " bytes" << std::endl;

    //==========================================================================
    std::cout << "\nA. comportamento com o ZIP ainda aberto" << std::endl;

    {
        auto archive = std::make_unique<juce::ZipFile> (zip);
        check (archive->getNumEntries() == 4, "A1 zip abriu com 4 entradas",
               juce::String (archive->getNumEntries()));
        archive->uncompressTo (staging, true);

        const bool deleted = zip.deleteFile();

        std::cout << "  info A2 delete com ZipFile vivo: "
                  << (deleted ? "permitido" : "recusado") << std::endl;
    }

    //==========================================================================
    std::cout << "\nB. apagar depois de fechar (a correcao)" << std::endl;

    if (! zip.existsAsFile() && ! buildSampleZip (zip))
    {
        std::cerr << "nao consegui recriar o zip" << std::endl;
        return 2;
    }

    {
        auto archive = std::make_unique<juce::ZipFile> (zip);
        archive->uncompressTo (staging, true);
        archive.reset();                       // <- exatamente o que a correcao faz

        check (zip.deleteFile(), "B1 apaga sem erro depois de fechar");
        check (! zip.existsAsFile(), "B2 arquivo realmente sumiu");
    }

    folder.deleteRecursively();

    std::cout << "\n"
              << (failures == 0 ? "TODOS OS TESTES PASSARAM" : "TESTES REPROVARAM")
              << std::endl;

    return failures == 0 ? 0 : 1;
}
