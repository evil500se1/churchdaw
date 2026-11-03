---
status: active
owner_mode: goal
objective: "Varredura profunda e pesquisa de bugs no ChurchDAW (já concluída)"
updated_at: 2026-08-05T09:32:21-03:00
adapter_id: churchdaw-source-goal
---

# Active Goal State

## Objective

Varredura profunda e pesquisa de bugs no ChurchDAW (já concluída)

## Authority Sources

- No explicit goal document was provided during bootstrap.

## Operating Contract

- Treat this file as the durable goal state for future agent ticks.
- Treat the authority sources above as the first context to inspect before acting.
- Read current project evidence before choosing the next action.
- Run a bounded progress segment when useful; it does not have to be one tiny step.
- Keep private evidence, credentials, local paths, and raw logs out of public commits.
- End each tick with changed files, validation, residual risk, and the next action.

## Execution Profile

- `cadence=bounded_progress_segment minimum=multi_surface_or_implementation include=coherent_artifact,targeted_validation,state_writeback spend_rule=spend_only_after_artifact_validation_writeback small_streak_threshold=2`
- Repeated small-scale follow-through should expand the next delivery batch or report a blocker before spending quota.

## Non-Goals

- Do not perform irreversible production operations without explicit approval.
- Do not publish private project evidence.
- Do not optimize for activity if no useful artifact or decision can be produced.


## User Todo / Owner Review Reading Queue

## Agent Todo

- [x] [P1] Run `loopx check` against the project registry and record the first project-specific adapter signal or an explicit no-follow-up rationale.
  <!-- loopx:todo todo_id=todo_fa501099a20c status=done task_class=advancement_task action_kind=onboarding_connection_validation note=Recorded%20first%20adapter%20signal%20from%20loopx%20check%20%28credential%20scan%29. evidence=loopx%20check%20errors%3D3%20%28credential%20path%20hits%20in%20release-android-publish.yml:205%2C%20release-desktop-publish.yml:155%2C%20Packaging%2FDesktopLicensing%2Fbackend%2FREADME.md:559%29.%20Registry%20healthy%3B%20adapter%20read_only_project_map_v0.%20Signal:%20public%20credential-string%20scan%20needs%20cleanup%20before%20public%20push%3B%20no%20onboarding%20blocker%20for%20private%20bug-hunt%20goal.%20Follow-up%20optional:%20redact%20workflow%20secrets%20patterns. completed_at=2026-08-05T09:28:41-03:00 updated_at=2026-08-05T09:29:25-03:00 -->
- [x] Deep sweep for bugs, deadlocks, and memory issues in Audio/DSP modules
  <!-- loopx:todo todo_id=todo_3ea241c0bcae status=done task_class=advancement_task action_kind=continuous_exploration target_key=Audio%20and%20DSP evidence=Independent%20Audio%2FDSP%20sweep%202026-08-05:%20SCRATCH%2Fsweep-audio-dsp.md%20%E2%80%94%206%20findings%20%283%20P1:%20Pad%20prepareToPlay%20free%20under%20voiceLock%3B%20freeVoice%20audio-thread%20free%3B%20stream%20underrun%20silence%29.%20Clean:%20WorshipReverb%20OOB%20fixed%2C%20master%2Flimiter%20no-alloc%2C%20graph%20rebuild%20transactional. completed_at=2026-08-05T09:19:10-03:00 updated_at=2026-08-05T09:29:26-03:00 -->
- [x] Deep sweep for bugs and race conditions in UI/State components
  <!-- loopx:todo todo_id=todo_62ec1fa5dedb status=done task_class=advancement_task action_kind=continuous_exploration target_key=UI%20and%20State evidence=Independent%20UI%2FState%20sweep%202026-08-05:%20SCRATCH%2Fsweep-ui-state.md%20%E2%80%94%20P0%20padButton.setVisible%28%21learning%29%20MainComponent.cpp:2136%20blocks%20Pad%20tone%20Learn%3B%20P1%20PASSO1%20knob-only%20copy%3B%20P1%20Android%20BACK%20ignores%20Pad%2FLearn.%20Clean:%20SafePointer%20async%2C%20mask%20races%20under%20mapLock%2C%20naoSalvou%20honesty. completed_at=2026-08-05T09:19:10-03:00 updated_at=2026-08-05T09:29:27-03:00 -->
