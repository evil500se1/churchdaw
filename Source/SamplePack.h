#pragma once

#include <juce_core/juce_core.h>

#include <cstring>
#include <memory>
#include <vector>

//==============================================================================
// CONTEINER DE SAMPLES ".chpack"
//
// Empacota a biblioteca inteira num arquivo unico e cifrado, para o instalador
// levar os samples embutidos sem que o comprador consiga simplesmente arrastar
// a pasta para um pendrive.
//
// ATE ONDE ISSO PROTEGE -- ler antes de prometer qualquer coisa a terceiros:
// nenhum esquema deste tipo e inquebravel, e a razao e matematica, nao de
// implementacao: para tocar o som o programa precisa decifrar, entao a chave
// mora dentro do binario que esta na maquina do comprador. Quem sabe usar um
// depurador chega nela. O ".ncw" da Native Instruments e exatamente esta ideia,
// e foi decodificado neste mesmo projeto em uma sessao.
//
// O que este formato ENTREGA de verdade:
//   1. acaba com a copia casual (o caso comum: copiar a pasta para um amigo);
//   2. o pacote so abre com a licenca do comprador;
//   3. a identificacao do comprador e ESTRUTURAL, nao um carimbo removivel:
//      ela entra na derivacao das chaves, entao apagar ou trocar o campo
//      simplesmente impede a decifragem em vez de "limpar" o arquivo.
//
// FORMATO (little-endian):
//   "CHPK"        4     magica
//   version       4     = 1
//   flags         4     reservado
//   buyerId      64     identificacao do comprador (UTF-8, com zeros a direita)
//   salt         16     aleatorio por pacote
//   indexOffset   8
//   indexSize     8     tamanho do indice JA cifrado
//   indexTag     32     SHA-256 do indice em claro (detecta adulteracao)
//   ...blobs...         cada arquivo cifrado com seu proprio contador
//   indice (cifrado):
//       count     4
//       por item: pathLen 4 | path (UTF-8) | offset 8 | size 8 | tag 32
//
// Os samples so existem decifrados em MEMORIA -- nada e escrito em claro no
// disco em momento algum.
//==============================================================================

namespace chpack
{

//==============================================================================
// ChaCha20 (RFC 8439). Cifra de fluxo: o mesmo caminho cifra e decifra.
// Escolhida no lugar do BlowFish do juce_cryptography por ser mais forte, mais
// rapida e por caber aqui sem acrescentar modulo ao CMake -- o projeto esta em
// mudanca constante e nao vale a pena mexer no build por causa disto.
//==============================================================================
class ChaCha20
{
public:
    // key = 32 bytes, nonce = 12 bytes
    ChaCha20 (const juce::uint8* key, const juce::uint8* nonce, juce::uint32 counter = 0) noexcept
    {
        static const juce::uint32 sigma[4] = { 0x61707865, 0x3320646e, 0x79622d32, 0x6b206574 };

        for (int i = 0; i < 4; ++i)
            state[i] = sigma[i];

        for (int i = 0; i < 8; ++i)
            state[4 + i] = readLE (key + i * 4);

        state[12] = counter;

        for (int i = 0; i < 3; ++i)
            state[13 + i] = readLE (nonce + i * 4);
    }

    // Cifra/decifra no lugar. Pode ser chamada varias vezes em sequencia: o
    // contador do bloco continua de onde parou, entao processar um arquivo em
    // pedacos da o mesmo resultado que processar de uma vez.
    void process (juce::uint8* data, size_t numBytes) noexcept
    {
        while (numBytes > 0)
        {
            if (keyStreamPos == 0 || keyStreamPos >= 64)
                generateBlock();

            const size_t take = juce::jmin (numBytes, (size_t) 64 - keyStreamPos);

            for (size_t i = 0; i < take; ++i)
                data[i] ^= keyStream[keyStreamPos + i];

            data += take;
            numBytes -= take;
            keyStreamPos += take;
        }
    }

private:
    static juce::uint32 readLE (const juce::uint8* p) noexcept
    {
        return (juce::uint32) p[0] | ((juce::uint32) p[1] << 8)
             | ((juce::uint32) p[2] << 16) | ((juce::uint32) p[3] << 24);
    }

