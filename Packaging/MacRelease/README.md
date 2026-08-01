# Release comercial do ChurchDAW para macOS

O workflow `Release comercial macOS` e o unico fluxo deste repositorio que
produz um candidato macOS distribuivel. O workflow de CI usa assinatura ad-hoc,
marca o DMG como `NAO-PARA-DISTRIBUICAO` e nunca deve ser entregue a clientes.

## Gatilho e protecao

1. Crie uma tag imutavel no formato `vMAJOR.MINOR.PATCH`. A versao deve ser
   exatamente igual a `project(MinhaDAW VERSION ...)` no commit marcado.
2. Execute manualmente o workflow usando essa tag e selecione `RELEASE`.
3. O job usa o GitHub Environment `macos-production`. Configure nesse
   environment revisores obrigatorios e restrinja as tags que podem usa-lo.
4. O workflow apenas publica um artefato privado do GitHub Actions. Publicar o
   DMG no site ou criar uma GitHub Release e uma etapa externa e explicita.

O `CFBundleVersion` e o `github.run_number`, que cresce entre execucoes do
workflow. O `CFBundleShortVersionString` vem do projeto e precisa coincidir com
a tag.

## Secrets exigidos no environment `macos-production`

- `MACOS_CERTIFICATE_P12_BASE64`: certificado **Developer ID Application** e
  chave privada exportados como PKCS#12 e codificados em base64.
- `MACOS_CERTIFICATE_P12_PASSWORD`: senha do PKCS#12.
- `APPLE_TEAM_ID`: Team ID Apple de 10 caracteres.
- `APPLE_NOTARY_KEY_P8_BASE64`: chave privada da API do App Store Connect em
  base64.
- `APPLE_NOTARY_KEY_ID`: Key ID de 10 caracteres.
- `APPLE_NOTARY_ISSUER_ID`: Issuer ID da API.

O script cria um keychain temporario, seleciona exatamente uma identidade
Developer ID Application do Team ID esperado e apaga o keychain e os arquivos
de segredo ao sair.

## Portao fail-closed

O job falha se qualquer item abaixo nao puder ser provado:

- checkout limpo e exatamente no commit da tag;
- versao, bundle ID, build number, deployment target e descricoes de permissao
  de microfone/Bluetooth esperados;
- binario universal `arm64` + `x86_64`, sem dependencias do diretorio de build;
- testes CTest de audio, atualizacao e licenciamento, mais o teste nativo da
  assinatura de licenca, aprovados;
- assinatura de cada Mach-O e bundle de dentro para fora, sem `codesign --deep`
  para assinar;
- Developer ID Application, Team ID, certificado valido, timestamp seguro,
  Hardened Runtime e conjunto exato de entitlements;
- notarizacao aceita do app, ticket grampeado e Gatekeeper aprovado;
- DMG assinado, notarizado, grampeado, montado e revalidado;
- hash SHA-256 calculado somente depois do ultimo `stapler`.

Os artefatos finais ficam em `release-macos/artifacts/` e os diagnosticos em
`release-macos/logs/`. Nenhum resultado de CI ad-hoc satisfaz esse portao.
