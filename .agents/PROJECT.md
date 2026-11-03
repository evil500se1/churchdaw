# Project: Sabrina Plugin Integration (ChurchDAW)

## Architecture
- **Plugin Storage**: `Plugins/Sabrina/bin/win_x64/Sabrina.dll`
- **Native Bridge Component**: `Source/Plugins/Sabrina/SabrinaProcessor.h` and `.cpp` (`SabrinaProcessor` class inheriting from `juce::AudioProcessor`).
- **Graph Node Integration**: `AudioProcessorGraph` in `Source/MainComponent.cpp` (`rebuildGraphConnections`). Inserted between `TimbresProcessor`/FX Chain and `MasterProcessor`.
- **UI Toggle Controls**: Dual mounting in `MainComponent` header action bar (Desktop + Mobile touch row) and `TimbresEditor` master controls strip.
- **Cross-Platform & Android Strategy**: On Windows Desktop, `SabrinaProcessor` dynamically bridges `Sabrina.dll` (PE32+ VST 2.4). On Android / non-Windows, `SabrinaProcessor` uses a native C++ DSP saturation/shaper fallback algorithm. On all platforms, setting Bypassed skips `processBlock` entirely (0% CPU load).

## Feature Inventory
| # | Feature | Description | Milestone | Source |
|---|---------|-------------|-----------|--------|
| 1 | Plugin Ingestion | Copy Sabrina.dll from Downloads to ChurchDAW_Source structure | M1 (DONE) | ORIGINAL_REQUEST R1 |
| 2 | C++ JUCE DSP Bridge | Implement AudioProcessor wrapper & wire into AudioProcessorGraph | M2 (DONE) | ORIGINAL_REQUEST R2 |
| 3 | Android Safe Fallback | Native C++ DSP fallback for Android/ARM non-PE target | M2 (DONE) | ORIGINAL_REQUEST R2 |
| 4 | UI Toggle Button | Add cross-platform Desktop & Mobile bypass toggle button | M3 (DONE) | ORIGINAL_REQUEST R3 |
| 5 | CMake Build Integration | Integrate Sabrina C++ bridge into CMakeLists.txt targets | M4 | ORIGINAL_REQUEST AC1 |
| 6 | DSP Audio Modification Test | Verify audio signal is modified when Sabrina is enabled | M4 | ORIGINAL_REQUEST AC2 |
| 7 | Zero-CPU Bypass Verification | Verify CPU consumption drops to zero when bypassed | M4 | ORIGINAL_REQUEST AC2 |

## Milestones
| # | Name | Scope | Dependencies | Status |
|---|------|-------|-------------|--------|
| M1 | Plugin Ingestion & Codebase Structuring | Copy Sabrina.dll & create Source/Plugins/Sabrina/ directory structure | None | DONE |
| M2 | C++ JUCE DSP Bridge & Android Fallback | Implement SabrinaProcessor & integrate into AudioProcessorGraph in MainComponent | M1 | DONE |
| M3 | Desktop & Mobile UI Bypass Toggle | Add UI Toggle Buttons in MainComponent header and TimbresEditor master controls | M2 | DONE |
| M4 | Cross-Platform Build & Audio Verification | Update CMakeLists.txt, verify compilation, audio modification & CPU bypass efficiency | M3 | IN_PROGRESS |

## Interface Contracts
### `SabrinaProcessor` ↔ `MainComponent` / `AudioProcessorGraph`
- **Class**: `class SabrinaProcessor : public juce::AudioProcessor`
- **Header**: `Source/Plugins/Sabrina/SabrinaProcessor.h`
- **Key Methods**:
  - `void setSabrinaBypassed(bool shouldBypass)`: Thread-safe atomic toggle.
  - `bool isSabrinaBypassed() const`: Returns current bypass state.
  - `void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override`: Performs DSP processing when active; zero-copy pass-through when bypassed.
- **Node Wiring in `MainComponent.cpp`**:
  ```cpp
  sabrinaNode = graph.addNode(std::make_unique<SabrinaProcessor>());
  // Connections:
  graph.addConnection({ { timbresNode->nodeID, 0 }, { sabrinaNode->nodeID, 0 } });
  graph.addConnection({ { timbresNode->nodeID, 1 }, { sabrinaNode->nodeID, 1 } });
  graph.addConnection({ { sabrinaNode->nodeID, 0 }, { masterNode->nodeID, 0 } });
  graph.addConnection({ { sabrinaNode->nodeID, 1 }, { masterNode->nodeID, 1 } });
  ```

## Code Layout
- `Plugins/Sabrina/bin/win_x64/Sabrina.dll`: Precompiled VST2 plugin binary.
- `Source/Plugins/Sabrina/SabrinaProcessor.h`: Native C++ JUCE AudioProcessor class header.
- `Source/Plugins/Sabrina/SabrinaProcessor.cpp`: Native C++ JUCE AudioProcessor implementation with Windows VST host loader and Android fallback DSP.
- `Source/MainComponent.h`: Graph node handles (`sabrinaNode`), header toggle button (`sabrinaBypassButton`).
- `Source/MainComponent.cpp`: Graph node insertion, connection building (`rebuildGraphConnections`), UI button initialization, event listener, and layout positioning.
- `Source/TimbresProcessor.h`: `TimbresEditor` master controls strip integration with `sabrinaToggleBtn`.
- `CMakeLists.txt`: Source list addition for `SabrinaProcessor.cpp/.h`.
