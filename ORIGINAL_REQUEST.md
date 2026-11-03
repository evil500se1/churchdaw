# Original User Request

## 2026-08-03T02:51:49Z

<USER_REQUEST>
# Teamwork Project Prompt — Draft

> Status: Launched.
> Goal: Craft prompt → get user approval → delegate to teamwork_preview

Redesign the Audio/MIDI button on the main mixer and the entire interior of the Audio/MIDI configuration center to have a highly premium, modern visual finish.

Working directory: C:\Users\Administrator\MinhaDAW
Integrity mode: development

## Requirements

### R1. Premium Mixer Button
Redesign the Audio/MIDI trigger button on the main mixer. It must shed its provisional appearance and adopt a high-end, polished aesthetic (e.g., custom drawing, refined borders/icons) that fits a premium professional audio application. 

### R2. Premium Configuration Center
Overhaul the interior layout and styling of the Audio/MIDI Center (`AndroidAudioMidiCenter.h`). Improve the typography, spacing, component alignments, and tab bar visuals. Eliminate any cramped areas, unstyled default components, or harsh boundaries.

## Acceptance Criteria

### Visual Polish & Stability
- [ ] The project compiles successfully for Android without breaking existing Audio/MIDI logic.
- [ ] A screenshot of the main mixer confirms the Audio/MIDI button looks customized and premium (no generic JUCE default button look).
- [ ] A screenshot of the open Audio/MIDI center confirms comfortable padding around all elements, modern typography for headers, and a clear, elegant tab navigation system.
- [ ] An independent UI review confirms no components are overlapping or illegible on mobile screen sizes.
</USER_REQUEST>

## 2026-08-03T04:30:02Z

<USER_REQUEST>
# Teamwork Project Prompt — Draft

> Status: Launched.
> Goal: Craft prompt → get user approval → delegate to teamwork_preview

Publish the new ChurchDAW release by committing all recent UI and MIDI routing changes and pushing them to the remote repository to trigger the automated GitHub Actions pipeline.

Working directory: C:\Users\Administrator\MinhaDAW

Integrity mode: development

## Requirements

### R1. Source Control Versioning
Stage and commit all the recent modifications (Premium Audio/MIDI UI redesign in `AndroidAudioMidiCenter.h` and `AudioMidiButton.h`, plus the per-layer MIDI routing logic in `TimbresProcessor.h`). The team must choose an appropriate commit strategy and descriptive message.

### R2. Remote Publishing
Push the newly created commit(s) to the remote repository to trigger the automated GitHub Actions pipeline.

## Acceptance Criteria

### Delivery Validation
- [ ] `git status` reports a clean working tree.
- [ ] `git log -1` shows a professionally formatted commit describing the new UI and MIDI features.
- [ ] The push to the remote repository succeeds with an exit code of 0, confirming the changes are en route to the CI/CD pipeline.
</USER_REQUEST>

## 2026-08-03T18:51:14Z

<USER_REQUEST>
# Teamwork Project Prompt — Draft

> Status: Launched
> Goal: Craft prompt → get user approval → delegate to teamwork_preview

Convert and integrate the 'PAD WORSHIP 2.0 ED BENTO' (currently in Kontakt .nki/.wav format) into ChurchDAW's native sample format. Ensure the new instrument is available in the Timbre Catalog, correctly mapped, and ready for use across Mac, Windows, and Android builds.

Working directory: C:\Users\Administrator\MinhaDAW
Integrity mode: development

## Requirements

### R1. Sample Mapping and Format Replication
Investigate how the existing timbres are structured and mapped within the ChurchDAW engine. Replicate this exact format to map the provided `.wav` files from the "PAD WORSHIP 2.0 ED BENTO" directory.

### R2. Download Pack Delivery
Package the newly mapped instrument as a Download Pack to be added to ChurchDAW's cloud/server catalog (or local manifest), rather than embedding the raw samples into the app binaries.

