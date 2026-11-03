# ChurchCam no iPhone e iPad

Esta atualização continua no app ChurchDAW existente, `com.churchdaw.app`, equipe `RH3476QHUV`. A consulta de leitura ao App Store Connect em 04/09/2026 confirmou a versão iOS 1.6.29 disponível e o último build 2026.8.18. O código prepara 1.6.32 (2026.9.5).

## Interface e gravação

- Botão ChurchCam direto na navegação, com controles adaptados para toque e recortes da tela.
- Duas composições: DAW completa com rosto, ou rosto com teclado 3D MIDI e marca ChurchDAW.
- Câmeras frontal e traseira, espelhamento e posição/tamanho do rosto na composição da DAW.
- Saída final da DAW no vídeo; microfone opcional, separado dos instrumentos e sem retorno nas caixas.
- 720p como padrão no iOS; 1080p selecionável. MP4 local em Documentos/ChurchCam.
- Compartilhamento pelo painel nativo do iOS, com os destinos disponíveis no aparelho.
- Durante a gravação, DAW reduz a prévia para permitir ajustes no mixer; Expandir retorna à tela completa.
- Ao entrar em segundo plano, a gravação finaliza e a câmera/microfone desligam. Os instrumentos mantêm o modo de áudio em segundo plano já existente. Ao voltar, câmera e microfone exigem nova ativação.

A implementação de câmera usa AVCaptureVideoDataOutput com sessão apenas de vídeo. O microfone usa o dispositivo de áudio principal, sem abrir um segundo AudioDeviceManager no iOS. O processamento da câmera e a codificação ocorrem fora do callback de áudio. A visualização usa o OpenGL ES do JUCE; a versão iOS do shader declara a precisão necessária.

## Compilação e validação

O Xcode precisa ter licença aceita e componentes iniciais instalados pelo titular. O script não aceita termos automaticamente:

```sh
bash Packaging/BuildIOSChurchCam.sh
```

O script configura o projeto Xcode para iOS e compila o alvo MinhaDAW para o simulador. Não envia builds à Apple. O workflow `.github/workflows/build-ios.yml` usa o mesmo contrato de identidade, versão, permissões e famílias de dispositivos, validado por `Tests/Studio/ValidateIOSBundle.py`.

Depois de validar no simulador, é necessário compilar e testar em aparelhos físicos usando os perfis do app existente. O simulador não substitui os testes de câmera, MIDI USB/Bluetooth, sessão de áudio e codificador do dispositivo.

Antes de TestFlight/App Store, verificar: câmera frontal/traseira e rotação em paisagem; permissões negadas; saída DAW com microfone desligado/ligado; fones e interface USB; troca/interrupção da rota de áudio; sustains e notas simultâneas no teclado 3D; gravação 720p/1080p; retorno ao mixer pela prévia pequena; compartilhamento; sair/voltar ao app; gravação longa e falta de armazenamento. O archive deve manter a identidade existente e evitar entitlements exclusivos do macOS e inter-app-audio.

## Notas para uma futura submissão

O ChurchCam pede câmera somente ao tocar Ligar webcam, e microfone somente ao tocar Ligar mic. Abrir o app ou o ChurchCam não solicita essas permissões. Os vídeos são processados e salvos localmente; o usuário escolhe se e onde compartilhar. A descrição de privacidade e as notas de revisão precisam refletir essas capacidades opcionais, substituindo afirmações antigas de ausência total de câmera/microfone. A classificação e os detalhes de privacidade devem ser conferidos no App Store Connect antes de submeter.

## Estado desta implementação

Em 05/09/2026, o Xcode compilou as versões para simulador e aparelho. Foram verificados iPhone 17 Pro e iPad Pro 11 (M5) no iOS 26.5, navegação direta, enquadramento, gravação com áudio real do Pad e compartilhamento. O teste também confirmou a preservação do último vídeo depois de uma reinstalação por atualização do simulador.

O archive 1.6.32 (2026.9.5), assinado com o perfil do app existente, foi exportado e validado pela Apple sem erros. O envio ao App Store Connect terminou com delivery UUID `6dab9ae2-2bc5-4224-a299-780d9d2d888c`. Isso confirma upload, não aprovação ou publicação na App Store. A Apple emitiu somente um aviso para elevar o mínimo de iOS 14 a iOS 15 nas submissões a partir da primavera de 2027.

Não havia aparelho físico conectado. Câmera física, MIDI USB/Bluetooth, rotas externas e gravação prolongada ainda precisam de validação em aparelho. O teste de câmera do simulador não substitui essas verificações.
