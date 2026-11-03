# Handoff Report: UI Layer & Visual Assets Technical Exploration

**Agent**: `teamwork_preview_explorer 2`  
**Working Directory**: `D:\ChurchDAW_Source\.agents\explorer_ui_assets`  
**Target Repository**: `D:\ChurchDAW_Source`  
**Timestamp**: 2026-08-04T15:53:00Z  

---

## 1. Observation

### 1.1 Directory Structure & UI Code Locations
The UI layer and visual asset files of ChurchDAW are organized across `D:\ChurchDAW_Source\Source`, `D:\ChurchDAW_Source\Source\ChurchOrgan\UI`, `D:\ChurchDAW_Source\Assets`, and `D:\ChurchDAW_Source\Packaging`:
- **Core UI Components**: `Source/MainComponent.h`, `Source/MainComponent.cpp`, `Source/NordLookAndFeel.h`, `Source/OledDisplay.h`, `Source/LevelMeter.h`, `Source/DeviceInfoBar.h`, `Source/AudioMidiButton.h`, `Source/PadPanel.h`, `Source/LiveScenePanel.h`, `Source/TimbrePicker.h`, `Source/LibraryPanel.h`, `Source/MidiLearnPanel.h`, `Source/StartupPanel.h`, `Source/AboutPanel.h`, `Source/AndroidAudioMidiCenter.h`, `Source/DesktopLoginPanel.h`, `Source/DesktopLoginPanel.cpp`, `Source/AndroidLoginPanel.h`, `Source/AndroidStageLayout.h`.
- **Organ UI Engine**: `Source/ChurchOrgan/ChurchOrganEditor.h`, `Source/ChurchOrgan/ChurchOrganEditor.cpp`, `Source/ChurchOrgan/ChurchOrganLayout.h`, `Source/ChurchOrgan/ChurchOrganLayout.cpp`, `Source/ChurchOrgan/ChurchOrganDesignTokens.h`, `Source/ChurchOrgan/UI/ChurchOrganControls.h`, `Source/ChurchOrgan/UI/ChurchOrganControls.cpp`, `Source/ChurchOrgan/UI/ChurchOrganViewModel.h`.
- **Visual Assets**: `Assets/glyph_piano.png` (84,699 B), `Assets/glyph_pad.png` (72,912 B), `Assets/glyph_organ.png` (71,144 B), `Assets/glyph_strings.png` (83,716 B), `Assets/glyph_ambience.png` (71,682 B), `Assets/glyph_bells.png` (64,210 B), `Assets/glyph_lead.png` (94,357 B), `Assets/icon.png` (1,693,164 B), `Assets/icon-android.png` (1,791,238 B), `Assets/icon-android-chroma.png` (1,839,111 B), `Assets/logo_round.png` (451,932 B), `Installer/ChurchDAW.ico`, `Packaging/PlayStore/app-icon-512.png`.

### 1.2 Class Map & Component Hierarchy
```
MainComponent (Root Window & Audio Engine Coordinator, juce::Component)
 ├── DeviceInfoBar (juce::Component, SettableTooltipClient) — Connected MIDI & Audio device chips
 ├── AudioMidiButton (juce::TextButton) — Custom trigger button for Audio/MIDI settings
 ├── OledDisplay (juce::Component, juce::Timer) — Nord Electro 7 program display, stacked channel list, chord detector
 ├── Slot[0..3] (Channel Cards) — 4 instrument/effect slots (Label, ComboBox, OpenButton, Volume Fader, Rotary Knobs, Level Meters)
 ├── Master Section — Master Fader (juce::Slider), LevelMeter (juce::Component, stereo peak/RMS meter)
 ├── PadPanel (juce::Component, overlay card) — Continuous Worship Pad controls (12-key keyboard, cutoff knob, volume knob)
 ├── LiveScenePanel (juce::Component, juce::Timer) — 10-scene Live Mode setlist panel
 ├── TimbrePicker (juce::Component, ListBoxModel) — Searchable popover/drawer for selecting timbres & sound packs
 ├── MidiLearnPanel (juce::Component) — 2-step wizard overlay embedded in OledDisplay during MIDI assignment
 ├── LibraryPanel (juce::Component) — Soundbank & sample pack download catalog panel
 ├── ChurchOrganEditor (juce::Component, docked/overlay) — Dedicated Church Organ console editor
 │    ├── ChurchOrganDrawbarBank (juce::Component) — 9 physical-style organ drawbars (16', 5 1/3', 8', 4', 2 2/3', 2', 1 3/5', 1 1/3', 1')
 │    │    └── ChurchOrganDrawbar[0..8] (juce::Slider) — Individual drawbar with color coding & MIDI feedback
 │    ├── ChurchOrganPanel (juce::Component) — Subsystem control containers (Rotary, Percussion, Worship Layer, Engine)
 │    └── Page Navigation Buttons — Live, Engine, Leslie, Worship, Presets
 ├── StartupPanel (juce::Component) — Initial splash, setup wizard, mandatory update screen
 ├── AboutPanel (juce::Component) — About modal dialog with brand logo, version, contact details
 ├── AndroidAudioMidiCenter (juce::Component, juce::Timer) — Full-screen Android Audio/MIDI/Latency center
 └── DesktopLoginPanel / AndroidLoginPanel (juce::Component) — License verification & account login
```