### R3. Standard UI Integration
Make the new pad available in the Timbre Catalog using ChurchDAW's standard UI. Do not use the custom artwork (Background.png, Wallpaper.jpg) for the app's UI.

## Acceptance Criteria

### Instrument Mapping
- [ ] A new instrument mapping file (e.g., .sfz or internal format) is successfully created in the correct ChurchDAW format.
- [ ] A script or programmatic check validates that the newly created mapping file correctly references the provided `.wav` samples without broken paths.

### Catalog Registration
- [ ] The new instrument is packaged correctly into a Download Pack format.
- [ ] The ChurchDAW catalog manifest (e.g., JSON file) is successfully updated to include the new "PAD WORSHIP 2.0 ED BENTO" pack.
</USER_REQUEST>

## Follow-up — 2026-08-03T19:17:21Z

<USER_REQUEST>
Official timbre display name in ChurchDAW: "New Pad".
Ensure that the name displayed in the DAW catalog / UI for this timbre is "New Pad". This must be reflected in the catalog manifests (catalog.json, timbre-catalog-v1.json) and any preset metadata.
</USER_REQUEST>

## Follow-up — 2026-08-03T19:35:02Z

<USER_REQUEST>
# Teamwork Project Prompt — Draft

> Status: Launched.
> Goal: Craft prompt → get user approval → delegate to teamwork_preview

Resolve the "manifest.json sha 256 não corresponde" error that occurs on macOS when opening libraries in the 1.6.0 client.

Working directory: C:\Users\Administrator\MinhaDAW
Integrity mode: development

## Requirements

### R1. Resolve SHA-256 Mismatch on macOS
Investigate and fix the issue causing the application to fail the SHA-256 validation for `manifest.json` on macOS. Ensure that the hash computation mechanism is robust against common cross-platform issues (e.g., CRLF vs LF line endings, encoding differences, or unexpected file modification during extraction).

### R2. Maintain Windows Compatibility
The fix must be strictly cross-platform. Ensure that the resolution for macOS does not break the existing manifest validation logic for Windows users.

## Acceptance Criteria

### Verification & Robustness
- [ ] The root cause of the SHA-256 mismatch is identified and documented.
- [ ] The C++ codebase compiles successfully without errors.
- [ ] An automated test, script, or independent code review confirms that `manifest.json` parsing and hashing now yield identical, correct results regardless of the host OS (macOS/Windows).
</USER_REQUEST>

## Follow-up — 2026-08-03T22:10:26Z

<USER_REQUEST>
Convert and integrate the "Ultimate Pianos 2" library (located in multi-part RAR archives in C:\Users\Administrator\Downloads\Telegram Desktop) into ChurchDAW's native sample format. Skip any piano models already present in ChurchDAW, convert all new pianos, pads, and layer instruments into ChurchDAW's JSON format, update the catalog manifest, and package them as download packs.

Working directory: C:\Users\Administrator\MinhaDAW
Integrity mode: development

## Requirements

### R1. Archive Extraction & Asset Inspection
Extract `Ultimate Pianos 2.part1.rar` (and its multi-part archives) from `C:\Users\Administrator\Downloads\Telegram Desktop` into a temporary staging directory using 7-Zip (`C:\Program Files\7-Zip\7z.exe`) or WinRAR, inspect the instrument structure (.nki, .sfz, wav mapping), and inventory all instruments.

### R2. Deduplication & Instrument Conversion
Compare the extracted instruments against ChurchDAW's existing catalog (`MinhaDAW\Pads`, `MinhaDAW\Pianos`, etc.). Ignore any piano models already present. For all new unique instruments (new pianos, pads, layers, etc.), create full sample mappings and ChurchDAW JSON metadata (`name`, `raw_label`, `category`, `samples`).

### R3. Catalog Registration & Download Pack Packaging
Package the newly created instruments as Download Packs via `BuildTimbreCatalog.ps1` (or catalog updater), ensuring they are validly registered with `ready: true` and ready for deployment.

## Acceptance Criteria