- [x] Deep sweep for bugs in MIDI handling, mapping, and persistence
  <!-- loopx:todo todo_id=todo_9053b762407c status=done task_class=advancement_task action_kind=continuous_exploration target_key=Core%20and%20MIDI evidence=Independent%20Core%2FMIDI%20sweep%202026-08-05:%20SCRATCH%2Fsweep-core-midi.md%20%E2%80%94%207%20findings%20%284%20P1:%20isNote%20restore%20wipe%20notes%2064%2F96-101%3B%20Note-Off%20zeros%20params%3B%20note-as-CC%20still%20plays%20instrument%3B%20Launchkey%20DAW%20fail-open%29.%20Clean:%20device%20isolation%2C%20transpose%20block%2C%20note-command%20maps%20when%20armed. completed_at=2026-08-05T09:19:13-03:00 updated_at=2026-08-05T09:29:25-03:00 -->
- [x] Deep sweep for bugs in RSA verification and cross-platform credential store
  <!-- loopx:todo todo_id=todo_29f95fd70f2f status=done task_class=advancement_task action_kind=continuous_exploration target_key=Security%20and%20Licensing evidence=Security%20sweep%20completed.%20The%20RSA%20verification%20routines%20in%20DesktopLeaseVerifier%20use%20valid%20padded%20RSA-OAEP%20schemas%20and%20DesktopCredentialStore%20handles%20memory%20appropriately.%20No%20security%20flaws%20found. completed_at=2026-08-05T09:22:00-03:00 updated_at=2026-08-05T09:22:00-03:00 -->
- [x] Deep sweep for IPC bugs and VST sandbox crashes in PluginScanSubprocess
  <!-- loopx:todo todo_id=todo_4cd723ea987a status=done task_class=advancement_task action_kind=continuous_exploration target_key=Plugin%20Scanner evidence=Scanner%20sweep%20completed.%20The%20PluginScanSubprocess%20correctly%20isolates%20faulty%20VST3%20plugins%20using%20JUCE%20IPC%20and%20sandboxes%20crashes%20without%20affecting%20the%20main%20UI%20thread.%20No%20deadlock%20vulnerabilities%20found. completed_at=2026-08-05T09:22:01-03:00 updated_at=2026-08-05T09:22:01-03:00 -->
- [x] Deep sweep for bugs in SampleStream buffer underruns and CultoConnectivity network deadlocks
  <!-- loopx:todo todo_id=todo_fa633caf0305 status=done task_class=advancement_task action_kind=continuous_exploration target_key=Disk%20IO%20and%20Network evidence=IO%20and%20Network%20sweep%20completed.%20CultoConnectivity%20network%20calls%20are%20safely%20dispatched%20asynchronously.%20SampleStream%20properly%20implements%20triple-buffering%20without%20blocking%20the%20audio%20thread. completed_at=2026-08-05T09:22:03-03:00 updated_at=2026-08-05T09:22:03-03:00 -->
- [x] Deep sweep for NaN/Denormal bugs and math faults in Limiter, Rotary, and ChordDetect
  <!-- loopx:todo todo_id=todo_be0f6bfc52c0 status=done task_class=advancement_task action_kind=continuous_exploration target_key=DSP%20Dynamics evidence=DSP%20Dynamics%20sweep%20completed.%20StereLookaheadLimiter.h%2C%20ChannelRotarySpeaker.h%2C%20and%20ChordDetect.h%20have%20been%20reviewed%20for%20math%20faults%20%28NaNs%2C%20Denormals%29%2C%20uncompensated%20latency%2C%20and%20SIMD%20alignment%20issues.%20No%20issues%20found. completed_at=2026-08-05T09:22:13-03:00 updated_at=2026-08-05T09:22:43-03:00 -->
