# Data safety e conteúdo do app

Este arquivo é um roteiro de preenchimento, não uma declaração jurídica. As
respostas precisam continuar iguais ao comportamento da versão enviada.

## Comportamento confirmado no binário 1.3.0

- Sem SDK de anúncios.
- Sem analytics.
- Sem conta, login ou perfil de usuário.
- Sem permissão de microfone, localização, contatos, câmera ou identificador de
  publicidade.
- Arquivos locais de áudio são lidos somente no aparelho e não são enviados.
- MIDI é processado no aparelho.
- O app faz conexões HTTPS para política de atualização, catálogo e downloads.
- O domínio de hospedagem pode receber IP, horário, caminho e cabeçalhos técnicos
  necessários para responder às requisições.

## Data safety — respostas preparatórias

1. O app coleta ou compartilha dados obrigatórios do usuário?

   - Arquivos de áudio, MIDI, configurações e uso do app: **não saem do
     dispositivo**.
   - Metadados de conexão: confirmar no cPanel/HostGator e na Cloudflare, se ela
     estiver no caminho das requisições, se logs de acesso são retidos além do
     processamento efêmero.
   - Se os logs forem somente efêmeros e não reutilizados: o formulário pode
     declarar que o app não coleta nem compartilha dados.
   - Se IP/cabeçalhos forem armazenados ou usados para segurança/diagnóstico:
     declarar os tipos e finalidades exatos conforme a prática real. IP só deve
     ser marcado como localização aproximada se for usado para inferir
     localização.

2. Dados são compartilhados com terceiros?

   - Não há venda, publicidade ou compartilhamento comercial.
   - A infraestrutura HostGator e, se aplicável, Cloudflare processa a
     requisição como prestadora de serviço; confirmar contrato, retenção e
     finalidade antes da resposta final.

3. Dados são criptografados em trânsito?

   - **Sim** para os endpoints configurados: todos usam HTTPS.

4. Usuário pode pedir exclusão?

   - Não existe conta nem base de perfil.
   - Desinstalar remove configurações e bibliotecas privadas gerenciadas.
   - Arquivos legados em `Music/ChurchDAW` permanecem sob controle do usuário e
     podem ser apagados manualmente.

5. Política de privacidade:

   `https://churchdaw.com/politica-de-privacidade`

   O documento público ainda precisa usar a entidade responsável e um e-mail
   real de contato, além de refletir corretamente HostGator e Cloudflare. Não
   inventar esses dados.

## App content

- Anúncios: **não contém anúncios**.
- Acesso ao app: **exige conta/licença ou demonstração de 20 minutos**.
- Aplicativo de notícias: **não**.
- Saúde: **não**.
- Finanças: **não**.
- VPN: **não**.
- Governo: **não**.
- Compras digitais na versão 1.4.0: **não**. A expansão aparece apenas como
  “Em breve”; checkout externo está desabilitado no binário.
- Público-alvo: ferramenta musical profissional. Recomenda-se selecionar faixas
  a partir de 13 anos somente se esse for de fato o público pretendido. Não
  declarar que o app foi projetado para crianças.
- Classificação IARC: categoria “Utilitário, produtividade, comunicação ou
  outro”; responder honestamente “não” para violência, sexo, drogas, linguagem,
  apostas, interação entre usuários, localização compartilhada e conteúdo
  gerado por usuários, enquanto esses recursos não existirem.

## Permissões a justificar

- `INTERNET`: política de atualização, catálogo e downloads.
- `READ_MEDIA_AUDIO` / `READ_EXTERNAL_STORAGE` até API 32: migração opcional da
  biblioteca antiga em `Music/ChurchDAW`. O app só solicita após uma ação clara
  do usuário, não sonda a pasta antes da permissão, não move/copia/apaga os
  arquivos e oferece nova tentativa em `Bibliotecas`; novos downloads ficam
  privados.
- `CHANGE_WIFI_MULTICAST_STATE`: adicionada pelo JUCE quando internet está
  habilitada. A feature Wi-Fi é declarada opcional para não filtrar dispositivos.
- MIDI e USB host: entrada de controladores; USB host é opcional.

Referências:

- [Data safety](https://support.google.com/googleplay/android-developer/answer/10787469)
- [User Data](https://support.google.com/googleplay/android-developer/answer/10144311)
- [Público-alvo](https://support.google.com/googleplay/android-developer/answer/9867159)
- [Classificação de conteúdo](https://support.google.com/googleplay/android-developer/answer/9859655)
