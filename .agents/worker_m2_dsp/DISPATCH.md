## 2026-08-05T10:37:17Z

<USER_REQUEST>
You are worker_m2_dsp.
Your working directory is: D:\ChurchDAW_Source\.agents\worker_m2_dsp
Read D:\ChurchDAW_Source\.agents\ORIGINAL_REQUEST.md and D:\ChurchDAW_Source\.agents\PROJECT.md first.

Also read the reports from Step 0 Explorers:
- D:\ChurchDAW_Source\.agents\explorer_dsp_engine\analysis.md
- D:\ChurchDAW_Source\.agents\explorer_dsp_engine\handoff.md

Your task for Milestone M2: C++ JUCE DSP Bridge & Android Fallback:
1. Create your working directory D:\ChurchDAW_Source\.agents\worker_m2_dsp if it doesn't exist.
2. Implement `Source/Plugins/Sabrina/SabrinaProcessor.h`:
   - Define `class SabrinaProcessor : public juce::AudioProcessor`.
   - Implement complete JUCE AudioProcessor boilerplate methods (`getName`, `prepareToPlay`, `releaseResources`, `processBlock`, `createEditor`, `hasEditor`, `acceptsMidi`, `producesMidi`, `isMidiEffect`, `getTailLengthSeconds`, `getNumPrograms`, `getCurrentProgram`, `setCurrentProgram`, `getProgramName`, `changeProgramName`, `getStateInformation`, `setStateInformation`).
   - Add thread-safe bypass state control (`std::atomic<bool> bypassed { false }`, `void setSabrinaBypassed(bool b)`, `bool isSabrinaBypassed() const`).
3. Implement `Source/Plugins/Sabrina/SabrinaProcessor.cpp`:
   - Implement `processBlock`:
     - If `bypassed.load(std::memory_order_relaxed)` is true, perform immediate zero-copy pass-through with 0% CPU consumption.
     - If active on Desktop Windows (`#if JUCE_WINDOWS && !JUCE_ANDROID`), support dynamic VST plugin hosting for `Plugins/Sabrina/bin/win_x64/Sabrina.dll` via JUCE plugin hosting, with smooth fallback to native C++ DSP saturation/shaper algorithm if VST host binary loading is unavailable.
     - If active on Android / non-Windows (`#if JUCE_ANDROID` or `#if !JUCE_WINDOWS`), run the native C++ DSP saturation/shaper algorithm (e.g. `std::tanh` warm saturation / harmonic drive) so audio processing is modified cleanly and safely on mobile without binary architecture incompatibility.
4. Integrate `SabrinaProcessor` into `Source/MainComponent.h` and `Source/MainComponent.cpp`:
   - In `MainComponent.h`: Add `juce::AudioProcessorGraph::Node::Ptr sabrinaNode;` and `std::shared_ptr<SabrinaProcessor> sabrinaProcessor;`.
   - In `MainComponent.cpp`: In `rebuildGraphConnections()`, instantiate `sabrinaProcessor` node, add `sabrinaNode` to `graph`, and wire signal flow from `timbresNode` -> `sabrinaNode` -> `masterNode`.
5. Verify files exist, verify clean compilation structure and graph node wiring.
6. Write your handoff report to `D:\ChurchDAW_Source\.agents\worker_m2_dsp\handoff.md`.
</USER_REQUEST>