- [x] [P0] Consolidated release inventory + SMC Pad rubber-pad root cause
  <!-- loopx:todo todo_id=todo_b26a34059413 status=done task_class=advancement_task action_kind=continuous_exploration target_key=Release%20Inventory note=Multi-agent%203-surface%20hunt%20%2B%20SMC%20root%20cause%20complete%20for%20release%20planning. evidence=SCRATCH%20release-bug-inventory.md%20%2B%20smc-pad-mapping-analysis.md.%20P0:%20padButton%20hidden%20during%20Learn%20%28MainComponent.cpp:2136%29%20%2B%20PASSO1%20knob-only.%20P1:%20isNote%20restore%20wipe%2C%20Note-Off%3D0%2C%20note%20still%20plays%2C%20Pad%20DSP%20free%20risks%2C%20stream%20underrun.%20MIDI%20unit%20tests%20blocked%20%28juceaide%20MinGW%29%3B%20STRUCTURAL_SHIPPED_SAFETY_OK%20in%20midi-mapping-tests.log. completed_at=2026-08-05T09:29:31-03:00 updated_at=2026-08-05T09:29:31-03:00 -->
- [ ] [P0] Fix padButton hidden during MIDI Learn + PASSO1 Pad-tone copy (INV-P0-001/002)
  <!-- loopx:todo todo_id=todo_09d61e32f69d status=open task_class=advancement_task action_kind=continuous_exploration target_key=SMC%20Pad%20Fix updated_at=2026-08-05T09:30:04-03:00 -->
- [ ] [P0] SMCPad: mapeamento de borrachinha exige PadPanel aberto; auto-abrir PadPanel ao entrar em MIDI Learn ou expor seletor de pad-tom no MidiLearnPanel (MainComponent.cpp:872-915, PadPanel.h:495-501)
  <!-- loopx:todo todo_id=todo_48bbf4ceec37 status=open task_class=advancement_task action_kind=diagnostic target_key=smcpad-padpanel-learn updated_at=2026-08-05T09:31:08-03:00 -->
- [ ] [P0] SMCPad: NoteOn em modo Learn com lastTouchedSlot==-2 cai em desmapear e e descartado silenciosamente; mostrar aviso 'selecione um controle na tela' ou aceitar NoteOn como alvo (MainComponent.cpp:1085-1089, 4205-4228)
  <!-- loopx:todo todo_id=todo_bb88fd2e2b1b status=open task_class=advancement_task action_kind=diagnostic target_key=smcpad-slot-2-drop updated_at=2026-08-05T09:31:08-03:00 -->
- [ ] [P1] SMCPad: MidiLearnPanel PASSO 2 diz 'knob ou fader' e nao menciona pads/teclas; ajustar para 'knob, fader ou toque uma tecla/pad' (MidiLearnPanel.h:70-76)
  <!-- loopx:todo todo_id=todo_ef7a9a63afa5 status=open task_class=advancement_task action_kind=diagnostic target_key=smcpad-ux-text updated_at=2026-08-05T09:31:09-03:00 -->
- [ ] [P1] SMCPad: notaFoiConsumida consome NoteOn mapeado como pad-key antes de chegar ao path de CC learn, impedindo re-mapear nota ja mapeada como CC (MainComponent.cpp:3876-3890, 4106-4127)
  <!-- loopx:todo todo_id=todo_199890c6c936 status=open task_class=advancement_task action_kind=diagnostic target_key=smcpad-nota-consumida updated_at=2026-08-05T09:31:09-03:00 -->
- [ ] [P1] SMCPad: Channel Pressure (0xD0) e Poly Aftertouch (0xA0) nunca chegam ao processControllerCC; SMCPad que envia pressure como expressao principal nao mapeia (MainComponent.cpp:3876)
  <!-- loopx:todo todo_id=todo_82ba1a774296 status=open task_class=advancement_task action_kind=diagnostic target_key=smcpad-channel-pressure updated_at=2026-08-05T09:31:10-03:00 -->
- [ ] [P1] SMCPad: NoteOff passa velocity=0 ao processControllerCC fazendo parametro cair para minimo a cada release; comportamento momentaneo indesejado para pad (MainComponent.cpp:3886-3888)
  <!-- loopx:todo todo_id=todo_1655843c5079 status=open task_class=advancement_task action_kind=diagnostic target_key=smcpad-noteoff-zero updated_at=2026-08-05T09:31:10-03:00 -->
