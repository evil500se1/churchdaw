(() => {
  'use strict';

  const androidScreens = {
    palco: [
      'Palco ao vivo com faders de curso longo.',
      'Até 6 camadas simultâneas visíveis com medidores individuais, Mute/Solo, Master Limiter e Panic instantâneo.',
      'Palco ao vivo do ChurchDAW no Android com Tonewheel Organ, Worship Hybrid, Pipe Organ e Master',
      'DESIGNER PREVIEW · SHELL ANDROID'
    ],
    canal: [
      'Edição dedicada por canal, feita para o toque.',
      'Ajuste oitava, transpose, compressor, reverb, delay, rotary e equalizador sem sair da música.',
      'Modo Editar Canal no Android com controles de oitava, transpose, compressor, reverb, delay e rotary',
      'DESIGNER PREVIEW · SHELL ANDROID'
    ],
    timbres: [
      'Encontre e ouça qualquer timbre antes de carregar.',
      'Filtre pianos, órgãos, pads, bells, strings e leads com busca rápida, favoritos e prévia com volume próprio.',
      'Seletor de instrumentos no Android com categorias, busca, favoritos e barra de prévia de áudio',
      'DESIGNER PREVIEW · SHELL ANDROID'
    ],
    cenas: [
      '10 Cenas Live prontas para cada momento do culto.',
      'Salve combinações completas de timbres e volumes para abertura, louvor e ministração com troca imediata.',
      'Barra de 10 Cenas Live aberta sobre o mixer de palco no Android',
      'DESIGNER PREVIEW · SHELL ANDROID'
    ],
    pad: [
      'Pad contínuo nos 12 tons com filtro ao vivo.',
      'Transições suaves sem cortar a ambiência anterior, com controle dedicado de volume e abertura de corte.',
      'Painel de Pad Contínuo nos 12 tons com controles de volume e corte de filtro no Android',
      'DESIGNER PREVIEW · SHELL ANDROID'
    ],
    midi: [
      'MIDI Learn visual direto na tela do celular.',
      'Toque no controle desejado, mova o fader, knob ou pedal do seu teclado USB/Bluetooth e saia tocando.',
      'Modo Mapear MIDI passo a passo sobre o console de palco no Android',
      'DESIGNER PREVIEW · SHELL ANDROID'
    ],
    churchcam: [
      'ChurchCam agora também no Android.',
      'Enquadre DAW + rosto ou Rosto + teclas em 720p/1080p, grave em MP4 com o som limpo da sessão e compartilhe.',
      'ChurchCam no Android com prévia ao vivo da mesa ChurchDAW e seletor de câmera frontal',
      'DESIGNER PREVIEW · SHELL ANDROID'
    ],
    biblioteca: [
      'Suas bibliotecas oficiais para tocar sem Wi-Fi.',
      'Baixe o pacote leve Culto Offline (Pad) ou a biblioteca completa Church Live direto pela ChurchDAW Store.',
      'Painel ChurchDAW Store Bibliotecas no Android com Culto offline (Pad) instalada e Church Live',
      'DESIGNER PREVIEW · SHELL ANDROID'
    ],
    audio: [
      'Novo motor Oboe nativo de baixa latência.',
      'Saída USB-C ou alto-falante, pareamento MIDI Bluetooth LE, botão Recuperar USB e curva de resposta do teclado.',
      'Painel Áudio e MIDI no Android com driver Oboe de baixa latência, MIDI Bluetooth e Recuperar USB',
      'DESIGNER PREVIEW · SHELL ANDROID'
    ]
  };

  const tabButtons = [...document.querySelectorAll('[data-android-screen]')];
  const imgEl = document.getElementById('android-gallery-image');
  const linkEl = document.getElementById('android-gallery-link');
  const titleEl = document.getElementById('android-gallery-title');
  const descEl = document.getElementById('android-gallery-description');
  const badgeEl = document.getElementById('android-capture-badge');

  if (tabButtons.length && imgEl && linkEl) {
    tabButtons.forEach((btn) => {
      btn.addEventListener('click', () => {
        const key = btn.dataset.androidScreen;
        const info = androidScreens[key];
        if (!info) return;
        const src = `assets/android-1635/${key}.webp?v=20261104b`;
        imgEl.src = src;
        imgEl.alt = info[2];
        linkEl.href = src;
        linkEl.dataset.caption = `${info[2]} · ChurchDAW Mobile`;
        if (titleEl) titleEl.textContent = info[0];
        if (descEl) descEl.textContent = info[1];
        if (badgeEl) badgeEl.innerHTML = `${info[3]} <span aria-hidden="true">↗</span>`;
        tabButtons.forEach((b) => b.setAttribute('aria-pressed', String(b === btn)));
      });
    });
  }

  const video = document.getElementById('android-1635-video');
  const playBtn = document.getElementById('android-film-play');
  if (video && playBtn) {
    const syncAndroidPlay = () => {
      playBtn.innerHTML = video.paused
        ? '<span class="play-small" aria-hidden="true">▶</span> Assistir · 36 s'
        : '<span class="play-small" aria-hidden="true">Ⅱ</span> Pausar vídeo';
    };
    playBtn.addEventListener('click', () => {
      if (video.paused) {
        video.play().catch(() => { video.focus(); });
      } else {
        video.pause();
      }
    });
    video.addEventListener('play', syncAndroidPlay);
    video.addEventListener('pause', syncAndroidPlay);
    video.addEventListener('ended', syncAndroidPlay);
  }
})();