    static juce::uint32 rotl (juce::uint32 x, int n) noexcept
    {
        return (x << n) | (x >> (32 - n));
    }

    static void quarterRound (juce::uint32& a, juce::uint32& b,
                              juce::uint32& c, juce::uint32& d) noexcept
    {
        a += b; d ^= a; d = rotl (d, 16);
        c += d; b ^= c; b = rotl (b, 12);
        a += b; d ^= a; d = rotl (d, 8);
        c += d; b ^= c; b = rotl (b, 7);
    }

    void generateBlock() noexcept
    {
        juce::uint32 w[16];

        for (int i = 0; i < 16; ++i)
            w[i] = state[i];

        for (int i = 0; i < 10; ++i)   // 20 rodadas = 10 rodadas duplas
        {
            quarterRound (w[0], w[4], w[8],  w[12]);
            quarterRound (w[1], w[5], w[9],  w[13]);
            quarterRound (w[2], w[6], w[10], w[14]);
            quarterRound (w[3], w[7], w[11], w[15]);

            quarterRound (w[0], w[5], w[10], w[15]);
            quarterRound (w[1], w[6], w[11], w[12]);
            quarterRound (w[2], w[7], w[8],  w[13]);
            quarterRound (w[3], w[4], w[9],  w[14]);
        }

        for (int i = 0; i < 16; ++i)
        {
            const juce::uint32 v = w[i] + state[i];
            keyStream[i * 4 + 0] = (juce::uint8) (v & 0xff);
            keyStream[i * 4 + 1] = (juce::uint8) ((v >> 8)  & 0xff);
            keyStream[i * 4 + 2] = (juce::uint8) ((v >> 16) & 0xff);
            keyStream[i * 4 + 3] = (juce::uint8) ((v >> 24) & 0xff);
        }

        ++state[12];        // proximo bloco
        keyStreamPos = 0;
    }

    juce::uint32 state[16] {};
    juce::uint8 keyStream[64] {};
    size_t keyStreamPos = 64;   // forca gerar o primeiro bloco
};

//==============================================================================
// SHA-256 (FIPS 180-4). Usado para derivar chaves e para o selo de integridade
// de cada arquivo -- sem ele, um pacote corrompido no meio do caminho viraria
// ruido branco no meio de um culto em vez de um erro claro.
//==============================================================================
class SHA256
{
public:
    SHA256() noexcept { reset(); }

    void update (const void* data, size_t len) noexcept
    {
        auto* p = static_cast<const juce::uint8*> (data);
        totalBits += (juce::uint64) len * 8;

        while (len > 0)
        {
            const size_t take = juce::jmin (len, (size_t) 64 - bufferLen);
            std::memcpy (buffer + bufferLen, p, take);
            bufferLen += take;
            p += take;
            len -= take;

            if (bufferLen == 64)
            {
                transform (buffer);
                bufferLen = 0;
            }
        }
    }

    // escreve 32 bytes em out
    void finish (juce::uint8* out) noexcept
    {
        const juce::uint64 bits = totalBits;

        const juce::uint8 pad = 0x80;
        update (&pad, 1);

        const juce::uint8 zero = 0;
        while (bufferLen != 56)
            update (&zero, 1);

        juce::uint8 lenBytes[8];
        for (int i = 0; i < 8; ++i)
            lenBytes[i] = (juce::uint8) ((bits >> (56 - i * 8)) & 0xff);

        // nao usar update() aqui: ele somaria estes 8 bytes ao total
        std::memcpy (buffer + bufferLen, lenBytes, 8);
        transform (buffer);

        for (int i = 0; i < 8; ++i)
        {
            out[i * 4 + 0] = (juce::uint8) ((h[i] >> 24) & 0xff);
            out[i * 4 + 1] = (juce::uint8) ((h[i] >> 16) & 0xff);
            out[i * 4 + 2] = (juce::uint8) ((h[i] >> 8)  & 0xff);
            out[i * 4 + 3] = (juce::uint8) (h[i] & 0xff);
        }

        reset();
    }