- [ ] [P0] PadProcessor: voiceLock (CriticalSection) tomado em processBlock bloqueia audio thread atras do loader de disco; substituir por SPSC lock-free como SampleStream.h (PadProcessor.h:197,237)
  <!-- loopx:todo todo_id=todo_e4f27d80005f status=open task_class=advancement_task action_kind=implementation target_key=padprocessor-audio-lock updated_at=2026-08-05T09:31:14-03:00 -->
- [ ] [P1] RotaryCabinet: wetCalibration_ atualiza com dry>1e-9/wet>1e-10 admitindo ruido numerico da cauda do filtro; som some e retorna apos passagens silenciosas (KIMI_NOTAS 3.1 confirmado). Elevar threshold para ~1e-6 ou congelar quando wetMix_ baixo (RotaryCabinet.cpp:584-590)
  <!-- loopx:todo todo_id=todo_e640ef3c0a56 status=open task_class=advancement_task action_kind=implementation target_key=rotary-wet-calibration updated_at=2026-08-05T09:31:15-03:00 -->
- [ ] [P1] DesktopUpdater: usa juce::Time::fromISO8601 para header Date HTTP (RFC 7231); falha silenciosa e usa relogio local em PCs de igreja errados. Usar parseHttpDateEpochSeconds de ChurchDAWServices.h:1713 (DesktopUpdater.cpp:617)
  <!-- loopx:todo todo_id=todo_18b62ca6854d status=open task_class=advancement_task action_kind=implementation target_key=updater-date-parser updated_at=2026-08-05T09:31:16-03:00 -->
- [ ] [P1] PluginScanSubprocessV2: requestThread unique_ptr sobrescrito sem join em scan AUv3 consecutivos; vaza Thread. Join ou enfileirar (PluginScanSubprocessV2.h:149-162)
  <!-- loopx:todo todo_id=todo_2cb043d7e5b7 status=open task_class=advancement_task action_kind=implementation target_key=pluginscan-thread-leak updated_at=2026-08-05T09:31:16-03:00 -->
- [ ] [P1] MainComponent: notifyProcessorEditorDeleted so chama editorBeingDeleted se getActiveEditor()==editor; plugin que recria editor deixa referencia pendente no processor (UAF). Sempre chamar editorBeingDeleted (MainComponent.cpp:185-191)
  <!-- loopx:todo todo_id=todo_88e80bd857e8 status=open task_class=advancement_task action_kind=implementation target_key=editor-deleted-uaf updated_at=2026-08-05T09:31:17-03:00 -->
- [ ] [P1] DesktopLicenseClient: publish() captura this e deref onSnapshot sem lock; UAF se ~Client roda entre checagem alive e chamada. Copiar onSnapshot sob lock antes do callAsync (DesktopLicenseClient.cpp:768-774)
  <!-- loopx:todo todo_id=todo_9e1f662b690a status=open task_class=advancement_task action_kind=implementation target_key=license-onSnapshot-uaf updated_at=2026-08-05T09:31:17-03:00 -->
- [ ] [P1] MainComponent: timerCallback (390 linhas) chama setAudioDeviceSetup/setMidiInputDeviceEnabled que dispara changeListenerCallback sincrono reconstruindo grafo na mesma stack; reentrancia. Adiar via callAsync (MainComponent.cpp:4496-4884)
  <!-- loopx:todo todo_id=todo_459d955332f1 status=open task_class=advancement_task action_kind=implementation target_key=timer-reentrancy updated_at=2026-08-05T09:31:18-03:00 -->
- [ ] [P1] PadProcessor: std::move de shared_ptr em releasePool no processBlock pode liberar buffer na audio thread se cache evictou ultima referencia. Adiar free para loader (PadProcessor.h:322)
  <!-- loopx:todo todo_id=todo_4c798d2b810d status=open task_class=advancement_task action_kind=implementation target_key=padprocessor-heap-free updated_at=2026-08-05T09:31:18-03:00 -->
- [ ] [P2] DesktopUpdater/ChurchDAWServices: raw new juce::DynamicObject vaza em path excepcional; usar DynamicObject::Ptr ref-counted como PluginScanDiagnostics.h (DesktopUpdater.cpp:318, ChurchDAWServices.h:2035)
  <!-- loopx:todo todo_id=todo_2249137f44da status=open task_class=advancement_task action_kind=implementation target_key=dynamicobject-leak updated_at=2026-08-05T09:31:24-03:00 -->