### 1.3 Exact Code References & Mechanics
1. **Design System & Styling (`NordLookAndFeel.h`)**:
   - `bgApp()` (`0xff0c0e12`), `bgCard()` (`0xff171b22`), `nordRed()` (`0xffe91845`), `nordRedBright()` (`0xffff2f5c`), `amberSolo()` (`0xffffb020`), `greenOk()` (`0xff4fd68a`), `redClip()` (`0xffff1744`).
   - Category accent colors: `catPiano()` (`0xff5b8a9a`), `catPad()` (`0xff8a6ba8`), `catBell()` (`0xffc4a35a`), `catStrings()` (`0xff4a9b8c`), `catOrgan()` (`0xffb7773c`), `catLead()` (`0xff4e86c5`), `catPlugin()` (`0xffc9823a`).
   - `drawChurchDAWMark()` (lines 158–257): Procedural vector drawing function rendering church silhouette, steeples, cross, stained glass, and keyboard keys. Used to avoid GPU PNG texture upload bugs on certain Android drivers.
   - Custom widget drawing: `drawRotarySlider` (lines 508–577), `drawLinearSlider` (lines 583–760), `drawToggleButton` (lines 765–800), `drawTickBox` (lines 474–501).

2. **UI-to-Audio Communication & Threading (`MainComponent.h`, `MainComponent.cpp`, `ChurchOrganViewModel.h`)**:
   - Class inheritance in `MainComponent`: `juce::Component`, `juce::ChangeListener`, `juce::ComponentListener`, `juce::MidiInputCallback`, `juce::AudioProcessorListener`, `juce::AsyncUpdater`, `juce::Timer`.
   - `ChurchOrganParameterSink` (in `ChurchOrganViewModel.h:124-144`): Interface providing decoupled, message-thread safe snapshot updates (`ChurchOrganUiSnapshot`) between `ChurchOrganEditor` UI and the DSP engine (`ChurchOrganLayerProcessor`).
   - `CCPickup` mechanism (in `MainComponent.h:522-536`): Prevents parameter jumps when physical MIDI knobs are turned by verifying incoming CC value matches current parameter value before engaging (`ccHasPickedUp`).
   - `LevelMeter` (in `LevelMeter.h:37-45`): Thread-safe peak/RMS metering using "read-and-reset" lambda callbacks (`fetchLevel`, `readAndResetLayerLevel()`). Timer runs at 10 Hz on mobile and 30 Hz on desktop to prevent message thread saturation.
   - `MidiLearnPanel` & `noteMappings` (in `MainComponent.h:230-280`): Thread-safe MIDI Learn system. Atomic bitmasks (`mascaraDeNotasMapeadas`, `mascaraDeNotasSuprimidas`) allow lock-free note interception on the MIDI thread while mapping data is edited under `mapLock` on the message thread.

3. **User Interaction Workflows**:
   - **Live Worship Mode**: `LiveScenePanel` provides 10 scene buttons with double-buffer protection (`geracaoDaCena`) so patch switches do not truncate active reverb tails or sustained pad notes.
   - **Continuous Pad Control**: `PadPanel` provides continuous background worship pads with a custom 12-key selector, cutoff brightness filter knob, and volume control.
   - **MIDI Learn Assignment**: 2-step workflow where the user clicks any GUI slider/knob (Step 1) and moves a hardware controller knob/fader or key (Step 2) to establish instant, auto-saved CC/Note bindings.

---

## 2. Logic Chain

