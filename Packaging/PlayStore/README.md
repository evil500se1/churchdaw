# ChurchDAW — pacote da Google Play

Esta pasta concentra os materiais de publicação do Android. Os arquivos finais
foram preparados para a ficha `pt-BR` do pacote `com.churchdaw.app`.

## Artes finais

| Arquivo | Uso | Especificação |
|---|---|---|
| `app-icon-512.png` | Ícone da ficha | PNG 32 bits, 512 × 512, abaixo de 1 MB |
| `feature-graphic-1024x500.png` | Arte de destaque | PNG 24 bits sem alpha, 1024 × 500 |
| `screenshot-01-performance-1920x1080.png` | Captura 1 | PNG 16:9, interface real do app |
| `screenshot-02-libraries-1920x1080.png` | Captura 2 | PNG 16:9, interface real do app |
| `screenshot-03-master-1920x1080.png` | Captura 3 | PNG 16:9, Master e reverb reais |
| `screenshot-04-channel-focus-1920x1080.png` | Captura 4 | PNG 16:9, canal real em foco |

As quatro capturas atendem ao conjunto recomendado para melhor elegibilidade
promocional. Todas usam telas reais do aparelho ARM64 final, sem mensagens de
diagnóstico e sem interface inventada.

Para regenerar as artes derivadas:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File .\Packaging\BuildPlayStoreAssets.ps1
```

Depois de qualquer alteração nas artes ou nos textos, rode o gate:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File .\Packaging\ValidatePlayStoreAssets.ps1
```

Ele valida dimensões, formato PNG, presença/ausência correta de alpha, tamanho
dos arquivos, limites da ficha, notas da versão e textos alternativos.

A arte de fundo foi criada com o gerador visual integrado e está preservada em
`feature-background-generated.png`. A interface mostrada na arte e nas capturas
é uma captura real do ChurchDAW, não uma UI inventada.

## Textos e formulários

- `listing-pt-BR.md`: nome, descrições e textos alternativos.
- `release-notes-pt-BR.txt`: notas da versão 1.3.0.
- `data-safety-and-app-content.md`: respostas preparatórias dos formulários.
- `submission-checklist.md`: ordem segura do primeiro envio e do teste de update.

## Fontes oficiais usadas

- [Requisitos das artes da ficha](https://support.google.com/googleplay/android-developer/answer/9866151)
- [Boas práticas e limites dos textos](https://support.google.com/googleplay/android-developer/answer/13393723)
- [Política de dados do usuário](https://support.google.com/googleplay/android-developer/answer/10144311)
- [Formulário Data safety](https://support.google.com/googleplay/android-developer/answer/10787469)
