# Checklist de envio — ChurchDAW Android 1.3.0

Estado preparado: `com.churchdaw.app`, versão `1.3.0`, `versionCode 3`.

## 1. Desbloquear a conta

- A conta pessoal de desenvolvedor **ChurchDaw** já foi criada.
- A tentativa de verificação dos documentos foi recusada: a Play Console mostra
  “problemas com as informações de verificação”.
- Repetir o envio com um documento oficial aceito, válido, em cores, legível,
  no formato original e com nome/endereço idênticos ao perfil de pagamentos;
  não enviar foto de tela, fotocópia ou arquivo editado.
- Enquanto a identidade não for aprovada, `Criar app` permanece desativado.
- Depois da aprovação, concluir a verificação de acesso a um aparelho Android
  físico e a verificação do telefone.
- Conta pessoal não exige D‑U‑N‑S. Não converter para organização nem iniciar
  cobrança adicional sem decisão explícita do proprietário.

## 2. Criar o aplicativo

- Nome: ChurchDAW.
- Idioma padrão: Português (Brasil).
- App ou jogo: App.
- Package ID: `com.churchdaw.app`.
- Decidir gratuito ou pago antes da primeira publicação. Bibliotecas digitais
  futuras devem usar Play Billing ou entitlement compatível, não pagamento
  externo dentro do app.

## 3. App Signing e artefato

- Ativar Play App Signing.
- Enviar o AAB release limpo `1.3.0` com `versionCode 3`.
- Antes do upload, confirmar que o `versionCode 3` ainda está livre na conta.
- Conferir se o certificado de upload informado pela Play coincide com:

  `6E:AC:FE:6B:E5:5C:20:6D:66:FC:D9:A6:65:BE:99:7F:82:EE:81:ED:3D:83:F2:F9:7F:F2:E4:F7:7D:71:01:AC`

- Guardar separadamente o certificado de App Signing criado pela Play.
- Não enviar à produção antes de validar a faixa interna.

## 4. Faixa interna

- Executar `VerifyAndroidRelease.ps1` no AAB final.
- Criar a faixa interna e enviar `versionCode 3`.
- Adicionar as contas de teste e instalar exclusivamente pelo link da faixa.
- APKs antigos instalados por ADB usam Android Debug e não atualizam diretamente
  para a assinatura ChurchDAW. Não alterar a chave oficial para contornar isso.
- Validar em aparelho ARM64 físico:
  - abertura e retorno do segundo plano;
  - ícone adaptativo;
  - MIDI USB real;
  - interface de áudio e áudio audível sem captura do microfone;
  - presets Cloud/Shimmer, envio por canal, Return master, persistência e PANIC;
  - download, pausa, retomada, cancelamento e recuperação;
  - pouco espaço, rede interrompida e reinício durante a instalação;
  - migração opcional de `Music/ChurchDAW`.

## 5. Provar o auto-update Play-only

1. manter o piso remoto abaixo ou igual à versão instalada;
2. instalar uma versão de base pela faixa interna;
3. enviar um AAB de teste com `versionCode` maior;
4. aguardar a Play oferecê-lo à mesma conta e ao mesmo aparelho;
5. somente então assinar e publicar uma política com o novo piso;
6. confirmar que cancelar mantém a DAW bloqueada;
7. concluir a atualização dentro do fluxo imediato da Play;
8. confirmar que o `PackageManager` libera o app apenas após instalar a versão
   suficiente;
9. restaurar ao final uma política de produção assinada e coerente.

Nunca eleve o piso antes de a nova versão estar disponível para todos os
usuários que serão bloqueados.

## 6. Ficha e artes

- Ícone 512 × 512 e feature graphic 1024 × 500: válidos.
- As quatro screenshots atuais são PNG RGB 1920 × 1080 sem alpha e estão
  prontas para a ficha.
- A captura de `Bibliotecas` já foi substituída pela tela final real: mostra
  `13.4 GB` e `Expansão` corretamente.
- Copiar os textos de `listing-pt-BR.md` após revisar cada promessa contra o
  binário testado.
- Informar um e-mail público de suporte real; não inventar contato.

## 7. Política, Data safety e App content

- URL pública canônica:
  `https://churchdaw.com/politica-de-privacidade.html`.
- A política ainda precisa confirmar a entidade responsável e um e-mail público
  de contato.
- Alinhar o texto com o uso de HostGator e, se estiver no caminho das
  requisições, Cloudflare.
- Confirmar retenção e finalidade de IP, horário, caminho e cabeçalhos nos logs
  da HostGator/Cloudflare antes de fechar o formulário Data safety.
- Não declarar “não coleta” até essa retenção estar confirmada.
- Preencher anúncios, acesso ao app, público-alvo, IARC e demais declarações de
  conteúdo.

## 8. Teste fechado e produção

- Por ser uma conta pessoal nova, preparar teste fechado com pelo menos 12
  testadores inscritos continuamente por 14 dias antes de pedir acesso à
  produção.
- Manter evidência dos testes e tratar falhas relatadas.
- Testar o Church Live de 13,4 GB ponta a ponta ou executar piloto controlado.
- Não prometer “sem bugs”, “latência zero” ou compatibilidade universal.
- Implementar Play Billing + entitlement antes de vender bibliotecas.
- Produção permanece bloqueada até conta, faixa interna, teste fechado,
  política, Data safety, conteúdo e testes físicos estarem concluídos.

Referências:

- [Verificação de contas de desenvolvedor](https://support.google.com/googleplay/android-developer/answer/10841920)
- [Teste interno](https://support.google.com/googleplay/android-developer/answer/9845334)
- [Requisito de teste para contas pessoais novas](https://support.google.com/googleplay/android-developer/answer/14151465)
