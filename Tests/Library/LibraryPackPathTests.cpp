#include <iostream>

#include <juce_core/juce_core.h>

#include "ChurchDAWServices.h"

namespace
{
int fail (const char* message)
{
    std::cerr << "LIBRARY_PACK_PATH_FAIL: " << message << '\n';
    return 1;
}
}

int main()
{
    const auto root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getNonexistentChildFile (
                              "ChurchDAW-LibraryPackPathTests", {}, false);

    if (root.createDirectory().failed())
        return fail ("nao foi possivel criar a raiz temporaria");

    ChurchDAWServices::LibraryPack pad;
    pad.id = "pad-continuo";
    pad.installFolder = "Pads";
    pad.version = 1;

    const auto canonical = root.getChildFile ("Pads");
    const auto legacy = root.getChildFile ("pad-continuo");

    if (legacy.getChildFile ("Pads").createDirectory().failed())
        return fail ("nao foi possivel criar o layout legado");

    if (pad.detectedInstalledFolderAt (root) != legacy)
        return fail ("layout legado do Pad nao foi detectado");

    if (canonical.createDirectory().failed())
        return fail ("nao foi possivel criar o destino canonico");

    if (pad.detectedInstalledFolderAt (root) != canonical)
        return fail ("destino canonico nao teve precedencia");

    ChurchDAWServices::LibraryPack other;
    other.id = "church-live";
    other.installFolder = "Library";

    if (other.detectedInstalledFolderAt (root) != root.getChildFile ("Library"))
        return fail ("fallback de outro pack foi alterado");

    if (! root.deleteRecursively())
        return fail ("nao foi possivel limpar a fixture");

    std::cout << "LIBRARY_PACK_PATH_OK\n";
    return 0;
}