### Asset Extraction & Inventory
- [ ] Multi-part RAR archive is cleanly extracted without missing parts or corruption.
- [ ] List of new vs existing timbres is generated and verified.

### Instrument Mapping & Catalog Integrity
- [ ] New instrument folders contain valid `.json` metadata and mapped `.wav` samples.
- [ ] Catalog generator validates that all samples exist and references in JSON are unbroken.
- [ ] Catalog manifest (`timbre-catalog-v1.json`) is updated with the new instruments.
</USER_REQUEST>

## Follow-up — 2026-08-03T22:18:44Z

The user provided two possible passwords for extracting the multi-part RAR archives (Ultimate Pianos 2.part1.rar):

1) `Simples&Vsts2025®`
2) `Samples&Vsts®`



## Follow-up — 2026-08-03T22:19:02Z

CONFIRMED: The correct password for the RAR archives is: `Samples&Vsts®`
(`Simples&Vsts2025®` returned password error, while `Samples&Vsts®` passed validation).

## Follow-up — 2026-08-03T22:57:09Z

<USER_REQUEST>
Resume and complete the integration of the "Ultimate Pianos 2" library into ChurchDAW:

Extraction folder: C:\Users\Administrator\Downloads\Telegram Desktop\extracted_ultimate\Ultimate Pianos 2
Extracted NCW samples are in C:\Users\Administrator\Downloads\Telegram Desktop\extracted_ultimate\Ultimate Pianos 2\Samples

Tasks:
1. Convert the NCW samples for the NEW instruments (Pads: up2_pad_angelic, up2_pad_cinematic, up2_pad_happy, up2_pad_majestic, up2_pad_odyssey, up2_pad_organic, up2_pad_sheltered, up2_pad_space, Drone Ocean View; Pianos: Grand Imperial XL, Grand Lady D L, Italian Grand XL, Royal Grand 3D XL, Silver Grand XL, Velvet Grand XL, Mellow Upright XL, Queen Upright L, Rain Piano L, Romantic Upright L, Saloon Upright L, Electric Grand 1, Wurlitzer 1, EP7 Tines Amped, EP8 Nefertiti) into 24-bit PCM WAV.
2. Build ChurchDAW native JSON metadata for each new instrument folder in MinhaDAW\Pianos or MinhaDAW\Pads.
3. Update the catalog manifest (timbre-catalog-v1.json) using BuildTimbreCatalog.ps1.

Working directory: C:\Users\Administrator\MinhaDAW
Integrity mode: development
</USER_REQUEST>

## 2026-08-04T02:05:45Z

<USER_REQUEST>
# Teamwork Project Prompt — Draft

> Status: Launched.
> Goal: Craft prompt → get user approval → delegate to teamwork_preview

Extract the "Black Upright" piano timbre from ChurchDAW and transfer it directly into the internal memory of the connected Casio keyboard so it can be played standalone.

Working directory: C:\Users\Administrator\MinhaDAW
Integrity mode: development

## Requirements

### R1. Hardware Identification & Capability Verification
Identify the exact model of the connected Casio keyboard via USB MIDI (e.g., using SysEx identity requests) to verify if its hardware supports custom sample uploading and to determine the required proprietary patch/sample format (e.g., `.tn7`, `.ac7`, raw PCM mapping, etc.).

### R2. Sample Extraction & Format Conversion
Extract the raw audio samples, loop points, and velocity layer mappings for the "Black Upright" timbre from the ChurchDAW library. Convert these assets into the specific proprietary file format required by the detected Casio model.

### R3. Hardware Transfer Protocol
Implement the data transfer protocol over USB MIDI (or serial data transfer) to upload the compiled custom patch file directly into the Casio keyboard's internal user memory bank.

## Acceptance Criteria