    static void hash (const void* data, size_t len, juce::uint8* out) noexcept
    {
        SHA256 s;
        s.update (data, len);
        s.finish (out);
    }

private:
    void reset() noexcept
    {
        static const juce::uint32 init[8] =
            { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
              0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };

        for (int i = 0; i < 8; ++i)
            h[i] = init[i];

        bufferLen = 0;
        totalBits = 0;
    }

    static juce::uint32 rotr (juce::uint32 x, int n) noexcept
    {
        return (x >> n) | (x << (32 - n));
    }

    void transform (const juce::uint8* block) noexcept
    {
        static const juce::uint32 k[64] =
        {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
        };

        juce::uint32 w[64];

        for (int i = 0; i < 16; ++i)
            w[i] = ((juce::uint32) block[i * 4] << 24) | ((juce::uint32) block[i * 4 + 1] << 16)
                 | ((juce::uint32) block[i * 4 + 2] << 8) | (juce::uint32) block[i * 4 + 3];

        for (int i = 16; i < 64; ++i)
        {
            const juce::uint32 s0 = rotr (w[i - 15], 7) ^ rotr (w[i - 15], 18) ^ (w[i - 15] >> 3);
            const juce::uint32 s1 = rotr (w[i - 2], 17) ^ rotr (w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        juce::uint32 a = h[0], b = h[1], c = h[2], d = h[3];
        juce::uint32 e = h[4], f = h[5], g = h[6], hh = h[7];

        for (int i = 0; i < 64; ++i)
        {
            const juce::uint32 S1 = rotr (e, 6) ^ rotr (e, 11) ^ rotr (e, 25);
            const juce::uint32 ch = (e & f) ^ ((~e) & g);
            const juce::uint32 t1 = hh + S1 + ch + k[i] + w[i];
            const juce::uint32 S0 = rotr (a, 2) ^ rotr (a, 13) ^ rotr (a, 22);
            const juce::uint32 maj = (a & b) ^ (a & c) ^ (b & c);
            const juce::uint32 t2 = S0 + maj;

            hh = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }

        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    juce::uint32 h[8] {};
    juce::uint8 buffer[64] {};
    size_t bufferLen = 0;
    juce::uint64 totalBits = 0;
};

//==============================================================================
// Derivacao da chave.
//
// Mistura tres coisas: o segredo do produto (embutido no binario), a licenca do
// comprador e o salt do pacote. As 200 mil iteracoes nao existem para deter
// quem depura o programa -- essa pessoa pega a chave pronta na memoria de
// qualquer jeito -- e sim para tornar inviavel testar licencas em massa contra
// um pacote vazado.
//==============================================================================
inline void deriveKey (const juce::String& productSecret,
                       const juce::String& licenseKey,
                       const juce::uint8* salt, size_t saltLen,
                       const juce::String& buyerId,
                       juce::uint8* outKey32) noexcept
{
    SHA256 s;

    const auto secret = productSecret.toRawUTF8();
    const auto lic    = licenseKey.trim().toUpperCase().toRawUTF8();
    const auto buyer  = buyerId.toRawUTF8();

    s.update (secret, std::strlen (secret));
    s.update (lic,    std::strlen (lic));
    s.update (salt,   saltLen);
    // o comprador entra na CHAVE: adulterar o campo no cabecalho nao "limpa" a
    // marca, apenas faz o pacote parar de abrir
    s.update (buyer,  std::strlen (buyer));
    s.finish (outKey32);

    juce::uint8 tmp[32];

    for (int i = 0; i < 200000; ++i)
    {
        std::memcpy (tmp, outKey32, 32);
        SHA256::hash (tmp, 32, outKey32);
    }
}

//==============================================================================
// Cabecalho e indice
//==============================================================================
static constexpr const char* magic = "CHPK";
static constexpr juce::uint32 formatVersion = 1;
static constexpr int buyerIdSize = 64;
static constexpr int saltSize = 16;
static constexpr int tagSize = 32;

struct Entry
{
    juce::String path;          // caminho relativo original ("Imported/Black Upright/C3-v90.aif")
    juce::uint64 offset = 0;    // posicao do blob cifrado no arquivo
    juce::uint64 size = 0;      // tamanho em claro
    juce::uint8 tag[tagSize] {};  // SHA-256 do conteudo em claro
};

// Nonce de cada arquivo: deriva do indice, entao nao precisa ser guardado, e
// dois arquivos nunca reusam o mesmo fluxo de chave (que e o unico jeito de
// quebrar uma cifra de fluxo sem saber a chave).
inline void nonceForIndex (juce::uint64 index, juce::uint8* out12) noexcept
{
    juce::uint8 h[32];
    juce::uint8 in[8];

    for (int i = 0; i < 8; ++i)
        in[i] = (juce::uint8) ((index >> (i * 8)) & 0xff);

    SHA256::hash (in, 8, h);
    std::memcpy (out12, h, 12);
}

//==============================================================================
// LEITOR -- e o que vai dentro do ChurchDAW.
//
// Os samples so aparecem decifrados em MEMORIA: createStreamFor devolve um
// MemoryInputStream, que o juce::AudioFormatManager le igual a um arquivo. Isso
// mantem a integracao pequena -- decodeZones passa a receber um stream em vez
// de um juce::File, e nada mais muda.
//==============================================================================
class Reader
{
public:
    Reader() = default;

    // Devolve false se o arquivo nao existir, nao for um .chpack, ou se a
    // licenca estiver errada (o indice nao passa no selo de integridade).
    bool open (const juce::File& packFile,
               const juce::String& productSecret,
               const juce::String& licenseKey)
    {
        entries.clear();
        file = packFile;

        stream = packFile.createInputStream();

        if (stream == nullptr)
            return fail ("pacote nao encontrado");

        char m[4] {};
        if (stream->read (m, 4) != 4 || std::memcmp (m, magic, 4) != 0)
            return fail ("nao e um pacote ChurchDAW");

        const auto version = (juce::uint32) stream->readInt();

        if (version != formatVersion)
            return fail ("versao de pacote incompativel");

        stream->readInt();   // flags

        char buyerRaw[buyerIdSize + 1] {};
        stream->read (buyerRaw, buyerIdSize);
        buyer = juce::String::fromUTF8 (buyerRaw);

        juce::uint8 salt[saltSize];
        stream->read (salt, saltSize);

        const auto indexOffset = (juce::uint64) stream->readInt64();
        const auto indexSize   = (juce::uint64) stream->readInt64();

        juce::uint8 indexTag[tagSize];
        stream->read (indexTag, tagSize);

        if (indexSize == 0 || indexSize > 64u * 1024u * 1024u)
            return fail ("indice invalido");

        deriveKey (productSecret, licenseKey, salt, saltSize, buyer, key);

        // decifra o indice
        std::vector<juce::uint8> idx ((size_t) indexSize);
        stream->setPosition ((juce::int64) indexOffset);

        if (stream->read (idx.data(), (int) indexSize) != (int) indexSize)
            return fail ("pacote truncado");

        juce::uint8 idxNonce[12];
        nonceForIndex (0xFFFFFFFFFFFFFFFFull, idxNonce);   // o indice tem nonce proprio
        ChaCha20 (key, idxNonce).process (idx.data(), idx.size());

        juce::uint8 check[tagSize];
        SHA256::hash (idx.data(), idx.size(), check);

        // Licenca errada da EXATAMENTE aqui: a chave derivada e outra, o indice
        // sai como ruido e o selo nao bate. Nao ha como distinguir "licenca
        // errada" de "arquivo corrompido" -- e nem interessa distinguir, porque
        // isso ajudaria quem esta tentando adivinhar licenca.
        if (std::memcmp (check, indexTag, tagSize) != 0)
            return fail ("licenca invalida ou pacote corrompido");

        juce::MemoryInputStream in (idx.data(), idx.size(), false);
        const int count = in.readInt();

        if (count < 0 || count > 5000000)
            return fail ("indice invalido");

        entries.reserve ((size_t) count);

        for (int i = 0; i < count; ++i)
        {
            Entry e;
            const int pathLen = in.readInt();

            if (pathLen <= 0 || pathLen > 4096)
                return fail ("indice invalido");

            juce::MemoryBlock pb ((size_t) pathLen);
            in.read (pb.getData(), pathLen);
            e.path = juce::String::fromUTF8 ((const char*) pb.getData(), pathLen);

            e.offset = (juce::uint64) in.readInt64();
            e.size   = (juce::uint64) in.readInt64();
            in.read (e.tag, tagSize);

            entries.push_back (std::move (e));
        }

        opened = true;
        error = {};
        return true;
    }

    bool isOpen() const noexcept              { return opened; }
    juce::String getError() const             { return error; }
    juce::String getBuyerId() const           { return buyer; }
    int getNumEntries() const noexcept        { return (int) entries.size(); }
    const Entry& getEntry (int i) const       { return entries[(size_t) i]; }

    juce::StringArray getPaths() const
    {
        juce::StringArray out;

        for (const auto& e : entries)
            out.add (e.path);

        return out;
    }

    int indexOf (const juce::String& path) const
    {
        for (size_t i = 0; i < entries.size(); ++i)
            if (entries[i].path.equalsIgnoreCase (path))
                return (int) i;

        return -1;
    }

    // Decifra UM sample para a memoria. Devolve nullptr se o selo nao bater
    // (pacote corrompido) -- melhor um canal mudo com erro no log do que ruido
    // branco em cima do culto.
    std::unique_ptr<juce::InputStream> createStreamFor (int entryIndex)
    {
        if (! opened || ! juce::isPositiveAndBelow (entryIndex, (int) entries.size()))
            return {};

        const auto& e = entries[(size_t) entryIndex];

        if (e.size > 512u * 1024u * 1024u)
            return {};

        juce::MemoryBlock data ((size_t) e.size);

        {
            const juce::ScopedLock sl (readLock);   // varios canais carregam em paralelo

            stream->setPosition ((juce::int64) e.offset);

            if (stream->read (data.getData(), (int) e.size) != (int) e.size)
                return {};
        }

        juce::uint8 nonce[12];
        nonceForIndex ((juce::uint64) entryIndex, nonce);
        ChaCha20 (key, nonce).process ((juce::uint8*) data.getData(), data.getSize());

        juce::uint8 check[tagSize];
        SHA256::hash (data.getData(), data.getSize(), check);

        if (std::memcmp (check, e.tag, tagSize) != 0)
            return {};

        return std::make_unique<juce::MemoryInputStream> (std::move (data));
    }

    std::unique_ptr<juce::InputStream> createStreamFor (const juce::String& path)
    {
        return createStreamFor (indexOf (path));
    }

private:
    bool fail (const juce::String& why)
    {
        error = why;
        opened = false;
        entries.clear();
        stream.reset();
        return false;
    }

    juce::File file;
    std::unique_ptr<juce::InputStream> stream;
    juce::CriticalSection readLock;
    std::vector<Entry> entries;
    juce::uint8 key[32] {};
    juce::String buyer, error;
    bool opened = false;
};

//==============================================================================
// GRAVADOR -- NAO vai no produto final.
//
// Fica aqui junto do leitor de proposito: os dois lados do formato tem de mudar
// juntos, e formato de arquivo que mora em dois lugares diferentes sempre acaba
// divergindo. Para gerar um pacote, chamar de um modo de linha de comando do
// proprio ChurchDAW (algo como "--pack <pasta> <saida> <licenca> <comprador>"),
// assim gravador e leitor compartilham exatamente este codigo.
//==============================================================================
struct WriteResult
{
    bool ok = false;
    juce::String message;
    juce::int64 bytesIn = 0, bytesOut = 0;
    int numFiles = 0;
};

inline WriteResult write (const juce::File& sourceFolder,
                          const juce::File& destFile,
                          const juce::String& productSecret,
                          const juce::String& licenseKey,
                          const juce::String& buyerId,
                          const juce::StringArray& extensions = { "*.aif", "*.aiff", "*.wav", "*.flac" },
                          std::function<void (int done, int total, const juce::String&)> onProgress = {})
{
    WriteResult r;

    if (! sourceFolder.isDirectory())
    {
        r.message = "pasta de origem nao existe";
        return r;
    }

    juce::Array<juce::File> files;

    for (const auto& ext : extensions)
        files.addArray (sourceFolder.findChildFiles (juce::File::findFiles, true, ext));

    if (files.isEmpty())
    {
        r.message = "nenhum sample encontrado";
        return r;
    }

    files.sort();

    destFile.deleteFile();
    destFile.getParentDirectory().createDirectory();

    auto out = destFile.createOutputStream();

    if (out == nullptr)
    {
        r.message = "nao consegui criar o arquivo de saida";
        return r;
    }

    juce::Random rng (juce::Time::getHighResolutionTicks());
    juce::uint8 salt[saltSize];

    for (int i = 0; i < saltSize; ++i)
        salt[i] = (juce::uint8) rng.nextInt (256);

    juce::uint8 key[32];
    deriveKey (productSecret, licenseKey, salt, saltSize, buyerId, key);

    // cabecalho (indexOffset/indexSize/indexTag entram depois, quando se sabe)
    const juce::int64 headerSize = 4 + 4 + 4 + buyerIdSize + saltSize + 8 + 8 + tagSize;

    for (juce::int64 i = 0; i < headerSize; ++i)
        out->writeByte (0);

    std::vector<Entry> entries;
    entries.reserve ((size_t) files.size());

    for (int i = 0; i < files.size(); ++i)
    {
        const auto& f = files.getReference (i);

        juce::MemoryBlock data;

        if (! f.loadFileAsData (data) || data.getSize() == 0)
            continue;

        Entry e;
        e.path = f.getRelativePathFrom (sourceFolder).replaceCharacter ('\\', '/');
        e.size = (juce::uint64) data.getSize();
        e.offset = (juce::uint64) out->getPosition();
        SHA256::hash (data.getData(), data.getSize(), e.tag);

        juce::uint8 nonce[12];
        nonceForIndex ((juce::uint64) entries.size(), nonce);
        ChaCha20 (key, nonce).process ((juce::uint8*) data.getData(), data.getSize());

        out->write (data.getData(), data.getSize());

        r.bytesIn += (juce::int64) e.size;
        entries.push_back (std::move (e));

        if (onProgress && (i % 50 == 0))
            onProgress (i, files.size(), f.getFileName());
    }

    // indice
    juce::MemoryOutputStream idx;
    idx.writeInt ((int) entries.size());

    for (const auto& e : entries)
    {
        const auto utf8 = e.path.toRawUTF8();
        const int len = (int) std::strlen (utf8);
        idx.writeInt (len);
        idx.write (utf8, (size_t) len);
        idx.writeInt64 ((juce::int64) e.offset);
        idx.writeInt64 ((juce::int64) e.size);
        idx.write (e.tag, tagSize);
    }

    juce::MemoryBlock idxData (idx.getData(), idx.getDataSize());

    juce::uint8 indexTag[tagSize];
    SHA256::hash (idxData.getData(), idxData.getSize(), indexTag);

    juce::uint8 idxNonce[12];
    nonceForIndex (0xFFFFFFFFFFFFFFFFull, idxNonce);
    ChaCha20 (key, idxNonce).process ((juce::uint8*) idxData.getData(), idxData.getSize());

    const juce::uint64 indexOffset = (juce::uint64) out->getPosition();
    out->write (idxData.getData(), idxData.getSize());
    out->flush();
    out.reset();

    // volta e escreve o cabecalho de verdade
    {
        juce::FileOutputStream head (destFile);

        if (head.failedToOpenFile())
        {
            r.message = "nao consegui gravar o cabecalho";
            return r;
        }

        head.setPosition (0);
        head.write (magic, 4);
        head.writeInt ((int) formatVersion);
        head.writeInt (0);   // flags

        char buyerRaw[buyerIdSize] {};
        const auto b = buyerId.toRawUTF8();
        std::strncpy (buyerRaw, b, buyerIdSize - 1);
        head.write (buyerRaw, buyerIdSize);

        head.write (salt, saltSize);
        head.writeInt64 ((juce::int64) indexOffset);
        head.writeInt64 ((juce::int64) idxData.getSize());
        head.write (indexTag, tagSize);
        head.flush();
    }

    r.ok = true;
    r.numFiles = (int) entries.size();
    r.bytesOut = destFile.getSize();
    r.message = "pacote gerado";
    return r;
}

} // namespace chpack
