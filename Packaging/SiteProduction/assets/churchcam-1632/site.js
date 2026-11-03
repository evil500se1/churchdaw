(() => {
  'use strict';
  const nav=document.querySelector('.nav'),menu=document.querySelector('.menu-button');
  function closeMenu(){nav.classList.remove('open');menu.setAttribute('aria-expanded','false');menu.setAttribute('aria-label','Abrir menu');menu.textContent='☰';}
  menu.addEventListener('click',()=>{const open=nav.classList.toggle('open');menu.setAttribute('aria-expanded',String(open));menu.setAttribute('aria-label',open?'Fechar menu':'Abrir menu');menu.textContent=open?'×':'☰';});
  document.querySelectorAll('.nav-links a').forEach(a=>a.addEventListener('click',closeMenu));
  document.addEventListener('keydown',e=>{if(e.key==='Escape'&&nav.classList.contains('open')){closeMenu();menu.focus();}});
  document.addEventListener('click',e=>{if(nav.classList.contains('open')&&!nav.contains(e.target))closeMenu();});
  const screens={
    mixer:['Seu setup inteiro em uma tela.','Canais, timbres, efeitos e master com acesso direto.','Mixer real do ChurchDAW com faders, instrumentos e master',1296,768],
    timbres:['Encontre o timbre que combina com você.','Categorias, busca, favoritos e prévia com volume independente.','Seletor real de pianos e timbres com busca e prévia de áudio',1296,768],
    plugins:['Seus plugins, organizados visualmente.','Instrumentos, efeitos e formatos em uma galeria com busca.','Galeria real com BigSky, Pianoteq 9 e Keyscape instalados na demonstração',1206,768],
    pad:['Construa a sua ambiência.','Pads de samples com filtro, envelope, reverb e controles por canal.','Editor real do Pad2, com envelope, reverb e filtro',1296,768],
    audio:['Seu equipamento, pronto para tocar.','Interface, buffer, entradas MIDI e resposta do teclado em um só painel.','Painel real de áudio e MIDI com Scarlett 2i2 e Launchkey',1296,768]
  };
  const buttons=[...document.querySelectorAll('[data-screen]')],galleryImage=document.getElementById('gallery-image'),galleryLink=document.getElementById('gallery-link');
  buttons.forEach(button=>button.addEventListener('click',()=>{
    const name=button.dataset.screen,screen=screens[name];if(!screen)return;
    const src=`assets/churchcam-1632/${name}.webp`;
    galleryImage.src=src;galleryImage.alt=screen[2];galleryImage.width=screen[3];galleryImage.height=screen[4];
    galleryLink.href=src;galleryLink.dataset.caption=screen[2]+' · captura real no macOS';
    document.getElementById('gallery-title').textContent=screen[0];document.getElementById('gallery-description').textContent=screen[1];
    buttons.forEach(b=>b.setAttribute('aria-pressed',String(b===button)));
  }));
  const dialog=document.getElementById('image-dialog'),image=document.getElementById('dialog-image');let opener=null;
  document.querySelectorAll('[data-lightbox]').forEach(a=>a.addEventListener('click',e=>{
    if(typeof dialog.showModal!=='function'||e.metaKey||e.ctrlKey||e.shiftKey||e.altKey)return;
    e.preventDefault();opener=a;image.src=a.href;image.alt=a.dataset.caption||a.querySelector('img')?.alt||'Captura real do ChurchDAW';document.getElementById('dialog-caption').textContent=image.alt;dialog.showModal();document.body.style.overflow='hidden';
  }));
  document.querySelector('.dialog-close').addEventListener('click',()=>dialog.close());
  dialog.addEventListener('click',e=>{if(e.target===dialog){const r=dialog.getBoundingClientRect();if(e.clientX<r.left||e.clientX>r.right||e.clientY<r.top||e.clientY>r.bottom)dialog.close();}});
  dialog.addEventListener('close',()=>{document.body.style.overflow='';if(opener)opener.focus();});
  const video=document.getElementById('churchcam-video'),play=document.querySelector('.film-play');
  play.addEventListener('click',()=>{if(video.paused){video.play().catch(()=>{video.focus();});}else video.pause();});
  function syncPlay(){play.innerHTML=video.paused?'<span class="play-small" aria-hidden="true">▶</span> Assistir · 2 min 10':'<span class="play-small" aria-hidden="true">Ⅱ</span> Pausar apresentação';}
  video.addEventListener('play',syncPlay);video.addEventListener('pause',syncPlay);video.addEventListener('ended',syncPlay);
  document.getElementById('year').textContent=String(new Date().getFullYear());
})();