1. **Observation**: `MainComponent.h` inherits from `juce::Component`, `juce::MidiInputCallback`, `juce::AudioProcessorListener`, `juce::AsyncUpdater`, and `juce::Timer`.
2. **Logic**: `MainComponent` serves as both the top-level GUI layout container and the event dispatcher between hardware MIDI inputs, JUCE audio processor nodes, and secondary UI views.

3. **Observation**: `NordLookAndFeel.h` centralizes design tokens (`bgApp`, `bgCard`, `nordRed`, `catPiano`, etc.) and overrides `drawRotarySlider`, `drawLinearSlider`, `drawToggleButton`, and `drawTickBox`.
4. **Logic**: The visual appearance across desktop (Windows/macOS) and mobile (Android/iOS) is unified under a dark Nord synthesizer theme with dark graphite background surfaces, dark red brand accents, and category-coded instrument indicators.

5. **Observation**: `LevelMeter.h` throttles UI repaint timers to 10 Hz on mobile platforms (`#if JUCE_ANDROID || JUCE_IOS`) and 30 Hz on desktop (`#else`), while `OledDisplay.h` caches shadows (`oledShadowCache`) and draws vector marks (`drawChurchDAWMark`).
6. **Logic**: Software rendering on mobile high-resolution displays (e.g., 2460x1080) can saturate the message thread CPU core. Throttling meter timers and caching expensive drop shadows ensures smooth GUI interaction and leaves CPU headroom for real-time audio threads.

7. **Observation**: The `ChurchOrganEditor` relies on `ChurchOrganParameterSink` and `ChurchOrganUiSnapshot` in `ChurchOrganViewModel.h`.
8. **Logic**: The organ UI is completely decoupled from JUCE `AudioProcessor` audio thread state. The UI operates on thread-safe value snapshots and dispatches parameter changes via the `ChurchOrganParameterSink` interface.

---

## 3. Caveats

- **No Caveats**: All UI components, header files, source files, LookAndFeel implementations, asset folders, view models, and threading mechanisms were directly inspected and verified in `D:\ChurchDAW_Source`.

---

## 4. Conclusion

ChurchDAW features a highly polished, professional dark Nord-style UI architecture built on JUCE V4 and custom C++ components. 

Key technical characteristics:
1. **Decoupled & Thread-Safe UI-to-Audio Architecture**: Communication relies on atomic indicators (`mascaraDeNotasMapeadas`, `applyingMappedCC`), snapshot interfaces (`ChurchOrganParameterSink`), CC pickup guard logic, and `AsyncUpdater` graph mutations.
2. **Optimized Multi-Platform Performance**: Meter update rates (10 Hz mobile / 30 Hz desktop), vector fallbacks (`drawChurchDAWMark`), and pre-rendered drop shadow caching prevent message-thread CPU bottlenecks.
3. **Comprehensive Visual Asset System**: High-resolution PNG category glyphs (`glyph_piano.png` through `glyph_lead.png`), launcher icons, round logo assets, and procedural vector drawing functions provide unified styling across Windows, macOS, Android, and iOS targets.
4. **Tailored Worship Workflows**: Integrated 10-scene Live Mode setlist panel, Continuous Worship Pad panel, 9-drawbar Organ Editor, and 2-step MIDI Learn system directly target real-time church performance needs.

---

## 5. Verification Method

To independently verify this UI and Visual Asset analysis:

1. **Inspect UI Components & Files**:
   - `D:\ChurchDAW_Source\Source\NordLookAndFeel.h` (Design tokens, custom controls, vector mark)
   - `D:\ChurchDAW_Source\Source\MainComponent.h` & `.cpp` (Root component hierarchy, event callbacks, MIDI mapping)
   - `D:\ChurchDAW_Source\Source\OledDisplay.h` (Program band, chord detection display)
   - `D:\ChurchDAW_Source\Source\LevelMeter.h` (Peak/RMS level meter, timer rates)
   - `D:\ChurchDAW_Source\Source\ChurchOrgan\ChurchOrganEditor.h` & `UI/ChurchOrganViewModel.h` (Organ UI & View Model snapshot interface)
   - `D:\ChurchDAW_Source\Assets\` (All instrument glyphs, logo, app icons)

2. **Verify Project Compilation & Projucer Setup**:
   - Inspect `D:\ChurchDAW_Source\ChurchDAW.jucer` lines 113–125 for asset registrations under `<GROUP id="ChDWast" name="Assets">`.
   - Inspect `D:\ChurchDAW_Source\CMakeLists.txt` lines 19–49 for JUCE GUI module links (`juce::juce_gui_basics`, `juce::juce_gui_extra`, `juce::juce_graphics`).
