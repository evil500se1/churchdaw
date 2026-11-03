# -*- coding: utf-8 -*-
"""
Gravador de Tela + Audio  -  ChurchDAW
======================================
Grava a tela do PC para fazer videos de divulgacao do ChurchDAW.

SOBRE O AUDIO (leia, e o ponto que mais confunde)
-------------------------------------------------
O "loopback" do Windows captura o que passa pelo mixer do Windows. O ASIO
NAO passa pelo mixer: ele fala direto com a placa, em modo exclusivo. Por
isso, enquanto o ChurchDAW estiver em ASIO, o loopback devolve o erro
"Invalid device" -- e nenhum programa consegue capturar aquele audio por
esse caminho. Nao e defeito deste gravador, e como o ASIO funciona.

Formas que REALMENTE funcionam (o programa oferece todas):
  A) Gravar de uma ENTRADA da placa.
     - Cabo do fone/saida da Scarlett para a entrada dela  ->  funciona
       com o ASIO ligado, sem instalar nada. Simples e imediato.
     - Ou, se instalar o Focusrite Control 2, ligue o "Loopback" dele:
       a saida do computador (inclusive ASIO) volta pelas entradas.
  B) Trocar o ChurchDAW para WASAPI so enquanto grava o video.
     O som fica identico; muda so a latencia, que nao importa em video.
     Depois volte para ASIO para tocar ao vivo.
  C) Gravar somente o VIDEO aqui e o AUDIO em separado (dentro do DAW),
     juntando os dois depois no editor. E o que estudio faz.
"""
import os, sys, time, threading, queue, subprocess, signal, datetime, shutil

# ---------------------------------------------------------------- ffmpeg
def achar_ffmpeg():
    aqui = os.path.dirname(os.path.abspath(sys.argv[0]))
    cand = [os.path.join(aqui, 'ffmpeg.exe'), shutil.which('ffmpeg')]
    wg = os.path.join(os.environ.get('LOCALAPPDATA', ''), 'Microsoft', 'WinGet', 'Packages')
    if os.path.isdir(wg):
        for raiz, _, arqs in os.walk(wg):
            if 'ffmpeg.exe' in arqs:
                cand.append(os.path.join(raiz, 'ffmpeg.exe'))
                break
    for c in cand:
        if c and os.path.isfile(c):
            return c
    return None

FFMPEG = achar_ffmpeg()

def encoder_funciona(nome):
    """Nao basta estar em -encoders: o h264_nvenc aparece na lista mesmo com
    driver antigo e falha ao abrir. Codificamos 1 quadro de teste de verdade."""
    if not FFMPEG:
        return False
    try:
        r = subprocess.run(
            [FFMPEG, '-hide_banner', '-loglevel', 'error',
             '-f', 'lavfi', '-i', 'testsrc=size=320x240:rate=1',
             '-frames:v', '1', '-c:v', nome, '-f', 'null', '-'],
            capture_output=True, text=True, timeout=60)
        return r.returncode == 0
    except Exception:
        return False

def escolher_encoder():
    for nome, args, desc in (
        ('h264_nvenc', ['-c:v', 'h264_nvenc', '-preset', 'p5', '-tune', 'hq',
                        '-rc', 'vbr', '-cq', '21', '-b:v', '0'],
         'NVENC (placa NVIDIA - deixa a CPU livre para o audio)'),
        ('h264_qsv', ['-c:v', 'h264_qsv', '-global_quality', '21'], 'Intel QuickSync'),
        ('h264_amf', ['-c:v', 'h264_amf', '-quality', 'quality', '-qp_i', '21'], 'AMD AMF'),
    ):
        if encoder_funciona(nome):
            return args, desc
    return (['-c:v', 'libx264', '-preset', 'veryfast', '-crf', '21'],
            'libx264 (por CPU - se o audio engasgar, use 30 fps ou so a janela)')

