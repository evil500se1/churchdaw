# Release comercial do ChurchDAW para macOS

O workflow `Release comercial macOS` produz o candidato comercial auditado por
commit e tag. O empacotador local `Packaging/BuildMacRelease.sh` tambem exige
Developer ID, notarizacao aceita, tickets grampeados e Gatekeeper aprovado no
app, no DMG e no app montado. Assinatura sozinha nao autoriza distribuicao.
Os builds de teste do CI e o modo local `-u` marcam o DMG como
`NAO-PARA-DISTRIBUICAO`; esses arquivos nunca devem ser entregues a clientes.

## Empacotamento local

```sh
bash Packaging/BuildMacRelease.sh -b build-mac -i IDENTIDADE_DEVELOPER_ID -p churchdaw -o dist-1.6.31
```

O perfil `churchdaw` precisa existir no Keychain. Para configura-lo, o titular
pode executar `xcrun notarytool store-credentials churchdaw` no Terminal e
preencher os prompts interativos. Nao inclua senhas em scripts ou historico.

Uma falha de notarizacao, stapler ou Gatekeeper encerra o script sem produzir
um DMG de release nem campos de policy. Os logs de notarizacao ficam no
diretorio de saida. O build original nao e modificado: toda assinatura ocorre
sobre uma copia temporaria.

O DMG tambem passa pela consulta `spctl --type install` usada pelos clientes
ate 1.6.30, alem da consulta `open` com `context:primary-signature` da nova
versao. Isso verifica a compatibilidade com o atualizador ja instalado.

A saida aprovada inclui `ChurchDAW-VERSAO.dmg.verification.json`. Esse laudo
contem o hash final (apos o stapler), tamanho, versao e os resultados de
verificacao. `PublishLocalDesktopRelease.ps1` exige esse arquivo por
`-MacVerificationReport` e compara o hash/tamanho antes de acessar credenciais
ou enviar qualquer arquivo. Nao reutilize um laudo de outro DMG.

Regressao local, sem acessar Apple/Keychain:
`python3 Tests/DesktopUpdate/MacReleasePipelineTests.py`.

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

O workflow serializa globalmente as releases macOS. Antes de expor qualquer
credencial, ele exige que o commit da tag seja igual ou ancestral de
`origin/main`. Depois da compilacao/notarizacao, consulta novamente a tag e o
branch `main` pela API do GitHub e so permite uploads se a proveniencia ainda
for valida.

## Secrets exigidos no environment `macos-production`

- `MACOS_CERTIFICATE_P12_BASE64`: certificado **Developer ID Application** e
  chave privada exportados como PKCS#12 e codificados em base64.
- `MACOS_CERTIFICATE_P12_PASSWORD`: senha do PKCS#12.
- `APPLE_TEAM_ID`: Team ID Apple de 10 caracteres.
- Variable `MACOS_EXPECTED_TEAM_ID`: o mesmo Team ID, fixado fora do secret
  para detectar troca acidental de identidade.
- Variable `MACOS_EXPECTED_CERTIFICATE_SHA256`: SHA-256 hexadecimal do DER do
  certificado Developer ID Application autorizado.
- `APPLE_NOTARY_KEY_P8_BASE64`: chave privada da API do App Store Connect em
  base64.
- `APPLE_NOTARY_KEY_ID`: Key ID de 10 caracteres.
- `APPLE_NOTARY_ISSUER_ID`: Issuer ID da API.

O script cria um keychain temporario, seleciona exatamente uma identidade
Developer ID Application do Team ID esperado, assina pelo SHA-1 integral dessa
identidade e exige que o SHA-256 do certificado leaf extraido de cada assinatura
seja o pin protegido. O keychain e os arquivos de segredo sao apagados ao sair.

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
- logs do notario com `status=Accepted` e sem `issues`, `problems` ou
  `warnings` nao vazios, tanto para o app quanto para o DMG;
- hash SHA-256 calculado somente depois do ultimo `stapler`.

Os artefatos finais ficam em `release-macos/artifacts/` e os diagnosticos em
`release-macos/logs/`. Nenhum resultado de CI ad-hoc satisfaz esse portao.
