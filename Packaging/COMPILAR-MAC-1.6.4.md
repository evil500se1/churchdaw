# Compilar e publicar o ChurchDAW 1.6.4 no macOS

O Windows já está publicado (política serial 13, version code 10611). Falta o Mac.

Tudo o que a 1.6.4 tem está no bundle git — **o Mac ainda não tem esse código**.

---

## 1. Levar o código para o Mac

No Windows já existe: `D:\ChurchDAW-1.6.4.bundle` (380 MB, histórico completo).

Copie por pendrive, AirDrop, ou direto pela rede. No Mac:

```bash
# se ainda não existe repo no Mac
git clone /caminho/para/ChurchDAW-1.6.4.bundle ~/ChurchDAW
cd ~/ChurchDAW

# se JÁ existe um repo lá
cd ~/ChurchDAW
git pull /caminho/para/ChurchDAW-1.6.4.bundle
```

Confirme que veio a versão certa:

```bash
grep '^project(MinhaDAW' CMakeLists.txt      # deve dizer VERSION 1.6.4
git log --oneline -6
```

---

## 2. Compilar

```bash
cd ~/ChurchDAW
cmake -S . -B build-mac -G Xcode \
      -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
      -DCHURCHDAW_ENABLE_DESKTOP_LICENSING=ON \
      -DCHURCHDAW_BUILD_AUDIO_TESTS=ON

cmake --build build-mac --config Release --target MinhaDAW -j8
```

Rode os testes antes de empacotar (a suíte passa 16/16 no Windows):

```bash
ctest --test-dir build-mac -C Release --output-on-failure --no-tests=error
```

---

## 3. Empacotar o DMG

**Sem assinar** (só para testar localmente):

```bash
./Packaging/BuildMacRelease.sh -b build-mac
```

**Assinado e notarizado** (para distribuir — é o que o Gatekeeper e o
DesktopUpdater exigem):

```bash
./Packaging/BuildMacRelease.sh -b build-mac \
    -i "Developer ID Application: SEU NOME (TEAMID)" \
    -n "seu-apple-id@exemplo.com" \
    -t TEAMID
```

Para descobrir sua identidade e team id:

```bash
security find-identity -v -p codesigning
```

O script informa o **SHA-256** e o **tamanho** do DMG no final. Anote os dois:
a política de atualização precisa deles.

---

## 4. Publicar (rodar no Windows, onde está a chave da política)

Copie o `.dmg` de volta para o Windows e me avise. Eu faço:

1. Upload do DMG para `/app/desktop/` por FTP
2. Assinatura da política **serial 14** (encadeando com a 13 que está no ar),
   agora com Windows **e** Mac no mesmo envelope
3. Upload da política e verificação

A chave `churchdaw-update-policy-private.pem` fica no Windows — não precisa
copiá-la para o Mac.

---

## Atenção: version code

A 1.6.4 usa **10611**, não 10604. A 1.6.3 foi publicada com 10610 (fora da
fórmula do CMakeLists, que daria 10603), então existe um piso histórico no
`CMakeLists.txt` que eleva qualquer código ≤ 10610 para 10611.

O updater compara **códigos**, não nomes de versão. Se o Mac sair com um código
menor que o do Windows, o mesmo app teria duas idades diferentes conforme a
plataforma. O piso já está no código versionado — só confirme na saída do
configure:

```
-- ChurchDAW version code: 10611
```