# ---------------------------------------------------------------- fontes de audio
def listar_fontes():
    """Devolve (loopback_padrao_ok, lista). Cada item:
       (rotulo, indice, canais, taxa, tipo)  tipo: 'loopback' | 'entrada'"""
    import pyaudiowpatch as pa
    p = pa.PyAudio()
    itens, padrao_ok = [], None
    try:
        wasapi = p.get_host_api_info_by_type(pa.paWASAPI)
        saida = p.get_device_info_by_index(wasapi['defaultOutputDevice'])
        nome_saida = saida['name']

        def abre(idx, canais, taxa):
            try:
                s = p.open(format=pa.paFloat32, channels=canais, rate=int(taxa),
                           frames_per_buffer=1024, input=True, input_device_index=idx)
                s.close()
                return True
            except Exception:
                return False

        for d in p.get_loopback_device_info_generator():
            eh_padrao = nome_saida in d['name']
            ok = abre(d['index'], 2, d['defaultSampleRate'])
            rot = 'Som do Windows: %s%s' % (d['name'].replace(' [Loopback]', ''),
                                            '  <-- saida padrao' if eh_padrao else '')
            if not ok:
                rot += '   [BLOQUEADO - ASIO em modo exclusivo]'
            if eh_padrao:
                padrao_ok = ok
                itens.insert(0, (rot, d['index'], 2, int(d['defaultSampleRate']),
                                 'loopback', ok))
            else:
                itens.append((rot, d['index'], 2, int(d['defaultSampleRate']),
                              'loopback', ok))

        vistos = set()
        for i in range(p.get_device_count()):
            di = p.get_device_info_by_index(i)
            if di['maxInputChannels'] <= 0 or di.get('isLoopbackDevice'):
                continue
            api = p.get_host_api_info_by_index(di['hostApi'])['name']
            if 'WASAPI' not in api:
                continue                        # WASAPI basta e evita duplicatas
            chave = di['name'].strip()
            if chave in vistos:
                continue
            vistos.add(chave)
            ok = abre(i, min(2, int(di['maxInputChannels'])), di['defaultSampleRate'])
            itens.append(('Entrada da placa: %s' % chave, i,
                          min(2, int(di['maxInputChannels'])),
                          int(di['defaultSampleRate']), 'entrada', ok))
    finally:
        p.terminate()
    return padrao_ok, itens

class CapturaAudio(threading.Thread):
    """Le a fonte escolhida e entrega s16le estereo para o ffmpeg."""

    def __init__(self, indice, canais, taxa):
        super().__init__(daemon=True)
        import pyaudiowpatch as pa
        self.pa_mod = pa
        self.p = pa.PyAudio()
        self.indice, self.canais, self.rate = indice, canais, taxa
        self.fila = queue.Queue(maxsize=200)
        self.parar = threading.Event()
        self.erro = None
        self.bloco = 1024

    def testar(self):
        pa = self.pa_mod
        try:
            s = self.p.open(format=pa.paFloat32, channels=self.canais, rate=self.rate,
                            frames_per_buffer=self.bloco, input=True,
                            input_device_index=self.indice)
            s.close()
            return None
        except Exception as e:
            return e

    def run(self):
        import numpy as np
        pa = self.pa_mod
        try:
            s = self.p.open(format=pa.paFloat32, channels=self.canais, rate=self.rate,
                            frames_per_buffer=self.bloco, input=True,
                            input_device_index=self.indice)
            while not self.parar.is_set():
                dados = s.read(self.bloco, exception_on_overflow=False)
                x = np.frombuffer(dados, dtype=np.float32)
                if self.canais == 1:
                    x = np.repeat(x, 2)          # mono -> estereo
                np.clip(x, -1.0, 1.0, out=x)
                try:
                    self.fila.put((x * 32767.0).astype('<i2').tobytes(), timeout=1.0)
                except queue.Full:
                    pass
            s.close()
        except Exception as e:
            self.erro = e
        finally:
            try:
                self.p.terminate()
            except Exception:
                pass

