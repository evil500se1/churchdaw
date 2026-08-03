#include "LiveScenes.h"

#include <iostream>

//==============================================================================
// Sonda do caminho de ESCRITA do arquivo de cenas.
//
// Motivo desta sonda: numa migracao real o log registrou "cenas migradas" — ou
// seja, existsAsFile() devolveu true — e mesmo assim o arquivo nao estava la
// depois. Ou a escrita mente, ou algo apaga o arquivo em seguida. Esta sonda
// reproduz exatamente a sequencia de salvarCenas() fora da interface, onde
// nada mais pode interferir.
//==============================================================================
namespace
{
int falha (const char* mensagem)
{
    std::cerr << "SCENE_FILE_FAIL: " << mensagem << '\n';
    return 1;
}
}

int main()
{
    const auto pasta = juce::File::getSpecialLocation (juce::File::tempDirectory)
                         .getNonexistentChildFile ("ChurchDAW-SceneFileProbe", {}, false);

    if (pasta.createDirectory().failed())
        return falha ("nao foi possivel criar a pasta temporaria");

    const auto destino = pasta.getChildFile ("live-scenes.xml");

    juce::XmlElement xml ("LIVESCENES");
    xml.setAttribute ("schema", 1);
    xml.setAttribute ("active", 2);

    for (int i = 0; i < ChurchDAWLive::numeroDeCenas; ++i)
    {
        ChurchDAWLive::Cena c;

        if (i < 3)
        {
            c.preenchida = true;
            c.nome = "Cena " + juce::String (i + 1);
            c.padTom = i;
        }

        ChurchDAWLive::escreverCena (xml, c, i);
    }

    // --- exatamente o que salvarCenas() faz ---
    destino.getParentDirectory().createDirectory();

    bool escreveu = false;
    {
        juce::TemporaryFile temp (destino);
        escreveu = xml.writeTo (temp.getFile()) && temp.overwriteTargetFileWithTemporary();
    }   // <- o destrutor de TemporaryFile roda AQUI

    if (! escreveu)
        return falha ("a escrita relatou falha");

    // O ponto exato do bug: existsAsFile() logo apos a escrita.
    if (! destino.existsAsFile())
        return falha ("escrita relatou sucesso mas o arquivo nao existe");

    if (destino.getSize() <= 0)
        return falha ("arquivo criado vazio");

    // E depois que o TemporaryFile saiu de escopo, continua existindo?
    if (! destino.existsAsFile())
        return falha ("arquivo sumiu depois do destrutor do TemporaryFile");

    // Sobra algum temporario orfao na pasta?
    const auto restos = pasta.findChildFiles (juce::File::findFiles, false, "*temp*");

    if (! restos.isEmpty())
        return falha ("sobrou arquivo temporario orfao na pasta");

    // E o conteudo volta inteiro?
    auto lido = juce::parseXML (destino);

    if (lido == nullptr || ! lido->hasTagName ("LIVESCENES"))
        return falha ("o arquivo gravado nao volta como XML valido");

    if (lido->getIntAttribute ("active", -99) != 2)
        return falha ("a cena ativa nao sobreviveu ao arquivo");

    int preenchidas = 0;

    for (auto* c : lido->getChildWithTagNameIterator ("SCENE"))
    {
        ChurchDAWLive::Cena cena;
        ChurchDAWLive::lerCena (*c, cena);

        if (cena.preenchida)
            ++preenchidas;
    }

    if (preenchidas != 3)
        return falha ("numero de cenas gravadas nao bate na volta");

    // Regravar por cima tem de funcionar: no palco isso acontece toda troca.
    for (int volta = 0; volta < 3; ++volta)
    {
        juce::TemporaryFile temp (destino);

        if (! (xml.writeTo (temp.getFile()) && temp.overwriteTargetFileWithTemporary()))
            return falha ("regravacao por cima do arquivo existente falhou");

        if (! destino.existsAsFile())
            return falha ("arquivo sumiu numa regravacao");
    }

    // ------------------------------------------------------------------
    // REGRA DO BACKUP: um arquivo VAZIO nunca substitui um backup bom.
    //
    // Regressao real encontrada em auditoria: o codigo apagava o backup e SO
    // ENTAO copiava. Uma queda de energia no meio de uma gravacao deixa o
    // arquivo com 0 byte; copiar isso por cima destruia o unico vestigio
    // recuperavel das 10 cenas — justamente com o codigo escrito para
    // preserva-las. Esta sonda fixa a ordem correta.
    // ------------------------------------------------------------------
    {
        const auto original = pasta.getChildFile ("cenas.xml");
        const auto backup = pasta.getChildFile ("cenas.xml.corrompido");

        if (! original.replaceWithText ("<LIVESCENES schema=\"1\"><SCENE index=\"0\"/></LIVESCENES>"))
            return falha ("nao consegui montar a fixture do backup");

        // Primeiro resgate: ha conteudo, o backup nasce com ele.
        const auto novo = pasta.getChildFile ("cenas.xml.corrompido.novo");
        novo.deleteFile();

        if (! (original.getSize() > 0 && original.copyFileTo (novo)))
            return falha ("a copia do arquivo com conteudo falhou");

        backup.deleteFile();

        if (! novo.moveFileTo (backup))
            return falha ("nao consegui promover a copia a backup");

        const auto tamanhoBom = backup.getSize();

        if (tamanhoBom <= 0)
            return falha ("o backup nasceu vazio");

        // Agora o original vira 0 byte (queda no meio da gravacao).
        if (! original.replaceWithText ({}))
            return falha ("nao consegui zerar o arquivo da fixture");

        // A MESMA sequencia do app: com 0 byte, nada pode acontecer ao backup.
        novo.deleteFile();

        if (original.getSize() > 0 && original.copyFileTo (novo))
        {
            backup.deleteFile();
            novo.moveFileTo (backup);
        }

        if (! backup.existsAsFile())
            return falha ("arquivo de 0 byte destruiu o backup");

        if (backup.getSize() != tamanhoBom)
            return falha ("arquivo de 0 byte sobrescreveu o backup bom");

        if (novo.existsAsFile())
            return falha ("sobrou um .corrompido.novo orfao na pasta");

        // PROVA DE QUE ESTA SONDA DISCRIMINA.
        //
        // Um teste que passa tanto no codigo certo quanto no errado nao serve
        // de nada. Aqui reproduzimos a ordem ANTIGA (apaga o backup e so entao
        // copia) e exigimos que ela DESTRUA o backup — se um dia isso deixar
        // de acontecer, e porque a sonda parou de medir o que promete.
        {
            const auto vitima = pasta.getChildFile ("ordem-errada.bak");

            if (! vitima.replaceWithText ("conteudo bom que nao pode sumir"))
                return falha ("nao consegui montar a fixture da ordem errada");

            vitima.deleteFile();            // <- o erro
            original.copyFileTo (vitima);   //    (original esta com 0 byte)

            if (vitima.existsAsFile() && vitima.getSize() > 0)
                return falha ("a sonda nao discrimina: a ordem errada preservou o backup");
        }
    }

    // ------------------------------------------------------------------
    // XML principal corrompido nunca pode virar um banco vazio. A politica
    // usada por restaurarCenas()/salvarCenas() mantem o arquivo intocado ate
    // que exista um banco legado valido para migrar.
    // ------------------------------------------------------------------
    {
        const auto corrompido = pasta.getChildFile ("live-scenes-corrompido.xml");
        const juce::String conteudoOriginal = "<LIVESCENES><SCENE index=\"0\"";

        if (! corrompido.replaceWithText (conteudoOriginal))
            return falha ("nao consegui montar XML corrompido");

        const auto xml = juce::parseXML (corrompido);
        const bool ilegivel = xml == nullptr || ! xml->hasTagName ("LIVESCENES");
        const bool bloqueado = ChurchDAWLive::deveBloquearGravacaoAposArquivoIlegivel (
            ilegivel, false);

        if (! bloqueado)
        {
            juce::XmlElement bancoVazio ("LIVESCENES");
            bancoVazio.writeTo (corrompido);
        }

        if (corrompido.loadFileAsString() != conteudoOriginal)
            return falha ("XML corrompido foi sobrescrito por banco vazio");

        if (ChurchDAWLive::deveBloquearGravacaoAposArquivoIlegivel (
                ilegivel, true))
            return falha ("banco legado valido nao liberou migracao protegida");
    }

    pasta.deleteRecursively();

    std::cout << "SCENE_FILE_OK\n";
    return 0;
}