### Hardware Interfacing & Integrity
- [ ] A diagnostic script successfully connects to the Casio keyboard, reads its SysEx ID, and correctly identifies the model and its sample import capabilities.
- [ ] The "Black Upright" audio samples are successfully converted into a valid Casio-compatible patch file without loss of critical velocity layers.
- [ ] The transfer script successfully transmits the data blocks to the Casio keyboard, confirming a complete and uncorrupted upload.
- [ ] (Manual Verification) The user can unplug the keyboard from the computer and play the "Black Upright" piano sound directly from the Casio's internal memory.
</USER_REQUEST>

## Follow-up — 2026-08-04T02:46:49Z

<USER_REQUEST>
# Teamwork Project Prompt — Draft

> Status: Launched
> Goal: Craft prompt → get user approval → delegate to teamwork_preview

Completely overhaul the connected Casio keyboard hardware memory: wipe all existing user timbres, compile and upload a massive multi-timbre payload from ChurchDAW (Black Upright, Pad 2, Pad Rev Shimmer, Cordas Suave), and attempt a UI firmware jailbreak.

Working directory: C:\Users\Administrator\MinhaDAW
Integrity mode: benchmark

## Requirements

### R1. Deep Memory Wipe
Execute a "Format User Bank" SysEx command to wipe all existing user patches from the Casio keyboard's internal memory to free up maximum storage space.

### R2. Multi-Timbre Extraction & Compression
Extract the following specific timbres from the ChurchDAW library: "Black Upright", "Pad 2", "Pad Rev Shimmer", and "Cordas Suave". Compress and compile these multi-layered samples into a single proprietary Casio payload (`.tn7`/`.tu7` or equivalent), ensuring it fits within the hardware's physical storage limit.

### R3. Firmware UI Jailbreak (Experimental)
Attempt to reverse-engineer the display firmware via USB SysEx to unlock the native LCD/Touch UI, allowing custom visual modifications ("premium interface"). Bypass standard security barriers if necessary, acknowledging the bricking risk.

### R4. Complete Hardware Transfer
Upload the compiled mega-payload into the Casio's memory using the bulk USB MIDI protocol, and perform end-to-end testing to verify all 4 timbres are playable standalone.

## Acceptance Criteria

### Hardware Interfacing & Integrity
- [ ] The keyboard memory is confirmed wiped via diagnostic read.
- [ ] All 4 timbres are successfully converted, compressed, and packed without exceeding the Casio's max memory limit.
- [ ] A transfer script successfully transmits the entire payload.
- [ ] An adversarial agent confirms the UI jailbreak mechanism either succeeds or definitively proves the firmware is physically locked/ROM-only.
- [ ] (Manual Verification) The user can play the 4 premium ChurchDAW timbres directly from the Casio.
</USER_REQUEST>

## 2026-08-05T13:27:18Z

<USER_REQUEST>
# Teamwork Project Prompt — Draft

> Status: Ready for launch — awaiting user approval.
> Goal: Craft prompt → get user approval → delegate to teamwork_preview

O objetivo é buscar o plugin compilado "Sabrina" (VST3 / AU / DLL) na pasta de Downloads e embuti-lo nativamente no motor do ChurchDAW. O sistema precisará de um "toggle" (chave liga/desliga) multiplataforma funcionando no Mac, Windows e Android.

Working directory: D:\ChurchDAW_Source
Integrity mode: demo

## Requirements

### R1. Coleta e Preparação do Plugin Sabrina
Identificar e copiar os binários do plugin "Sabrina" da pasta `C:\Users\Fabiopc\Downloads` para dentro da estrutura do projeto `ChurchDAW_Source`. O agente pode investigar o código-fonte/arquivos do plugin (se disponíveis) para entender seu comportamento.

### R2. Host Nativo de Efeitos (JUCE AudioProcessorGraph)
Implementar uma ponte nativa no C++ que carregue o binário VST3/AU do Sabrina no pipeline de áudio do ChurchDAW (ex: no `TimbresProcessor` ou Master). 
*(Aviso: Como o Android não roda DLL/VST3 nativo do Windows/Mac, a implementação deve usar compilação condicional ou prover um bypass seguro no Android caso o formato compilado não suporte ARM mobile).*