# ---------------------------------------------------------------- gravacao
def gravar(destino, fps=30, janela=None, audio=None, segundos=None):
    """audio = (indice, canais, taxa) ou None para gravar SEM audio."""
    if not FFMPEG:
        print('  ERRO: ffmpeg.exe nao encontrado nesta pasta.')
        return False

    cap = None
    if audio is not None:
        cap = CapturaAudio(*audio)
        err = cap.testar()                       # testa ANTES de abrir o ffmpeg
        if err is not None:
            print('  ERRO no audio: %s' % err)
            return False

    venc, desc_enc = escolher_encoder()
    entrada_video = ['-f', 'gdigrab', '-framerate', str(fps), '-draw_mouse', '1',
                     '-i', ('title=' + janela) if janela else 'desktop']

    cmd = [FFMPEG, '-hide_banner', '-loglevel', 'error', '-y', *entrada_video]
    if cap is not None:
        cmd += ['-f', 's16le', '-ar', str(cap.rate), '-ac', '2',
                '-thread_queue_size', '4096', '-i', 'pipe:0',
                '-map', '0:v', '-map', '1:a']
    else:
        cmd += ['-map', '0:v']
    cmd += [*venc, '-pix_fmt', 'yuv420p',
            '-vf', 'scale=trunc(iw/2)*2:trunc(ih/2)*2',
            '-fps_mode', 'cfr', '-r', str(fps)]
    if cap is not None:
        cmd += ['-c:a', 'aac', '-b:a', '192k']
    cmd += ['-movflags', '+faststart', destino]

    print('')
    print('  video  : %s' % desc_enc)
    print('  captura: %s' % ('janela "%s"' % janela if janela else 'tela inteira'))
    print('  audio  : %s' % ('SEM audio (so video)' if cap is None
                             else 'dispositivo %d, %d Hz' % (cap.indice, cap.rate)))
    print('  arquivo: %s' % destino)
    print('')

    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE if cap else subprocess.DEVNULL,
                            creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
    if cap is not None:
        cap.start()
        time.sleep(0.4)
        if cap.erro:
            print('  ERRO no audio: %s' % cap.erro)
            proc.kill()
            return False

    t0 = time.time()
    fim = threading.Event()
    if segundos:
        print('  >>> GRAVANDO %d s (modo automatico). <<<' % segundos)
        threading.Thread(target=lambda: (time.sleep(segundos), fim.set()),
                         daemon=True).start()
    else:
        print('  >>> GRAVANDO.  Pressione ENTER para parar. <<<')
        def _esperar():
            try:
                input()
            except Exception:
                pass
            fim.set()
        threading.Thread(target=_esperar, daemon=True).start()

    try:
        while not fim.is_set():
            if proc.poll() is not None:
                break
            if cap is None:
                time.sleep(0.2)
                continue
            try:
                pcm = cap.fila.get(timeout=0.5)
            except queue.Empty:
                continue
            try:
                proc.stdin.write(pcm)
            except (BrokenPipeError, OSError):
                break
    except KeyboardInterrupt:
        pass

    if cap is not None:
        cap.parar.set()
        try:
            proc.stdin.close()
        except Exception:
            pass
    try:
        proc.send_signal(signal.CTRL_BREAK_EVENT)
    except Exception:
        pass
    try:
        proc.wait(timeout=20)
    except subprocess.TimeoutExpired:
        proc.terminate()
        try:
            proc.wait(timeout=8)
        except subprocess.TimeoutExpired:
            proc.kill()

    ok = os.path.isfile(destino) and os.path.getsize(destino) > 10000
    print('')
    if ok:
        print('  PRONTO: %.0f s, %.1f MB' % (time.time() - t0,
                                             os.path.getsize(destino) / 1e6))
        print('  %s' % destino)
    else:
        print('  FALHOU: o arquivo nao foi gravado.')
    return ok

