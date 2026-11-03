## 2026-08-05T15:24:11Z
Execute full code modifications to genuinely integrate SabrinaProcessor into the MinhaDAW target, DSP graph, and dual UI toggles according to the blueprint in D:\ChurchDAW_Source\.agents\explorer_m4_remediation\analysis.md:

1. `CMakeLists.txt`:
   Add `Source/Plugins/Sabrina/SabrinaProcessor.cpp` and `Source/SabrinaSafeLoad.c` to `target_sources(MinhaDAW PRIVATE ...)`.

2. `Source/MainComponent.h`:
   - `#include "Plugins/Sabrina/SabrinaProcessor.h"`
   - Add `sabrinaNode` to AudioProcessorGraph NodeIDs list (`audioInNode, audioOutNode, midiInNode, masterNode, padNode, sabrinaNode;`).
   - Add `SabrinaProcessor* sabrinaProcessor = nullptr;`.
   - Add `juce::TextButton sabrinaBypassButton { "Sabrina" };`.

3. `Source/MainComponent.cpp`:
   - In constructor: add `sabrinaNode` to graph via `graph.addNode (std::make_unique<SabrinaProcessor>())` and save pointer.
   - Configure `sabrinaBypassButton` (addAndMakeVisible, setClickingTogglesState(true), setToggleState(true), setColour nordRed, onClick callback toggling `sabrinaProcessor->setSabrinaBypassed(!active)`).
   - In `rebuildGraphConnections()`: connect `timbresNode` / `fxChain` output to `sabrinaNode` (if present), and `sabrinaNode` output to `masterNode`.
   - In `resized()`: add `sabrinaBypassButton` into header bar layout bounds for Desktop and Mobile modes.

4. `Source/TimbresProcessor.h` (`TimbresEditor`):
   - Add `juce::TextButton sabrinaToggleBtn { "SABRINA" };` and `std::function<void(bool active)> onSabrinaToggle;`.
   - Configure button, toggle state, nordRed colour, and `onClick` handler in `TimbresEditor` constructor/setup.
   - In `TimbresEditor::resized()`, position `sabrinaToggleBtn` in the master live controls strip.
   - In `MainComponent.cpp` when initializing `TimbresEditor`, set `editor->onSabrinaToggle` to update `sabrinaProcessor->setSabrinaBypassed(!active)` and sync `sabrinaBypassButton`.

5. Build & Verification:
   - Run CMake / MSVC build to compile target `MinhaDAW` (or `ChurchDAWSabrinaProcessorTests`).
   - Execute `.\build_msvc\ChurchDAWSabrinaProcessorTests_artefacts\Release\ChurchDAWSabrinaProcessorTests.exe` to verify isolated unit test suite passes with `SABRINA_VERIFY_OK`.

6. Handoff:
   Write your handoff report to `D:\ChurchDAW_Source\.agents\worker_m4_remediate\handoff.md` detailing modified files, exact changes, build output, test execution output, and verification results. Notify orchestrator via send_message when complete.
