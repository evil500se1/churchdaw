/* Silent, visibility-aware decorative video. Never opens camera or audio. */
(()=>{
  'use strict';
  const video=document.getElementById('piano-cinema');
  const button=document.getElementById('cinema-toggle');
  if(!video||!button)return;
  const art=video.closest('.cinema-art');
  const label=button.querySelector('.cinema-toggle-label');
  const icon=button.querySelector('.cinema-toggle-icon');
  const reduced=window.matchMedia('(prefers-reduced-motion: reduce)');
  const connection=navigator.connection;
  const conservative=()=>reduced.matches||Boolean(connection?.saveData)||/^(slow-)?2g$/.test(connection?.effectiveType||'');
  let userChoice=null,visible=false,loaded=false,failed=false;
  video.muted=true;video.defaultMuted=true;
  function sync(){
    const playing=!video.paused&&!video.ended;
    button.setAttribute('aria-pressed',String(playing));
    label.textContent=playing?'Pausar animação':'Reproduzir animação';
    icon.textContent=playing?'Ⅱ':'▶';
  }
  function shouldPlay(){return !failed&&visible&&!document.hidden&&userChoice!==false&&(userChoice===true||!conservative());}
  function load(){
    if(loaded)return;
    video.src=window.matchMedia('(max-width: 700px)').matches?video.dataset.mobileSrc:video.dataset.desktopSrc;
    loaded=true;video.load();
  }
  function update(){
    if(shouldPlay()){
      load();
      const attempt=video.play();
      attempt?.catch(()=>{sync();});
    }else video.pause();
    sync();
  }
  button.hidden=false;
  button.addEventListener('click',()=>{
    userChoice=video.paused;update();
  });
  video.addEventListener('playing',()=>{art.classList.add('has-video');sync();});
  video.addEventListener('pause',sync);
  video.addEventListener('error',()=>{failed=true;art.classList.remove('has-video');button.hidden=true;video.removeAttribute('src');});
  document.addEventListener('visibilitychange',update);
  function preferenceChanged(){userChoice=null;update();}
  if(reduced.addEventListener)reduced.addEventListener('change',preferenceChanged);
  else if(reduced.addListener)reduced.addListener(preferenceChanged);
  connection?.addEventListener?.('change',preferenceChanged);
  if('IntersectionObserver' in window){
    const observer=new IntersectionObserver(entries=>{visible=entries[0].isIntersecting;update();},{threshold:0.15});
    observer.observe(art);
  }else{visible=true;update();}
})();