# ---------------------------------------------------------------- menu
def explicar_asio():
    print('')
    print('  ---------------------------------------------------------------')
    print('  O som do Windows esta BLOQUEADO porque o ChurchDAW esta em ASIO')
    print('  (modo exclusivo). O ASIO passa por fora do mixer do Windows,')
    print('  entao NENHUM programa consegue captura-lo por loopback.')
    print('')
    print('  Como resolver, mantendo o ASIO:')
    print('   * Ligue um cabo da SAIDA da Scarlett na ENTRADA dela e escolha')
    print('     abaixo "Entrada da placa". Funciona na hora, sem instalar nada.')
    print('   * Ou instale o Focusrite Control 2 e ligue o "Loopback" dele:')
    print('     a saida do PC volta pelas entradas, tudo digital.')
    print('')
    print('  Sem ASIO (mais simples para video):')
    print('   * Troque o driver do ChurchDAW para WASAPI enquanto grava.')
    print('     O som e o mesmo; muda so a latencia, que nao importa em video.')
    print('  ---------------------------------------------------------------')

def escolher_fonte():
    print('\n  Procurando fontes de audio...')
    try:
        padrao_ok, itens = listar_fontes()
    except Exception as e:
        print('  Nao consegui listar o audio: %s' % e)
        return None
    if padrao_ok is False:
        explicar_asio()
    print('')
    print('  FONTES DE AUDIO:')
    for n, (rot, idx, ch, tx, tipo, ok) in enumerate(itens, 1):
        print('   %d) %-72s %s' % (n, rot, '' if ok else '(indisponivel)'))
    print('   0) Gravar SEM audio (junto o som depois no editor)')
    esc = input('\n  Escolha [1]: ').strip() or '1'
    if esc == '0':
        return None
    try:
        rot, idx, ch, tx, tipo, ok = itens[int(esc) - 1]
    except Exception:
        print('  Opcao invalida.')
        return None
    if not ok:
        print('  Essa fonte esta indisponivel agora.')
        return None
    print('  -> %s' % rot)
    return (idx, ch, tx)

def main():
    if '--segundos' in sys.argv:
        a = sys.argv
        seg = int(a[a.index('--segundos') + 1])
        saida = a[a.index('--saida') + 1] if '--saida' in a else os.path.join(
            os.path.expanduser('~'), 'Videos', 'ChurchDAW',
            'auto-%s.mp4' % datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
        os.makedirs(os.path.dirname(saida), exist_ok=True)
        jan = a[a.index('--janela') + 1] if '--janela' in a else None
        fps = int(a[a.index('--fps') + 1]) if '--fps' in a else 30
        aud = None
        if '--sem-audio' not in a:
            if '--dispositivo' in a:
                import pyaudiowpatch as pa
                p = pa.PyAudio()
                i = int(a[a.index('--dispositivo') + 1])
                di = p.get_device_info_by_index(i)
                aud = (i, min(2, int(di['maxInputChannels'])), int(di['defaultSampleRate']))
                p.terminate()
            else:
                try:
                    _, itens = listar_fontes()
                    viaveis = [x for x in itens if x[5]]
                    aud = (viaveis[0][1], viaveis[0][2], viaveis[0][3]) if viaveis else None
                except Exception:
                    aud = None
        sys.exit(0 if gravar(saida, fps=fps, janela=jan, audio=aud, segundos=seg) else 1)

    print('=' * 66)
    print('  GRAVADOR DE TELA + AUDIO   -   ChurchDAW')
    print('=' * 66)
    if not FFMPEG:
        print('  ffmpeg.exe nao encontrado. Coloque-o nesta pasta.')
        input('\n  ENTER para sair.')
        return
    pasta = os.path.join(os.path.expanduser('~'), 'Videos', 'ChurchDAW')
    os.makedirs(pasta, exist_ok=True)

    print('  1) Tela inteira            (recomendado)')
    print('  2) Somente a janela do ChurchDAW')
    print('  3) Tela inteira, 60 fps')
    print('  4) Sair')
    op = input('\n  Escolha [1]: ').strip() or '1'
    if op == '4':
        return
    janela = 'ChurchDAW' if op == '2' else None
    fps = 60 if op == '3' else 30

    aud = escolher_fonte()

    destino = os.path.join(pasta, 'ChurchDAW-%s.mp4' %
                           datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
    for i in (3, 2, 1):
        print('  comecando em %d...' % i)
        time.sleep(1)
    gravar(destino, fps=fps, janela=janela, audio=aud)
    input('\n  ENTER para fechar.')

if __name__ == '__main__':
    main()