### R3. Toggle de Bypass na UI
Criar um botão/toggle na interface principal (JUCE UI) acessível em Desktop e Mobile. Quando o botão for desativado (Bypass), o áudio deve pular o processamento do Sabrina para economizar 100% da CPU alocada a ele.

## Acceptance Criteria

### Compilação Multiplataforma
- [ ] O projeto compila com sucesso usando o CMake e o compilador padrão sem quebrar a compilação cruzada (Android x Desktop).

### Validação do Áudio
- [ ] Um agente julgador (ou teste unitário adicionado) valida que o sinal de áudio sofre alteração quando o Sabrina está ativado.
- [ ] O tempo de processamento DSP (latência) cai significativamente quando o Toggle do Sabrina está desligado, comprovando que o Bypass suspende o processamento.
</USER_REQUEST>

## 2026-09-03T03:06:03Z

<USER_REQUEST>
This is a single self-contained fix; keep it small and focused.

Redesenhar completamente o canal Master do ChurchDAW no formato de console de estúdio profissional (SSL/Neve), tornando-o mais intuitivo, ergonômico e visualmente integrado com os canais ao vivo.

Working directory: C:\ChurchDAW-rc-1.6.27
Integrity mode: development

## Requirements

### R1. Layout Master Console Integrado
Reestruturar a coluna Master em 3 blocos bem proporcionados:
- **Master FX (Topo):** Slots de insert de efeitos claros e botão de edição de Reverb com controle direto de retorno (Return Level).
- **Master Bus & Medição (Centro):** Fader Master vertical longo com acabamento idêntico aos faders de canal, acompanhado por medidor estéreo de pico (L/R) de alta resolução e medidor de redução do Limiter contra distorção do som da igreja.
- **Live Utility & Emergency (Rodapé):** Botão PANIC grande e tátil com iluminação vermelha de alerta, controle de Transpose em dial central com visor numérico nítido do semitom (-12 a +12) e anel de LEDs, e interruptores de Sustain e Inverter pedal.

### R2. Preservação Total de Áudio e DSP
- Conservar a integridade de áudio, curvas de ganho, Master Limiter com true-peak e compatibilidade MIDI de hardware.
- Todos os testes unitários de DSP e segurança MIDI devem ser executados e passar com 100% de sucesso.

### R3. Implantação e Disponibilidade no Sistema
- Compilar o binário em Release e implantá-lo nos diretórios de execução do usuário (`Desktop\ChurchDAW-NOVO.exe` e `AppData\Local\Programs\ChurchDAW\ChurchDAW.exe`), garantindo que o bundle de backup para Mac Mini também seja atualizado.

## Acceptance Criteria

### Visual & Usabilidade
- [ ] O fader Master tem curso vertical estendido de pelo menos 180px e acabamento em alumínio usinado escovado.
- [ ] O medidor de pico estéreo (L/R) e o medidor de redução do Limiter ficam perfeitamente alinhados ao lado do fader, sem sobreposição de textos.
- [ ] O botão PANIC é destacado como alvo de toque prioritário no rodapé da coluna.
- [ ] O display de Transpose mostra o número do semitom (-12 a +12) com alto contraste no centro do dial.
- [ ] O rack Master FX permite visualizar o status dos plugins e controlar o retorno do Reverb de forma limpa e direta.

### Validação Automatizada
- [ ] Compilação do target `MinhaDAW` em Release concluída com código de saída 0.
- [ ] Execução de `ChurchDAWMasterLimiterTests.exe` finalizada com status `MASTER_LIMITER_TESTS_OK`.
- [ ] Execução de `ChurchDAWMidiMappingSafetyTests.exe` finalizada com status `MIDI_MAPPING_SAFETY_OK`.
- [ ] Executável copiado e verificado com hash SHA-256 idêntico em `Desktop\ChurchDAW-NOVO.exe`.
- [ ] Bundle Git atualizado em `C:\Users\Administrator\churchdaw-latest-1.6.30.bundle`.
</USER_REQUEST>