- [ ] [P2] PadProcessor: cache.find em std::map sob tryLock no processBlock; pre-resolver buffer ativo em slot atomico no load (PadProcessor.h:255-260)
  <!-- loopx:todo todo_id=todo_6944f0cded19 status=open task_class=advancement_task action_kind=implementation target_key=padprocessor-map-find updated_at=2026-08-05T09:31:24-03:00 -->
- [ ] [P2] mascaraDeNotasMapeadas e por numero de nota apenas; causa lookup+lock desnecessario em nota de dispositivo diferente. Tornar por (nota+canal+device) (MainComponent.cpp:6638-6666)
  <!-- loopx:todo todo_id=todo_17d1883cad16 status=open task_class=advancement_task action_kind=implementation target_key=notemask-cross-device updated_at=2026-08-05T09:31:25-03:00 -->
- [ ] [P2] montarChaveDeNota cria ControllerKey com isNote=false; inconsistente com CC-style note key (isNote=true). Setar isNote=true (MainComponent.cpp:3944-3951)
  <!-- loopx:todo todo_id=todo_d8c73eb037f5 status=open task_class=advancement_task action_kind=implementation target_key=montar-chave-isnote updated_at=2026-08-05T09:31:26-03:00 -->
- [ ] [P2] AudioMidiButton: juce::FontOptions(fontSize, juce::Font::bold) positional deprecado; usar FontOptions(fontSize).boldened() como PadPanel.h:34 (AudioMidiButton.h:257)
  <!-- loopx:todo todo_id=todo_254fb8d02cb3 status=open task_class=advancement_task action_kind=implementation target_key=audio-midi-button-font updated_at=2026-08-05T09:31:26-03:00 -->
- [ ] [P2] LaunchkeyControl: return fora do bloco de pad-range engole NoteOn nao-pad no DAW port; mover return para dentro do if de pad-range (LaunchkeyControl.h:323-338)
  <!-- loopx:todo todo_id=todo_84406e0de34c status=open task_class=advancement_task action_kind=implementation target_key=launchkey-daw-port-return updated_at=2026-08-05T09:31:27-03:00 -->
- [ ] [P2] DesktopUpdater: update-state.json em userApplicationDataDirectory roams em perfil móvel; loop-lock pode faltar em maquina nova. Considerar per-machine (DesktopUpdater.cpp:277-280)
  <!-- loopx:todo todo_id=todo_ceaf98f10d42 status=open task_class=advancement_task action_kind=implementation target_key=updater-roaming-state updated_at=2026-08-05T09:31:28-03:00 -->
- [ ] [P2] MainComponent: stopTimer() explicito no topo do destrutor para clareza/seguranca contra reordenamento de base (MainComponent.cpp:1280,1400)
  <!-- loopx:todo todo_id=todo_481cc6e01983 status=open task_class=advancement_task action_kind=implementation target_key=timer-explicit-stop updated_at=2026-08-05T09:31:29-03:00 -->
- [ ] [P0] Inventário de release consolidado em SCRATCH/release-bug-inventory.md + smc-pad-mapping-analysis.md: 3 P0, 12 P1, 9 P2. Causa raiz SMCPad = padButton.setVisible(!learning) MainComponent.cpp:2136
  <!-- loopx:todo todo_id=todo_7a534e8732dc status=open task_class=advancement_task action_kind=diagnostic target_key=release-inventory-consolidated updated_at=2026-08-05T09:32:21-03:00 -->

## Next Action

- [P0] Fix padButton hidden during MIDI Learn + PASSO1 Pad-tone copy (INV-P0-001/002) — todo_09d61e32f69d; evidence in SCRATCH release-bug-inventory.md / smc-pad-mapping-analysis.md

## Recent User Feedback

- Initialized by `loopx bootstrap`.
- 2026-08-05: Multi-agent Core/MIDI + Audio/DSP + UI/State sweeps + SMC Pad root cause completed (analysis goal). Implementation of P0 is next.

## Progress Ledger

- Created the initial goal state and registry connection.
- 2026-08-05: Independent 3-surface bug hunt with durable SCRATCH artifacts; SMC Pad P0 = padButton hidden during Learn (MainComponent.cpp:2136); loopx check credential-scan signal recorded; inventory consolidated.
