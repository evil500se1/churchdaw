#include "MidiMappingSafety.h"
#include "MidiMappingPanel.h"
#include "PerformanceMidiSafety.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <thread>
#include <vector>

namespace
{
    [[noreturn]] void fail (const char* message)
    {
        std::cerr << "MIDI_MAPPING_SAFETY_FAIL: " << message << std::endl;
        std::exit (1);
    }

    void expect (bool condition, const char* message)
    {
        if (! condition)
            fail (message);
    }

    juce::File pairJournalFor (const juce::File& ccFile)
    {
        return ChurchDAWMidiMapping::midiMapPairJournalFile (ccFile);
    }

    juce::String pendingTransactionIdFor (const juce::File& ccFile)
    {
        const auto xml = juce::parseXML (pairJournalFor (ccFile));
        return xml != nullptr ? xml->getStringAttribute ("transactionId")
                              : juce::String();
    }

    juce::String journalStateFor (const juce::File& ccFile)
    {
        const auto xml = juce::parseXML (pairJournalFor (ccFile));
        return xml != nullptr ? xml->getStringAttribute ("state")
                              : juce::String();
    }

    juce::String ownedMapText (bool isCC,
                               const juce::String& transactionId,
                               const juce::String& marker)
    {
        juce::XmlElement xml (isCC ? "MIDIMAP" : "NOTEMAP");
        xml.setAttribute ("schema", 1);
        xml.setAttribute ("transactionId", transactionId);
        xml.setAttribute ("marker", marker);
        return xml.toString (juce::XmlElement::TextFormat().singleLine());
    }

    bool replaceWithOwnedMap (const juce::File& file,
                              bool isCC,
                              const juce::String& transactionId,
                              const juce::String& marker)
    {
        return file.replaceWithText (
            ownedMapText (isCC, transactionId, marker));
    }

    class FakeHostedParameter final : public juce::HostedAudioProcessorParameter
    {
    public:
        FakeHostedParameter (juce::String parameterId, juce::String parameterName)
            : HostedAudioProcessorParameter (1), id (std::move (parameterId)),
              name (std::move (parameterName)) {}

        float getValue() const override { return value; }
        void setValue (float next) override { value = next; }
        float getDefaultValue() const override { return 0.0f; }
        juce::String getName (int) const override { return name; }
        juce::String getLabel() const override { return {}; }
        float getValueForText (const juce::String&) const override { return value; }
        juce::String getParameterID() const override { return id; }

        void rename (juce::String next) { name = std::move (next); }

    private:
        juce::String id;
        juce::String name;
        float value = 0.0f;
    };
}

int main (int argc, char* argv[])
{
    // Child mode used by the real interprocess lock test below. A held lock
    // exits 23 after the bounded timeout; a free lock exits 0.
    if (argc == 4
         && juce::String (argv[1]) == "--probe-midi-map-pair-lock")
    {
        ChurchDAWMidiMapping::MidiMapPairFileLock lock {
            juce::File (argv[2]), juce::File (argv[3]) };
        ChurchDAWMidiMapping::MidiMapPairFileLock::ScopedAccess access (
            lock, 200);
        return access.isLocked() ? 0 : 23;
    }

#if JUCE_ANDROID || JUCE_MAC || JUCE_LINUX || JUCE_BSD
    if (argc == 3
         && juce::String (argv[1])
                == "--probe-midi-map-sandbox-lock")
    {
        ChurchDAWMidiMapping::MidiMapSandboxFileLock lock {
            juce::File (argv[2]) };
        return lock.enter (200) ? (lock.exit(), 0) : 23;
    }
#endif

    // A) startup gate + merge safety. A profile cannot save until both maps
    // have been restored, and a stale custom mapping by sourceName wins over
    // an automatic default on the new Android source id.
    {
        ChurchDAWMidiMapping::MidiMapLifecycleGate gate;
        expect (! gate.canSave(), "startup gate opened before both restores");
        gate.ccLoaded = true;
        expect (! gate.canSave(), "startup gate opened after only CC restore");
        gate.noteLoaded = true;
        expect (gate.canSave(), "startup gate did not open after both restores");

        using Key = ChurchDAWMidiMapping::ControllerKey;
        std::map<Key, int> custom;
        custom[{ "midi:old", 1, 7, false, "Arturia KeyLab" }] = 21;
        custom[{ "midi:old", 1, 22, false, "Arturia KeyLab" }] = 22;
        expect (ChurchDAWMidiMapping::hasMappingForSourceName (
                    custom, "Arturia KeyLab", 1, 7, false),
                "custom stale mapping was not detected by profile merge");
        expect (! ChurchDAWMidiMapping::hasMappingForSourceName (
                    custom, "Arturia KeyLab", 1, 1, false),
                "profile merge falsely found an unrelated CC");
        std::map<Key, int> emptyActions;
        expect (! ChurchDAWMidiMapping::shouldAddAutomaticMapping (
                    custom, emptyActions, "midi:new", "Arturia KeyLab", 1, 7, false),
                "automatic profile would recreate a custom stale CC7");
        expect (ChurchDAWMidiMapping::shouldAddAutomaticMapping (
                    custom, emptyActions, "midi:new", "Arturia KeyLab", 1, 1, false),
                "automatic profile did not add an absent default CC1");
    }

    // B) HostedAudioProcessorParameter is deliberately not an
    // AudioProcessorParameterWithID. Its ID survives rename and reorder, and
    // duplicate display names do not become an ambiguous lookup.
    {
        auto first = std::make_unique<FakeHostedParameter> ("hosted-a", "Duplicate");
        auto second = std::make_unique<FakeHostedParameter> ("hosted-b", "Duplicate");
        juce::Array<juce::AudioProcessorParameter*> before {
            first.get(), second.get() };
        expect (ChurchDAWMidiMapping::stableParameterId (first.get()) == "hosted-a",
                "hosted parameter ID was not read");
        first->rename ("Renamed");
        juce::Array<juce::AudioProcessorParameter*> after {
            second.get(), first.get() };
        expect (ChurchDAWMidiMapping::resolveParameterIndex (
                    after, "hosted-a", "Duplicate") == 1,
                "hosted ID did not survive rename/reorder/duplicate name");
        expect (ChurchDAWMidiMapping::resolveParameterIndex (
                    before, "hosted-b", "Duplicate") == 1,
                "second duplicate hosted ID was not resolved independently");

        auto duplicateId = std::make_unique<FakeHostedParameter> (
            "hosted-a", "Independent name");
        juce::Array<juce::AudioProcessorParameter*> duplicateIds {
            first.get(), duplicateId.get() };
        expect (ChurchDAWMidiMapping::resolveParameterIndex (
                    duplicateIds, "hosted-a", {}) == -1,
                "duplicate stable parameter ID resolved to an arbitrary target");
        expect (! ChurchDAWMidiMapping::parameterIdentityIsUnique (
                    duplicateIds, 0),
                "duplicate stable parameter ID was accepted for MIDI Learn");
        expect (ChurchDAWMidiMapping::parameterIdentityIsUnique (
                    before, 0),
                "unique hosted parameter identity was rejected for MIDI Learn");

        auto duplicateLegacyName = std::make_unique<FakeHostedParameter> (
            "hosted-c", "Duplicate");
        juce::Array<juce::AudioProcessorParameter*> duplicateLegacyNames {
            second.get(), duplicateLegacyName.get() };
        expect (ChurchDAWMidiMapping::resolveParameterIndex (
                    duplicateLegacyNames, {}, "Duplicate") == -1,
                "duplicate legacy parameter name resolved to an arbitrary target");
    }

    // B2) Real CC+NOTE pair round-trip. Session one writes the exact schema via
    // the helpers used by MainComponent, commits all four pair targets, and
    // then destroys every in-memory object. Session two reloads from disk while
    // the hosted plug-in is absent, then resolves it after rename/reorder.
    {
        const auto directory = juce::File::getSpecialLocation (
            juce::File::tempDirectory).getNonexistentChildFile (
                "churchdaw-midi-map-restart-" + juce::Uuid().toString(),
                {}, false);
        expect (directory.createDirectory(),
                "could not create MIDI restart round-trip directory");

        const auto ccFile = directory.getChildFile ("midi-cc-map.xml");
        const auto noteFile = directory.getChildFile ("midi-note-map.xml");
        const auto ccBackup = ccFile.getSiblingFile (
            ccFile.getFileName() + ".last-good");
        const auto noteBackup = noteFile.getSiblingFile (
            noteFile.getFileName() + ".last-good");

        {
            juce::PluginDescription kontakt;
            kontakt.name = "Kontakt";
            kontakt.pluginFormatName = "VST3";
            kontakt.manufacturerName = "Native Instruments";
            kontakt.fileOrIdentifier = "com.native-instruments.Kontakt";
            kontakt.uniqueId = 0x4b4e544b;
            const auto pluginId =
                ChurchDAWMidiMapping::hostedPluginIdentityForDescription (
                    kontakt);

            auto learnedParameter = std::make_unique<FakeHostedParameter> (
                "kontakt-cutoff", "Old Tone");
            ChurchDAWMidiMapping::PersistedCCTarget target {
                0, 0, -1,
                learnedParameter->getName (128),
                ChurchDAWMidiMapping::stableParameterId (
                    learnedParameter.get()),
                pluginId
            };
            const ChurchDAWMidiMapping::ControllerKey fader {
                "midi:session-one-device", 1, 7, false, "Stage Fader"
            };
            const ChurchDAWMidiMapping::ControllerKey note {
                "midi:session-one-device", 1, 60, false, "Stage Fader"
            };

            ChurchDAWMidiMapping::MidiMapPairFileLock pairLock (
                ccFile, noteFile);
            ChurchDAWMidiMapping::MidiMapPairRollback transaction (
                ccFile, noteFile, pairLock);
            expect (transaction.capture ("restart-round-trip"),
                    "could not capture restart round-trip transaction");
            const auto transactionId = pairLock.getActiveTransactionId();
            expect (transactionId.isNotEmpty(),
                    "restart round-trip transaction had no ID");

            juce::XmlElement ccXml ("MIDIMAP");
            ccXml.setAttribute ("schema", 1);
            ccXml.setAttribute ("complete", 1);
            ccXml.setAttribute ("transactionId", transactionId);
            ccXml.setAttribute ("entryCount", 1);
            ccXml.setAttribute ("explicitEmpty", 0);
            ChurchDAWMidiMapping::appendPersistedCCMapping (
                ccXml, fader, target,
                ChurchDAWMidiMapping::sourceNameForPersistence (
                    fader.sourceName, {}));

            juce::XmlElement noteXml ("NOTEMAP");
            noteXml.setAttribute ("schema", 1);
            noteXml.setAttribute ("complete", 1);
            noteXml.setAttribute ("transactionId", transactionId);
            noteXml.setAttribute ("entryCount", 1);
            noteXml.setAttribute ("explicitEmpty", 0);
            ChurchDAWMidiMapping::appendPersistedNoteMapping (
                noteXml, note, 8 /* tapTempo */, -1,
                ChurchDAWMidiMapping::sourceNameForPersistence (
                    note.sourceName, {}));

            const auto ccText = ccXml.toString (
                juce::XmlElement::TextFormat().singleLine());
            const auto noteText = noteXml.toString (
                juce::XmlElement::TextFormat().singleLine());
            expect (ChurchDAWMidiMapping::midiMapSerializedPayloadFitsSafetyLimits (
                        ccText)
                     && ChurchDAWMidiMapping::midiMapSerializedPayloadFitsSafetyLimits (
                        noteText),
                    "restart round-trip payload exceeded app limits");
            expect (ChurchDAWMidiMapping::replaceFileWithTextAtomically (
                        ccFile, ccText)
                     && ChurchDAWMidiMapping::replaceFileWithTextAtomically (
                        ccBackup, ccText)
                     && ChurchDAWMidiMapping::replaceFileWithTextAtomically (
                        noteFile, noteText)
                     && ChurchDAWMidiMapping::replaceFileWithTextAtomically (
                        noteBackup, noteText),
                    "restart round-trip could not publish pair targets");
            expect (transaction.commit(),
                    "restart round-trip pair transaction did not commit");
        }

        // New process/session: no objects from the writer scope survive.
        {
            auto ccXml = ChurchDAWMidiMapping::loadBoundedXmlFile (ccFile);
            auto noteXml = ChurchDAWMidiMapping::loadBoundedXmlFile (noteFile);
            auto ccLastGood = ChurchDAWMidiMapping::loadBoundedXmlFile (
                ccBackup);
            auto noteLastGood = ChurchDAWMidiMapping::loadBoundedXmlFile (
                noteBackup);
            expect (ccXml != nullptr && noteXml != nullptr
                     && ccLastGood != nullptr && noteLastGood != nullptr,
                    "restart round-trip could not reload the complete quartet");
            expect (ChurchDAWMidiMapping::isMidiMapXmlValid (
                        *ccXml, "MIDIMAP", "MAP", "CCACAO")
                     && ChurchDAWMidiMapping::isMidiMapXmlValid (
                        *noteXml, "NOTEMAP", "NOTE"),
                    "restart round-trip pair failed real schema validation");
            expect (ccXml->getStringAttribute ("transactionId")
                        == noteXml->getStringAttribute ("transactionId"),
                    "restart round-trip loaded mixed CC/NOTE generations");
            expect (ccLastGood->toString (
                        juce::XmlElement::TextFormat().singleLine())
                        == ccXml->toString (
                            juce::XmlElement::TextFormat().singleLine())
                     && noteLastGood->toString (
                        juce::XmlElement::TextFormat().singleLine())
                        == noteXml->toString (
                            juce::XmlElement::TextFormat().singleLine()),
                    "restart round-trip last-good files differ from the committed pair");

            std::map<ChurchDAWMidiMapping::ControllerKey,
                     ChurchDAWMidiMapping::PersistedCCTarget> loadedCC;
            std::vector<ChurchDAWMidiMapping::PersistedNoteMapping> loadedNotes;
            for (auto* element : ccXml->getChildWithTagNameIterator ("MAP"))
            {
                ChurchDAWMidiMapping::PersistedCCMapping mapping;
                expect (ChurchDAWMidiMapping::readPersistedCCMapping (
                            *element, mapping),
                        "restart round-trip could not decode CC entry");
                loadedCC.emplace (std::move (mapping.control),
                                  std::move (mapping.target));
            }
            for (auto* element : noteXml->getChildWithTagNameIterator ("NOTE"))
            {
                ChurchDAWMidiMapping::PersistedNoteMapping mapping;
                expect (ChurchDAWMidiMapping::readPersistedNoteMapping (
                            *element, mapping),
                        "restart round-trip could not decode NOTE entry");
                loadedNotes.push_back (std::move (mapping));
            }

            expect (loadedCC.size() == 1
                     && loadedCC.begin()->first.cc == 7
                     && loadedCC.begin()->first.channel == 1
                     && loadedCC.begin()->first.sourceName == "Stage Fader",
                    "normal CC/fader mapping did not survive restart");
            expect (loadedNotes.size() == 1
                     && loadedNotes.front().control.cc == 60
                     && loadedNotes.front().action == 8
                     && loadedNotes.front().argument == -1,
                    "NOTE/action mapping did not survive restart");

            const auto& loadedTarget = loadedCC.begin()->second;
            expect (loadedTarget.pluginId.isNotEmpty(),
                    "hosted plug-in identity was not persisted");
            expect (ChurchDAWMidiMapping::MidiTargetIdentityPolicy::classifyProcessor (
                        loadedTarget.pluginId, {}, false)
                        == ChurchDAWMidiMapping::ProcessorTargetMatch::pending
                     && loadedCC.size() == 1,
                    "unavailable plug-in deleted or retargeted its pending mapping");

            juce::PluginDescription returnedKontakt;
            returnedKontakt.name = "Kontakt 8"; // display name may change
            returnedKontakt.pluginFormatName = "VST3";
            returnedKontakt.manufacturerName = "Native Instruments";
            returnedKontakt.fileOrIdentifier =
                "com.native-instruments.Kontakt";
            returnedKontakt.uniqueId = 0x4b4e544b;
            const auto returnedIdentity =
                ChurchDAWMidiMapping::hostedPluginIdentityForDescription (
                    returnedKontakt);
            expect (returnedIdentity == loadedTarget.pluginId
                     && ChurchDAWMidiMapping::MidiTargetIdentityPolicy::classifyProcessor (
                        loadedTarget.pluginId, returnedIdentity, true)
                        == ChurchDAWMidiMapping::ProcessorTargetMatch::ready,
                    "same hosted plug-in identity did not become ready");

            auto other = std::make_unique<FakeHostedParameter> (
                "kontakt-resonance", "Old Tone");
            auto returned = std::make_unique<FakeHostedParameter> (
                "kontakt-cutoff", "Brightness");
            juce::Array<juce::AudioProcessorParameter*> reordered {
                other.get(), returned.get()
            };
            expect (ChurchDAWMidiMapping::resolveParameterIndex (
                        reordered, loadedTarget.paramId,
                        loadedTarget.paramName) == 1,
                    "stable parameter ID did not survive rename/reorder after restart");

            auto duplicate = std::make_unique<FakeHostedParameter> (
                "kontakt-cutoff", "Second brightness");
            juce::Array<juce::AudioProcessorParameter*> duplicateIds {
                returned.get(), duplicate.get()
            };
            expect (ChurchDAWMidiMapping::resolveParameterIndex (
                        duplicateIds, loadedTarget.paramId,
                        loadedTarget.paramName) == -1,
                    "duplicate stable ID silently retargeted after restart");

            juce::PluginDescription replacement;
            replacement.name = "Unrelated Synth";
            replacement.pluginFormatName = "VST3";
            replacement.manufacturerName = "Another Vendor";
            replacement.fileOrIdentifier = "com.example.UnrelatedSynth";
            replacement.uniqueId = 0x4b4e544c;
            const auto replacementIdentity =
                ChurchDAWMidiMapping::hostedPluginIdentityForDescription (
                    replacement);
            expect (ChurchDAWMidiMapping::MidiTargetIdentityPolicy::classifyProcessor (
                        loadedTarget.pluginId, replacementIdentity, true)
                        == ChurchDAWMidiMapping::ProcessorTargetMatch::incompatible,
                    "same-slot replacement plug-in was accepted by parameter ID");
            expect (ChurchDAWMidiMapping::resolveParameterIndex (
                        reordered, loadedTarget.paramId,
                        loadedTarget.paramName) == 1,
                    "replacement counterexample did not expose same parameter ID");

            expect (ChurchDAWMidiMapping::MidiTargetIdentityPolicy::classifyProcessor (
                        {}, returnedIdentity, true)
                        == ChurchDAWMidiMapping::ProcessorTargetMatch::needsExplicitRelink,
                    "legacy external map without plug-in identity retargeted silently");
            expect (ChurchDAWMidiMapping::MidiTargetIdentityPolicy::classifyProcessor (
                        {}, ChurchDAWMidiMapping::masterProcessorIdentity(), true)
                        == ChurchDAWMidiMapping::ProcessorTargetMatch::needsExplicitRelink,
                    "legacy internal parameter map was treated as unambiguous");
            expect (ChurchDAWMidiMapping::MidiTargetIdentityPolicy::classifyProcessor (
                        ChurchDAWMidiMapping::masterProcessorIdentity(),
                        ChurchDAWMidiMapping::masterProcessorIdentity(), true)
                        == ChurchDAWMidiMapping::ProcessorTargetMatch::ready,
                    "new internal mapping did not carry explicit identity");

            juce::PluginDescription movedKontakt = returnedKontakt;
            movedKontakt.fileOrIdentifier =
                "com.native-instruments.moved.Kontakt";
            const auto movedIdentity =
                ChurchDAWMidiMapping::hostedPluginIdentityForDescription (
                    movedKontakt);
            expect (movedIdentity != loadedTarget.pluginId
                     && ChurchDAWMidiMapping::MidiTargetIdentityPolicy::classifyProcessor (
                            loadedTarget.pluginId, movedIdentity, true)
                          == ChurchDAWMidiMapping::ProcessorTargetMatch::incompatible,
                    "real path change was silently accepted");
        }

        expect (directory.deleteRecursively(),
                "could not remove MIDI restart round-trip directory");
    }

    // B3) The exact policy used by apply/panel/remap/delete keeps A/B/A
    // identities separate, and an old empty identity is migrated only by an
    // explicit relink followed by a real atomic CC+NOTE pair transaction.
    {
        juce::PluginDescription pluginA;
        pluginA.name = "Kontakt A";
        pluginA.pluginFormatName = "VST3";
        pluginA.fileOrIdentifier = "/Library/Audio/Plug-Ins/VST3/Kontakt.vst3";
        pluginA.uniqueId = 0x11111111;
        const auto identityA =
            ChurchDAWMidiMapping::hostedPluginIdentityForDescription (pluginA);

        auto renamedA = pluginA;
        renamedA.name = "Kontakt A Renamed";
        expect (ChurchDAWMidiMapping::hostedPluginIdentityForDescription (
                    renamedA) == identityA,
                "display-name change changed canonical plug-in identity");
        const auto legacyV1Identity =
            ChurchDAWMidiMapping::canonicalizePersistedProcessorIdentity (
                pluginA.createIdentifierString());
        expect (legacyV1Identity.startsWith ("hosted:v1:")
                 && legacyV1Identity != identityA,
                "32-bit JUCE identity was trusted as hosted:v2");
        expect (ChurchDAWMidiMapping::MidiTargetIdentityPolicy::classifyProcessor (
                    legacyV1Identity, identityA, true)
                 == ChurchDAWMidiMapping::ProcessorTargetMatch::incompatible,
                "hosted:v1 was auto-trusted against hosted:v2");

        juce::PluginDescription collisionAa = pluginA;
        collisionAa.name = "Collision A";
        collisionAa.fileOrIdentifier = "Aa";
        collisionAa.uniqueId = 0x12345678;
        collisionAa.deprecatedUid = 0x76543210;
        auto collisionBB = collisionAa;
        collisionBB.name = "Collision B";
        collisionBB.fileOrIdentifier = "BB";
        expect (collisionAa.fileOrIdentifier.hashCode()
                    == collisionBB.fileOrIdentifier.hashCode(),
                "Aa/BB did not reproduce JUCE hashCode32 collision");
        expect (ChurchDAWMidiMapping::hostedPluginIdentityForDescription (
                    collisionAa)
                 != ChurchDAWMidiMapping::hostedPluginIdentityForDescription (
                    collisionBB),
                "hosted:v2 collapsed Aa/BB with the same UID");
        auto uidChangedA = pluginA;
        uidChangedA.uniqueId ^= 0x01010101;
        expect (ChurchDAWMidiMapping::hostedPluginIdentityForDescription (
                    uidChangedA) != identityA,
                "hosted:v2 ignored a UID change");

        juce::PluginDescription pluginB = pluginA;
        pluginB.name = "Plugin B";
        pluginB.fileOrIdentifier =
            "/Library/Audio/Plug-Ins/VST3/Other.vst3";
        pluginB.uniqueId = 0x22222222;
        const auto identityB =
            ChurchDAWMidiMapping::hostedPluginIdentityForDescription (pluginB);

        ChurchDAWMidiMapping::PersistedCCTarget targetA {
            0, 0, -1, "Cutoff", "cutoff", identityA
        };
        ChurchDAWMidiMapping::PersistedCCTarget targetB {
            0, 7, -1, "Cutoff", "cutoff", identityB
        };
        ChurchDAWMidiMapping::PersistedCCTarget legacy {
            0, 0, -1, "Cutoff", {}, {}
        };

        expect (ChurchDAWMidiMapping::MidiTargetIdentityPolicy::sameTarget (
                    targetA, targetA),
                "A did not match itself in target policy");
        expect (! ChurchDAWMidiMapping::MidiTargetIdentityPolicy::sameTarget (
                    targetA, targetB),
                "panel policy crossed A into B with same parameter ID");
        expect (ChurchDAWMidiMapping::MidiTargetIdentityPolicy::classifyProcessor (
                    targetA.pluginId, identityB, true)
                     == ChurchDAWMidiMapping::ProcessorTargetMatch::incompatible,
                "apply policy crossed A into B");
        expect (ChurchDAWMidiMapping::MidiTargetIdentityPolicy::classifyProcessor (
                    targetA.pluginId, identityA, true)
                     == ChurchDAWMidiMapping::ProcessorTargetMatch::ready,
                "A/B/A sequence did not return to A");

        std::vector<ChurchDAWMidiMapping::PersistedCCTarget> mappings {
            targetA
        };
        mappings.erase (std::remove_if (
            mappings.begin(), mappings.end(), [&] (const auto& candidate)
            {
                return ChurchDAWMidiMapping::MidiTargetIdentityPolicy::sameTarget (
                    candidate, targetB);
            }), mappings.end());
        expect (mappings.size() == 1
                 && mappings.front().pluginId == identityA,
                "remap/delete policy for B erased pending A");
        expect (ChurchDAWMidiMapping::MidiTargetIdentityPolicy::mayExplicitlyRelink (
                    legacy, targetA)
                 && ChurchDAWMidiMapping::MidiTargetIdentityPolicy::mayExplicitlyRelink (
                    targetA, targetB),
                "explicit legacy/path relink was not offered");
        expect (ChurchDAWMidiMapping::MidiTargetIdentityPolicy::sameParameterCoordinates (
                    legacy, targetA),
                "name-only legacy target did not fall back to exact name");

        expect (ChurchDAWMidiMapping::classifyPersistedTargetPanelRow (
                    false,
                    ChurchDAWMidiMapping::ProcessorTargetMatch::ready,
                    true)
                 == ChurchDAWMidiMapping::PersistedTargetPanelRow::active,
                "active external target would disappear from panel");
        expect (ChurchDAWMidiMapping::classifyPersistedTargetPanelRow (
                    true,
                    ChurchDAWMidiMapping::ProcessorTargetMatch::ready,
                    true)
                 == ChurchDAWMidiMapping::PersistedTargetPanelRow::consumedByBuiltInRow,
                "built-in target would be duplicated in panel");
        expect (ChurchDAWMidiMapping::classifyPersistedTargetPanelRow (
                    false,
                    ChurchDAWMidiMapping::ProcessorTargetMatch::pending,
                    false)
                 == ChurchDAWMidiMapping::PersistedTargetPanelRow::unavailable,
                "unavailable target did not remain manageable");
        expect (ChurchDAWMidiMapping::classifyPersistedTargetPanelRow (
                    false,
                    ChurchDAWMidiMapping::ProcessorTargetMatch::incompatible,
                    true)
                 == ChurchDAWMidiMapping::PersistedTargetPanelRow::needsRelink,
                "incompatible current plug-in did not require explicit relink");
        expect (ChurchDAWMidiMapping::classifyPersistedTargetPanelRow (
                    false,
                    ChurchDAWMidiMapping::ProcessorTargetMatch::ready,
                    false)
                 == ChurchDAWMidiMapping::PersistedTargetPanelRow::unresolvedParameter,
                "unresolved parameter map did not remain deletable");
        expect (ChurchDAWMidiMapping::currentProcessorIdentityMatchesSelection (
                    identityA, identityA)
                 && ! ChurchDAWMidiMapping::currentProcessorIdentityMatchesSelection (
                    identityA, identityB),
                "A-selected-then-C identity guard accepted replacement");
        expect (ChurchDAWMidiMapping::classifyMidiTargetLocation (
                    -1, -1, 4, 100, 101)
                 == ChurchDAWMidiMapping::MidiTargetLocationKind::master,
                "legacy Master row did not resolve slot -1");
        FakeHostedParameter masterDb { "masterdb", "Master (dB)" };
        juce::Array<juce::AudioProcessorParameter*> masterParameters;
        masterParameters.add (&masterDb);
        expect (ChurchDAWMidiMapping::resolveParameterIndexForTargetLocation (
                    masterParameters, {}, "Master",
                    ChurchDAWMidiMapping::MidiTargetLocationKind::master) == 0,
                "legacy Master CC7 name did not resolve dedicated processor");
        expect (ChurchDAWMidiMapping::MidiTargetIdentityPolicy::sameParameterCoordinates (
                    { -1, -1, -1, "Master", {}, {} },
                    { -1, 0, -1, "Master (dB)", "masterdb",
                      ChurchDAWMidiMapping::masterProcessorIdentity() }),
                "legacy/new Master coordinates did not share dedicated slot");

        FakeHostedParameter uniqueCutoff { "cutoff", "Cutoff" };
        juce::Array<juce::AudioProcessorParameter*> uniqueNameParameters;
        uniqueNameParameters.add (&uniqueCutoff);
        expect (ChurchDAWMidiMapping::resolveParameterIndex (
                    uniqueNameParameters, {}, "Cutoff") == 0,
                "name-only legacy parameter did not resolve uniquely");
        FakeHostedParameter duplicateCutoff { "other-cutoff", "Cutoff" };
        uniqueNameParameters.add (&duplicateCutoff);
        expect (ChurchDAWMidiMapping::resolveParameterIndex (
                    uniqueNameParameters, {}, "Cutoff") == -1,
                "duplicate legacy parameter name relinked ambiguously");

        const ChurchDAWMidiMapping::ControllerKey oldSourceKey {
            "midi:old-source", 2, 74, false, "Relink Controller"
        };
        const ChurchDAWMidiMapping::ControllerKey newSourceKey {
            "midi:new-source", 2, 74, false, "Relink Controller"
        };
        std::map<ChurchDAWMidiMapping::ControllerKey,
                 ChurchDAWMidiMapping::PersistedCCTarget> repluggedMap {
            { oldSourceKey, legacy }
        };
        const auto exactOld = repluggedMap.find (oldSourceKey);
        expect (exactOld != repluggedMap.end()
                 && ChurchDAWMidiMapping::controllerKeysExactlyEqual (
                     exactOld->first, oldSourceKey)
                 && ChurchDAWMidiMapping::persistedTargetsExactlyEqual (
                     exactOld->second, legacy),
                "exact relink entry was not found");
        repluggedMap.erase (exactOld);
        repluggedMap[newSourceKey] = targetA;
        expect (repluggedMap.size() == 1
                 && repluggedMap.find (oldSourceKey) == repluggedMap.end()
                 && repluggedMap.find (newSourceKey) != repluggedMap.end(),
                "replug left old-source and new-source mappings together");

        const ChurchDAWMidiMapping::ControllerKey aliasOld {
            "midi:a-old", 2, 74, false, "Alias Controller"
        };
        const ChurchDAWMidiMapping::ControllerKey aliasCurrent {
            "midi:z-current", 2, 74, false, "Alias Controller"
        };
        std::map<ChurchDAWMidiMapping::ControllerKey,
                 ChurchDAWMidiMapping::PersistedCCTarget> aliasMappings {
            { aliasOld, targetA },
            { aliasCurrent, targetB }
        };
        const std::vector<ChurchDAWMidiMapping::MidiSourceDescriptor>
            aliasSources { { aliasCurrent.sourceId,
                             aliasCurrent.sourceName } };
        const auto aliasConflict =
            ChurchDAWMidiMapping::findPhysicalMidiControlIf (
                aliasMappings, aliasCurrent, aliasSources,
                [&] (const auto& mapping)
                {
                    return ! ChurchDAWMidiMapping::persistedTargetsExactlyEqual (
                        mapping.second, targetA);
                });
        expect (aliasConflict != aliasMappings.end()
                 && aliasConflict->first.sourceId
                        == aliasCurrent.sourceId
                 && aliasConflict->second.pluginId == identityB,
                "physical alias scan stopped before a later conflicting target");

        const auto directory = juce::File::getSpecialLocation (
            juce::File::tempDirectory).getNonexistentChildFile (
                "churchdaw-midi-explicit-relink-" + juce::Uuid().toString(),
                {}, false);
        expect (directory.createDirectory(),
                "could not create explicit relink directory");
        const auto ccFile = directory.getChildFile ("cc.xml");
        const auto noteFile = directory.getChildFile ("note.xml");
        const auto ccBackup = ccFile.getSiblingFile (
            ccFile.getFileName() + ".last-good");
        const auto noteBackup = noteFile.getSiblingFile (
            noteFile.getFileName() + ".last-good");
        const ChurchDAWMidiMapping::ControllerKey ccKey {
            "midi:relink", 2, 74, false, "Relink Controller"
        };
        const ChurchDAWMidiMapping::ControllerKey noteKey {
            "midi:relink", 2, 64, false, "Relink Controller"
        };

        const auto publishPair = [&] (
            const ChurchDAWMidiMapping::PersistedCCTarget& target,
            const juce::String& reason)
        {
            ChurchDAWMidiMapping::MidiMapPairFileLock lock (
                ccFile, noteFile);
            ChurchDAWMidiMapping::MidiMapPairRollback transaction (
                ccFile, noteFile, lock);
            if (! transaction.capture (reason))
                return false;
            const auto transactionId = lock.getActiveTransactionId();

            juce::XmlElement ccXml ("MIDIMAP");
            ccXml.setAttribute ("schema", 1);
            ccXml.setAttribute ("complete", 1);
            ccXml.setAttribute ("transactionId", transactionId);
            ccXml.setAttribute ("entryCount", 1);
            ccXml.setAttribute ("explicitEmpty", 0);
            ChurchDAWMidiMapping::appendPersistedCCMapping (
                ccXml, ccKey, target, ccKey.sourceName);

            juce::XmlElement noteXml ("NOTEMAP");
            noteXml.setAttribute ("schema", 1);
            noteXml.setAttribute ("complete", 1);
            noteXml.setAttribute ("transactionId", transactionId);
            noteXml.setAttribute ("entryCount", 1);
            noteXml.setAttribute ("explicitEmpty", 0);
            ChurchDAWMidiMapping::appendPersistedNoteMapping (
                noteXml, noteKey, 8, -1, noteKey.sourceName);

            const auto ccText = ccXml.toString (
                juce::XmlElement::TextFormat().singleLine());
            const auto noteText = noteXml.toString (
                juce::XmlElement::TextFormat().singleLine());
            return ChurchDAWMidiMapping::replaceFileWithTextAtomically (
                        ccFile, ccText)
                && ChurchDAWMidiMapping::replaceFileWithTextAtomically (
                        ccBackup, ccText)
                && ChurchDAWMidiMapping::replaceFileWithTextAtomically (
                        noteFile, noteText)
                && ChurchDAWMidiMapping::replaceFileWithTextAtomically (
                        noteBackup, noteText)
                && transaction.commit();
        };

        expect (publishPair (legacy, "legacy-before-explicit-relink"),
                "could not persist legacy pair before explicit relink");
        {
            const auto legacyXml =
                ChurchDAWMidiMapping::loadBoundedXmlFile (ccFile);
            ChurchDAWMidiMapping::PersistedCCMapping loaded;
            expect (legacyXml != nullptr
                     && ChurchDAWMidiMapping::readPersistedCCMapping (
                         *legacyXml->getChildByName ("MAP"), loaded)
                     && loaded.target.pluginId.isEmpty(),
                    "legacy identity was not preserved before confirmation");
        }

        // A failed relink must leave the complete pre-click generation intact.
        {
            ChurchDAWMidiMapping::MidiMapPairFileLock lock (
                ccFile, noteFile);
            ChurchDAWMidiMapping::MidiMapPairRollback failedRelink (
                ccFile, noteFile, lock);
            expect (failedRelink.capture ("failed-explicit-relink"),
                    "could not capture failed relink checkpoint");
            juce::XmlElement partialCC ("MIDIMAP");
            partialCC.setAttribute ("schema", 1);
            partialCC.setAttribute ("complete", 1);
            partialCC.setAttribute ("transactionId",
                                    lock.getActiveTransactionId());
            partialCC.setAttribute ("entryCount", 1);
            partialCC.setAttribute ("explicitEmpty", 0);
            ChurchDAWMidiMapping::appendPersistedCCMapping (
                partialCC, ccKey, targetA, ccKey.sourceName);
            expect (ChurchDAWMidiMapping::replaceFileWithTextAtomically (
                        ccFile, partialCC.toString (
                            juce::XmlElement::TextFormat().singleLine())),
                    "could not inject failed relink half-write");
            expect (ChurchDAWMidiMapping::rollbackMidiMapPairPreservingFutureSchema (
                        failedRelink, ccFile, noteFile),
                    "failed relink did not roll back the pair");

            const auto restoredCC =
                ChurchDAWMidiMapping::loadBoundedXmlFile (ccFile);
            const auto restoredNote =
                ChurchDAWMidiMapping::loadBoundedXmlFile (noteFile);
            const auto restoredCCBackup =
                ChurchDAWMidiMapping::loadBoundedXmlFile (ccBackup);
            const auto restoredNoteBackup =
                ChurchDAWMidiMapping::loadBoundedXmlFile (noteBackup);
            ChurchDAWMidiMapping::PersistedCCMapping restoredMapping;
            expect (restoredCC != nullptr && restoredNote != nullptr
                     && restoredCCBackup != nullptr
                     && restoredNoteBackup != nullptr
                     && ChurchDAWMidiMapping::readPersistedCCMapping (
                         *restoredCC->getChildByName ("MAP"), restoredMapping)
                     && restoredMapping.target.pluginId.isEmpty()
                     && restoredCC->toString() == restoredCCBackup->toString()
                     && restoredNote->toString() == restoredNoteBackup->toString(),
                    "failed relink changed XML/backup generation");
        }

        expect (publishPair (targetA, "explicit-relink-confirmed"),
                "explicit relink did not commit the atomic pair");
        {
            const auto ccXml = ChurchDAWMidiMapping::loadBoundedXmlFile (ccFile);
            const auto noteXml = ChurchDAWMidiMapping::loadBoundedXmlFile (
                noteFile);
            const auto ccLastGood = ChurchDAWMidiMapping::loadBoundedXmlFile (
                ccBackup);
            const auto noteLastGood = ChurchDAWMidiMapping::loadBoundedXmlFile (
                noteBackup);
            ChurchDAWMidiMapping::PersistedCCMapping loaded;
            ChurchDAWMidiMapping::PersistedNoteMapping loadedNote;
            expect (ccXml != nullptr && noteXml != nullptr
                     && ccLastGood != nullptr && noteLastGood != nullptr
                     && ChurchDAWMidiMapping::readPersistedCCMapping (
                         *ccXml->getChildByName ("MAP"), loaded)
                     && ChurchDAWMidiMapping::readPersistedNoteMapping (
                         *noteXml->getChildByName ("NOTE"), loadedNote)
                     && loaded.target.pluginId == identityA
                     && loadedNote.action == 8
                     && ccXml->getStringAttribute ("transactionId")
                          == noteXml->getStringAttribute ("transactionId")
                     && ccLastGood->getStringAttribute ("transactionId")
                          == ccXml->getStringAttribute ("transactionId")
                     && noteLastGood->getStringAttribute ("transactionId")
                          == noteXml->getStringAttribute ("transactionId"),
                    "round-trip after explicit relink lost identity or pair atomicity");
        }

        expect (directory.deleteRecursively(),
                "could not remove explicit relink directory");
    }

    // B4) The same snapshot validator used by MainComponent fails closed when
    // a selected processor/target/map changes. Panel selection restoration is
    // based on complete semantic identity, never duplicated display labels.
    {
        using Validation =
            ChurchDAWMidiMapping::ParameterCommandValidation;
        const ChurchDAWMidiMapping::ControllerKey control {
            "midi:controller-a", 1, 74, false, "Controller A"
        };
        const ChurchDAWMidiMapping::PersistedCCTarget targetA {
            0, 3, -1, "Cutoff", "cutoff", "hosted:v2:plugin-a"
        };

        ChurchDAWMidiMapping::ParameterCommandSnapshot expected;
        expected.selectedTarget = targetA;
        expected.expectedCurrentPluginId = targetA.pluginId;
        expected.expectedEntryPresent = true;
        expected.persistedControl = control;
        expected.persistedTarget = targetA;

        ChurchDAWMidiMapping::ParameterCommandObservation current;
        current.selectedTarget = targetA;
        current.currentPluginId = targetA.pluginId;
        current.entryPresent = true;
        current.persistedControl = control;
        current.persistedTarget = targetA;
        expect (ChurchDAWMidiMapping::validateParameterCommandSnapshot (
                    expected, current) == Validation::valid,
                "unchanged parameter command snapshot was rejected");

        auto replacedProcessor = current;
        replacedProcessor.currentPluginId = "hosted:v2:plugin-c";
        replacedProcessor.selectedTarget.pluginId =
            replacedProcessor.currentPluginId;
        expect (ChurchDAWMidiMapping::validateParameterCommandSnapshot (
                    expected, replacedProcessor)
                    == Validation::currentProcessorChanged,
                "same-slot/plugin-C replacement passed stale command guard");

        auto changedTarget = current;
        changedTarget.selectedTarget.param = 4;
        expect (ChurchDAWMidiMapping::validateParameterCommandSnapshot (
                    expected, changedTarget)
                    == Validation::selectedTargetChanged,
                "changed selected parameter passed stale command guard");

        auto removedEntry = current;
        removedEntry.entryPresent = false;
        expect (ChurchDAWMidiMapping::validateParameterCommandSnapshot (
                    expected, removedEntry)
                    == Validation::entryPresenceChanged,
                "removed persisted entry passed stale command guard");

        auto replacedEntry = current;
        replacedEntry.persistedTarget.paramName = "Resonance";
        expect (ChurchDAWMidiMapping::validateParameterCommandSnapshot (
                    expected, replacedEntry)
                    == Validation::persistedEntryChanged,
                "replaced persisted payload passed stale command guard");
        expect (ChurchDAWMidiMapping::parameterLearnTargetStillCurrent (
                    70, 70, targetA.pluginId, targetA.pluginId, true)
                 && ! ChurchDAWMidiMapping::parameterLearnTargetStillCurrent (
                        70, 71, targetA.pluginId, targetA.pluginId, true)
                 && ! ChurchDAWMidiMapping::parameterLearnTargetStillCurrent (
                        70, 70, targetA.pluginId,
                        "hosted:v2:plugin-c", true),
                "armed Learn target accepted stale topology/plugin C");

        MidiMappingPanel::Command kontaktA;
        kontaktA.name = "Kontakt  -  Cutoff";
        kontaktA.category = "Plugins externos";
        kontaktA.type = "CC";
        kontaktA.device = "Controller";
        kontaktA.control = "CC 74";
        kontaktA.midiChannel = 1;
        kontaktA.number = 74;
        kontaktA.target =
            MidiMappingPanel::TargetKind::processorParameter;
        kontaktA.parameterSlot = 0;
        kontaktA.parameterIndex = 3;
        kontaktA.parameterId = "cutoff";
        kontaktA.parameterName = "Cutoff";
        kontaktA.expectedCurrentPluginId = "hosted:v2:kontakt-a";
        kontaktA.expectedParameterEntryPresent = true;
        kontaktA.persistedParameterSlot = 0;
        kontaktA.persistedParameterIndex = 3;
        kontaktA.persistedParameterId = "cutoff";
        kontaktA.persistedParameterName = "Cutoff";
        kontaktA.persistedPluginId = "hosted:v2:kontakt-a";
        kontaktA.mappedSourceId = "midi:controller";
        kontaktA.mappedSourceName = "Controller";
        kontaktA.mappedChannel = 1;
        kontaktA.mappedNumber = 74;

        auto kontaktB = kontaktA;
        kontaktB.parameterSlot = 1;
        kontaktB.persistedParameterSlot = 1;
        kontaktB.expectedCurrentPluginId = "hosted:v2:kontakt-b";
        kontaktB.persistedPluginId = "hosted:v2:kontakt-b";
        expect (! MidiMappingPanel::Command::sameStableIdentity (
                    kontaktA, kontaktB),
                "same-label Kontakt twins collapsed into one panel identity");

        auto refreshedA = kontaktA;
        refreshedA.name = "Renamed Kontakt  -  Cutoff";
        refreshedA.category = "Other display category";
        refreshedA.device = "Pretty controller name";
        refreshedA.control = "Knob 1";
        refreshedA.parameterIndex = 9;
        refreshedA.parameterName = "Brightness";
        refreshedA.mapped = ! kontaktA.mapped;
        refreshedA.conflict = ! kontaktA.conflict;
        refreshedA.needsRelink = ! kontaktA.needsRelink;
        expect (MidiMappingPanel::Command::sameStableIdentity (
                    kontaktA, refreshedA),
                "display/status-only refresh lost semantic panel selection");

        const std::array<MidiMappingPanel::Command, 2> reordered {
            kontaktB, refreshedA
        };
        int restoredIndex = -1;
        int semanticMatches = 0;
        for (size_t index = 0; index < reordered.size(); ++index)
            if (MidiMappingPanel::Command::sameStableIdentity (
                    reordered[index], kontaktA))
            {
                restoredIndex = static_cast<int> (index);
                ++semanticMatches;
            }
        expect (semanticMatches == 1 && restoredIndex == 1,
                "reordered twin rows did not restore the exact selected target");
    }

    // B5) Controller values use a fixed latest-wins route table. The message
    // thread observes publication order across routes, old generations never
    // become current, and capacity overflow remains explicit/fail-open.
    {
        using Dispatcher =
            ChurchDAWMidiMapping::FixedControllerRouteDispatcher<2, 32>;
        using PublishResult =
            ChurchDAWMidiMapping::ControllerRoutePublishResult;
        static_assert (std::is_trivially_copyable<
                           ChurchDAWMidiMapping::ControllerRouteEvent>::value,
                       "controller callback events must remain POD");

        Dispatcher dispatcher;
        dispatcher.beginRouteUpdate();
        const auto routeA = dispatcher.publishRoute (
            0, "device-a", 8, 1, 74, false);
        const auto routeB = dispatcher.publishRoute (
            1, "device-b", 8, 2, 71, false);
        dispatcher.finishRouteUpdate (9, 2, 2);
        expect (routeA != 0 && routeB != 0
                 && ! dispatcher.overflowed(),
                "fixed controller route table did not publish two routes");

        expect (dispatcher.tryPublish (
                    "device-a", 8, 1, 74, false, 10, 9)
                    == PublishResult::published
                 && dispatcher.tryPublish (
                    "device-a", 8, 1, 74, false, 127, 9)
                    == PublishResult::published
                 && dispatcher.tryPublish (
                    "device-b", 8, 2, 71, false, 64, 9)
                    == PublishResult::published,
                "mapped controller burst was not published");

        ChurchDAWMidiMapping::ControllerRouteEvent eventA;
        ChurchDAWMidiMapping::ControllerRouteEvent eventB;
        expect (dispatcher.tryConsumeNext (eventA)
                 && dispatcher.tryConsumeNext (eventB)
                 && eventA.routeSlot == 0 && eventA.value == 127
                 && eventB.routeSlot == 1 && eventB.value == 64
                 && eventA.publicationSequence
                        < eventB.publicationSequence
                 && dispatcher.coalescedCount() == 1,
                "latest-wins/global route publication order was violated");
        expect (! dispatcher.tryConsumeNext (eventA)
                 && ! dispatcher.hasPending(),
                "controller route drain left a phantom event");
        expect (dispatcher.tryPublish (
                    "device-a", 8, 1, 74, false, 1, 10)
                    == PublishResult::staleTable,
                "stale mapping generation was accepted by route table");

        expect (dispatcher.tryPublish (
                    "device-a", 8, 1, 74, false, 42, 9)
                    == PublishResult::published,
                "could not stage an old-generation pending route event");
        dispatcher.beginRouteUpdate();
        const auto reusedRoute = dispatcher.publishRoute (
            0, "device-c", 8, 3, 20, false);
        dispatcher.finishRouteUpdate (10, 1, 1);
        expect (reusedRoute != 0 && reusedRoute != routeA
                 && dispatcher.tryConsumeNext (eventA)
                 && eventA.routeGeneration == routeA
                 && eventA.mappingGeneration == 9
                 && dispatcher.tryPublish (
                        "device-a", 8, 1, 74, false, 1, 10)
                        == PublishResult::notMapped
                 && dispatcher.tryPublish (
                        "device-c", 8, 3, 20, false, 99, 10)
                        == PublishResult::published
                 && dispatcher.tryConsumeNext (eventA)
                 && eventA.routeGeneration == reusedRoute
                 && eventA.mappingGeneration == 10,
                "reused route slot accepted an event from its old generation");

        ChurchDAWMidiMapping::FixedControllerRouteDispatcher<1, 16>
            boundedRoutes;
        boundedRoutes.beginRouteUpdate();
        expect (boundedRoutes.publishRoute (
                    0, "a", 1, 1, 1, false) != 0,
                "bounded route did not publish its first entry");
        boundedRoutes.finishRouteUpdate (3, 2, 1);
        expect (boundedRoutes.overflowed()
                 && boundedRoutes.overflowEpisodeCount() == 1
                 && boundedRoutes.tryPublish (
                        "b", 1, 1, 2, false, 1, 3)
                        == PublishResult::notMapped,
                "route capacity overflow did not fail open explicitly");
    }

    // B6) Parameter callbacks publish only bounded POD events. Concurrent
    // producers never allocate through the mailbox; full capacity drops, clear
    // drains Learn state, and old topology generations are rejected.
    {
        ChurchDAWMidiMapping::ParameterTouchMailbox<64> mailbox;
        int processors[4] {};
        std::atomic<int> publishFailures { 0 };
        std::atomic<int> producersReady { 0 };
        std::atomic<bool> startPublishing { false };
        std::array<std::array<juce::uint64, 8>, 4>
            acceptedSequences {};
        std::vector<std::thread> producers;
        for (int producer = 0; producer < 4; ++producer)
            producers.emplace_back ([&, producer]
            {
                producersReady.fetch_add (1, std::memory_order_release);
                while (! startPublishing.load (std::memory_order_acquire))
                    std::this_thread::yield();
                for (int parameter = 0; parameter < 8; ++parameter)
                {
                    juce::uint64 acceptedSequence = 0;
                    if (mailbox.tryPublish (
                            &processors[producer],
                            producer * 8 + parameter, 7, 70,
                            &acceptedSequence))
                        acceptedSequences[static_cast<size_t> (producer)]
                                         [static_cast<size_t> (parameter)] =
                            acceptedSequence;
                    else
                        publishFailures.fetch_add (
                            1, std::memory_order_relaxed);
                }
            });
        while (producersReady.load (std::memory_order_acquire) != 4)
            std::this_thread::yield();
        startPublishing.store (true, std::memory_order_release);
        for (auto& producer : producers)
            producer.join();

        std::map<juce::uint64, std::pair<std::uintptr_t, int>>
            acceptedPayloads;
        for (int producer = 0; producer < 4; ++producer)
            for (int parameter = 0; parameter < 8; ++parameter)
                if (const auto sequence =
                        acceptedSequences[static_cast<size_t> (producer)]
                                         [static_cast<size_t> (parameter)];
                    sequence != 0)
                    acceptedPayloads.emplace (
                        sequence,
                        std::make_pair (
                            reinterpret_cast<std::uintptr_t> (
                                &processors[producer]),
                            producer * 8 + parameter));

        int consumed = 0;
        juce::uint64 previousSequence = 0;
        ChurchDAWMidiMapping::ParameterTouchEvent touch;
        while (mailbox.tryConsume (touch))
        {
            expect (ChurchDAWMidiMapping::parameterTouchBelongsToGeneration (
                        touch, 7, 70),
                    "concurrent mailbox publication corrupted an event");
            const auto accepted = acceptedPayloads.find (
                touch.publicationSequence);
            expect (touch.publicationSequence > previousSequence
                     && accepted != acceptedPayloads.end()
                     && accepted->second.first == touch.processor
                     && accepted->second.second == touch.parameterIndex,
                    "MPSC mailbox inverted or corrupted global CAS order");
            previousSequence = touch.publicationSequence;
            ++consumed;
        }
        const auto failedPublishes =
            publishFailures.load (std::memory_order_relaxed);
        expect (consumed
                    == static_cast<int> (acceptedPayloads.size())
                 && consumed + failedPublishes == 32
                 && mailbox.droppedCount()
                        == static_cast<juce::uint64> (failedPublishes),
                "fixed mailbox corrupted accepted/dropped contention accounting");

        ChurchDAWMidiMapping::ParameterTouchMailbox<2> bounded;
        expect (bounded.tryPublish (&processors[0], 1, 10, 100)
                 && bounded.tryPublish (&processors[1], 2, 10, 100)
                 && ! bounded.tryPublish (&processors[2], 3, 10, 100)
                 && bounded.droppedCount() == 1,
                "mailbox overflow did not drop/fail-open");
        bounded.clear();
        expect (! bounded.tryConsume (touch),
                "mailbox clear left a Learn event ready");

        expect (bounded.tryPublish (&processors[0], 4, 11, 110)
                 && bounded.tryConsume (touch)
                 && ! ChurchDAWMidiMapping::parameterTouchBelongsToGeneration (
                        touch, 12, 110)
                 && ! ChurchDAWMidiMapping::parameterTouchBelongsToGeneration (
                        touch, 11, 111),
                "stale pointer/generation event was accepted");

        ChurchDAWMidiMapping::ParameterTouchMailbox<4> reusable;
        juce::uint64 lastAcceptedSequence = 0;
        for (int event = 0; event < 12; ++event)
        {
            juce::uint64 acceptedSequence = 0;
            expect (reusable.tryPublish (
                        &processors[event % 4], event, 20, 200,
                        &acceptedSequence)
                     && reusable.tryConsume (touch)
                     && touch.parameterIndex == event
                     && touch.publicationSequence == acceptedSequence
                     && acceptedSequence > lastAcceptedSequence,
                    "mailbox slot reuse/wrap changed FIFO acceptance order");
            lastAcceptedSequence = acceptedSequence;
        }
        expect (reusable.droppedCount() == 0
                 && ! reusable.tryConsume (touch),
                "mailbox wrap left stale payload or reported a false drop");

        ChurchDAWMidiMapping::ParameterTouchMailbox<4> filtered;
        expect (filtered.tryPublish (&processors[0], 1, 30, 300)
                 && filtered.tryPublish (&processors[1], 2, 29, 300)
                 && filtered.tryPublish (&processors[2], 3, 30, 300),
                "generation-filter mailbox setup failed");
        int lastAcceptedParameter = -1;
        juce::uint64 lastAcceptedPublication = 0;
        while (filtered.tryConsume (touch))
            if (ChurchDAWMidiMapping::parameterTouchBelongsToGeneration (
                    touch, 30, 300))
            {
                expect (touch.publicationSequence
                            > lastAcceptedPublication,
                        "accepted Learn touches were not globally ordered");
                lastAcceptedPublication = touch.publicationSequence;
                lastAcceptedParameter = touch.parameterIndex;
            }
        expect (lastAcceptedParameter == 3,
                "last Learn target followed stale/physical slot order");

        ChurchDAWMidiMapping::FixedMidiLearnControlQueue<2, 32, 32>
            learnControls;
        expect (learnControls.tryPublish (
                    "source", 6, "name", 4, 1, 74, 127, false,
                    false, 40),
                "Learn control queue setup failed");
        learnControls.clear(); // producers are absent/drained in this test
        ChurchDAWMidiMapping::FixedMidiLearnControlQueue<2, 32, 32>::
            Event learnedControl;
        expect (! learnControls.tryConsume (learnedControl)
                 && learnControls.tryPublish (
                        "source", 6, "name", 4, 1, 75, 64, false,
                        false, 41)
                 && learnControls.tryConsume (learnedControl)
                 && learnedControl.number == 75
                 && learnedControl.targetGeneration == 41,
                "drained Learn control queue was not reusable");

        // The callback lifetime gate is a real nested counter, not a one-shot
        // CAS. A second/reentrant callback must still execute the mandatory
        // transpose path while the first callback is active.
        ChurchDAWMidiMapping::ParameterCallbackLifetimeGate callbackGate;
        auto outerCallback = callbackGate.tryEnter();
        auto nestedCallback = callbackGate.tryEnter();
        int synchronisedTranspose = 0;
        if (nestedCallback)
            synchronisedTranspose = ChurchDAWMidiMapping::
                globalTransposeSemitonesFromNormalised (
                    19.0f / 24.0f); // +7 in the fixed -12..+12 range
        expect (outerCallback && nestedCallback
                 && callbackGate.activeCount() == 2
                 && synchronisedTranspose == 7,
                "callback contention dropped mandatory global transpose");

        callbackGate.closeTemporarily();
        auto rejectedWhileClosed = callbackGate.tryEnter();
        expect (! rejectedWhileClosed && callbackGate.isClosed()
                 && callbackGate.activeCount() == 2,
                "high-bit close admitted a callback or corrupted nesting");
        nestedCallback = {};
        outerCallback = {};
        expect (callbackGate.isDrained() && callbackGate.reopen(),
                "nested callback leases did not drain/reopen exactly once");

        auto afterTemporaryReopen = callbackGate.tryEnter();
        expect (afterTemporaryReopen
                 && callbackGate.activeCount() == 1,
                "temporary callback barrier did not reopen");
        afterTemporaryReopen = {};
        callbackGate.closePermanently();
        expect (callbackGate.isDrained()
                 && ! callbackGate.reopen()
                 && ! callbackGate.tryEnter(),
                "permanent callback retirement reopened or accepted work");

        ChurchDAWMidiMapping::ParameterCallbackRoute callbackRoute;
        callbackRoute.publish (&processors[0], 500);
        ChurchDAWMidiMapping::ParameterCallbackRoute::Snapshot routeA;
        expect (callbackRoute.snapshotIfCurrent (
                    &processors[0], routeA)
                 && routeA.topologyGeneration == 500,
                "active callback route did not validate A");
        expect (callbackRoute.invalidate (&processors[0])
                 && ! callbackRoute.snapshotIfCurrent (
                        &processors[0], routeA),
                "about-to-retire did not invalidate A before destruction");
        callbackRoute.publish (&processors[1], 501);
        ChurchDAWMidiMapping::ParameterTouchEvent staleA {
            reinterpret_cast<std::uintptr_t> (&processors[0]),
            3, 12, routeA.topologyGeneration
        };
        expect (! ChurchDAWMidiMapping::parameterTouchBelongsToGeneration (
                    staleA, 12, 501),
                "callback started under A read/was accepted as generation C");

        struct TeardownProbe
        {
            int schedules = 0;
            int destructions = 0;
        } teardownProbe;
        ChurchDAWMidiMapping::DeferredOwnerTeardownGate ownerGate;
        ownerGate.setDrainReadyCallback (
            &teardownProbe, [] (void* context) noexcept
            {
                ++static_cast<TeardownProbe*> (context)->schedules;
            });
        auto outerFrame = ownerGate.tryEnter();
        auto nestedFrame = ownerGate.tryEnter();
        const bool firstTeardownRequest = ownerGate.requestClose();
        const bool duplicateTeardownRequest = ownerGate.requestClose();
        expect (outerFrame && nestedFrame
                 && firstTeardownRequest
                 && ! duplicateTeardownRequest
                 && ownerGate.activeCount() == 2
                 && teardownProbe.schedules == 0
                 && ! ownerGate.tryEnter(),
                "nested owner frames did not close/coalesce fail-closed");

        nestedFrame = {};
        expect (ownerGate.activeCount() == 1
                 && teardownProbe.schedules == 0
                 && teardownProbe.destructions == 0,
                "nested release scheduled or destroyed before the last frame");
        outerFrame = {};
        expect (ownerGate.isClosedAndDrained()
                 && teardownProbe.schedules == 1
                 && teardownProbe.destructions == 0,
                "last frame did not schedule exactly one deferred drain");

        if (ownerGate.consumeScheduledDrain())
            ++teardownProbe.destructions; // emulates the later AsyncUpdater
        expect (teardownProbe.destructions == 1
                 && ! ownerGate.consumeScheduledDrain()
                 && ownerGate.resetAfterDrain()
                 && ownerGate.tryEnter(),
                "deferred owner teardown ran inline, duplicated, or failed to reset");

        ChurchDAWMidiMapping::SinglePendingRetirementSlot retiredNode;
        expect (retiredNode.tryReserve (101)
                 && retiredNode.tryReserve (101)
                 && ! retiredNode.tryReserve (102)
                 && retiredNode.hasPending()
                 && retiredNode.node() == 101
                 && ! retiredNode.clearIf (102)
                 && retiredNode.clearIf (101)
                 && retiredNode.tryReserve (102),
                "one-node retirement slot exceeded capacity or was not reusable");
    }

    // C) One pure helper drives rekeying for CC, CC action, and note maps.
    // Unique A->B moves; duplicate names and destination collisions preserve
    // every entry and sourceName.
    {
        using Key = ChurchDAWMidiMapping::ControllerKey;
        using Descriptor = ChurchDAWMidiMapping::MidiSourceDescriptor;
        const std::vector<Descriptor> unique { { "B", "Controller" } };
        std::map<Key, int> cc { { { "midi:A", 1, 21, false, "Controller" }, 21 } };
        std::map<Key, int> actions { { { "midi:A", 1, 22, false, "Controller" }, 22 } };
        std::map<Key, int> notes { { { "midi:A", 1, 60, true, "Controller" }, 60 } };
        expect (ChurchDAWMidiMapping::rekeyMidiMapByUniqueSourceName (cc, unique).moved == 1,
                "CC A->B rekey did not move");
        expect (ChurchDAWMidiMapping::rekeyMidiMapByUniqueSourceName (actions, unique).moved == 1,
                "CC action A->B rekey did not move");
        expect (ChurchDAWMidiMapping::rekeyMidiMapByUniqueSourceName (notes, unique).moved == 1,
                "note A->B rekey did not move");
        expect (cc.begin()->first.sourceId == "midi:B"
                    && cc.begin()->first.sourceName == "Controller",
                "CC rekey lost sourceName");

        const std::vector<Descriptor> duplicate {
            { "B", "Controller" }, { "C", "Controller" } };
        std::map<Key, int> ambiguous {
            { { "midi:A", 1, 21, false, "Controller" }, 1 } };
        expect (ChurchDAWMidiMapping::rekeyMidiMapByUniqueSourceName (
                    ambiguous, duplicate).ambiguous == 1
                    && ambiguous.begin()->first.sourceId == "midi:A",
                "duplicate sourceName was rekeyed instead of staying stale");

        std::map<Key, int> collision {
            { { "midi:A", 1, 21, false, "Controller" }, 1 },
              { { "midi:B", 1, 21, false, "Controller" }, 2 } };
        const auto collisionResult =
            ChurchDAWMidiMapping::rekeyMidiMapByUniqueSourceName (collision, unique);
        expect (collisionResult.collisions > 0 && collision.size() == 2,
                "rekey collision discarded one of two mappings");

        std::map<Key, int> normalisationCollision {
            { { "B", 1, 21, false, "Controller old" }, 1 },
              { { "midi:B", 1, 21, false, "Controller new" }, 2 } };
        const auto normalisationResult =
            ChurchDAWMidiMapping::rekeyMidiMapByUniqueSourceName (
                normalisationCollision, {});
        expect (normalisationResult.collisions > 0
                    && normalisationCollision.size() == 2,
                "source-id normalisation collision discarded a mapping");

        std::map<Key, int> crossMapParameter {
            { { "midi:old-parameter", 1, 7, false, "Controller" }, 1 } };
        std::map<Key, int> crossMapAction {
            { { "midi:old-action", 1, 7, false, "Controller" }, 2 } };
        ChurchDAWMidiMapping::rekeyMidiMapByUniqueSourceName (
            crossMapParameter, unique);
        ChurchDAWMidiMapping::rekeyMidiMapByUniqueSourceName (
            crossMapAction, unique);
        expect (ChurchDAWMidiMapping::midiMapCollectionsHaveConflictingCCControls (
                    crossMapParameter, crossMapAction),
                "Android rekey missed a cross-map CC parameter/action collision");

        std::map<Key, int> unnamedOffline {
            { { "midi:old", 1, 7, false, {} }, 1 } };
        expect (ChurchDAWMidiMapping::hasUnresolvedUnnamedMidiSource (
                    unnamedOffline, unique),
                "offline unnamed source did not suspend automatic profiles");

        std::map<Key, int> namedOffline {
            { { "midi:old", 1, 7, false, "Controller" }, 1 } };
        expect (! ChurchDAWMidiMapping::hasUnresolvedUnnamedMidiSource (
                    namedOffline, unique),
                "named offline source unnecessarily suspended automatic profiles");

        std::map<Key, int> unnamedAvailable {
            { { "midi:B", 1, 7, false, {} }, 1 } };
        expect (! ChurchDAWMidiMapping::hasUnresolvedUnnamedMidiSource (
                    unnamedAvailable, unique),
                "available unnamed source unnecessarily suspended automatic profiles");

        expect (ChurchDAWMidiMapping::samePhysicalMidiControl (
                    Key { "midi:old-id", 1, 60, true, "Controller" },
                    Key { "midi:new-id", 1, 60, true, " controller " },
                    unique),
                "same Android controller was not matched through its stable name");
        expect (! ChurchDAWMidiMapping::samePhysicalMidiControl (
                    Key { "midi:old-id", 1, 60, true, "Controller" },
                    Key { "midi:new-id", 1, 60, true, " controller " },
                    duplicate),
                "ambiguous same-name controllers were merged");
        expect (! ChurchDAWMidiMapping::samePhysicalMidiControl (
                    Key { "midi:old-id", 1, 60, true, "Controller A" },
                    Key { "midi:new-id", 1, 60, true, "Controller B" }),
                "different controllers were treated as the same physical input");
    }

    // D) Offline persistence prefers the preserved name.
    expect (ChurchDAWMidiMapping::normalizeMidiSourceId (
                "churchdaw:launchkey-daw") == "churchdaw:launchkey-daw",
            "virtual Launchkey source ID changed namespace during restore");
    expect (ChurchDAWMidiMapping::normalizeMidiSourceId (
                "CHURCHDAW:launchkey-daw") == "churchdaw:launchkey-daw",
            "virtual source namespace was not canonicalised");
    expect (ChurchDAWMidiMapping::normalizeMidiSourceId (
                "midi:churchdaw:launchkey-daw")
                == "churchdaw:launchkey-daw",
            "legacy double-prefixed Launchkey DAW ID was not migrated");
    expect (ChurchDAWMidiMapping::normalizeMidiSourceId ("B") == "midi:B",
            "physical MIDI source ID was not normalised");
    expect (ChurchDAWMidiMapping::normalizeMidiSourceId ("MIDI:B") == "midi:B",
            "physical MIDI namespace was not canonicalised");
    expect (ChurchDAWMidiMapping::resolveUniqueMidiSourceId (
                "churchdaw:launchkey-daw", "Launchkey DAW",
                { { "physical-port", "Launchkey DAW" } })
                == "churchdaw:launchkey-daw",
            "physical source with same name stole a virtual Launchkey mapping");
    expect (ChurchDAWMidiMapping::sourceNameForPersistence (
                "Saved Controller", {}) == "Saved Controller",
            "offline sourceName was not preserved");
    expect (ChurchDAWMidiMapping::sourceNameForPersistence ({}, "Online Controller")
                == "Online Controller",
            "available sourceName was not used");
    expect (ChurchDAWMidiMapping::midiSourceNamesEqual (
                "  ARTURIA KeyLab ", "Arturia KeyLab"),
            "stable device-name matching did not tolerate OS case/spacing changes");

    // E) Real temporary files exercise atomic promotion, recovery, schema,
    // explicit empty maps, and preservation of an existing last-good file.
    {
        const auto directory = juce::File::getSpecialLocation (
            juce::File::tempDirectory).getNonexistentChildFile (
                "churchdaw-midi-m0", {}, false);
        directory.createDirectory();
        const auto mainFile = directory.getChildFile ("midi-note-map.xml");
        const auto lastGood = directory.getChildFile ("midi-note-map.xml.last-good");

        juce::XmlElement valid ("NOTEMAP");
        valid.setAttribute ("schema", 1);
        valid.setAttribute ("complete", 1);
        valid.setAttribute ("entryCount", 1);
        valid.setAttribute ("explicitEmpty", 0);
        auto* note = valid.createNewChildElement ("NOTE");
        note->setAttribute ("source", "midi:A");
        note->setAttribute ("channel", 1);
        note->setAttribute ("note", 60);
        note->setAttribute ("action", 1);
        note->setAttribute ("arg", 0);
        expect (valid.writeTo (mainFile), "could not create valid temp MIDI map");
        expect (ChurchDAWMidiMapping::promoteValidMidiMapToLastGood (
                    mainFile, lastGood, "NOTEMAP", "NOTE")
                    && lastGood.existsAsFile(),
                "valid main was not atomically promoted to last-good");

        mainFile.replaceWithText ("<BROKEN");
        expect (ChurchDAWMidiMapping::recoverMidiMapXml (
                    mainFile, lastGood, "NOTEMAP", "NOTE") != nullptr,
                "corrupt main did not recover last-good");
        auto repairedMain = juce::parseXML (mainFile);
        expect (repairedMain != nullptr
                    && ChurchDAWMidiMapping::isMidiMapXmlValid (
                        *repairedMain, "NOTEMAP", "NOTE"),
                "last-good recovery did not repair the corrupt main file");

        juce::XmlElement explicitEmpty ("NOTEMAP");
        explicitEmpty.setAttribute ("schema", 1);
        explicitEmpty.setAttribute ("complete", 1);
        explicitEmpty.setAttribute ("entryCount", 0);
        explicitEmpty.setAttribute ("explicitEmpty", 1);
        expect (ChurchDAWMidiMapping::isMidiMapXmlValid (
                    explicitEmpty, "NOTEMAP", "NOTE"),
                "explicit empty map was rejected");

        juce::XmlElement unexpectedEmpty ("NOTEMAP");
        unexpectedEmpty.setAttribute ("schema", 1);
        unexpectedEmpty.setAttribute ("complete", 1);
        unexpectedEmpty.setAttribute ("entryCount", 0);
        unexpectedEmpty.setAttribute ("explicitEmpty", 0);
        expect (! ChurchDAWMidiMapping::isMidiMapXmlValid (
                    unexpectedEmpty, "NOTEMAP", "NOTE"),
                "unexpected empty map replaced a valid snapshot");

        juce::XmlElement unknownChild ("NOTEMAP");
        unknownChild.setAttribute ("schema", 1);
        unknownChild.setAttribute ("complete", 1);
        unknownChild.setAttribute ("entryCount", 1);
        unknownChild.setAttribute ("explicitEmpty", 0);
        unknownChild.createNewChildElement ("UNKNOWN");
        expect (! ChurchDAWMidiMapping::isMidiMapXmlValid (
                    unknownChild, "NOTEMAP", "NOTE"),
                "unknown child was accepted in a complete MIDI snapshot");

        juce::XmlElement legacyUnknown ("NOTEMAP");
        legacyUnknown.setAttribute ("schema", 1);
        legacyUnknown.createNewChildElement ("UNKNOWN");
        expect (! ChurchDAWMidiMapping::isMidiMapXmlReadable (
                    legacyUnknown, "NOTEMAP", "NOTE"),
                "unknown child was accepted through the legacy reader");

        juce::XmlElement legacyValid ("NOTEMAP");
        legacyValid.setAttribute ("schema", 1);
        auto* legacyNote = legacyValid.createNewChildElement ("NOTE");
        legacyNote->setAttribute ("source", "midi:A");
        legacyNote->setAttribute ("channel", 1);
        legacyNote->setAttribute ("note", 60);
        legacyNote->setAttribute ("action", 1);
        legacyNote->setAttribute ("arg", 0);
        expect (ChurchDAWMidiMapping::isMidiMapXmlReadable (
                    legacyValid, "NOTEMAP", "NOTE"),
                "valid pre-metadata MIDI map was not readable");

        juce::XmlElement semanticallyInvalid ("NOTEMAP");
        semanticallyInvalid.setAttribute ("schema", 1);
        semanticallyInvalid.setAttribute ("complete", 1);
        semanticallyInvalid.setAttribute ("entryCount", 1);
        semanticallyInvalid.setAttribute ("explicitEmpty", 0);
        auto* badNote = semanticallyInvalid.createNewChildElement ("NOTE");
        badNote->setAttribute ("source", "midi:A");
        badNote->setAttribute ("channel", 0);
        badNote->setAttribute ("note", 200);
        badNote->setAttribute ("action", 1);
        badNote->setAttribute ("arg", 99);
        expect (! ChurchDAWMidiMapping::isMidiMapXmlValid (
                    semanticallyInvalid, "NOTEMAP", "NOTE"),
                "semantically invalid NOTE was promoted as last-good");

        badNote->setAttribute ("source", "   ");
        badNote->setAttribute ("channel", 1);
        badNote->setAttribute ("note", 60);
        badNote->setAttribute ("arg", 0);
        expect (! ChurchDAWMidiMapping::isMidiMapXmlValid (
                    semanticallyInvalid, "NOTEMAP", "NOTE"),
                "whitespace-only MIDI source was accepted");
        expect (ChurchDAWMidiMapping::normalizeMidiSourceId ("   ").isEmpty(),
                "whitespace-only source produced a live MIDI identifier");

        juce::XmlElement malformedInteger ("NOTEMAP");
        malformedInteger.setAttribute ("schema", 1);
        malformedInteger.setAttribute ("complete", 1);
        malformedInteger.setAttribute ("entryCount", 1);
        malformedInteger.setAttribute ("explicitEmpty", 0);
        auto* malformedNote = malformedInteger.createNewChildElement ("NOTE");
        malformedNote->setAttribute ("source", "midi:A");
        malformedNote->setAttribute ("channel", "1junk");
        malformedNote->setAttribute ("note", 60);
        malformedNote->setAttribute ("action", 1);
        malformedNote->setAttribute ("arg", 0);
        expect (! ChurchDAWMidiMapping::isMidiMapXmlValid (
                    malformedInteger, "NOTEMAP", "NOTE"),
                "malformed numeric attribute was partially parsed and accepted");

        juce::XmlElement partialMetadata ("NOTEMAP");
        partialMetadata.setAttribute ("schema", 1);
        partialMetadata.setAttribute ("complete", 1);
        expect (! ChurchDAWMidiMapping::isMidiMapXmlReadable (
                    partialMetadata, "NOTEMAP", "NOTE"),
                "partial snapshot metadata was accepted as a legacy map");

        juce::XmlElement legacyEmpty ("NOTEMAP");
        legacyEmpty.setAttribute ("schema", 1);
        expect (! ChurchDAWMidiMapping::isMidiMapXmlReadable (
                    legacyEmpty, "NOTEMAP", "NOTE"),
                "empty legacy map could overwrite a populated last-good snapshot");

        juce::XmlElement explicitModernEmpty ("NOTEMAP");
        explicitModernEmpty.setAttribute ("schema", 1);
        explicitModernEmpty.setAttribute ("complete", 1);
        explicitModernEmpty.setAttribute ("entryCount", 0);
        explicitModernEmpty.setAttribute ("explicitEmpty", 1);
        expect (ChurchDAWMidiMapping::isMidiMapXmlReadable (
                    explicitModernEmpty, "NOTEMAP", "NOTE"),
                "explicit modern empty map was rejected");

        juce::XmlElement validCC ("MIDIMAP");
        validCC.setAttribute ("schema", 1);
        validCC.setAttribute ("complete", 1);
        validCC.setAttribute ("entryCount", 2);
        validCC.setAttribute ("explicitEmpty", 0);
        auto* parameterMap = validCC.createNewChildElement ("MAP");
        parameterMap->setAttribute ("source", "midi:A");
        parameterMap->setAttribute ("channel", 1);
        parameterMap->setAttribute ("cc", 7);
        parameterMap->setAttribute ("isNote", 0);
        parameterMap->setAttribute ("slot", -1);
        parameterMap->setAttribute ("insert", -1);
        auto* actionMap = validCC.createNewChildElement ("CCACAO");
        actionMap->setAttribute ("source", "midi:A");
        actionMap->setAttribute ("channel", 1);
        actionMap->setAttribute ("cc", 21);
        actionMap->setAttribute ("action", 7);
        actionMap->setAttribute ("arg", 15);
        actionMap->setAttribute ("modo", 1);
        expect (ChurchDAWMidiMapping::isMidiMapXmlValid (
                    validCC, "MIDIMAP", "MAP", "CCACAO"),
                "valid parameter/action CC snapshot was rejected");
        actionMap->setAttribute ("arg", 16);
        expect (! ChurchDAWMidiMapping::isMidiMapXmlValid (
                    validCC, "MIDIMAP", "MAP", "CCACAO"),
                "out-of-range persisted CC action was accepted");

        juce::XmlElement duplicateIdentity ("NOTEMAP");
        duplicateIdentity.setAttribute ("schema", 1);
        duplicateIdentity.setAttribute ("complete", 1);
        duplicateIdentity.setAttribute ("entryCount", 2);
        duplicateIdentity.setAttribute ("explicitEmpty", 0);
        for (const auto& source : { juce::String ("B"), juce::String ("midi:B") })
        {
            auto* duplicate = duplicateIdentity.createNewChildElement ("NOTE");
            duplicate->setAttribute ("source", source);
            duplicate->setAttribute ("channel", 1);
            duplicate->setAttribute ("note", 60);
            duplicate->setAttribute ("action", 1);
            duplicate->setAttribute ("arg", 0);
        }
        expect (! ChurchDAWMidiMapping::isMidiMapXmlValid (
                    duplicateIdentity, "NOTEMAP", "NOTE"),
                "normalised duplicate identities would collapse during restore");

        juce::XmlElement numericDuplicate ("NOTEMAP");
        numericDuplicate.setAttribute ("schema", 1);
        numericDuplicate.setAttribute ("complete", 1);
        numericDuplicate.setAttribute ("entryCount", 2);
        numericDuplicate.setAttribute ("explicitEmpty", 0);
        for (const auto& channel : { juce::String ("01"), juce::String ("1") })
        {
            auto* duplicate = numericDuplicate.createNewChildElement ("NOTE");
            duplicate->setAttribute ("source", "midi:A");
            duplicate->setAttribute ("channel", channel);
            duplicate->setAttribute ("note", 60);
            duplicate->setAttribute ("action", 1);
            duplicate->setAttribute ("arg", 0);
        }
        expect (! ChurchDAWMidiMapping::isMidiMapXmlValid (
                    numericDuplicate, "NOTEMAP", "NOTE"),
                "numeric spellings of the same mapping were not deduplicated");

        juce::XmlElement future ("NOTEMAP");
        future.setAttribute ("schema", 2);
        expect (ChurchDAWMidiMapping::hasFutureMidiMapSchema (
                    future, "NOTEMAP")
                    && ! ChurchDAWMidiMapping::isMidiMapXmlReadable (
                        future, "NOTEMAP", "NOTE"),
                "future MIDI schema was not protected");
        expect (future.writeTo (mainFile)
                    && ChurchDAWMidiMapping::midiMapFilesHaveFutureSchema (
                        mainFile, lastGood, "NOTEMAP"),
                "future schema written after startup was not detected in the primary");
        expect (valid.writeTo (mainFile) && future.writeTo (lastGood)
                    && ChurchDAWMidiMapping::midiMapFilesHaveFutureSchema (
                        mainFile, lastGood, "NOTEMAP"),
                "future schema written after startup was not detected in last-good");

        mainFile.replaceWithText ("<BROKEN-COPY");
        const auto lastGoodBeforeCopy = lastGood.loadFileAsString();
        const auto corruptCopy =
            ChurchDAWMidiMapping::uniqueCorruptCopyTarget (mainFile);
        expect (ChurchDAWMidiMapping::atomicCopyFile (mainFile, corruptCopy)
                    && lastGood.loadFileAsString() == lastGoodBeforeCopy,
                "copying a corrupt main modified the previous last-good");

        lastGood.replaceWithText ("<ALSO-BROKEN");
        mainFile.replaceWithText ("<BROKEN-AGAIN");
        expect (ChurchDAWMidiMapping::recoverMidiMapXml (
                    mainFile, lastGood, "NOTEMAP", "NOTE") == nullptr,
                "two corrupt MIDI snapshots produced a false recovery");
        directory.deleteRecursively();
    }

    // E2) Parsing and inspection have separate failure domains. Only a fully
    // bounded byte snapshot may be classified as malformed XML and recovered;
    // an existing file that cannot be inspected makes the complete pair fail
    // closed without copying or replacing any target.
    {
        expect (ChurchDAWMidiMapping::parseBoundedXmlText (
                    "<!DOCTYPE NOTEMAP [<!ENTITY x 'boom'>]>"
                    "<NOTEMAP>&x;</NOTEMAP>") == nullptr,
                "DTD/entity input reached the XML parser");

        juce::String tooDeep;
        for (int i = 0; i < 40; ++i)
            tooDeep += "<N>";
        for (int i = 0; i < 40; ++i)
            tooDeep += "</N>";
        expect (ChurchDAWMidiMapping::parseBoundedXmlText (
                    tooDeep) == nullptr,
                "over-depth XML was accepted");

        juce::String tooManyElements ("<ROOT>");
        for (int i = 0; i < 16384; ++i)
            tooManyElements += "<N/>";
        tooManyElements += "</ROOT>";
        expect (ChurchDAWMidiMapping::parseBoundedXmlText (
                    tooManyElements) == nullptr,
                "over-element-limit XML was accepted");

        const std::uint8_t invalidUtf8Bytes[] = {
            '<', 'N', '>', 0xc3, 0x28, '<', '/', 'N', '>'
        };
        juce::MemoryBlock invalidUtf8 (
            invalidUtf8Bytes, sizeof (invalidUtf8Bytes));
        expect (ChurchDAWMidiMapping::parseBoundedXmlData (
                    invalidUtf8) == nullptr,
                "invalid UTF-8 reached the XML parser");

        // The writer must validate the final serialized bytes, not the tree
        // before it injects transactionId. Build a semantically readable
        // legacy CC map exactly at the accepted 8 MiB boundary, then prove the
        // owner attribute pushes it over and is rejected before any disk write.
        juce::XmlElement boundaryMap ("MIDIMAP");
        std::array<juce::XmlElement*, 128> boundaryEntries {};
        for (int i = 0; i < (int) boundaryEntries.size(); ++i)
        {
            auto* entry = boundaryMap.createNewChildElement ("MAP");
            entry->setAttribute ("source", "boundary-source-" + juce::String (i));
            entry->setAttribute ("channel", 1);
            entry->setAttribute ("cc", 1);
            entry->setAttribute ("slot", -1);
            boundaryEntries[(size_t) i] = entry;
        }

        const auto boundaryFormat =
            juce::XmlElement::TextFormat().singleLine();
        auto boundaryPayload = boundaryMap.toString (boundaryFormat);
        auto remainingBoundaryBytes =
            ChurchDAWMidiMapping::maxMidiMapFileBytes
            - (juce::int64) boundaryPayload.getNumBytesAsUTF8();
        expect (remainingBoundaryBytes > 0,
                "boundary fixture base unexpectedly exceeded the file limit");

        const auto maxAttributeBytes =
            ChurchDAWMidiMapping::midiMapXmlLimits().maxStringBytes;
        for (auto* entry : boundaryEntries)
        {
            const auto source = entry->getStringAttribute ("source");
            const auto capacity = (juce::int64) maxAttributeBytes
                - (juce::int64) source.getNumBytesAsUTF8();
            const auto added = juce::jmin (remainingBoundaryBytes, capacity);
            if (added > 0)
                entry->setAttribute (
                    "source",
                    source + juce::String::repeatedString ("x", (int) added));
            remainingBoundaryBytes -= added;
        }

        expect (remainingBoundaryBytes == 0,
                "boundary fixture could not reach the exact file limit");
        boundaryPayload = boundaryMap.toString (boundaryFormat);
        expect ((juce::int64) boundaryPayload.getNumBytesAsUTF8()
                    == ChurchDAWMidiMapping::maxMidiMapFileBytes,
                "exact-limit MIDI payload size was not constructed");
        expect (ChurchDAWMidiMapping::isMidiMapXmlReadable (
                    boundaryMap, "MIDIMAP", "MAP", "CCACAO"),
                "exact-limit legacy MIDI map was not semantically readable");
        expect (ChurchDAWMidiMapping::midiMapSerializedPayloadFitsSafetyLimits (
                    boundaryPayload),
                "exact-limit serialized MIDI payload was rejected");

        boundaryMap.setAttribute (
            "transactionId", "12345678-1234-1234-1234-123456789abc");
        const auto transformedBoundaryPayload =
            boundaryMap.toString (boundaryFormat);
        expect ((juce::int64) transformedBoundaryPayload.getNumBytesAsUTF8()
                    > ChurchDAWMidiMapping::maxMidiMapFileBytes,
                "transactionId did not cross the serialized byte boundary");
        expect (! ChurchDAWMidiMapping::midiMapSerializedPayloadFitsSafetyLimits (
                    transformedBoundaryPayload),
                "oversized final MIDI payload was accepted for publication");

        // Export embeds two already-serialized maps as XML text nodes. Even
        // when each inner payload is individually valid at 8 MiB, escaping and
        // the bundle envelope make the final file larger than the importer's
        // 16 MiB bound. No such bundle may be offered as a successful export.
        const juce::String bundleTransactionId =
            "12345678-1234-1234-1234-123456789abc";
        boundaryMap.setAttribute ("schema", 1);
        boundaryMap.setAttribute ("complete", 1);
        boundaryMap.setAttribute (
            "entryCount", (int) boundaryEntries.size());
        boundaryMap.setAttribute ("explicitEmpty", 0);

        auto exactCCPayload = boundaryMap.toString (boundaryFormat);
        auto excessCCBytes =
            (juce::int64) exactCCPayload.getNumBytesAsUTF8()
            - ChurchDAWMidiMapping::maxMidiMapFileBytes;
        expect (excessCCBytes > 0,
                "modern CC boundary fixture did not exceed the limit before trimming");
        for (auto* entry : boundaryEntries)
        {
            const auto source = entry->getStringAttribute ("source");
            const auto removable = juce::jmax (
                (juce::int64) 0,
                (juce::int64) source.length() - (juce::int64) 32);
            const auto removed = juce::jmin (excessCCBytes, removable);
            if (removed > 0)
                entry->setAttribute (
                    "source",
                    source.substring (0, source.length() - (int) removed));
            excessCCBytes -= removed;
        }
        expect (excessCCBytes == 0,
                "modern CC boundary fixture could not be trimmed exactly");
        exactCCPayload = boundaryMap.toString (boundaryFormat);
        expect ((juce::int64) exactCCPayload.getNumBytesAsUTF8()
                    == ChurchDAWMidiMapping::maxMidiMapFileBytes
                    && ChurchDAWMidiMapping::isMidiMapXmlValid (
                        boundaryMap, "MIDIMAP", "MAP", "CCACAO")
                    && ChurchDAWMidiMapping::midiMapSerializedPayloadFitsSafetyLimits (
                        exactCCPayload),
                "exact-limit modern CC export payload was not valid");

        juce::XmlElement exactNoteMap ("NOTEMAP");
        exactNoteMap.setAttribute ("schema", 1);
        exactNoteMap.setAttribute ("complete", 1);
        exactNoteMap.setAttribute ("transactionId", bundleTransactionId);
        exactNoteMap.setAttribute ("entryCount", 128);
        exactNoteMap.setAttribute ("explicitEmpty", 0);
        std::array<juce::XmlElement*, 128> exactNoteEntries {};
        for (int i = 0; i < (int) exactNoteEntries.size(); ++i)
        {
            auto* entry = exactNoteMap.createNewChildElement ("NOTE");
            entry->setAttribute (
                "source", "bundle-note-source-" + juce::String (i));
            entry->setAttribute ("channel", 1);
            entry->setAttribute ("note", 1);
            entry->setAttribute ("action", 1);
            entry->setAttribute ("arg", i % 12);
            exactNoteEntries[(size_t) i] = entry;
        }

        auto exactNotePayload = exactNoteMap.toString (boundaryFormat);
        auto remainingNoteBytes =
            ChurchDAWMidiMapping::maxMidiMapFileBytes
            - (juce::int64) exactNotePayload.getNumBytesAsUTF8();
        expect (remainingNoteBytes > 0,
                "NOTE bundle fixture base unexpectedly exceeded the file limit");
        for (auto* entry : exactNoteEntries)
        {
            const auto source = entry->getStringAttribute ("source");
            const auto capacity = (juce::int64) maxAttributeBytes
                - (juce::int64) source.getNumBytesAsUTF8();
            const auto added = juce::jmin (remainingNoteBytes, capacity);
            if (added > 0)
                entry->setAttribute (
                    "source",
                    source + juce::String::repeatedString ("y", (int) added));
            remainingNoteBytes -= added;
        }
        expect (remainingNoteBytes == 0,
                "NOTE bundle fixture could not reach the exact file limit");
        exactNotePayload = exactNoteMap.toString (boundaryFormat);
        expect ((juce::int64) exactNotePayload.getNumBytesAsUTF8()
                    == ChurchDAWMidiMapping::maxMidiMapFileBytes
                    && ChurchDAWMidiMapping::isMidiMapXmlValid (
                        exactNoteMap, "NOTEMAP", "NOTE")
                    && ChurchDAWMidiMapping::midiMapSerializedPayloadFitsSafetyLimits (
                        exactNotePayload),
                "exact-limit modern NOTE export payload was not valid");

        juce::XmlElement oversizedBundle ("CHURCHDAW_MIDI_MAP_BUNDLE");
        oversizedBundle.setAttribute ("version", 1);
        oversizedBundle.setAttribute ("exportedAt", "2026-08-17T00:00:00Z");
        oversizedBundle.createNewChildElement ("CC_MAP")->addTextElement (
            exactCCPayload);
        oversizedBundle.createNewChildElement ("NOTE_MAP")->addTextElement (
            exactNotePayload);
        const auto oversizedBundlePayload = oversizedBundle.toString();
        expect ((juce::int64) oversizedBundlePayload.getNumBytesAsUTF8()
                    > ChurchDAWMidiMapping::maxMidiMapBundleBytes,
                "two exact-limit maps did not exceed the final bundle limit");
        expect (! ChurchDAWMidiMapping::midiMapSerializedBundleFitsSafetyLimits (
                    oversizedBundlePayload),
                "unimportable oversized MIDI bundle passed export preflight");

        juce::XmlElement smallCC ("MIDIMAP");
        smallCC.setAttribute ("schema", 1);
        smallCC.setAttribute ("complete", 1);
        smallCC.setAttribute ("transactionId", bundleTransactionId);
        smallCC.setAttribute ("entryCount", 0);
        smallCC.setAttribute ("explicitEmpty", 1);
        juce::XmlElement smallNote ("NOTEMAP");
        smallNote.setAttribute ("schema", 1);
        smallNote.setAttribute ("complete", 1);
        smallNote.setAttribute ("transactionId", bundleTransactionId);
        smallNote.setAttribute ("entryCount", 0);
        smallNote.setAttribute ("explicitEmpty", 1);
        juce::XmlElement smallBundle ("CHURCHDAW_MIDI_MAP_BUNDLE");
        smallBundle.setAttribute ("version", 1);
        smallBundle.createNewChildElement ("CC_MAP")->addTextElement (
            smallCC.toString (boundaryFormat));
        smallBundle.createNewChildElement ("NOTE_MAP")->addTextElement (
            smallNote.toString (boundaryFormat));
        const auto smallBundlePayload = smallBundle.toString();
        auto reopenedBundle = ChurchDAWMidiMapping::parseBoundedXmlText (
            smallBundlePayload,
            ChurchDAWMidiMapping::midiMapBundleXmlLimits());
        expect (ChurchDAWMidiMapping::midiMapSerializedBundleFitsSafetyLimits (
                    smallBundlePayload)
                    && reopenedBundle != nullptr,
                "small importable MIDI bundle failed export preflight");
        auto* reopenedCCNode = reopenedBundle != nullptr
            ? reopenedBundle->getChildByName ("CC_MAP") : nullptr;
        auto* reopenedNoteNode = reopenedBundle != nullptr
            ? reopenedBundle->getChildByName ("NOTE_MAP") : nullptr;
        auto reopenedCC = reopenedCCNode != nullptr
            ? ChurchDAWMidiMapping::parseBoundedXmlText (
                reopenedCCNode->getAllSubText()) : nullptr;
        auto reopenedNote = reopenedNoteNode != nullptr
            ? ChurchDAWMidiMapping::parseBoundedXmlText (
                reopenedNoteNode->getAllSubText()) : nullptr;
        expect (reopenedCC != nullptr
                    && ChurchDAWMidiMapping::isMidiMapXmlValid (
                        *reopenedCC, "MIDIMAP", "MAP", "CCACAO")
                    && reopenedNote != nullptr
                    && ChurchDAWMidiMapping::isMidiMapXmlValid (
                        *reopenedNote, "NOTEMAP", "NOTE")
                    && reopenedCC->getStringAttribute ("transactionId")
                        == reopenedNote->getStringAttribute ("transactionId"),
                "small MIDI bundle did not round-trip through import limits");

        const auto directory = juce::File::getSpecialLocation (
            juce::File::tempDirectory).getNonexistentChildFile (
                "churchdaw-midi-uninspectable", {}, false);
        expect (directory.createDirectory(),
                "could not create uninspectable-map fixture directory");
        const auto cc = directory.getChildFile ("cc.xml");
        const auto note = directory.getChildFile ("note.xml");
        const auto noteLastGood = note.getSiblingFile (
            note.getFileName() + ".last-good");
        expect (cc.replaceWithText (
                    "<MIDIMAP schema=\"1\" complete=\"1\" entryCount=\"0\" explicitEmpty=\"1\"/>")
                    && noteLastGood.replaceWithText (
                    "<NOTEMAP schema=\"1\" complete=\"1\" entryCount=\"0\" explicitEmpty=\"1\"/>"),
                "could not prepare bounded-inspection pair");

        juce::MemoryOutputStream oversizedFuture;
        oversizedFuture << "<NOTEMAP schema=\"2\"><FUTURE>";
        const juce::String padding =
            juce::String::repeatedString ("x", 4096);
        while ((juce::int64) oversizedFuture.getDataSize()
                    <= ChurchDAWMidiMapping::maxMidiMapFileBytes)
            oversizedFuture << padding;
        oversizedFuture << "</FUTURE></NOTEMAP>";
        const auto oversizedBytes = oversizedFuture.getMemoryBlock();
        expect ((juce::int64) oversizedBytes.getSize()
                    > ChurchDAWMidiMapping::maxMidiMapFileBytes
                    && note.replaceWithData (
                        oversizedBytes.getData(), oversizedBytes.getSize()),
                "could not create well-formed oversized future map");

        const auto beforeHash = ChurchDAWMidiMapping::midiMapDataSha256 (
            oversizedBytes);
        juce::MemoryBlock inspected;
        expect (ChurchDAWMidiMapping::inspectMidiMapTarget (
                    note, inspected)
                    == ChurchDAWMidiMapping::MidiMapTargetInspection::unknown
                    && ! ChurchDAWMidiMapping::midiMapPairTargetsInspectable (
                        cc, note),
                "oversized target was classified as corrupt/inspectable");
        expect (ChurchDAWMidiMapping::recoverMidiMapXml (
                    note, noteLastGood, "NOTEMAP", "NOTE") == nullptr,
                "uninspectable primary incorrectly entered last-good recovery");

        ChurchDAWMidiMapping::MidiMapPairRollback blocked (cc, note);
        expect (! blocked.capture ("oversized-future")
                    && blocked.hadUninspectableTargetConflict()
                    && ! pairJournalFor (cc).exists(),
                "uninspectable quartet published a transaction journal");

        juce::MemoryBlock preserved;
        expect (note.loadFileAsData (preserved)
                    && preserved.getSize() == oversizedBytes.getSize()
                    && ChurchDAWMidiMapping::midiMapDataSha256 (preserved)
                        == beforeHash,
                "oversized future map was copied, repaired or overwritten");

        // Runtime probe for the descriptor-level durability helper. This catches
        // O_RDONLY+fsync failures that still compile on Linux/Android.
        const auto syncProbe = directory.getChildFile ("sync-probe.tmp");
        expect (syncProbe.replaceWithText ("durability-probe")
                    && ChurchDAWMidiMapping::syncPreparedMidiMapFile (syncProbe)
                    && syncProbe.loadFileAsString() == "durability-probe",
                "prepared-file durability helper failed at runtime");
        directory.deleteRecursively();
    }

    // E3) Note ON suppression uses bounded, allocation-free storage. Every ON
    // that is consumed first reserves one matching OFF debt; capacity/depth
    // exhaustion therefore fails the complete gesture open.
    {
        using ChurchDAWMidiMapping::ControllerKey;
        using ChurchDAWMidiMapping::SuppressedMidiNoteLedger;
        using ChurchDAWMidiMapping::SuppressedNoteRelease;

        const ControllerKey key { "midi:keyboard-a", 1, 60, false };
        SuppressedMidiNoteLedger<256, 255> ledger;
        expect (ledger.tryAcquire (key) && ledger.tryAcquire (key),
                "two note ON debts were not reserved");
        expect (ledger.consumeRelease (key)
                    == SuppressedNoteRelease::stillSuppressed
                    && ledger.contains (key),
                "first OFF escaped while a second ON debt remained");
        expect (ledger.consumeRelease (key)
                    == SuppressedNoteRelease::releasedLast
                    && ! ledger.contains (key),
                "second OFF did not close the second ON debt");

        SuppressedMidiNoteLedger<256, 255> depthLedger;
        for (int i = 0; i < 255; ++i)
            expect (depthLedger.tryAcquire (key),
                    "valid ledger depth failed before its fixed limit");
        expect (! depthLedger.tryAcquire (key)
                    && ! depthLedger.contains (key)
                    && ! depthLedger.shouldSuppressCurrentGesture (key)
                    && depthLedger.consumeRelease (key)
                        == SuppressedNoteRelease::notSuppressed
                    && ! depthLedger.tryAcquire (key),
                "depth-256 ON or its matching OFF did not fail open");
        depthLedger.clear();
        expect (depthLedger.tryAcquire (key)
                    && depthLedger.contains (key),
                "transport reset did not clear depth fail-open latch");

        SuppressedMidiNoteLedger<256, 255> capacityLedger;
        for (int i = 0; i < 256; ++i)
            expect (capacityLedger.tryAcquire ({
                        "midi:source-" + juce::String (i),
                        1 + (i % 16), i % 128, false }),
                    "fixed ledger filled before 256 independent keys");
        const ControllerKey firstKey { "midi:source-0", 1, 0, false };
        const ControllerKey key257 { "midi:source-257", 1, 1, false };
        expect (! capacityLedger.tryAcquire (key257)
                    && capacityLedger.size() == 256
                    && capacityLedger.failedOpenForCapacity()
                    && ! capacityLedger.contains (firstKey),
                "257th independent key did not fail open");
        expect (capacityLedger.consumeRelease (firstKey)
                    == SuppressedNoteRelease::notSuppressed
                    && ! capacityLedger.tryAcquire (key257)
                    && capacityLedger.consumeRelease (key257)
                        == SuppressedNoteRelease::notSuppressed,
                "capacity fail-open allowed a later debt to swallow raw OFF");
        capacityLedger.clear();
        expect (! capacityLedger.failedOpenForCapacity()
                    && capacityLedger.tryAcquire (key257),
                "transport reset did not clear capacity fail-open latch");

        SuppressedMidiNoteLedger<256, 255> identities;
        const ControllerKey sourceAChannel1 {
            "midi:source-a", 1, 64, false };
        const ControllerKey sourceBChannel1 {
            "midi:source-b", 1, 64, false };
        const ControllerKey sourceAChannel2 {
            "midi:source-a", 2, 64, false };
        expect (identities.tryAcquire (sourceAChannel1)
                    && identities.tryAcquire (sourceBChannel1)
                    && identities.tryAcquire (sourceAChannel2),
                "source/channel-independent debts collided");
        identities.clearSourceChannel ("midi:source-a", 1);
        expect (! identities.contains (sourceAChannel1)
                    && identities.contains (sourceBChannel1)
                    && identities.contains (sourceAChannel2),
                "source/channel reset cleared an unrelated debt");

        // Structural clear/import commit and rollback operate on separate map
        // snapshots; neither is allowed to reset the live gesture ledger.
        std::map<ControllerKey, int> structuralMap;
        structuralMap[key] = 1;
        expect (identities.tryAcquire (key),
                "could not seed live debt before structural transactions");
        structuralMap.clear();                    // clear + commit
        structuralMap[key257] = 2;                // import + rollback
        structuralMap.clear();
        expect (identities.contains (key),
                "clear/import transaction altered the live note ledger");
    }

    // F) The CC/note pair is one crash-safe, interprocess transaction. Its
    // journal contains exact binary length + SHA-256 snapshots, rollback is
    // all-or-none, and stale long-lived app instances fail CAS instead of
    // overwriting a newer map.
    {
        const auto directory = juce::File::getSpecialLocation (
            juce::File::tempDirectory).getNonexistentChildFile (
                "churchdaw-midi-pair-rollback", {}, false);
        directory.createDirectory();
        const auto cc = directory.getChildFile ("cc.xml");
        const auto note = directory.getChildFile ("note.xml");
        const auto ccLastGood = directory.getChildFile ("cc.xml.last-good");
        const auto noteLastGood = directory.getChildFile ("note.xml.last-good");

        expect (cc.replaceWithText ("old-cc")
                    && note.replaceWithText ("old-note")
                    && ccLastGood.replaceWithText ("old-cc-last-good")
                    && noteLastGood.replaceWithText ("old-note-last-good"),
                "could not prepare pair rollback fixtures");

        ChurchDAWMidiMapping::MidiMapPairRollback rollback (cc, note);
        expect (rollback.capture ("test"),
                "could not capture complete MIDI map pair");
        const auto liveJournal = pairJournalFor (cc);
        const auto liveJournalXml = juce::parseXML (liveJournal);
        const auto transactionId = pendingTransactionIdFor (cc);
        bool journalFilesAreChecksummed = liveJournalXml != nullptr
            && liveJournalXml->getNumChildElements() == 4;
        if (liveJournalXml != nullptr)
            for (auto* file : liveJournalXml->getChildIterator())
                journalFilesAreChecksummed = journalFilesAreChecksummed
                    && file->hasAttribute ("byteSize")
                    && file->getStringAttribute ("sha256").length() == 64
                    && file->getChildByName ("CONTENTS") != nullptr;
        expect (liveJournalXml != nullptr
                    && liveJournalXml->getIntAttribute ("version") == 3
                    && transactionId.isNotEmpty()
                    && journalFilesAreChecksummed
                    && ! ChurchDAWMidiMapping::midiMapPairJournalAllowsWrite (
                        cc, {})
                    && ChurchDAWMidiMapping::midiMapPairJournalAllowsWrite (
                        cc, transactionId)
                    && ! ChurchDAWMidiMapping::midiMapPairJournalAllowsWrite (
                        cc, "another-transaction"),
                "v3 pair journal did not publish exact guarded snapshots");

        // A foreign writer may replace the fixed journal path after capture.
        // Even a valid committed marker is not proof that THIS active
        // transaction can begin mutating its quartet: commit/restore would no
        // longer own an exact checkpoint. Standalone access may regard it as
        // closed, then the original pending journal is restored for this test.
        const auto liveJournalText = liveJournal.loadFileAsString();
        auto foreignCommittedJournal = juce::parseXML (liveJournalText);
        expect (foreignCommittedJournal != nullptr,
                "could not clone live journal for foreign-owner test");
        if (foreignCommittedJournal != nullptr)
        {
            foreignCommittedJournal->setAttribute (
                "transactionId", "foreign-committed-owner");
            foreignCommittedJournal->setAttribute ("state", "committed");
            const bool installedForeignCommitted =
                ChurchDAWMidiMapping::replaceFileWithTextAtomically (
                    liveJournal,
                    foreignCommittedJournal->toString (
                        juce::XmlElement::TextFormat().singleLine()));
            const bool rejectedForActiveTransaction =
                installedForeignCommitted
                && ! ChurchDAWMidiMapping::midiMapPairJournalAllowsWrite (
                    cc, transactionId);
            const bool acceptedAsClosedForStandalone =
                installedForeignCommitted
                && ChurchDAWMidiMapping::midiMapPairJournalAllowsWrite (
                    cc, {});
            const bool restoredOriginalPending =
                ChurchDAWMidiMapping::replaceFileWithTextAtomically (
                    liveJournal, liveJournalText);
            expect (installedForeignCommitted
                        && rejectedForActiveTransaction
                        && acceptedAsClosedForStandalone
                        && restoredOriginalPending
                        && ChurchDAWMidiMapping::midiMapPairJournalAllowsWrite (
                            cc, transactionId),
                    "foreign committed journal authorised an active half-save");
        }
        expect (replaceWithOwnedMap (cc, true, transactionId, "new-cc")
                    && replaceWithOwnedMap (
                        note, false, transactionId, "new-note")
                    && replaceWithOwnedMap (
                        ccLastGood, true, transactionId, "new-cc-last-good")
                    && replaceWithOwnedMap (
                        noteLastGood, false, transactionId,
                        "new-note-last-good"),
                "could not mutate complete owned MIDI map pair");
        expect (! rollback.targetsUnchangedSinceCapture(),
                "pair transaction missed a target changed after checkpoint");
        expect (rollback.restore()
                    && cc.loadFileAsString() == "old-cc"
                    && note.loadFileAsString() == "old-note"
                    && ccLastGood.loadFileAsString() == "old-cc-last-good"
                    && noteLastGood.loadFileAsString() == "old-note-last-good",
                "pair rollback did not restore all four exact MIDI files");

        // Exact means bytes, not UTF-8 text. Exercise embedded NUL and invalid
        // UTF-8 in all four targets so journal recovery cannot silently use a
        // String conversion that truncates or normalises the snapshot.
        const auto binaryCC = directory.getChildFile ("binary-cc.xml");
        const auto binaryNote = directory.getChildFile ("binary-note.xml");
        const auto binaryCCLastGood = binaryCC.getSiblingFile (
            binaryCC.getFileName() + ".last-good");
        const auto binaryNoteLastGood = binaryNote.getSiblingFile (
            binaryNote.getFileName() + ".last-good");
        const unsigned char binaryBytesA[] {
            0x00, 0xff, 0xc3, 0x28, 0x41, 0x00, 0x80 };
        const unsigned char binaryBytesB[] {
            0xfe, 0x00, 0xf0, 0x28, 0x8c, 0xbc, 0x42 };
        const juce::MemoryBlock expectedBinaryA (
            binaryBytesA, sizeof (binaryBytesA));
        const juce::MemoryBlock expectedBinaryB (
            binaryBytesB, sizeof (binaryBytesB));
        expect (ChurchDAWMidiMapping::replaceFileWithDataAtomically (
                    binaryCC, expectedBinaryA)
                    && ChurchDAWMidiMapping::replaceFileWithDataAtomically (
                        binaryNote, expectedBinaryB)
                    && ChurchDAWMidiMapping::replaceFileWithDataAtomically (
                        binaryCCLastGood, expectedBinaryB)
                    && ChurchDAWMidiMapping::replaceFileWithDataAtomically (
                        binaryNoteLastGood, expectedBinaryA),
                "could not prepare binary pair fixtures");
        ChurchDAWMidiMapping::MidiMapPairRollback binaryRollback (
            binaryCC, binaryNote);
        expect (binaryRollback.capture ("binary-exact"),
                "could not checkpoint binary pair");
        const auto binaryTransactionId = pendingTransactionIdFor (
            binaryCC);
        expect (replaceWithOwnedMap (
                    binaryCC, true, binaryTransactionId, "binary-new-cc")
                    && replaceWithOwnedMap (
                        binaryNote, false, binaryTransactionId,
                        "binary-new-note")
                    && replaceWithOwnedMap (
                        binaryCCLastGood, true, binaryTransactionId,
                        "binary-new-cc-last-good")
                    && replaceWithOwnedMap (
                        binaryNoteLastGood, false, binaryTransactionId,
                        "binary-new-note-last-good")
                    && binaryRollback.restore(),
                "binary pair rollback failed");
        juce::MemoryBlock actualBinaryCC;
        juce::MemoryBlock actualBinaryNote;
        juce::MemoryBlock actualBinaryCCLastGood;
        juce::MemoryBlock actualBinaryNoteLastGood;
        expect (ChurchDAWMidiMapping::loadStableMidiMapFileData (
                    binaryCC, actualBinaryCC)
                    && ChurchDAWMidiMapping::loadStableMidiMapFileData (
                        binaryNote, actualBinaryNote)
                    && ChurchDAWMidiMapping::loadStableMidiMapFileData (
                        binaryCCLastGood, actualBinaryCCLastGood)
                    && ChurchDAWMidiMapping::loadStableMidiMapFileData (
                        binaryNoteLastGood, actualBinaryNoteLastGood)
                    && actualBinaryCC == expectedBinaryA
                    && actualBinaryNote == expectedBinaryB
                    && actualBinaryCCLastGood == expectedBinaryB
                    && actualBinaryNoteLastGood == expectedBinaryA,
                "binary rollback changed NUL/invalid UTF-8 bytes");

        // One shared lock also rejects nested transactions in this process.
        {
            ChurchDAWMidiMapping::MidiMapPairFileLock sharedLock (cc, note);
            ChurchDAWMidiMapping::MidiMapPairRollback first (
                cc, note, sharedLock);
            ChurchDAWMidiMapping::MidiMapPairRollback overlapping (
                cc, note, sharedLock);
            expect (first.capture ("first-shared")
                        && ! overlapping.capture ("overlap-shared")
                        && first.commit()
                        && overlapping.capture ("after-first")
                        && overlapping.commit(),
                    "shared pair lock allowed overlapping transactions or did not reopen");
        }


        // Distinct lock objects for the same pair must also serialize threads.
        // POSIX fcntl locks alone are process-wide and would otherwise let this
        // second object enter from the same process.
        {
            ChurchDAWMidiMapping::MidiMapPairFileLock firstObject (cc, note);
            ChurchDAWMidiMapping::MidiMapPairFileLock secondObject (cc, note);
            ChurchDAWMidiMapping::MidiMapPairFileLock::ScopedAccess firstAccess (
                firstObject, 1000);
            std::atomic<bool> secondEntered { true };
            std::thread contender ([&]
            {
                ChurchDAWMidiMapping::MidiMapPairFileLock::ScopedAccess access (
                    secondObject, 200);
                secondEntered.store (access.isLocked());
            });
            contender.join();
            expect (firstAccess.isLocked() && ! secondEntered.load(),
                    "distinct same-process pair locks overlapped");
        }

        // This launches the test binary as a distinct OS process. Recovery +
        // both startup reads can therefore hold one real interprocess lock.
        const auto executable = juce::File::getSpecialLocation (
            juce::File::currentExecutableFile);
        const auto runLockProbe = [&] ()
        {
            juce::StringArray arguments;
            arguments.add (executable.getFullPathName());
            arguments.add ("--probe-midi-map-pair-lock");
            arguments.add (cc.getFullPathName());
            arguments.add (note.getFullPathName());
            juce::ChildProcess child;
            expect (child.start (arguments),
                    "could not launch interprocess MIDI lock probe");
            expect (child.waitForProcessToFinish (5000),
                    "interprocess MIDI lock probe did not finish");
            return child.getExitCode();
        };
        {
            ChurchDAWMidiMapping::MidiMapPairFileLock startupLock (cc, note);
            ChurchDAWMidiMapping::MidiMapPairFileLock::ScopedAccess startup (
                startupLock, 1000);
            expect (startup.isLocked() && runLockProbe() == 23,
                    "another process entered during the startup pair snapshot");
        }
        expect (runLockProbe() == 0,
                "interprocess pair lock did not reopen after startup snapshot");

       #if JUCE_ANDROID || JUCE_MAC || JUCE_LINUX || JUCE_BSD
        // This is the exact explicit-path fcntl backend selected on Android.
        // Run it on desktop too so the sandbox-specific implementation is not
        // merely hidden behind an Android compile branch.
        const auto sandboxLockFile =
            ChurchDAWMidiMapping::midiMapPairLockFile (cc, note);
        const auto runSandboxLockProbe = [&] ()
        {
            juce::StringArray arguments;
            arguments.add (executable.getFullPathName());
            arguments.add ("--probe-midi-map-sandbox-lock");
            arguments.add (sandboxLockFile.getFullPathName());
            juce::ChildProcess child;
            expect (child.start (arguments),
                    "could not launch sandbox lock probe");
            expect (child.waitForProcessToFinish (5000),
                    "sandbox lock probe did not finish");
            return child.getExitCode();
        };
        expect (sandboxLockFile.getParentDirectory()
                    == cc.getParentDirectory(),
                "Android pair lock escaped the app-private map directory");
        {
            ChurchDAWMidiMapping::MidiMapSandboxFileLock sandboxLock (
                sandboxLockFile);
            expect (sandboxLock.enter (1000)
                        && runSandboxLockProbe() == 23,
                    "sandbox fcntl lock allowed a second process");
            sandboxLock.exit();
        }
        expect (runSandboxLockProbe() == 0,
                "sandbox fcntl lock did not reopen after release");
       #endif

        // Force commit() itself to fail by replacing its journal file with a
        // directory. The transaction retains the lock until restore() is
        // attempted, but restore must reject a missing/foreign/corrupt journal
        // without touching any target.
        {
            const auto ccBeforeFailedCommit = cc.loadFileAsString();
            const auto noteBeforeFailedCommit = note.loadFileAsString();
            ChurchDAWMidiMapping::MidiMapPairFileLock failedCommitLock (
                cc, note);
            ChurchDAWMidiMapping::MidiMapPairRollback failedCommit (
                cc, note, failedCommitLock);
            ChurchDAWMidiMapping::MidiMapPairRollback overlapping (
                cc, note, failedCommitLock);
            expect (failedCommit.capture ("forced-commit-failure"),
                    "could not checkpoint forced commit failure");
            const auto failedCommitId = pendingTransactionIdFor (cc);
            const auto journalAsDirectory = pairJournalFor (cc);
            const auto externallyMutatedCC = ownedMapText (
                true, failedCommitId, "before-failed-commit");
            expect (replaceWithOwnedMap (
                        cc, true, failedCommitId, "before-failed-commit")
                        && journalAsDirectory.deleteFile()
                        && journalAsDirectory.createDirectory()
                        && ! failedCommit.commit()
                        && runLockProbe() == 23
                        && ! overlapping.capture ("transaction-b"),
                    "failed commit released its transaction lock");
            expect (journalAsDirectory.deleteRecursively()
                        && ! failedCommit.restore()
                        && cc.loadFileAsString() == externallyMutatedCC
                        && note.loadFileAsString() == noteBeforeFailedCommit,
                    "restore touched targets without its exact journal");
            expect (cc.replaceWithText (ccBeforeFailedCommit),
                    "could not reset fixture after foreign-journal rejection");
        }
        expect (runLockProbe() == 0,
                "failed-commit restore did not release the pair lock");

        // commit may have already published a durable committed marker before
        // a later verification fails. restore must re-arm that exact journal
        // to pending before target 1, so a crash halfway through rollback is
        // recoverable on the next startup.
        {
            const auto rearmCC = directory.getChildFile ("rearm-cc.xml");
            const auto rearmNote = directory.getChildFile ("rearm-note.xml");
            const auto rearmCCBackup = rearmCC.getSiblingFile (
                rearmCC.getFileName() + ".last-good");
            const auto rearmNoteBackup = rearmNote.getSiblingFile (
                rearmNote.getFileName() + ".last-good");
            expect (rearmCC.replaceWithText ("rearm-old-cc")
                        && rearmNote.replaceWithText ("rearm-old-note")
                        && rearmCCBackup.replaceWithText (
                            "rearm-old-cc-backup")
                        && rearmNoteBackup.replaceWithText (
                            "rearm-old-note-backup"),
                    "could not prepare restore re-arm fixtures");

            {
                ChurchDAWMidiMapping::MidiMapPairRollback rearm (
                    rearmCC, rearmNote);
                expect (rearm.capture ("restore-rearm"),
                        "could not capture restore re-arm transaction");
                const auto owner = pendingTransactionIdFor (rearmCC);
                expect (replaceWithOwnedMap (
                            rearmCC, true, owner, "rearm-new-cc")
                            && replaceWithOwnedMap (
                                rearmNote, false, owner, "rearm-new-note")
                            && replaceWithOwnedMap (
                                rearmCCBackup, true, owner,
                                "rearm-new-cc-backup")
                            && replaceWithOwnedMap (
                                rearmNoteBackup, false, owner,
                                "rearm-new-note-backup"),
                        "could not mutate restore re-arm quartet");

                ChurchDAWMidiMapping::MidiMapPairRollback::
                    failCommitAfterCommittedJournalForTests (true);
                expect (! rearm.commit()
                            && journalStateFor (rearmCC) == "committed",
                        "commit failpoint did not leave committed marker");
                ChurchDAWMidiMapping::MidiMapPairRollback::
                    failRestoreAfterTargetCountForTests (1);
                expect (! rearm.restore()
                            && journalStateFor (rearmCC) == "pending",
                        "partial restore was not protected by a pending journal");
            }

            expect (ChurchDAWMidiMapping::MidiMapPairRollback::
                        recoverPendingTransaction (rearmCC, rearmNote)
                        && rearmCC.loadFileAsString() == "rearm-old-cc"
                        && rearmNote.loadFileAsString() == "rearm-old-note"
                        && rearmCCBackup.loadFileAsString()
                            == "rearm-old-cc-backup"
                        && rearmNoteBackup.loadFileAsString()
                            == "rearm-old-note-backup"
                        && journalStateFor (rearmCC) == "committed",
                    "startup did not finish a partially interrupted restore");
        }

        // A legacy/editor writer can ignore the pair lock and publish after
        // restore's quartet preflight. Re-check the individual target at the
        // last portable point before replace/delete: the foreign first target
        // must survive and no earlier target in the reverse loop may change.
        {
            const auto raceCC = directory.getChildFile (
                "restore-race-cc.xml");
            const auto raceNote = directory.getChildFile (
                "restore-race-note.xml");
            const auto raceCCBackup = raceCC.getSiblingFile (
                raceCC.getFileName() + ".last-good");
            const auto raceNoteBackup = raceNote.getSiblingFile (
                raceNote.getFileName() + ".last-good");
            expect (raceCC.replaceWithText ("race-old-cc")
                        && raceNote.replaceWithText ("race-old-note")
                        && raceCCBackup.replaceWithText (
                            "race-old-cc-backup")
                        && raceNoteBackup.replaceWithText (
                            "race-old-note-backup"),
                    "could not prepare restore race fixtures");

            // A committed v1 journal is a valid closed compatibility marker,
            // but its ownerless semantics must not leak into the brand-new v3
            // transaction captured immediately afterward.
            juce::XmlElement committedV1 (
                "CHURCHDAW_MIDI_MAP_PAIR_TRANSACTION");
            committedV1.setAttribute ("version", 1);
            committedV1.setAttribute ("state", "committed");
            const std::array<juce::String, 4> raceOldContents {{
                "race-old-cc", "race-old-note",
                "race-old-cc-backup", "race-old-note-backup" }};
            for (size_t i = 0; i < raceOldContents.size(); ++i)
            {
                auto* file = committedV1.createNewChildElement ("FILE");
                file->setAttribute ("index", (int) i);
                file->setAttribute ("existed", 1);
                file->createNewChildElement ("CONTENTS")
                    ->addTextElement (raceOldContents[i]);
            }
            expect (ChurchDAWMidiMapping::replaceFileWithTextAtomically (
                        pairJournalFor (raceCC),
                        committedV1.toString (
                            juce::XmlElement::TextFormat().singleLine())),
                    "could not publish committed v1 restore race journal");

            ChurchDAWMidiMapping::MidiMapPairRollback race (
                raceCC, raceNote);
            expect (race.capture ("restore-foreign-after-preflight"),
                    "could not capture restore race transaction");
            const auto owner = pendingTransactionIdFor (raceCC);
            const auto racePendingJournal = juce::parseXML (
                pairJournalFor (raceCC));
            expect (racePendingJournal != nullptr
                        && racePendingJournal->getIntAttribute (
                            "version") == 3
                        && owner.isNotEmpty()
                        && replaceWithOwnedMap (
                        raceCC, true, owner, "race-new-cc")
                        && replaceWithOwnedMap (
                            raceNote, false, owner, "race-new-note")
                        && replaceWithOwnedMap (
                            raceCCBackup, true, owner,
                            "race-new-cc-backup")
                        && replaceWithOwnedMap (
                            raceNoteBackup, false, owner,
                            "race-new-note-backup"),
                    "could not publish owned restore race quartet");

            ChurchDAWMidiMapping::MidiMapPairRollback::
                failRestoreWithForeignTargetAfterPreflightForTests (3);
            expect (! race.restore()
                        && journalStateFor (raceCC) == "pending",
                    "foreign post-preflight target did not fail restore closed");
            const auto foreign =
                ChurchDAWMidiMapping::loadBoundedXmlFile (
                    raceNoteBackup);
            const auto untouchedCC =
                ChurchDAWMidiMapping::loadBoundedXmlFile (raceCC);
            const auto untouchedNote =
                ChurchDAWMidiMapping::loadBoundedXmlFile (raceNote);
            const auto untouchedCCBackup =
                ChurchDAWMidiMapping::loadBoundedXmlFile (
                    raceCCBackup);
            expect (foreign != nullptr
                        && foreign->getStringAttribute ("transactionId")
                            == "foreign-after-restore-preflight"
                        && foreign->getStringAttribute ("testPayload")
                            == "must-be-preserved"
                        && untouchedCC != nullptr
                        && untouchedCC->getStringAttribute ("transactionId")
                            == owner
                        && untouchedNote != nullptr
                        && untouchedNote->getStringAttribute ("transactionId")
                            == owner
                        && untouchedCCBackup != nullptr
                        && untouchedCCBackup->getStringAttribute (
                            "transactionId") == owner,
                    "restore overwrote foreign target or changed a partial prefix");

            expect (replaceWithOwnedMap (
                        raceNoteBackup, false, owner,
                        "race-new-note-backup")
                        && ChurchDAWMidiMapping::MidiMapPairRollback::
                            recoverPendingTransaction (raceCC, raceNote)
                        && raceCC.loadFileAsString() == "race-old-cc"
                        && raceNote.loadFileAsString() == "race-old-note"
                        && raceCCBackup.loadFileAsString()
                            == "race-old-cc-backup"
                        && raceNoteBackup.loadFileAsString()
                            == "race-old-note-backup"
                        && journalStateFor (raceCC) == "committed",
                    "recovery did not finish after foreign race was resolved");
        }

        // A directory at the fixed journal path is not "no journal". Startup
        // must fail closed before either map restore is allowed to run.
        {
            const auto nonregularCC = directory.getChildFile (
                "nonregular-journal-cc.xml");
            const auto nonregularNote = directory.getChildFile (
                "nonregular-journal-note.xml");
            const auto nonregularJournal = pairJournalFor (nonregularCC);
            expect (nonregularCC.replaceWithText ("cc")
                        && nonregularNote.replaceWithText ("note")
                        && nonregularJournal.createDirectory()
                        && ! ChurchDAWMidiMapping::MidiMapPairRollback::recoverPendingTransaction (
                            nonregularCC, nonregularNote)
                        && nonregularJournal.deleteRecursively(),
                    "non-regular journal path was treated as no journal");
        }

        // Pair-wide schema ownership: a future NOTE blocks an otherwise-current
        // CC transaction before journal creation or any target mutation.
        {
            const auto schemaCC = directory.getChildFile (
                "pair-schema-cc.xml");
            const auto schemaNote = directory.getChildFile (
                "pair-schema-note.xml");
            const auto currentCC = juce::String (
                "<MIDIMAP schema=\"1\" complete=\"1\" entryCount=\"0\" explicitEmpty=\"1\"/>");
            const auto futureNote = juce::String (
                "<NOTEMAP schema=\"2\" complete=\"1\" entryCount=\"0\" explicitEmpty=\"1\"/>");
            expect (schemaCC.replaceWithText (currentCC)
                        && schemaNote.replaceWithText (futureNote),
                    "could not prepare pair-wide future schema fixtures");
            ChurchDAWMidiMapping::MidiMapPairRollback futurePreflight (
                schemaCC, schemaNote);
            expect (! futurePreflight.capture ("opposite-future-half")
                        && futurePreflight.hadFutureSchemaConflict()
                        && schemaCC.loadFileAsString() == currentCC
                        && schemaNote.loadFileAsString() == futureNote
                        && ! pairJournalFor (schemaCC).exists(),
                    "future NOTE did not block the complete pair before mutation");
        }

        const auto absentCC = directory.getChildFile ("absent-cc.xml");
        const auto absentNote = directory.getChildFile ("absent-note.xml");
        const auto absentCCLastGood = directory.getChildFile (
            "absent-cc.xml.last-good");
        const auto absentNoteLastGood = directory.getChildFile (
            "absent-note.xml.last-good");
        ChurchDAWMidiMapping::MidiMapPairRollback absentRollback (
            absentCC, absentNote);
        expect (absentRollback.capture ("test-absent"),
                "could not capture absent MIDI map pair");
        const auto absentId = pendingTransactionIdFor (absentCC);
        expect (replaceWithOwnedMap (
                    absentCC, true, absentId, "created-cc")
                    && replaceWithOwnedMap (
                        absentNote, false, absentId, "created-note")
                    && replaceWithOwnedMap (
                        absentCCLastGood, true, absentId,
                        "created-cc-last-good")
                    && replaceWithOwnedMap (
                        absentNoteLastGood, false, absentId,
                        "created-note-last-good"),
                "could not create absent owned MIDI map pair fixtures");
        ChurchDAWMidiMapping::MidiMapPairRollback::
            failRestoreAfterTargetCountForTests (1);
        expect (! absentRollback.restore()
                    && journalStateFor (absentCC) == "pending",
                "partial removal of originally-absent targets closed its journal");
        expect (ChurchDAWMidiMapping::MidiMapPairRollback::
                    recoverPendingTransaction (absentCC, absentNote)
                    && ! absentCC.exists()
                    && ! absentNote.exists()
                    && ! absentCCLastGood.exists()
                    && ! absentNoteLastGood.exists()
                    && journalStateFor (absentCC) == "committed",
                "startup did not durably finish originally-absent targets");

        // Saving promotes the captured primary to last-good before replacing
        // the primary. If the replacement then fails, that promoted old XML is
        // ours even though it carries an older transaction ID.
        const auto promotionCC = directory.getChildFile ("promotion-cc.xml");
        const auto promotionNote = directory.getChildFile (
            "promotion-note.xml");
        const auto promotionCCLastGood = directory.getChildFile (
            "promotion-cc.xml.last-good");
        const auto promotionNoteLastGood = directory.getChildFile (
            "promotion-note.xml.last-good");
        expect (promotionCC.replaceWithText ("promotion-primary-cc")
                    && promotionNote.replaceWithText (
                        "promotion-primary-note")
                    && promotionCCLastGood.replaceWithText (
                        "promotion-older-cc-backup")
                    && promotionNoteLastGood.replaceWithText (
                        "promotion-older-note-backup"),
                "could not prepare last-good promotion fixtures");
        ChurchDAWMidiMapping::MidiMapPairRollback promotionRollback (
            promotionCC, promotionNote);
        expect (promotionRollback.capture ("promotion-before-failure")
                    && ChurchDAWMidiMapping::atomicCopyFile (
                        promotionCC, promotionCCLastGood)
                    && ChurchDAWMidiMapping::atomicCopyFile (
                        promotionNote, promotionNoteLastGood)
                    && promotionRollback.restore()
                    && promotionCCLastGood.loadFileAsString()
                        == "promotion-older-cc-backup"
                    && promotionNoteLastGood.loadFileAsString()
                        == "promotion-older-note-backup",
                "rollback treated its own primary-to-last-good promotion as foreign");

        expect (cc.replaceWithText ("crash-old-cc")
                    && note.replaceWithText ("crash-old-note")
                    && ccLastGood.replaceWithText ("crash-old-cc-last-good")
                    && noteLastGood.replaceWithText ("crash-old-note-last-good"),
                "could not prepare durable journal fixtures");
        {
            ChurchDAWMidiMapping::MidiMapPairRollback interrupted (cc, note);
            expect (interrupted.capture ("before-crash"),
                    "could not create durable pair journal");
            const auto ownId = pendingTransactionIdFor (cc);
            expect (replaceWithOwnedMap (
                        cc, true, ownId, "half-new-cc")
                        && replaceWithOwnedMap (
                            ccLastGood, true, ownId,
                            "half-new-cc-last-good"),
                    "could not simulate interrupted pair write");
            // No commit/restore: destruction simulates process termination.
        }
        expect (ChurchDAWMidiMapping::MidiMapPairRollback::recoverPendingTransaction (
                    cc, note)
                    && cc.loadFileAsString() == "crash-old-cc"
                    && note.loadFileAsString() == "crash-old-note"
                    && ccLastGood.loadFileAsString() == "crash-old-cc-last-good"
                    && noteLastGood.loadFileAsString() == "crash-old-note-last-good",
                "startup recovery did not roll back an interrupted pair write");

        // If even one target belongs to a future/foreign writer, recovery
        // touches none of the quartet and keeps the journal as a visible block.
        const auto futureCrashCC = directory.getChildFile (
            "future-crash-cc.xml");
        const auto futureCrashNote = directory.getChildFile (
            "future-crash-note.xml");
        const auto futureCrashCCLastGood = directory.getChildFile (
            "future-crash-cc.xml.last-good");
        const auto futureCrashNoteLastGood = directory.getChildFile (
            "future-crash-note.xml.last-good");
        expect (futureCrashCC.replaceWithText ("future-old-cc")
                    && futureCrashNote.replaceWithText ("future-old-note")
                    && futureCrashCCLastGood.replaceWithText (
                        "future-old-cc-last-good")
                    && futureCrashNoteLastGood.replaceWithText (
                        "future-old-note-last-good"),
                "could not prepare all-or-none future recovery fixtures");
        const juce::String futureCrashCCXml (
            "<MIDIMAP schema=\"2\" transactionId=\"future-writer\"/>");
        juce::String futureOwnedNoteXml;
        {
            ChurchDAWMidiMapping::MidiMapPairRollback interruptedFuture (
                futureCrashCC, futureCrashNote);
            expect (interruptedFuture.capture ("before-future-crash"),
                    "could not checkpoint future-writer recovery");
            const auto ownId = pendingTransactionIdFor (futureCrashCC);
            futureOwnedNoteXml = ownedMapText (
                false, ownId, "own-interrupted-note");
            expect (ownId.isNotEmpty()
                        && futureCrashCC.replaceWithText (futureCrashCCXml)
                        && futureCrashNote.replaceWithText (futureOwnedNoteXml),
                    "could not simulate future writer during interrupted save");
        }
        const auto futureJournal = pairJournalFor (futureCrashCC);
        expect (! ChurchDAWMidiMapping::MidiMapPairRollback::recoverPendingTransaction (
                    futureCrashCC, futureCrashNote)
                    && futureCrashCC.loadFileAsString() == futureCrashCCXml
                    && futureCrashNote.loadFileAsString() == futureOwnedNoteXml
                    && futureCrashCCLastGood.loadFileAsString()
                        == "future-old-cc-last-good"
                    && futureCrashNoteLastGood.loadFileAsString()
                        == "future-old-note-last-good"
                    && futureJournal.existsAsFile(),
                "future writer caused selective rollback or false recovery");
        expect (futureJournal.deleteFile(),
                "could not clean blocked future-writer journal fixture");

        const auto foreignCC = directory.getChildFile ("foreign-cc.xml");
        const auto foreignNote = directory.getChildFile ("foreign-note.xml");
        expect (foreignCC.replaceWithText ("foreign-old-cc")
                    && foreignNote.replaceWithText ("foreign-old-note"),
                "could not prepare foreign-writer fixtures");
        ChurchDAWMidiMapping::MidiMapPairRollback foreignTransaction (
            foreignCC, foreignNote);
        expect (foreignTransaction.capture ("before-foreign-writer"),
                "could not checkpoint foreign-writer transaction");
        const auto ownedId = pendingTransactionIdFor (foreignCC);
        const juce::String foreignCCXml (
            "<MIDIMAP schema=\"1\" transactionId=\"foreign-id\"/>");
        const auto foreignOwnedNote = ownedMapText (
            false, ownedId, "own-interrupted-note");
        expect (ownedId.isNotEmpty()
                    && foreignCC.replaceWithText (foreignCCXml)
                    && foreignNote.replaceWithText (foreignOwnedNote)
                    && ! foreignTransaction.restore()
                    && foreignCC.loadFileAsString() == foreignCCXml
                    && foreignNote.loadFileAsString() == foreignOwnedNote
                    && pairJournalFor (foreignCC).existsAsFile(),
                "foreign transaction caused a mixed rollback or false success");
        expect (pairJournalFor (foreignCC).deleteFile(),
                "could not clean blocked foreign-writer journal fixture");

        ChurchDAWMidiMapping::MidiMapPairRollback committed (cc, note);
        expect (committed.capture ("before-commit"),
                "could not checkpoint a complete committed pair");
        const auto committedId = pendingTransactionIdFor (cc);
        const auto committedCC = ownedMapText (
            true, committedId, "committed-cc");
        const auto committedNote = ownedMapText (
            false, committedId, "committed-note");
        const auto committedCCLastGood = ownedMapText (
            true, committedId, "committed-cc-last-good");
        const auto committedNoteLastGood = ownedMapText (
            false, committedId, "committed-note-last-good");
        expect (cc.replaceWithText (committedCC)
                    && note.replaceWithText (committedNote)
                    && ccLastGood.replaceWithText (committedCCLastGood)
                    && noteLastGood.replaceWithText (committedNoteLastGood)
                    && committed.commit(),
                "could not durably commit a complete MIDI map pair");
        expect (ChurchDAWMidiMapping::MidiMapPairRollback::recoverPendingTransaction (
                    cc, note)
                    && cc.loadFileAsString() == committedCC
                    && note.loadFileAsString() == committedNote
                    && ccLastGood.loadFileAsString() == committedCCLastGood
                    && noteLastGood.loadFileAsString()
                        == committedNoteLastGood,
                "committed journal was incorrectly rolled back");

        ChurchDAWMidiMapping::MidiMapPairRollback futureWriter (cc, note);
        const juce::String futureCC (
            "<MIDIMAP schema=\"2\" transactionId=\"future-writer\"/>");
        const auto noteBeforeFutureWriter = note.loadFileAsString();
        expect (futureWriter.capture ("before-future-writer")
                    && cc.replaceWithText (futureCC)
                    && ! ChurchDAWMidiMapping::rollbackMidiMapPairPreservingFutureSchema (
                        futureWriter, cc, note)
                    && cc.loadFileAsString() == futureCC
                    && note.loadFileAsString() == noteBeforeFutureWriter
                    && pairJournalFor (cc).existsAsFile(),
                "rollback overwrote a future schema or reported false success");
        expect (pairJournalFor (cc).deleteFile(),
                "could not clean blocked future schema journal fixture");

        const auto crashAbsentCC = directory.getChildFile (
            "crash-absent-cc.xml");
        const auto crashAbsentNote = directory.getChildFile (
            "crash-absent-note.xml");
        const auto crashAbsentCCLastGood = directory.getChildFile (
            "crash-absent-cc.xml.last-good");
        const auto crashAbsentNoteLastGood = directory.getChildFile (
            "crash-absent-note.xml.last-good");
        {
            ChurchDAWMidiMapping::MidiMapPairRollback interruptedAbsent (
                crashAbsentCC, crashAbsentNote);
            expect (interruptedAbsent.capture ("before-crash-absent"),
                    "could not checkpoint absent pair");
            const auto ownId = pendingTransactionIdFor (crashAbsentCC);
            expect (replaceWithOwnedMap (
                        crashAbsentCC, true, ownId, "created-cc")
                        && replaceWithOwnedMap (
                            crashAbsentNote, false, ownId, "created-note")
                        && replaceWithOwnedMap (
                            crashAbsentCCLastGood, true, ownId,
                            "created-cc-last-good")
                        && replaceWithOwnedMap (
                            crashAbsentNoteLastGood, false, ownId,
                            "created-note-last-good"),
                    "could not simulate interrupted absent-pair creation");
        }
        expect (ChurchDAWMidiMapping::MidiMapPairRollback::recoverPendingTransaction (
                    crashAbsentCC, crashAbsentNote)
                    && ! crashAbsentCC.exists()
                    && ! crashAbsentNote.exists()
                    && ! crashAbsentCCLastGood.exists()
                    && ! crashAbsentNoteLastGood.exists(),
                "startup recovery kept files created after an absent checkpoint");

        const auto emptyCC = directory.getChildFile ("empty-cc.xml");
        const auto emptyNote = directory.getChildFile ("empty-note.xml");
        const auto emptyCCLastGood = directory.getChildFile (
            "empty-cc.xml.last-good");
        const auto emptyNoteLastGood = directory.getChildFile (
            "empty-note.xml.last-good");
        expect (emptyCC.replaceWithText ({})
                    && emptyNote.replaceWithText ({})
                    && emptyCCLastGood.replaceWithText ({})
                    && emptyNoteLastGood.replaceWithText ({}),
                "could not prepare empty durable journal fixtures");
        {
            ChurchDAWMidiMapping::MidiMapPairRollback interruptedEmpty (
                emptyCC, emptyNote);
            expect (interruptedEmpty.capture ("before-crash-empty"),
                    "could not checkpoint empty pair");
            const auto ownId = pendingTransactionIdFor (emptyCC);
            expect (replaceWithOwnedMap (
                        emptyCC, true, ownId, "new-cc")
                        && replaceWithOwnedMap (
                            emptyNote, false, ownId, "new-note")
                        && replaceWithOwnedMap (
                            emptyCCLastGood, true, ownId,
                            "new-cc-last-good")
                        && replaceWithOwnedMap (
                            emptyNoteLastGood, false, ownId,
                            "new-note-last-good"),
                    "could not simulate interrupted replacement of empty pair");
        }
        expect (ChurchDAWMidiMapping::MidiMapPairRollback::recoverPendingTransaction (
                    emptyCC, emptyNote)
                    && emptyCC.existsAsFile() && emptyCC.getSize() == 0
                    && emptyNote.existsAsFile() && emptyNote.getSize() == 0
                    && emptyCCLastGood.existsAsFile()
                    && emptyCCLastGood.getSize() == 0
                    && emptyNoteLastGood.existsAsFile()
                    && emptyNoteLastGood.getSize() == 0,
                "startup recovery did not preserve existing empty files");

        // Backward-compatible recovery of the first durable journal format.
        const auto legacyCC = directory.getChildFile ("legacy-v1-cc.xml");
        const auto legacyNote = directory.getChildFile ("legacy-v1-note.xml");
        const auto legacyCCLastGood = directory.getChildFile (
            "legacy-v1-cc.xml.last-good");
        const auto legacyNoteLastGood = directory.getChildFile (
            "legacy-v1-note.xml.last-good");
        const std::array<juce::File, 4> legacyFiles {{
            legacyCC, legacyNote, legacyCCLastGood, legacyNoteLastGood }};
        const std::array<juce::String, 4> legacyContents {{
            "legacy-old-cc", "legacy-old-note",
            "legacy-old-cc-last-good", "legacy-old-note-last-good" }};
        juce::XmlElement legacyJournal (
            "CHURCHDAW_MIDI_MAP_PAIR_TRANSACTION");
        legacyJournal.setAttribute ("version", 1);
        legacyJournal.setAttribute ("state", "pending");
        for (size_t i = 0; i < legacyFiles.size(); ++i)
        {
            expect (legacyFiles[i].replaceWithText (legacyContents[i]),
                    "could not prepare v1 journal target");
            auto* file = legacyJournal.createNewChildElement ("FILE");
            file->setAttribute ("index", (int) i);
            file->setAttribute ("existed", 1);
            file->createNewChildElement ("CONTENTS")
                ->addTextElement (legacyContents[i]);
        }
        expect (ChurchDAWMidiMapping::replaceFileWithTextAtomically (
                    pairJournalFor (legacyCC),
                    legacyJournal.toString (
                        juce::XmlElement::TextFormat().singleLine())),
                "could not publish v1 compatibility journal");
        for (size_t i = 0; i < legacyFiles.size(); ++i)
            expect (replaceWithOwnedMap (
                        legacyFiles[i], i == 0 || i == 2, {},
                        "legacy-interrupted-" + juce::String ((int) i)),
                    "could not mutate v1 journal target");
        expect (ChurchDAWMidiMapping::MidiMapPairRollback::recoverPendingTransaction (
                    legacyCC, legacyNote),
                "v1 durable journal did not recover after update");
        for (size_t i = 0; i < legacyFiles.size(); ++i)
            expect (legacyFiles[i].loadFileAsString() == legacyContents[i],
                    "v1 journal did not restore exact legacy bytes");

        // Version 2 introduced transaction ownership while still storing
        // legacy text payloads. Clients may update directly from that build.
        const auto legacyV2CC = directory.getChildFile ("legacy-v2-cc.xml");
        const auto legacyV2Note = directory.getChildFile (
            "legacy-v2-note.xml");
        const auto legacyV2CCLastGood = legacyV2CC.getSiblingFile (
            legacyV2CC.getFileName() + ".last-good");
        const auto legacyV2NoteLastGood = legacyV2Note.getSiblingFile (
            legacyV2Note.getFileName() + ".last-good");
        const std::array<juce::File, 4> legacyV2Files {{
            legacyV2CC, legacyV2Note,
            legacyV2CCLastGood, legacyV2NoteLastGood }};
        const std::array<juce::String, 4> legacyV2Contents {{
            "v2-old-cc", "v2-old-note",
            "v2-old-cc-last-good", "v2-old-note-last-good" }};
        const juce::String legacyV2TransactionId = "legacy-v2-owner";
        juce::XmlElement legacyV2Journal (
            "CHURCHDAW_MIDI_MAP_PAIR_TRANSACTION");
        legacyV2Journal.setAttribute ("version", 2);
        legacyV2Journal.setAttribute ("state", "pending");
        legacyV2Journal.setAttribute (
            "transactionId", legacyV2TransactionId);
        for (size_t i = 0; i < legacyV2Files.size(); ++i)
        {
            expect (legacyV2Files[i].replaceWithText (
                        legacyV2Contents[i]),
                    "could not prepare v2 journal target");
            auto* file = legacyV2Journal.createNewChildElement ("FILE");
            file->setAttribute ("index", (int) i);
            file->setAttribute ("existed", 1);
            file->createNewChildElement ("CONTENTS")
                ->addTextElement (legacyV2Contents[i]);
        }
        expect (ChurchDAWMidiMapping::replaceFileWithTextAtomically (
                    pairJournalFor (legacyV2CC),
                    legacyV2Journal.toString (
                        juce::XmlElement::TextFormat().singleLine())),
                "could not publish v2 compatibility journal");
        for (size_t i = 0; i < legacyV2Files.size(); ++i)
            expect (replaceWithOwnedMap (
                        legacyV2Files[i], i == 0 || i == 2,
                        legacyV2TransactionId,
                        "v2-interrupted-" + juce::String ((int) i)),
                    "could not mutate v2 journal target");
        expect (ChurchDAWMidiMapping::MidiMapPairRollback::recoverPendingTransaction (
                    legacyV2CC, legacyV2Note),
                "v2 durable journal did not recover after update");
        for (size_t i = 0; i < legacyV2Files.size(); ++i)
            expect (legacyV2Files[i].loadFileAsString()
                        == legacyV2Contents[i],
                    "v2 journal did not restore exact legacy text");

        // A corrupt v3 digest is rejected before any target is touched.
        const auto checksumCC = directory.getChildFile ("checksum-cc.xml");
        const auto checksumNote = directory.getChildFile ("checksum-note.xml");
        expect (checksumCC.replaceWithText ("checksum-old-cc")
                    && checksumNote.replaceWithText ("checksum-old-note"),
                "could not prepare checksum journal fixtures");
        {
            ChurchDAWMidiMapping::MidiMapPairRollback checksumTransaction (
                checksumCC, checksumNote);
            expect (checksumTransaction.capture ("checksum-corruption"),
                    "could not create checksum journal fixture");
        }
        auto checksumJournalXml = juce::parseXML (pairJournalFor (checksumCC));
        expect (checksumJournalXml != nullptr
                    && checksumJournalXml->getFirstChildElement() != nullptr,
                "could not parse checksum journal fixture");
        checksumJournalXml->getFirstChildElement()->setAttribute (
            "sha256", juce::String::repeatedString ("0", 64));
        expect (ChurchDAWMidiMapping::replaceFileWithTextAtomically (
                    pairJournalFor (checksumCC),
                    checksumJournalXml->toString (
                        juce::XmlElement::TextFormat().singleLine()))
                    && ! ChurchDAWMidiMapping::MidiMapPairRollback::recoverPendingTransaction (
                        checksumCC, checksumNote)
                    && checksumCC.loadFileAsString() == "checksum-old-cc"
                    && checksumNote.loadFileAsString() == "checksum-old-note"
                    && pairJournalFor (checksumCC).existsAsFile(),
                "corrupt v3 digest changed targets or reported false recovery");
        expect (pairJournalFor (checksumCC).deleteFile(),
                "could not clean corrupt checksum journal fixture");

        // JUCE's legacy decoder trusts the decimal prefix before the dot and
        // allocates that amount. A tiny journal claiming ~2 GB must be rejected
        // by our bounded preflight before the decoder is called.
        const auto prefixCC = directory.getChildFile ("prefix-cc.xml");
        const auto prefixNote = directory.getChildFile ("prefix-note.xml");
        expect (prefixCC.replaceWithText ("prefix-old-cc")
                    && prefixNote.replaceWithText ("prefix-old-note"),
                "could not prepare bounded-prefix fixtures");
        {
            ChurchDAWMidiMapping::MidiMapPairRollback prefixTransaction (
                prefixCC, prefixNote);
            expect (prefixTransaction.capture ("prefix-corruption"),
                    "could not create bounded-prefix journal");
        }
        auto prefixJournalXml = juce::parseXML (
            pairJournalFor (prefixCC));
        auto* prefixFile = prefixJournalXml != nullptr
            ? prefixJournalXml->getFirstChildElement() : nullptr;
        auto* prefixContents = prefixFile != nullptr
            ? prefixFile->getChildByName ("CONTENTS") : nullptr;
        expect (prefixFile != nullptr && prefixContents != nullptr,
                "could not inspect bounded-prefix journal");
        prefixFile->setAttribute ("byteSize", 0);
        prefixFile->setAttribute (
            "sha256",
            ChurchDAWMidiMapping::midiMapDataSha256 ({}));
        prefixContents->deleteAllTextElements();
        prefixContents->addTextElement ("2147483647.");
        prefixJournalXml->setAttribute ("state", "committed");
        expect (ChurchDAWMidiMapping::replaceFileWithTextAtomically (
                    pairJournalFor (prefixCC),
                    prefixJournalXml->toString (
                        juce::XmlElement::TextFormat().singleLine()))
                    && ! ChurchDAWMidiMapping::midiMapPairJournalAllowsWrite (
                        prefixCC, {})
                    && ! ChurchDAWMidiMapping::MidiMapPairRollback::recoverPendingTransaction (
                        prefixCC, prefixNote)
                    && prefixCC.loadFileAsString() == "prefix-old-cc"
                    && prefixNote.loadFileAsString() == "prefix-old-note",
                "unbounded base64 prefix was decoded or changed targets");
        expect (pairJournalFor (prefixCC).deleteFile(),
                "could not clean bounded-prefix fixture");

        // Two app instances load V0. Instance A commits V1; B's old revision
        // must then reject capture before it can publish a journal or mutate.
        const auto revisionCC = directory.getChildFile ("revision-cc.xml");
        const auto revisionNote = directory.getChildFile ("revision-note.xml");
        const auto revisionCCLastGood = directory.getChildFile (
            "revision-cc.xml.last-good");
        const auto revisionNoteLastGood = directory.getChildFile (
            "revision-note.xml.last-good");
        expect (revisionCC.replaceWithText ("revision-v0-cc")
                    && revisionNote.replaceWithText ("revision-v0-note")
                    && revisionCCLastGood.replaceWithText (
                        "revision-v0-cc-last-good")
                    && revisionNoteLastGood.replaceWithText (
                        "revision-v0-note-last-good"),
                "could not prepare stale revision fixtures");
        ChurchDAWMidiMapping::MidiMapPairRevision revisionA;
        ChurchDAWMidiMapping::MidiMapPairRevision revisionB;
        expect (revisionA.refresh (revisionCC, revisionNote)
                    && revisionB.refresh (revisionCC, revisionNote),
                "could not capture two V0 app revisions");
        ChurchDAWMidiMapping::MidiMapPairFileLock revisionLock (
            revisionCC, revisionNote);
        ChurchDAWMidiMapping::MidiMapPairRollback writerA (
            revisionCC, revisionNote, revisionLock, &revisionA);
        expect (writerA.capture ("writer-a"),
                "fresh revision could not start pair transaction");
        const auto writerAId = pendingTransactionIdFor (revisionCC);
        expect (replaceWithOwnedMap (
                    revisionCC, true, writerAId, "revision-v1-cc")
                    && replaceWithOwnedMap (
                        revisionNote, false, writerAId, "revision-v1-note")
                    && replaceWithOwnedMap (
                        revisionCCLastGood, true, writerAId,
                        "revision-v1-cc-last-good")
                    && replaceWithOwnedMap (
                        revisionNoteLastGood, false, writerAId,
                        "revision-v1-note-last-good")
                    && writerA.commit()
                    && revisionA.matches (revisionCC, revisionNote)
                    && ! revisionB.matches (revisionCC, revisionNote),
                "writer A did not publish V1 or stale B failed to notice it");
        const auto revisionV1CC = revisionCC.loadFileAsString();
        ChurchDAWMidiMapping::MidiMapPairRollback staleWriterB (
            revisionCC, revisionNote, revisionLock, &revisionB);
        expect (! staleWriterB.capture ("stale-writer-b")
                    && staleWriterB.hadRevisionConflict()
                    && revisionCC.loadFileAsString() == revisionV1CC
                    && journalStateFor (revisionCC) == "committed"
                    && ChurchDAWMidiMapping::midiMapPairJournalAllowsWrite (
                        revisionCC, {}),
                "stale app instance could overwrite or checkpoint newer maps");

        // A target may change after capture if an old/non-cooperating build
        // bypasses the process lock. Closing that no-op checkpoint must not
        // refresh the loaded CAS to foreign bytes. The exact journal is
        // acknowledged by SHA, startup preserves V1, and a second capture by
        // the stale in-memory instance remains blocked until reload.
        const auto externalCC = directory.getChildFile ("external-cc.xml");
        const auto externalNote = directory.getChildFile (
            "external-note.xml");
        const auto externalCCLastGood = externalCC.getSiblingFile (
            externalCC.getFileName() + ".last-good");
        const auto externalNoteLastGood = externalNote.getSiblingFile (
            externalNote.getFileName() + ".last-good");
        expect (externalCC.replaceWithText ("external-v0-cc")
                    && externalNote.replaceWithText ("external-v0-note")
                    && externalCCLastGood.replaceWithText (
                        "external-v0-cc-last-good")
                    && externalNoteLastGood.replaceWithText (
                        "external-v0-note-last-good"),
                "could not prepare external-close fixtures");
        ChurchDAWMidiMapping::MidiMapPairRevision externalRevision;
        expect (externalRevision.refresh (externalCC, externalNote),
                "could not capture external-close V0 revision");
        ChurchDAWMidiMapping::MidiMapPairFileLock externalLock (
            externalCC, externalNote);
        ChurchDAWMidiMapping::MidiMapPairRollback externalCheckpoint (
            externalCC, externalNote, externalLock, &externalRevision);
        expect (externalCheckpoint.capture ("before-external-v1"),
                "could not create external-close checkpoint");
        const auto staleCheckpointId = pendingTransactionIdFor (
            externalCC);
        const juce::String externalWriterId = "foreign-v1-owner";
        expect (replaceWithOwnedMap (
                    externalCC, true, externalWriterId, "external-v1-cc")
                    && replaceWithOwnedMap (
                        externalNote, false, externalWriterId,
                        "external-v1-note")
                    && replaceWithOwnedMap (
                        externalCCLastGood, true, externalWriterId,
                        "external-v1-cc-last-good")
                    && replaceWithOwnedMap (
                        externalNoteLastGood, false, externalWriterId,
                        "external-v1-note-last-good")
                    && externalCheckpoint.closeWithoutMutation()
                    && externalCheckpoint.preservedExternalChanges()
                    && ! externalRevision.isInitialised()
                    && ChurchDAWMidiMapping::midiMapPairHasValidAbandonMarker (
                        pairJournalFor (externalCC), staleCheckpointId),
                "external close adopted V1 or failed to acknowledge its own journal");
        const auto externalV1CC = externalCC.loadFileAsString();
        expect (ChurchDAWMidiMapping::MidiMapPairRollback::recoverPendingTransaction (
                    externalCC, externalNote)
                    && externalCC.loadFileAsString() == externalV1CC,
                "startup recovery overwrote acknowledged external V1");
        ChurchDAWMidiMapping::MidiMapPairRollback staleAfterExternal (
            externalCC, externalNote, externalLock, &externalRevision);
        expect (! staleAfterExternal.capture ("stale-after-external")
                    && staleAfterExternal.hadRevisionConflict()
                    && externalCC.loadFileAsString() == externalV1CC,
                "second stale capture adopted or overwrote external V1");

        const auto journal = pairJournalFor (cc);
        const auto ccBeforeCorruptJournal = cc.loadFileAsString();
        const auto noteBeforeCorruptJournal = note.loadFileAsString();
        const auto ccLastGoodBeforeCorruptJournal =
            ccLastGood.loadFileAsString();
        const auto noteLastGoodBeforeCorruptJournal =
            noteLastGood.loadFileAsString();
        expect (journal.replaceWithText ("<BROKEN-JOURNAL")
                    && ! ChurchDAWMidiMapping::MidiMapPairRollback::recoverPendingTransaction (
                        cc, note)
                    && journal.existsAsFile()
                    && cc.loadFileAsString() == ccBeforeCorruptJournal
                    && note.loadFileAsString() == noteBeforeCorruptJournal
                    && ccLastGood.loadFileAsString()
                        == ccLastGoodBeforeCorruptJournal
                    && noteLastGood.loadFileAsString()
                        == noteLastGoodBeforeCorruptJournal,
                "corrupt journal changed MIDI targets or reported false recovery");
        directory.deleteRecursively();
    }

    // A physical note cannot simultaneously be a command and a continuous
    // parameter control: runtime consumes NOTEMAP first, making the MAP dead.
    {
        juce::XmlElement ccMap ("MIDIMAP");
        auto* cc = ccMap.createNewChildElement ("MAP");
        cc->setAttribute ("source", "midi:keyboard");
        cc->setAttribute ("channel", 1);
        cc->setAttribute ("cc", 60);
        cc->setAttribute ("isNote", 1);

        juce::XmlElement noteMap ("NOTEMAP");
        auto* note = noteMap.createNewChildElement ("NOTE");
        note->setAttribute ("source", "keyboard");
        note->setAttribute ("channel", 1);
        note->setAttribute ("note", 60);

        expect (ChurchDAWMidiMapping::midiMapPairHasConflictingNoteControls (
                    ccMap, noteMap),
                "cross-file duplicate note control was accepted");
        note->setAttribute ("note", 61);
        expect (! ChurchDAWMidiMapping::midiMapPairHasConflictingNoteControls (
                    ccMap, noteMap),
                "distinct cross-file note controls were rejected");

        std::map<ChurchDAWMidiMapping::ControllerKey, int> parameterMappings;
        std::map<ChurchDAWMidiMapping::ControllerKey, int> noteMappings;
        parameterMappings.emplace (
            ChurchDAWMidiMapping::ControllerKey {
                "midi:current", 1, 60, true, "Controller" }, 1);
        noteMappings.emplace (
            ChurchDAWMidiMapping::ControllerKey {
                "midi:current", 1, 60, false, "Controller" }, 1);
        expect (ChurchDAWMidiMapping::midiMapCollectionsHaveConflictingNoteControls (
                    parameterMappings, noteMappings),
                "post-rekey in-memory note conflict was accepted");
        noteMappings.clear();
        noteMappings.emplace (
            ChurchDAWMidiMapping::ControllerKey {
                "midi:current", 1, 61, false, "Controller" }, 1);
        expect (! ChurchDAWMidiMapping::midiMapCollectionsHaveConflictingNoteControls (
                    parameterMappings, noteMappings),
                "distinct post-rekey note controls were rejected");

        parameterMappings.clear();
        noteMappings.clear();
        const ChurchDAWMidiMapping::ControllerKey noteParameter60 {
            "midi:current", 1, 60, true, "Controller" };
        const ChurchDAWMidiMapping::ControllerKey noteParameter61 {
            "midi:current", 1, 61, true, "Controller" };
        const ChurchDAWMidiMapping::ControllerKey noteAction60 {
            "midi:current", 1, 60, false, "Controller" };
        const ChurchDAWMidiMapping::ControllerKey noteAction61 {
            "midi:current", 1, 61, false, "Controller" };
        parameterMappings.emplace (noteParameter60, 1);
        parameterMappings.emplace (noteParameter61, 2);
        noteMappings.emplace (noteAction60, 1);
        noteMappings.emplace (noteAction61, 2);
        expect (ChurchDAWMidiMapping::midiMapCollectionNoteConflictCount (
                    parameterMappings, noteMappings) == 2,
                "multiple legacy note conflicts were not counted");
        noteMappings.erase (noteAction60);
        expect (ChurchDAWMidiMapping::midiMapCollectionNoteConflictCount (
                    parameterMappings, noteMappings) == 1,
                "removing one legacy note conflict did not register progress");

        std::map<ChurchDAWMidiMapping::ControllerKey, int> ccActions;
        ccActions.emplace (
            ChurchDAWMidiMapping::ControllerKey {
                "midi:current", 1, 60, false, "Controller" }, 1);
        parameterMappings.clear();
        parameterMappings.emplace (
            ChurchDAWMidiMapping::ControllerKey {
                "midi:current", 1, 60, false, "Controller" }, 1);
        expect (ChurchDAWMidiMapping::midiMapCollectionsHaveConflictingCCControls (
                    parameterMappings, ccActions),
                "post-rekey CC parameter/action collision was accepted");
        ccActions.clear();
        ccActions.emplace (
            ChurchDAWMidiMapping::ControllerKey {
                "midi:current", 1, 61, false, "Controller" }, 1);
        expect (! ChurchDAWMidiMapping::midiMapCollectionsHaveConflictingCCControls (
                    parameterMappings, ccActions),
                "distinct CC parameter/action controls were rejected");

        parameterMappings.clear();
        ccActions.clear();
        const ChurchDAWMidiMapping::ControllerKey cc60 {
            "midi:current", 1, 60, false, "Controller" };
        const ChurchDAWMidiMapping::ControllerKey cc61 {
            "midi:current", 1, 61, false, "Controller" };
        parameterMappings.emplace (cc60, 1);
        parameterMappings.emplace (cc61, 2);
        ccActions.emplace (cc60, 1);
        ccActions.emplace (cc61, 2);
        expect (ChurchDAWMidiMapping::midiMapCollectionCCConflictCount (
                    parameterMappings, ccActions,
                    [] (int) { return true; }) == 2,
                "multiple legacy CC conflicts were not counted");
        ccActions.erase (cc60);
        expect (ChurchDAWMidiMapping::midiMapCollectionCCConflictCount (
                    parameterMappings, ccActions,
                    [] (int) { return true; }) == 1,
                "removing one legacy CC conflict did not register progress");
    }

    // G) Dirty epoch remains dirty when a write races with a newer change;
    // stable successful shutdown clears it, while failed writes do not.
    {
        ChurchDAWMidiMapping::MidiDirtyEpoch epoch;
        const auto captured = epoch.snapshot();
        epoch.markDirty();
        expect (! epoch.finish (captured, true) && epoch.isDirty(),
                "new MIDI change was incorrectly marked clean");
        const auto stable = epoch.snapshot();
        expect (epoch.finish (stable, true) && ! epoch.isDirty(),
                "stable shutdown flush did not clear dirty epoch");
        epoch.markDirty();
        const auto failed = epoch.snapshot();
        expect (! epoch.finish (failed, false) && epoch.isDirty(),
                "failed shutdown flush lost dirty state");
        ChurchDAWMidiMapping::MidiDirtyEpoch ccEpoch;
        ChurchDAWMidiMapping::MidiDirtyEpoch noteEpoch;
        ccEpoch.markDirty();
        noteEpoch.markDirty();
        expect (ccEpoch.finish (ccEpoch.snapshot(), true)
                    && ! ccEpoch.isDirty() && noteEpoch.isDirty(),
                "saving CC incorrectly marked the note map clean");
    }

    expect (! ChurchDAWMidiMapping::mayWriteMidiMapSnapshot (
                true, true, false),
            "automatic empty save was allowed after a read failure");
    expect (ChurchDAWMidiMapping::mayWriteMidiMapSnapshot (
                true, true, true),
            "explicit user clear could not authorise an empty snapshot");
    expect (ChurchDAWMidiMapping::mayWriteMidiMapSnapshot (
                true, false, false),
            "a real replacement map was blocked after a read failure");
    expect (ChurchDAWMidiMapping::shouldBlockEmptySaveAfterFailedRecovery (
                false, true),
            "corrupt last-good without a main file did not block empty autosave");
    expect (! ChurchDAWMidiMapping::shouldBlockEmptySaveAfterFailedRecovery (
                false, false),
            "fresh install without snapshots incorrectly blocked empty save");

    bool blockedEmptyCCSave = true;
    bool blockedEmptyNoteSave = true;
    ChurchDAWMidiMapping::applyMidiMapPairCommitToEmptySaveProtection (
        false, true, true, blockedEmptyCCSave, blockedEmptyNoteSave);
    expect (blockedEmptyCCSave && blockedEmptyNoteSave,
            "failed pair/half-save released empty-save protection");

    ChurchDAWMidiMapping::applyMidiMapPairCommitToEmptySaveProtection (
        true, true, false, blockedEmptyCCSave, blockedEmptyNoteSave);
    expect (! blockedEmptyCCSave && blockedEmptyNoteSave,
            "CC-only commit released the untouched NOTE guard");

    blockedEmptyCCSave = true;
    blockedEmptyNoteSave = true;
    ChurchDAWMidiMapping::applyMidiMapPairCommitToEmptySaveProtection (
        true, false, true, blockedEmptyCCSave, blockedEmptyNoteSave);
    expect (blockedEmptyCCSave && ! blockedEmptyNoteSave,
            "NOTE-only commit released the untouched CC guard");

    blockedEmptyCCSave = true;
    blockedEmptyNoteSave = true;
    ChurchDAWMidiMapping::applyMidiMapPairCommitToEmptySaveProtection (
        true, true, true, blockedEmptyCCSave, blockedEmptyNoteSave);
    expect (! blockedEmptyCCSave && ! blockedEmptyNoteSave,
            "complete pair commit did not release both empty-save guards");

    using Float = juce::AudioParameterFloat;

    auto volume = std::make_unique<Float> (
        juce::ParameterID { "vol0", 1 }, "Canal 1 Volume", 0.0f, 1.0f, 1.0f);
    auto attack = std::make_unique<Float> (
        juce::ParameterID { "attack0", 1 }, "Canal 1 Attack", 0.0f, 2.0f, 0.003f);
    auto transpose = std::make_unique<juce::AudioParameterInt> (
        juce::ParameterID { "transp0", 1 }, "Canal 1 Transpor", -24, 24, 0);

    // Simula uma versao nova que inseriu Attack antes de Transpose.
    juce::Array<juce::AudioProcessorParameter*> reordered {
        volume.get(), attack.get(), transpose.get()
    };

    expect (ChurchDAWMidiMapping::resolveParameterIndex (
                reordered, "transp0", "Canal 1 Transpor") == 2,
            "channel transpose must never be a MIDI Learn target");

    expect (ChurchDAWMidiMapping::resolveParameterIndex (
                reordered, "vol0", "Canal 1 Volume") == 0,
            "volume ID must not retarget to transpose");

    auto padCutoff = std::make_unique<Float> (
        juce::ParameterID { "padCutoff", 1 }, "Pad Cutoff", 0.0f, 1.0f, 1.0f);
    auto padVolume = std::make_unique<Float> (
        juce::ParameterID { "padVolume", 1 }, "Pad Volume", 0.0f, 1.5f, 0.9f);
    auto futurePadParameter = std::make_unique<Float> (
        juce::ParameterID { "padFuture", 1 }, "Pad Future", 0.0f, 1.0f, 0.0f);

    // Simula uma versao futura que inseriu/reordenou parametros do Pad. Os
    // knobs de Volume/Corte precisam continuar chegando ao mesmo destino pelo
    // stable ID, inclusive no slot virtual negativo usado pelo Pad Continuo.
    juce::Array<juce::AudioProcessorParameter*> reorderedPad {
        futurePadParameter.get(), padVolume.get(), padCutoff.get()
    };
    expect (ChurchDAWMidiMapping::resolveParameterIndex (
                reorderedPad, "padVolume", "Pad Volume") == 1,
            "Pad volume stable ID did not survive parameter reorder");
    expect (ChurchDAWMidiMapping::resolveParameterIndex (
                reorderedPad, "padCutoff", "Pad Cutoff") == 2,
            "Pad cutoff stable ID did not survive parameter reorder");
    expect (ChurchDAWMidiMapping::isSafeStoredTarget (-3, "padVolume", {}),
            "Pad volume virtual-slot mapping must survive restart");
    expect (ChurchDAWMidiMapping::isSafeStoredTarget (-3, "padCutoff", {}),
            "Pad cutoff virtual-slot mapping must survive restart");

    expect (ChurchDAWMidiMapping::resolveParameterIndex (
                reordered, {}, "Canal 1 Transpor") == -1,
            "named pre-ID transpose mappings must be rejected");

    expect (ChurchDAWMidiMapping::resolveParameterIndex (
                reordered, {}, {}) == -1,
            "index-only mappings must never resolve");
    expect (ChurchDAWMidiMapping::resolveParameterIndex (
                reordered, "missing-id", "Canal 1 Transpor") == -1,
            "orphan stable ID must not fall back to a generic name");

    expect (! ChurchDAWMidiMapping::isSafeStoredTarget (0, {}, {}),
            "instrument index-only target must be rejected");
    // MAPEAR O TRANSPOSE FOI LIBERADO EM 05/08/2026, A PEDIDO DO CLIENTE.
    // Um alvo COM stable ID e explicito: o musico escolheu aquele parametro.
    // O que continua bloqueado e o mapa LEGADO (sem ID, so nome), porque ali o
    // alvo e adivinhado por texto e um acerto por engano transporia o culto.
    expect (ChurchDAWMidiMapping::isSafeStoredTarget (0, "transp0", {}),
            "channel transpose stable-ID target must be accepted");
    expect (ChurchDAWMidiMapping::isSafeStoredTarget (0, "gtransp", {}),
            "global transpose stable-ID target must be accepted");
    expect (! ChurchDAWMidiMapping::isSafeStoredTarget (
                0, {}, "Canal 1 Transpor"),
            "legacy named transpose target must be rejected");
    expect (ChurchDAWMidiMapping::isSafeStoredTarget (0, "vol0", {}),
            "ordinary stable-ID target must remain accepted");
    expect (ChurchDAWMidiMapping::isSafeStoredTarget (0, {}, "Canal 1 Volume"),
            "named legacy target must be accepted");
    expect (ChurchDAWMidiMapping::isSafeStoredTarget (-1, {}, {}),
            "master mapping has no mutable instrument index");

    expect (! ChurchDAWMidiMapping::isMappableController (64),
            "sustain pedal must never restore as a parameter map");
    expect (! ChurchDAWMidiMapping::isMappableController (66),
            "sostenuto pedal must never restore as a parameter map");
    expect (! ChurchDAWMidiMapping::isMappableController (67),
            "soft pedal must never restore as a parameter map");
    expect (! ChurchDAWMidiMapping::isMappableController (121),
            "channel-mode reset must never restore as a parameter map");
    expect (! ChurchDAWMidiMapping::isMappableController (101),
            "RPN select must never restore as a parameter map");
    // CC 6 (Data Entry MSB) FOI LIBERADO EM 05/08/2026 -- ver a justificativa
    // completa em MidiMappingSafety.h. Resumo: no Launchkey do cliente os
    // faders sao CC 5..13, entao o CC 6 e o FADER 2 do aparelho dele; enquanto
    // bloqueado, esse fader era silenciosamente inmapeavel.
    // O RPN em si continua protegido: CC 98..101 seguem bloqueados abaixo, e o
    // CC 6 so carrega valor de RPN quando um deles foi aberto antes.
    expect (ChurchDAWMidiMapping::isMappableController (6),
            "Data Entry (fader 2 do Launchkey) must remain mappable");
    expect (ChurchDAWMidiMapping::isMappableController (28),
            "ordinary encoder CC must remain mappable");
    expect (ChurchDAWMidiMapping::isMappableController (11),
            "expression pedal must remain available to explicit MIDI Learn");
    expect (! ChurchDAWMidiMapping::shouldForwardRawControllerToInstruments (11),
            "unmapped expression pedal must not leak into the piano");
    expect (ChurchDAWMidiMapping::shouldForwardRawControllerToInstruments (1),
            "ordinary modulation CC must still reach instruments");
    expect (ChurchDAWMidiMapping::shouldForwardRawControllerToInstruments (64),
            "sustain must still reach the source-aware sustain path");

    ChurchDAWMidiMapping::RawCCTransactionGestureLatch transactionGesture;
    const ChurchDAWMidiMapping::ControllerKey transactionSustain {
        "midi:keyboard-A", 1, 64, false, "Keyboard A" };
    const ChurchDAWMidiMapping::ControllerKey unrelatedSustain {
        "midi:keyboard-B", 1, 64, false, "Keyboard B" };
    transactionGesture.observeRawDuringTransaction (
        transactionSustain, 127);
    expect (transactionGesture.hasPendingGesture()
                && ! transactionGesture.shouldContinueRawGesture (
                    unrelatedSustain, 0),
            "transaction sustain latch leaked across MIDI sources");
    expect (transactionGesture.shouldContinueRawGesture (
                transactionSustain, 0)
                && ! transactionGesture.hasPendingGesture(),
            "raw sustain release was consumed after a map transaction");

    const ChurchDAWMidiMapping::ControllerKey oneShotPad {
        "midi:keyboard-A", 1, 21, false, "Keyboard A" };
    transactionGesture.observeRawDuringTransaction (oneShotPad, 127);
    expect (! transactionGesture.hasPendingGesture()
                && ! transactionGesture.shouldContinueRawGesture (
                    oneShotPad, 0),
            "a 127-only stateless pad remained latched after a map transaction");

    ChurchDAWMidiMapping::RawCCTransactionGestureLatch fullRawGestures;
    for (size_t i = 0;
         i < ChurchDAWMidiMapping::RawCCTransactionGestureLatch::
                 maxTrackedGestures;
         ++i)
    {
        const ChurchDAWMidiMapping::ControllerKey key {
            "midi:raw-capacity-" + juce::String ((juce::int64) i),
            1 + (int) (i % 16),
            64 + (int) (i % 6), false, {} };
        expect (fullRawGestures.observeRawDuringTransaction (key, 127),
                "raw gesture latch overflowed before fixed capacity");
    }
    const ChurchDAWMidiMapping::ControllerKey overflowRawGesture {
        "midi:raw-capacity-overflow", 1, 64, false, {} };
    expect (! fullRawGestures.observeRawDuringTransaction (
                overflowRawGesture, 127)
                && fullRawGestures.overflowed()
                && fullRawGestures.hasPendingGesture()
                && fullRawGestures.shouldContinueRawGesture (
                    overflowRawGesture, 0),
            "raw gesture capacity exhaustion did not fail open");
    fullRawGestures.clear();
    expect (! fullRawGestures.overflowed()
                && ! fullRawGestures.hasPendingGesture(),
            "raw gesture reset did not clear fixed-capacity overflow");

    ChurchDAWMidiMapping::SourceAwareSustainState sustainBeforeTransaction;
    sustainBeforeTransaction.update ("midi:keyboard-A", 1, true);
    sustainBeforeTransaction.seedRawTransactionGestures (
        transactionGesture);
    expect (transactionGesture.hasPendingGesture()
                && transactionGesture.shouldContinueRawGesture (
                    transactionSustain, 0)
                && ! transactionGesture.hasPendingGesture(),
            "sustain held before a map transaction did not preserve its release");

    ChurchDAWMidiMapping::RawCCTransactionGestureLatch physicalPedals;
    const ChurchDAWMidiMapping::ControllerKey preHeldSostenuto {
        "midi:pedalboard", 2, 66, false, "Pedalboard" };
    physicalPedals.observeRawDuringTransaction (
        preHeldSostenuto, 127);
    auto transactionSeededFromPhysical = physicalPedals;
    expect (transactionSeededFromPhysical.shouldContinueRawGesture (
                preHeldSostenuto, 0)
                && ! transactionSeededFromPhysical.hasPendingGesture(),
            "CC66 held before a map transaction lost its raw release");

    const auto route8 = ChurchDAWMidiMapping::launchkeyEncoderRoute (7);
    expect (route8.target
                == ChurchDAWMidiMapping::LaunchkeyEncoderTarget::none,
            "Launchkey knob 8 must never have a fixed transpose route");

    // O toggle do Audio/MIDI precisa controlar a politica que o app usa de
    // verdade, e nao apenas esconder a interface. Desligado = a porta DAW volta
    // imediatamente a ser uma entrada MIDI comum. Ligado = reserva exclusiva,
    // exceto se o filtro deixaria o usuario sem teclado algum.
    const auto launchkeyDisabled =
        ChurchDAWMidiMapping::launchkeyCommonInputPolicy (
            false, 2, 1);
    expect (! launchkeyDisabled.reserveDawPort
                && ! launchkeyDisabled.failSafeKeepingDawPort
                && launchkeyDisabled.shouldEnableAsCommonInput (true),
            "disabled Launchkey auto-map must never reserve/filter the DAW port");

    const auto launchkeyEnabled =
        ChurchDAWMidiMapping::launchkeyCommonInputPolicy (
            true, 2, 1);
    expect (launchkeyEnabled.reserveDawPort
                && ! launchkeyEnabled.failSafeKeepingDawPort
                && ! launchkeyEnabled.shouldEnableAsCommonInput (true)
                && launchkeyEnabled.shouldEnableAsCommonInput (false),
            "enabled Launchkey auto-map must reserve only the DAW port");

    const auto launchkeyDawOnly =
        ChurchDAWMidiMapping::launchkeyCommonInputPolicy (
            true, 1, 1);
    expect (! launchkeyDawOnly.reserveDawPort
                && launchkeyDawOnly.failSafeKeepingDawPort
                && launchkeyDawOnly.shouldEnableAsCommonInput (true),
            "DAW-only device naming must fail open instead of muting all MIDI");

    const auto launchkeyNoDevices =
        ChurchDAWMidiMapping::launchkeyCommonInputPolicy (
            true, 0, 0);
    expect (! launchkeyNoDevices.reserveDawPort
                && ! launchkeyNoDevices.failSafeKeepingDawPort,
            "no-device state must not invent a reserved Launchkey port");

    using Key = ChurchDAWMidiMapping::ControllerKey;
    std::map<Key, juce::String> isolatedMappings;
    const Key keyboardA { "device-A", 1, 28 };
    const Key keyboardAChannel2 { "device-A", 2, 28 };
    const Key keyboardB { "device-B", 1, 28 };
    const Key launchkeyDaw { "churchdaw:launchkey-daw", 1, 28 };
    isolatedMappings[keyboardA] = "transp0";

    const Key sameIdentityDifferentMetadata { "device-A", 1, 28, false,
                                              "Keyboard renamed" };
    expect (sameIdentityDifferentMetadata.sourceName == "Keyboard renamed"
                && !(sameIdentityDifferentMetadata < keyboardA)
                && !(keyboardA < sameIdentityDifferentMetadata),
            "sourceName must be metadata, not part of MIDI lookup ordering");

    expect (isolatedMappings.find (keyboardA) != isolatedMappings.end(),
            "exact source/channel/CC mapping missing");
    expect (isolatedMappings.find (keyboardAChannel2) == isolatedMappings.end(),
            "same device on another channel leaked into transpose");
    expect (isolatedMappings.find (keyboardB) == isolatedMappings.end(),
            "another device with same CC leaked into transpose");
    expect (isolatedMappings.find (launchkeyDaw) == isolatedMappings.end(),
            "Launchkey DAW port collided with musical MIDI mapping");

    // Sustain e global no motor, mas precisa continuar abaixado enquanto
    // QUALQUER porta fisica ainda estiver em 127. A porta que sobe primeiro
    // nao pode cortar o acorde da outra.
    ChurchDAWMidiMapping::SourceAwareSustainState sustainSources;
    auto sustainDecision = sustainSources.update ("midi:keyboard-A", 1, true);
    expect (sustainDecision.forwardTransition
                && sustainDecision.aggregateDown,
            "first pedal down must reach the engine");

    sustainDecision = sustainSources.update ("midi:keyboard-B", 1, true);
    expect (! sustainDecision.forwardTransition
                && sustainDecision.aggregateDown,
            "second pedal down must not duplicate aggregate transition");

    sustainDecision = sustainSources.update ("midi:keyboard-A", 1, false);
    expect (! sustainDecision.forwardTransition
                && sustainDecision.aggregateDown,
            "one pedal up cut another source that is still down");

    sustainDecision = sustainSources.update ("midi:keyboard-B", 1, false);
    expect (sustainDecision.forwardTransition
                && ! sustainDecision.aggregateDown,
            "last pedal up must release aggregate sustain");

    sustainSources.update ("midi:keyboard-A", 1, true);
    sustainSources.update ("midi:keyboard-B", 2, true);
    const juce::StringArray onlyKeyboardB { "midi:keyboard-B" };
    expect (! sustainSources.retainOnlySources (onlyKeyboardB)
                && sustainSources.isDown(),
            "hot-plug pruning cut a remaining held pedal");
    expect (sustainSources.retainOnlySources ({})
                && ! sustainSources.isDown(),
            "removing the last held source did not release aggregate sustain");

    ChurchDAWMidiMapping::SourceAwareSustainState fullSustainSources;
    for (size_t i = 0;
         i < ChurchDAWMidiMapping::SourceAwareSustainState::
                 maxTrackedSources;
         ++i)
        (void) fullSustainSources.update (
            "midi:sustain-capacity-" + juce::String ((juce::int64) i),
            1 + (int) (i % 16), true);
    (void) fullSustainSources.update (
        "midi:sustain-capacity-overflow", 1, true);
    expect (fullSustainSources.overflowed()
                && fullSustainSources.isDown(),
            "sustain source capacity exhaustion did not latch fail-open");
    for (size_t i = 0;
         i < ChurchDAWMidiMapping::SourceAwareSustainState::
                 maxTrackedSources;
         ++i)
        (void) fullSustainSources.update (
            "midi:sustain-capacity-" + juce::String ((juce::int64) i),
            1 + (int) (i % 16), false);
    expect (fullSustainSources.isDown()
                && fullSustainSources.retainOnlySources ({})
                && ! fullSustainSources.isDown()
                && ! fullSustainSources.overflowed(),
            "untracked sustain overflow released early or survived reset");

    // Soak deterministico: um milhao de eventos com CC igual, mas origem/canal
    // diferente, nao pode encontrar o alvo de transpose.
    for (int event = 0; event < 1000000; ++event)
    {
        const Key incoming {
            (event & 1) == 0 ? "device-B" : "churchdaw:launchkey-daw",
            1 + (event % 16),
            28
        };
        expect (isolatedMappings.find (incoming) == isolatedMappings.end(),
                "transpose source isolation failed during soak");
    }

    juce::MidiBuffer panic;
    ChurchDAWPerformanceMidiSafety::appendFullPanic (panic, 0);

    int pitchCentres = 0;
    int controllerResets = 0;
    bool channelSeen[17] {};

    for (const auto meta : panic)
    {
        const auto message = meta.getMessage();

        if (message.isPitchWheel())
        {
            expect (message.getPitchWheelValue() == 8192,
                    "panic pitch wheel must be centered");
            ++pitchCentres;
            channelSeen[message.getChannel()] = true;
        }

        if (message.isController() && message.getControllerNumber() == 121)
            ++controllerResets;
    }

    expect (pitchCentres == 16, "panic must center pitch on all 16 channels");
    expect (controllerResets == 16, "panic must reset controllers on all 16 channels");

    for (int channel = 1; channel <= 16; ++channel)
        expect (channelSeen[channel], "panic missed a MIDI channel");

    // Mapped PANIC and FIFO-overflow PANIC both enter this same orchestrator.
    // Prove every global engine boundary runs once and in the safe order; the
    // performance-panel path may report that Pad Synth was already stopped.
    std::array<int, 4> panicOrder {};
    int panicStep = 0;
    int continuousPadStops = 0;
    int continuousPadRefreshes = 0;
    int padSynthPanics = 0;
    int transportResets = 0;
    ChurchDAWPerformanceMidiSafety::performGlobalPanic (
        [&]
        {
            ++continuousPadStops;
            panicOrder[(std::size_t) panicStep++] = 1;
        },
        [&]
        {
            ++continuousPadRefreshes;
            panicOrder[(std::size_t) panicStep++] = 2;
        },
        [&]
        {
            ++padSynthPanics;
            panicOrder[(std::size_t) panicStep++] = 3;
        },
        [&]
        {
            ++transportResets;
            panicOrder[(std::size_t) panicStep++] = 4;
        });
    expect (continuousPadStops == 1
                && continuousPadRefreshes == 1
                && padSynthPanics == 1
                && transportResets == 1,
            "global panic did not invoke every engine exactly once");
    expect (panicOrder == std::array<int, 4> { 1, 2, 3, 4 },
            "global panic engine order changed");

    ChurchDAWPerformanceMidiSafety::performGlobalPanic (
        [&] { ++continuousPadStops; },
        [&] { ++continuousPadRefreshes; },
        [&] { ++padSynthPanics; },
        [&] { ++transportResets; },
        true);
    expect (continuousPadStops == 2
                && continuousPadRefreshes == 2
                && padSynthPanics == 1
                && transportResets == 2,
            "pre-requested Pad Synth panic was duplicated or global reset skipped");

    // A mesma nota em dois canais pode ter sido ligada com transposes
    // diferentes. Cada note-off precisa voltar ao deslocamento do seu proprio
    // note-on; compartilhar somente [layer][note] deixava nota presa no VST/AU.
    ChurchDAWPerformanceMidiSafety::NoteTransposeLedger<5> transposeLedger;
    expect (transposeLedger.recordNoteOn (0, 1, 60, 72),
            "channel 1 transpose note-on was unexpectedly rejected");
    expect (transposeLedger.recordNoteOn (0, 2, 60, 55),
            "channel 2 transpose note-on was unexpectedly rejected");
    expect (transposeLedger.translatedNoteOff (0, 1, 60) == 72,
            "channel 1 note-off lost its original transpose");
    expect (transposeLedger.translatedNoteOff (0, 2, 60) == 55,
            "channel 2 note-off collided with channel 1");
    expect (transposeLedger.translatedNoteOff (1, 1, 60) == 60,
            "transpose ledger leaked across layers");

    // Se o Note-Off chegar em outro canal por causa de transporte host/MIDI,
    // devemos recuperar o deslocamento da mesma nota no layer e evitar tom preso.
    expect (transposeLedger.recordNoteOn (0, 1, 64, 76),
            "channel 1 transpose note-on for channel-mismatch fallback test");
    expect (transposeLedger.translatedNoteOff (0, 2, 64) == 76,
            "channel-mismatch note-off must recover stored transpose");

    // Retrigger sobreposto NA MESMA layer/canal/nota: um unico offset
    // sobrescrevia o primeiro note-on e deixava a voz antiga presa.
    expect (transposeLedger.recordNoteOn (3, 4, 64, 76),
            "overlapping transpose note-on #1 was rejected");
    expect (transposeLedger.recordNoteOn (3, 4, 64, 59),
            "overlapping transpose note-on #2 was rejected");
    expect (transposeLedger.translatedNoteOff (3, 4, 64) == 76,
            "overlapping note-on lost FIFO transpose #1");
    expect (transposeLedger.translatedNoteOff (3, 4, 64) == 59,
            "overlapping note-on lost FIFO transpose #2");
    expect (transposeLedger.translatedNoteOff (3, 4, 64) == 64,
            "consumed ledger entry was not cleared");

    // O nono retrigger nao pode substituir nenhuma das oito traducoes ainda
    // pendentes. O chamador recebe false e pode manter a nota sem transpose.
    transposeLedger.clear();

    for (std::size_t retrigger = 0;
         retrigger
             < ChurchDAWPerformanceMidiSafety::
                   NoteTransposeLedger<5>::maxOverlappingNoteOns;
         ++retrigger)
    {
        expect (transposeLedger.recordNoteOn (
                    4, 16, 40, 48 + (int) retrigger),
                "ledger rejected a retrigger before reaching capacity");
    }

    expect (! transposeLedger.recordNoteOn (4, 16, 40, 96),
            "ninth retrigger must fail instead of overwriting the FIFO");

    for (std::size_t retrigger = 0;
         retrigger
             < ChurchDAWPerformanceMidiSafety::
                   NoteTransposeLedger<5>::maxOverlappingNoteOns;
         ++retrigger)
    {
        expect (transposeLedger.translatedNoteOff (4, 16, 40)
                    == 48 + (int) retrigger,
                "full ledger did not preserve an accepted FIFO entry");
    }

    expect (transposeLedger.translatedNoteOff (4, 16, 40) == 40,
            "rejected ninth retrigger unexpectedly entered the ledger");
    expect (transposeLedger.recordNoteOn (4, 16, 40, 91),
            "drained ledger did not accept a new retrigger");
    transposeLedger.clearLayer (4);
    expect (transposeLedger.translatedNoteOff (4, 16, 40) == 40,
            "clearLayer did not discard a post-capacity entry");
    expect (! transposeLedger.recordNoteOn (-1, 1, 60, 72),
            "invalid ledger coordinates must fail explicitly");

    expect (transposeLedger.recordNoteOn (1, 3, 48, 60),
            "layer 1 transpose note-on was unexpectedly rejected");
    expect (transposeLedger.recordNoteOn (2, 3, 48, 36),
            "layer 2 transpose note-on was unexpectedly rejected");
    transposeLedger.clearLayer (1);
    expect (transposeLedger.translatedNoteOff (1, 3, 48) == 48,
            "layer swap retained a stale transpose entry");
    expect (transposeLedger.translatedNoteOff (2, 3, 48) == 36,
            "layer-local clear leaked into another channel strip");

    transposeLedger.clear();
    expect (transposeLedger.translatedNoteOff (0, 1, 60) == 60,
            "panic/reset did not clear note transpose ledger");

    // O AU/VST recebe um bend durante a frase, mas o teclado perde o retorno
    // ao centro. A proxima frase precisa comecar centrada.
    ChurchDAWPerformanceMidiSafety::PitchBendGuard pitchGuard;
    juce::MidiBuffer pitchSource;
    juce::MidiBuffer pitchOutput;
    pitchSource.ensureSize (64 * 1024);
    pitchOutput.ensureSize (64 * 1024);

    pitchSource.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
    pitchSource.addEvent (juce::MidiMessage::pitchWheel (1, 12000), 8);
    pitchSource.addEvent (juce::MidiMessage::noteOff (1, 60), 24);
    pitchGuard.process (pitchSource, pitchOutput);

    int centresBeforeFirstPhrase = 0;

    for (const auto meta : pitchOutput)
        if (meta.getMessage().isPitchWheel()
             && meta.getMessage().getPitchWheelValue()
                    == ChurchDAWPerformanceMidiSafety::PitchBendGuard::centreValue)
            ++centresBeforeFirstPhrase;

    expect (centresBeforeFirstPhrase == 0,
            "intentional bend during held note was overwritten");

    pitchSource.clear();
    pitchSource.addEvent (juce::MidiMessage::noteOn (1, 64, (juce::uint8) 100), 5);
    pitchGuard.process (pitchSource, pitchOutput);

    bool sawCentreBeforeNextNote = false;
    bool sawNextNote = false;

    for (const auto meta : pitchOutput)
    {
        const auto message = meta.getMessage();

        if (message.isPitchWheel()
             && message.getPitchWheelValue()
                    == ChurchDAWPerformanceMidiSafety::PitchBendGuard::centreValue
             && ! sawNextNote)
            sawCentreBeforeNextNote = true;
        else if (message.isNoteOn())
            sawNextNote = true;
    }

    expect (sawCentreBeforeNextNote && sawNextNote,
            "stale bend was not centered before next phrase");

    // Pitch Wheel novo durante o silencio e um pre-bend explicito e deve
    // sobreviver ate o ataque seguinte.
    pitchSource.clear();
    pitchSource.addEvent (juce::MidiMessage::noteOff (1, 64), 0);
    pitchGuard.process (pitchSource, pitchOutput);
    pitchSource.clear();
    pitchSource.addEvent (juce::MidiMessage::pitchWheel (1, 10000), 2);
    pitchSource.addEvent (juce::MidiMessage::noteOn (1, 67, (juce::uint8) 100), 6);
    pitchGuard.process (pitchSource, pitchOutput);

    int preBendCentres = 0;
    bool sawExplicitPreBend = false;

    for (const auto meta : pitchOutput)
    {
        const auto message = meta.getMessage();

        if (message.isPitchWheel()
             && message.getPitchWheelValue()
                    == ChurchDAWPerformanceMidiSafety::PitchBendGuard::centreValue)
            ++preBendCentres;

        if (message.isPitchWheel() && message.getPitchWheelValue() == 10000)
            sawExplicitPreBend = true;
    }

    expect (sawExplicitPreBend && preBendCentres == 0,
            "explicit pre-bend was incorrectly centered");

    // Legato no mesmo sample nao cria uma nova frase nem um degrau de pitch.
    pitchSource.clear();
    pitchSource.addEvent (juce::MidiMessage::noteOff (1, 67), 12);
    pitchSource.addEvent (juce::MidiMessage::noteOn (1, 69, (juce::uint8) 100), 12);
    pitchGuard.process (pitchSource, pitchOutput);

    int sameSampleCentres = 0;

    for (const auto meta : pitchOutput)
        if (meta.getMessage().isPitchWheel()
             && meta.getMessage().getPitchWheelValue()
                    == ChurchDAWPerformanceMidiSafety::PitchBendGuard::centreValue)
            ++sameSampleCentres;

    expect (sameSampleCentres == 0,
            "same-sample legato was treated as a stale new phrase");

    // Sustain mantem a frase viva mesmo sem teclas fisicamente pressionadas.
    pitchGuard.reset();
    pitchSource.clear();
    pitchSource.addEvent (juce::MidiMessage::noteOn (2, 55, (juce::uint8) 100), 0);
    pitchSource.addEvent (juce::MidiMessage::pitchWheel (2, 11000), 2);
    pitchSource.addEvent (juce::MidiMessage::controllerEvent (2, 64, 127), 4);
    pitchSource.addEvent (juce::MidiMessage::noteOff (2, 55), 6);
    pitchSource.addEvent (juce::MidiMessage::noteOn (2, 59, (juce::uint8) 100), 10);
    pitchGuard.process (pitchSource, pitchOutput);

    int sustainCentres = 0;

    for (const auto meta : pitchOutput)
        if (meta.getMessage().isPitchWheel()
             && meta.getMessage().getPitchWheelValue()
                    == ChurchDAWPerformanceMidiSafety::PitchBendGuard::centreValue)
            ++sustainCentres;

    expect (sustainCentres == 0,
            "sustain-held phrase was incorrectly centered");

    // CC120/123 sao fronteiras GLOBAIS. Um PANIC originado no canal 4 deve
    // centralizar e limpar tambem o bend+sustain herdado no canal 1. Todos os
    // eventos ordinarios do sample preservam a ordem; os 16 centros e os
    // all-off coalescidos ficam no fim.
    const auto exerciseGlobalAllOff = [&] (int allOffController,
                                           bool sameSample,
                                           bool allOffBeforeNote)
    {
        constexpr int boundarySample = 8;
        const int newNoteSample = sameSample ? boundarySample : 9;

        pitchGuard.reset();
        pitchSource.clear();
        pitchSource.addEvent (
            juce::MidiMessage::noteOn (
                1, 50, (juce::uint8) 100), 0);
        pitchSource.addEvent (
            juce::MidiMessage::pitchWheel (
                1, 13000), 2);
        pitchSource.addEvent (
            juce::MidiMessage::controllerEvent (
                1, 64, 127), 3);
        pitchSource.addEvent (
            juce::MidiMessage::noteOff (1, 50), 4);
        pitchSource.addEvent (
            juce::MidiMessage::controllerEvent (
                2, 11, 31), boundarySample);

        const auto addDuplicateAllOff = [&]
        {
            pitchSource.addEvent (
                juce::MidiMessage::controllerEvent (
                    4, allOffController, 0), boundarySample);
            pitchSource.addEvent (
                juce::MidiMessage::controllerEvent (
                    4, allOffController, 0), boundarySample);
        };

        if (! sameSample || allOffBeforeNote)
            addDuplicateAllOff();

        pitchSource.addEvent (
            juce::MidiMessage::controllerEvent (
                2, 11, 63), boundarySample);
        pitchSource.addEvent (
            juce::MidiMessage::noteOn (
                1, 57, (juce::uint8) 100), newNoteSample);

        if (sameSample && ! allOffBeforeNote)
            addDuplicateAllOff();

        pitchGuard.process (pitchSource, pitchOutput);

        int eventIndex = 0;
        int markerOneIndex = -1;
        int markerTwoIndex = -1;
        int newNoteIndex = -1;
        int firstCentreIndex = -1;
        int lastCentreIndex = -1;
        int lastOrdinaryBoundaryIndex = -1;
        int allOffIndex = -1;
        int lastBoundaryEventIndex = -1;
        int centreCount = 0;
        int allOffCount = 0;
        bool centreChannels[17] {};

        for (const auto meta : pitchOutput)
        {
            const auto message = meta.getMessage();
            const bool isBoundaryCentre =
                meta.samplePosition == boundarySample
                && message.isPitchWheel()
                && message.getPitchWheelValue()
                     == ChurchDAWPerformanceMidiSafety::
                            PitchBendGuard::centreValue;
            const bool isExpectedAllOff =
                meta.samplePosition == boundarySample
                && message.isController()
                && message.getControllerNumber() == allOffController;

            if (meta.samplePosition == boundarySample)
                lastBoundaryEventIndex = eventIndex;

            if (isBoundaryCentre)
            {
                if (firstCentreIndex < 0)
                    firstCentreIndex = eventIndex;

                lastCentreIndex = eventIndex;
                ++centreCount;
                centreChannels[message.getChannel()] = true;
            }
            else if (isExpectedAllOff)
            {
                allOffIndex = eventIndex;
                ++allOffCount;
                expect (message.getChannel() == 4,
                        "global all-off changed the source MIDI channel");
            }
            else if (meta.samplePosition == boundarySample)
            {
                lastOrdinaryBoundaryIndex = eventIndex;
            }

            if (message.isController()
                 && message.getChannel() == 2
                 && message.getControllerNumber() == 11)
            {
                if (message.getControllerValue() == 31)
                    markerOneIndex = eventIndex;
                else if (message.getControllerValue() == 63)
                    markerTwoIndex = eventIndex;
            }

            if (message.isNoteOn()
                 && message.getChannel() == 1
                 && message.getNoteNumber() == 57)
                newNoteIndex = eventIndex;

            ++eventIndex;
        }

        expect (markerOneIndex >= 0 && markerTwoIndex > markerOneIndex,
                "ordinary same-sample event order was not preserved");
        expect (centreCount == 16,
                "global all-off must emit 16 real pitch centres");
        expect (firstCentreIndex > lastOrdinaryBoundaryIndex,
                "pitch centres must follow ordinary boundary-sample events");
        expect (allOffCount == 1,
                "duplicate all-off was not coalesced per channel/controller");
        expect (allOffIndex > lastCentreIndex,
                "all-off must follow every global pitch centre");
        expect (allOffIndex == lastBoundaryEventIndex,
                "all-off was not the final event in its sample");
        expect (newNoteIndex >= 0,
                "cross-channel all-off/new-note stream was incomplete");

        if (sameSample)
            expect (newNoteIndex < firstCentreIndex,
                    "same-sample note was not kept before the all-off boundary");
        else
            expect (newNoteIndex > allOffIndex,
                    "next-sample note did not follow the all-off boundary");

        for (int channel = 1; channel <= 16; ++channel)
            expect (centreChannels[channel],
                    "global all-off missed a pitch-centre channel");

        if (sameSample)
        {
            // A nota do canal 1 ocorreu antes do boundary adiado e precisa ter
            // sido descartada junto com sustain. Depois do reset, o note-off
            // abaixo e orfao e encerra o pre-bend; sem reset ele encontraria a
            // tecla+sustain antigos e nao produziria o centro esperado.
            pitchSource.clear();
            pitchSource.addEvent (
                juce::MidiMessage::pitchWheel (1, 10000), 0);
            pitchSource.addEvent (
                juce::MidiMessage::noteOff (1, 57), 1);
            pitchSource.addEvent (
                juce::MidiMessage::noteOn (
                    1, 60, (juce::uint8) 100), 3);
            pitchGuard.process (pitchSource, pitchOutput);

            int postBoundaryCentres = 0;
            bool sawPostBoundaryPreBend = false;

            for (const auto meta : pitchOutput)
            {
                const auto message = meta.getMessage();

                if (message.isPitchWheel()
                     && message.getPitchWheelValue()
                          == ChurchDAWPerformanceMidiSafety::
                                 PitchBendGuard::centreValue)
                    ++postBoundaryCentres;

                if (message.isPitchWheel()
                     && message.getPitchWheelValue() == 10000)
                    sawPostBoundaryPreBend = true;
            }

            expect (sawPostBoundaryPreBend
                     && postBoundaryCentres == 1,
                    "global all-off did not reset channel 1 state");
        }
    };

    for (const int allOffController : { 120, 123 })
    {
        exerciseGlobalAllOff (allOffController, false, true);
        exerciseGlobalAllOff (allOffController, true, true);
        exerciseGlobalAllOff (allOffController, true, false);
    }

    const auto countCentres = [&]
    {
        int count = 0;

        for (const auto meta : pitchOutput)
            if (meta.getMessage().isPitchWheel()
                 && meta.getMessage().getPitchWheelValue()
                      == ChurchDAWPerformanceMidiSafety::
                             PitchBendGuard::centreValue)
                ++count;

        return count;
    };

    // O sustain da interface existe fora do fluxo MIDI. Enquanto ligado, um
    // note-off não encerra a frase; na borda ON->OFF entre callbacks, a próxima
    // nota deve receber centro mesmo sem aparecer CC64 no bloco.
    pitchGuard.reset();
    pitchSource.clear();
    pitchSource.addEvent (
        juce::MidiMessage::noteOn (
            1, 60, (juce::uint8) 100), 0);
    pitchSource.addEvent (
        juce::MidiMessage::pitchWheel (1, 12000), 2);
    pitchSource.addEvent (
        juce::MidiMessage::noteOff (1, 60), 4);
    pitchGuard.process (
        pitchSource, pitchOutput, true, 0);
    expect (countCentres() == 0,
            "UI global sustain ended a still-sustained phrase");

    pitchSource.clear();
    pitchSource.addEvent (
        juce::MidiMessage::noteOn (
            1, 62, (juce::uint8) 100), 5);
    pitchSource.addEvent (
        juce::MidiMessage::noteOff (1, 62), 7);
    pitchGuard.process (
        pitchSource, pitchOutput, true, 0);
    expect (countCentres() == 0,
            "UI global sustain recentered a continued phrase");

    pitchSource.clear();
    pitchSource.addEvent (
        juce::MidiMessage::noteOn (
            1, 64, (juce::uint8) 100), 0);
    pitchGuard.process (
        pitchSource, pitchOutput, false, 0);
    expect (countCentres() == 1,
            "UI sustain ON->OFF did not close the previous phrase");

    // O pedal físico também é global no host. CC64 no canal 4 precisa manter a
    // voz/bend do canal 1 vivo; a soltura global então encerra a frase de todos.
    pitchGuard.reset();
    pitchSource.clear();
    pitchSource.addEvent (
        juce::MidiMessage::noteOn (
            1, 50, (juce::uint8) 100), 0);
    pitchSource.addEvent (
        juce::MidiMessage::pitchWheel (1, 12500), 1);
    pitchSource.addEvent (
        juce::MidiMessage::controllerEvent (
            4, 64, 127), 2);
    pitchSource.addEvent (
        juce::MidiMessage::noteOff (1, 50), 3);
    pitchSource.addEvent (
        juce::MidiMessage::noteOn (
            1, 52, (juce::uint8) 100), 5);
    pitchGuard.process (
        pitchSource, pitchOutput, false, 0);
    expect (countCentres() == 0,
            "cross-channel CC64 did not sustain channel 1");

    pitchSource.clear();
    pitchSource.addEvent (
        juce::MidiMessage::noteOff (1, 52), 0);
    pitchSource.addEvent (
        juce::MidiMessage::controllerEvent (
            4, 64, 0), 2);
    pitchSource.addEvent (
        juce::MidiMessage::noteOn (
            1, 55, (juce::uint8) 100), 5);
    pitchGuard.process (
        pitchSource, pitchOutput, false, 1);
    expect (countCentres() == 1,
            "cross-channel CC64 OFF did not close channel 1 phrase");

    // Reprepare zera o espelho interno, mas não move fisicamente o pedal. O
    // estado de início informado pelo host deve sobreviver sem retunar a voz.
    pitchGuard.requestPitchCentresOnNextBlock();
    pitchSource.clear();
    pitchGuard.process (
        pitchSource, pitchOutput, false, 1);
    expect (countCentres() == 16,
            "reprepare did not emit all pitch centres");

    pitchSource.clear();
    pitchSource.addEvent (
        juce::MidiMessage::noteOn (
            1, 57, (juce::uint8) 100), 0);
    pitchSource.addEvent (
        juce::MidiMessage::pitchWheel (1, 11000), 1);
    pitchSource.addEvent (
        juce::MidiMessage::noteOff (1, 57), 2);
    pitchSource.addEvent (
        juce::MidiMessage::noteOn (
            1, 59, (juce::uint8) 100), 5);
    pitchGuard.process (
        pitchSource, pitchOutput, false, 1);
    expect (countCentres() == 0,
            "physical sustain held across reprepare was forgotten");

    // Reabertura da rota centraliza todos os canais no primeiro bloco real.
    pitchGuard.requestPitchCentresOnNextBlock();
    pitchSource.clear();
    pitchGuard.process (pitchSource, pitchOutput);

    int reconnectCentres = 0;
    bool reconnectChannels[17] {};

    for (const auto meta : pitchOutput)
    {
        const auto message = meta.getMessage();

        if (message.isPitchWheel()
             && message.getPitchWheelValue()
                    == ChurchDAWPerformanceMidiSafety::PitchBendGuard::centreValue)
        {
            ++reconnectCentres;
            reconnectChannels[message.getChannel()] = true;
        }
    }

    expect (reconnectCentres == 16,
            "reconnection must center all 16 MIDI channels");

    for (int channel = 1; channel <= 16; ++channel)
        expect (reconnectChannels[channel],
                "reconnection missed a MIDI channel");

    // CC121 tambem recebe centro explicito para plugins que implementam o
    // reset de controladores de forma incompleta.
    pitchSource.clear();
    pitchSource.addEvent (juce::MidiMessage::controllerEvent (3, 121, 0), 7);
    pitchGuard.process (pitchSource, pitchOutput);

    bool cc121Centre = false;

    for (const auto meta : pitchOutput)
        if (meta.samplePosition == 7
             && meta.getMessage().isPitchWheel()
             && meta.getMessage().getChannel() == 3
             && meta.getMessage().getPitchWheelValue()
                    == ChurchDAWPerformanceMidiSafety::PitchBendGuard::centreValue)
            cc121Centre = true;

    expect (cc121Centre,
            "CC121 reset did not explicitly center pitch");

    // CC121 zera controladores, mas não encerra teclas. Se o guard esquecer a
    // nota 60 ainda pressionada, o NoteOff62 abaixo cria um falso fim de frase
    // e recentraliza abruptamente o ataque 64 (e a nota 60 que segue soando).
    pitchGuard.reset();
    pitchSource.clear();
    pitchSource.addEvent (
        juce::MidiMessage::noteOn (
            1, 60, (juce::uint8) 100), 0);
    pitchSource.addEvent (
        juce::MidiMessage::controllerEvent (
            1, 121, 0), 1);
    pitchSource.addEvent (
        juce::MidiMessage::pitchWheel (
            1, 9000), 2);
    pitchSource.addEvent (
        juce::MidiMessage::noteOn (
            1, 62, (juce::uint8) 100), 3);
    pitchSource.addEvent (
        juce::MidiMessage::noteOff (
            1, 62), 4);
    pitchSource.addEvent (
        juce::MidiMessage::noteOn (
            1, 64, (juce::uint8) 100), 5);
    pitchGuard.process (
        pitchSource, pitchOutput);

    int spuriousCentresBefore64 = 0;
    bool sawBentAttack64 = false;

    for (const auto meta : pitchOutput)
    {
        const auto message = meta.getMessage();

        if (meta.samplePosition == 5
             && message.isPitchWheel()
             && message.getPitchWheelValue()
                  == ChurchDAWPerformanceMidiSafety::
                         PitchBendGuard::centreValue)
            ++spuriousCentresBefore64;

        if (meta.samplePosition == 5
             && message.isNoteOn()
             && message.getChannel() == 1
             && message.getNoteNumber() == 64)
            sawBentAttack64 = true;
    }

    expect (sawBentAttack64
             && spuriousCentresBefore64 == 0,
            "CC121 forgot a held note and centered a continued phrase");

    // Soltar as duas teclas reais deve encerrar a frase normalmente; só então
    // a próxima nota recebe o centro que remove o bend herdado de 9000.
    pitchSource.clear();
    pitchSource.addEvent (
        juce::MidiMessage::noteOff (
            1, 64), 0);
    pitchSource.addEvent (
        juce::MidiMessage::noteOff (
            1, 60), 1);
    pitchSource.addEvent (
        juce::MidiMessage::noteOn (
            1, 65, (juce::uint8) 100), 3);
    pitchGuard.process (
        pitchSource, pitchOutput);

    int centresAfterRealRelease = 0;
    bool sawNextPhrase65 = false;

    for (const auto meta : pitchOutput)
    {
        const auto message = meta.getMessage();

        if (meta.samplePosition == 3
             && message.isPitchWheel()
             && message.getPitchWheelValue()
                  == ChurchDAWPerformanceMidiSafety::
                         PitchBendGuard::centreValue)
            ++centresAfterRealRelease;

        if (meta.samplePosition == 3
             && message.isNoteOn()
             && message.getChannel() == 1
             && message.getNoteNumber() == 65)
            sawNextPhrase65 = true;
    }

    expect (sawNextPhrase65
             && centresAfterRealRelease == 1,
            "held-note tracking did not close after the real releases");

    //==========================================================================
    // PAD CONTINUO ACIONADO POR CC — o "pad de borracha" (07/08/2026)
    //
    // NAO HA HARDWARE. Os pads sao dos clientes (SMC Mixer, SMC Pad, M-VAVE,
    // Akai, Korg, Behringer...) e o dono foi explicito: "tem que ser na
    // confianca, os clientes que vao testar". Entao a UNICA prova possivel e
    // esta: cada aparelho do mercado vira uma sequencia de bytes aqui dentro.
    //
    // A tentativa anterior de aceitar CC foi revertida justamente por nao ter
    // um unico teste — 1200 linhas de heuristica temporal que ninguem conseguia
    // defender. Se alguem quebrar qualquer uma das regras abaixo, este alvo
    // FALHA e a regressao aparece antes do domingo.
    //==========================================================================
    {
        using namespace ChurchDAWMidiMapping;
        using Modo = ModoDeBotaoCC;
        using Decisao = DecisaoDeBotaoCC;

        // Pequeno palco de mentira: o tom do Pad e um int, exatamente como o
        // atomico requestedKey do PadProcessor. As duas funcoes exercitadas
        // (decidirBotaoCC + tomDepoisDoBotao) sao as MESMAS que rodam no app.
        struct Palco
        {
            EstadoDeBotaoCC estado;
            Modo modo = Modo::alternar;
            int tomDoBotao = 7;          // "Pad Sol"
            int tomDoPad = -1;           // -1 = Pad calado
            unsigned int relogio = 0;
            int acoes = 0;

            Decisao mandar (int valor, unsigned int avancoMs = 150)
            {
                relogio += avancoMs;
                const auto d = decidirBotaoCC (estado, modo, valor, relogio);

                if (d != Decisao::nada)
                {
                    ++acoes;
                    tomDoPad = tomDepoisDoBotao (tomDoPad, tomDoBotao, d);
                }

                return d;
            }
        };

        //----------------------------------------------------------------------
        // A lista de CCs mapeaveis como ACAO e propria e mais permissiva que a
        // que protege parametro: e em CC 64 (sustain) que sai de fabrica boa
        // parte dos footswitches baratos, e com a lista antiga eles eram
        // silenciosamente inmapeaveis como botao.
        expect (isMappableAction (64) && isMappableAction (66) && isMappableAction (67),
                "footswitch de fabrica (CC 64/66/67) tem de ser mapeavel como acao");
        expect (! isMappableController (64),
                "a lista que protege PARAMETRO nao pode ter sido afrouxada junto");
        expect (! isMappableAction (0) && ! isMappableAction (6)
                 && ! isMappableAction (32) && ! isMappableAction (38)
                 && ! isMappableAction (98) && ! isMappableAction (101),
                "Bank Select / Data Entry / RPN nunca sao gesto de musico");
        expect (! isMappableAction (120) && ! isMappableAction (127)
                 && ! isMappableAction (-1) && ! isMappableAction (128),
                "Channel Mode e CC fora de faixa nunca viram botao");

        //----------------------------------------------------------------------
        // C8 — DESPEJO DE ESTADO NO BOOT / TROCA DE BANCO.
        // O FCB1010 despeja os CCs ao trocar de banco, e um footswitch de travar
        // deixado na posicao "ligado" manda um valor alto assim que o cabo entra.
        // Sem a guarda, o Pad Continuo comeca a tocar por cima da igreja sem
        // ninguem encostar em nada — e nao ha gesto humano para correlacionar.
        {
            Palco p;
            p.estado.armarSincronizacaoDeBoot (p.relogio);
            expect (p.mandar (127) == Decisao::nada,
                    "dump alto dentro da janela de boot nao pode acionar o Pad");
            expect (p.tomDoPad == -1, "despejo de boot ligou o Pad sozinho");
        }
        {
            // Mesma guarda no modo por nivel, que e onde ela e ainda mais
            // critica: ali um valor alto significa literalmente "ligado".
            Palco p; p.modo = Modo::seguirNivel;
            p.estado.armarSincronizacaoDeBoot (p.relogio);
            expect (p.mandar (127) == Decisao::nada && p.tomDoPad == -1,
                     "no modo por nivel o despejo de boot tambem nao pode ligar");
        }
        {
            Palco p;
            p.estado.armarSincronizacaoDeBoot (p.relogio);
            expect (p.mandar (127, kMsJanelaSincronizacaoDeBoot + 1)
                        == Decisao::alternar
                     && p.tomDoPad == 7,
                    "fora da janela de boot o primeiro toque deve agir");
        }

        //----------------------------------------------------------------------
        // C1 — PAD/PEDAL MOMENTANEO 127/0 (M-VAVE Chocolate, FCB1010 de fabrica,
        // LPD8, MPD218, SMC-PAD). Alterna no APERTO; o "soltei" nao faz nada.
        {
            Palco p;
            expect (p.mandar (127) == Decisao::alternar && p.tomDoPad == 7,
                    "pad momentaneo: o PRIMEIRO aperto tem de LIGAR o tom");
            expect (p.mandar (0) == Decisao::nada && p.tomDoPad == 7,
                    "FALHA FATAL: soltar o pad desligou o tom no meio do culto");
            expect (p.mandar (127) == Decisao::alternar && p.tomDoPad == -1,
                    "pad momentaneo: o aperto seguinte tem de DESLIGAR");
            expect (p.mandar (0) == Decisao::nada && p.tomDoPad == -1,
                    "o soltar nunca pode religar o tom");
        }
        {
            // Ciclo real Learn -> Concluir: o gesto usado para aprender e mudo,
            // mas seu nivel fisico permanece conhecido. O primeiro toque de
            // palco seguinte nao pode ser descartado.
            Palco p;
            p.relogio += 150;
            (void) decidirBotaoCC (
                p.estado, p.modo, 127, p.relogio);   // aprender (descartado)
            p.relogio += 150;
            (void) decidirBotaoCC (
                p.estado, p.modo, 0, p.relogio);     // release ainda no Learn

            expect (p.acoes == 0 && p.tomDoPad == -1,
                    "gesto de aprendizado precisa permanecer mudo");
            expect (p.mandar (127) == Decisao::alternar
                        && p.tomDoPad == 7,
                    "primeiro toque apos Concluir precisa agir");
        }

        //----------------------------------------------------------------------
        // PROVA ESTRUTURAL DA FALHA FATAL: no modo padrao, 200 apertos e soltas
        // de um pedal momentaneo NUNCA produzem um "desligar" vindo do release.
        // E o que garante que ninguem consiga reintroduzir o "so soa enquanto eu
        // seguro" por acidente.
        {
            Palco p;
            p.mandar (0);
            int desligamentosPeloRelease = 0;

            for (int i = 0; i < 200; ++i)
            {
                p.mandar (127, 400);
                if (p.mandar (0, 250) == Decisao::desligar)
                    ++desligamentosPeloRelease;
            }

            expect (desligamentosPeloRelease == 0,
                    "o modo padrao produziu 'desligar' a partir do soltar");
        }

        //----------------------------------------------------------------------
        // C2 — PAD COM TRAVA 127/0 (nanoPAD2 em Toggle, BeatStep em Toggle).
        // Marcado a mao no menu do tom, ele segue o nivel e casa com o LED.
        {
            Palco p; p.modo = Modo::seguirNivel;
            expect (p.mandar (127, 3000) == Decisao::ligar
                        && p.tomDoPad == 7,
                    "trava: primeiro valor alto liga");
            expect (p.mandar (0, 3000) == Decisao::desligar, "trava: 0 desliga");
            expect (p.mandar (127, 3000) == Decisao::ligar && p.tomDoPad == 7,
                    "trava: valor alto liga");
            expect (p.mandar (0, 3000) == Decisao::desligar && p.tomDoPad == -1,
                    "trava: valor baixo desliga");
        }
        {
            // E no modo PADRAO ele ainda funciona, so que a cada dois toques —
            // falha MACIA e deliberada. Isto esta testado de proposito: e o
            // preco documentado de nao adivinhar o tipo do aparelho.
            Palco p;
            expect (p.mandar (127, 3000) == Decisao::alternar
                        && p.tomDoPad == 7,
                    "modo padrao age no primeiro valor alto");
            expect (p.mandar (0, 3000) == Decisao::nada
                        && p.tomDoPad == 7,
                    "modo padrao ignora a descida com seguranca");
        }

        //----------------------------------------------------------------------
        // C3 — M-VAVE CHOCOLATE PLUS em toggle custom: "ligado" vale 1.
        // Este e o aparelho que a regra ">= 64 liga" deixaria COMPLETAMENTE
        // MORTO, com o pior sintoma possivel no telefone: "o pad nao faz nada".
        {
            Palco p; p.modo = Modo::seguirNivel;
            expect (p.mandar (1) == Decisao::ligar && p.tomDoPad == 7,
                    "M-VAVE: primeiro valor 1 liga");
            expect (p.mandar (0) == Decisao::desligar && p.tomDoPad == -1,
                    "M-VAVE: 0 desliga");
            expect (p.mandar (1) == Decisao::ligar && p.tomDoPad == 7,
                    "M-VAVE Chocolate Plus manda 1 para LIGADO — nao pode morrer");
        }
        {
            Palco p;
            expect (p.mandar (1) == Decisao::alternar && p.tomDoPad == 7,
                    "primeiro valor 1 conta como aperto no modo padrao");
            expect (p.mandar (0) == Decisao::nada && p.tomDoPad == 7,
                    "valor 0 permanece release no modo padrao");
        }

        //----------------------------------------------------------------------
        // C4 — PAD DE DISPARO: manda sempre 127 e NUNCA manda 0. Sem tratamento
        // proprio ele funcionaria uma vez so, porque nunca mais haveria borda.
        {
            Palco p;
            expect (p.mandar (127, 2000) == Decisao::alternar
                        && p.tomDoPad == 7,
                    "pad de disparo: primeira mensagem e o primeiro toque");
            expect (p.mandar (127, 2000) == Decisao::alternar
                        && p.tomDoPad == -1,
                    "pad de disparo: cada mensagem e um toque novo");
            expect (p.mandar (127, 2000) == Decisao::alternar
                        && p.tomDoPad == 7,
                    "pad de disparo: o toque seguinte liga novamente");
        }

        //----------------------------------------------------------------------
        // C5 — PAD SENSIVEL A FORCA (valor escalado pela batida). O toque LEVE
        // tem de funcionar: com limiar ">= 64" isso virava o classico "so
        // funciona se eu bater forte".
        {
            Palco p;
            expect (p.mandar (51) == Decisao::alternar && p.tomDoPad == 7,
                    "primeiro toque medio tem de acionar o Pad");
            p.mandar (0);
            expect (p.mandar (12) == Decisao::alternar && p.tomDoPad == -1,
                    "toque LEVE (valor 12) tem de acionar o Pad");
            p.mandar (0);
            expect (p.mandar (106) == Decisao::alternar && p.tomDoPad == 7,
                    "toque forte tem de acionar igual ao leve");
        }

        //----------------------------------------------------------------------
        // C6 — DOIS VALORES FIXOS E NENHUM DELES E ZERO (ON=64 / OFF=63, gerado
        // por editores como o do BeatStep). Toda logica baseada em "zero = soltei"
        // quebra aqui: a regra "todo valor > 0 alterna" dispararia no aperto E no
        // soltar, os dois se cancelariam e O PAD NUNCA LIGARIA — falha total e
        // silenciosa. Por isso o alcance e APRENDIDO do proprio controle.
        {
            Palco p;                                    // C6b: momentaneo 64/63
            expect (p.mandar (64) == Decisao::alternar && p.tomDoPad == 7,
                    "pad 64/63: o primeiro aperto liga (nao existe zero aqui)");
            expect (p.mandar (63) == Decisao::nada && p.tomDoPad == 7,
                    "pad 64/63: o soltar nao pode cancelar o proprio aperto");
            expect (p.mandar (64) == Decisao::alternar && p.tomDoPad == -1,
                    "pad 64/63: o toque seguinte tem de desligar");
        }
        {
            Palco p; p.modo = Modo::seguirNivel;        // C6a: com trava 64/63
            expect (p.mandar (64, 3000) == Decisao::ligar && p.tomDoPad == 7,
                    "trava 64/63: primeiro 64 liga");
            expect (p.mandar (63, 3000) == Decisao::desligar && p.tomDoPad == -1,
                    "trava 64/63: 63 e o nivel desligado");
            expect (p.mandar (64, 3000) == Decisao::ligar && p.tomDoPad == 7,
                    "trava 64/63: 64 e o nivel ligado");
        }

        //----------------------------------------------------------------------
        // C7 — FLUXO DE PRESSAO DENTRO DE UM UNICO TOQUE. Sem a zona morta, cada
        // oscilacao cruzaria um limiar unico e o tom ligaria/desligaria varias
        // vezes por segundo, com o dedo parado em cima do pad.
        {
            Palco p;
            p.mandar (0);                                          // repouso

            const int rampa[] = { 5, 10, 31, 64, 111, 100, 96, 90 };
            const int antes = p.acoes;

            for (int v : rampa)
                p.mandar (v, 35);

            expect (p.acoes - antes == 1,
                    "fluxo de pressao num toque so pode produzir UMA acao");
            expect (p.tomDoPad == 7, "o toque de pressao tem de ter ligado o tom");
        }

        //----------------------------------------------------------------------
        // VIGIA DE "CARA DE KNOB". Um knob/fader/pedal de expressao encostado com
        // o alvo armado criaria o botao do Pad em silencio, e no culto cada
        // movimento daquele controle alternaria o tom.
        {
            EstadoDeBotaoCC knob;
            for (int v = 0; v <= 127; v += 7)
                (void) decidirBotaoCC (knob, Modo::alternar, v, (unsigned int) v);

            expect (knob.pareceKnob(),
                    "um knob varrido tem de ser reconhecido como controle continuo");
        }
        {
            // Mas um pad sensivel a forca manda uma velocity diferente por
            // batida. Desmapear por causa disso seria pior que o problema.
            EstadoDeBotaoCC padDeForca;
            const int batidas[] = { 90, 0, 51, 0, 118, 0 };
            unsigned int t = 0;

            for (int v : batidas)
                (void) decidirBotaoCC (padDeForca, Modo::alternar, v, t += 200);

            expect (! padDeForca.pareceKnob(),
                    "pad sensivel a forca nao pode ser confundido com knob");
        }

        //----------------------------------------------------------------------
        // ANTI-REPIQUE. Nao classifica nada: so impede que um pad que reenvia
        // valores conte como varios toques. O pior que ele pode fazer e engolir
        // uma acao — nunca inverter estado.
        {
            Palco p;
            p.mandar (0);
            expect (p.mandar (127, 500) == Decisao::alternar, "primeiro aperto");
            p.mandar (0, 10);
            expect (p.mandar (127, 10) == Decisao::nada,
                    "duas acoes em 20 ms nao sao dois toques humanos");
            expect (p.tomDoPad == 7, "o repique nao pode desligar o tom");
        }

        //----------------------------------------------------------------------
        // REQUISITO 6 — O PAD MUDA DE TOM POR OUTRO CAMINHO (cena, tela, tecla).
        // Nao existe estado de liga/desliga guardado no mapa de CC: o alternar e
        // resolvido a partir do tom que o Pad TEM AGORA.
        {
            Palco p;
            p.mandar (0);
            p.mandar (127);                                  // pad de borracha liga Sol
            expect (p.tomDoPad == 7, "o pad tinha de ter ligado em Sol");

            p.tomDoPad = -1;   // <- a CENA calou o Pad, sem passar pelo pad fisico
            p.mandar (0);
            expect (p.mandar (127) == Decisao::alternar && p.tomDoPad == 7,
                    "depois da cena calar o Pad, o pad de borracha tem de LIGAR");

            p.tomDoPad = 2;    // <- a CENA pos o Pad em Re
            p.mandar (0);
            p.mandar (127);
            expect (p.tomDoPad == 7,
                    "com o Pad em outro tom, o pad de 'Sol' liga em Sol — nao desliga");
        }
        {
            // A mesma garantia, agora direto na regra pura.
            expect (tomDepoisDoBotao (-1, 7, Decisao::alternar) == 7, "calado -> liga");
            expect (tomDepoisDoBotao (7, 7, Decisao::alternar) == -1, "no tom -> cala");
            expect (tomDepoisDoBotao (2, 7, Decisao::alternar) == 7, "noutro tom -> troca");
            expect (tomDepoisDoBotao (2, 7, Decisao::desligar) == 2,
                    "desligar um tom que nao esta soando nao pode calar o Pad");
            expect (tomDepoisDoBotao (5, 7, Decisao::nada) == 5, "'nada' nao mexe em nada");
        }

        //----------------------------------------------------------------------
        // ESQUECER ESTADO NAO PODE SACRIFICAR O PROXIMO GESTO. A excecao de
        // boot e testada acima e precisa ser armada explicitamente.
        {
            Palco p;
            p.mandar (0); p.mandar (127);
            expect (p.tomDoPad == 7, "estado inicial do teste");

            p.estado.esquecer();
            expect (p.mandar (127, 5000) == Decisao::alternar
                        && p.tomDoPad == -1,
                    "depois de esquecer, o primeiro gesto age sem segundo toque");
        }

        //----------------------------------------------------------------------
        // O MESMO CC EM DOIS CANAIS / DOIS APARELHOS SAO CONTROLES DIFERENTES.
        // Um pad M-VAVE costuma aparecer em duas portas (BLE e USB) ao mesmo
        // tempo; se as duas virassem a MESMA chave, os dois toggles se
        // cancelariam e o Pad nunca ligaria.
        {
            const ControllerKey a { "midi:porta-ble", 1, 20, false };
            const ControllerKey b { "midi:porta-usb", 1, 20, false };
            const ControllerKey c { "midi:porta-ble", 2, 20, false };
            const ControllerKey nota { "midi:porta-ble", 1, 20, true };

            expect ((a < b) || (b < a), "portas diferentes = controles diferentes");
            expect ((a < c) || (c < a), "canais diferentes = controles diferentes");
            expect ((a < nota) || (nota < a),
                    "CC 20 e NOTA 20 nunca podem ser a mesma entrada de mapa");
            expect (! (a < a), "a mesma chave nao pode ser menor que ela mesma");

            std::map<ControllerKey, int> mapa;
            mapa[a] = 1; mapa[b] = 2; mapa[c] = 3; mapa[nota] = 4;
            expect (mapa.size() == 4,
                    "o mesmo CC mapeado em portas/canais/tipos distintos gera 4 entradas");
        }

        //----------------------------------------------------------------------
        // DOIS APARELHOS COMANDANDO O MESMO TOM, ALTERNADOS. Cada um tem estado
        // proprio; o tom do Pad e a unica coisa compartilhada.
        {
            Palco a, b;
            a.mandar (0);
            b.mandar (0);

            a.mandar (127);
            b.tomDoPad = a.tomDoPad;       // o Pad e um so
            expect (b.tomDoPad == 7, "o primeiro pad ligou o tom");

            b.mandar (127);
            expect (b.tomDoPad == -1,
                    "o segundo pad tem de desligar o tom que o primeiro ligou");
        }

        //----------------------------------------------------------------------
        // DURANTE UMA TRANSACAO DE MAPA, NENHUMA ACAO PODE SER COALESCIDA.
        // Todos os padKey compartilham um unico estado global, portanto ate
        // eventos adjacentes do mesmo botao dependem do tom que veio antes.
        {
            DeferredMidiActionQueue<64> queue;
            expect (queue.push (1, 0, Decisao::alternar)
                        && queue.push (1, 1, Decisao::alternar)
                        && queue.push (1, 0, Decisao::alternar),
                    "A/B/A nao coube na fila diferida vazia");

            std::array<DeferredMidiActionQueue<64>::Output, 64> events {};
            const auto count = queue.copyTo (events);
            int pad = -1;
            for (size_t i = 0; i < count; ++i)
                pad = tomDepoisDoBotao (
                    pad, events[i].argument, events[i].decision);
            expect (count == 3
                        && events[0].argument == 0
                        && events[1].argument == 1
                        && events[2].argument == 0
                        && pad == 0,
                    "A alternar/B alternar/A alternar perdeu ordem ou estado final");
        }
        {
            DeferredMidiActionQueue<64> queue;
            expect (queue.push (1, 0, Decisao::ligar)
                        && queue.push (1, 0, Decisao::desligar),
                    "A ligar/A desligar nao coube na fila diferida vazia");

            std::array<DeferredMidiActionQueue<64>::Output, 64> events {};
            const auto count = queue.copyTo (events);
            int pad = 1; // B ja estava soando antes da transacao.
            for (size_t i = 0; i < count; ++i)
                pad = tomDepoisDoBotao (
                    pad, events[i].argument, events[i].decision);
            expect (count == 2 && pad == -1,
                    "B + A ligar/A desligar nao terminou em OFF");
        }
        {
            DeferredMidiActionQueue<64> queue;
            expect (queue.push (3, 4, Decisao::alternar)
                        && queue.push (4, -1, Decisao::ligar)
                        && queue.push (3, 2, Decisao::desligar),
                    "one-shots intercalados nao entraram na fila");
            std::array<DeferredMidiActionQueue<64>::Output, 64> events {};
            expect (queue.copyTo (events) == 3
                        && events[0].actionKind == 3
                        && events[0].argument == 4
                        && events[1].actionKind == 4
                        && events[2].actionKind == 3
                        && events[2].argument == 2
                        && events[0].sequence < events[1].sequence
                        && events[1].sequence < events[2].sequence,
                    "one-shots foram agrupados ou movidos para outra sequencia");
        }
        {
            DeferredMidiActionQueue<64> queue;
            for (int i = 0; i < 64; ++i)
                expect (queue.push (1, i % 12, Decisao::alternar),
                        "fila diferida recusou antes da capacidade 64");
            expect (! queue.push (1, 11, Decisao::desligar)
                        && queue.overflowed()
                        && queue.size() == 64,
                    "evento 65 sobrescreveu a fila ou nao armou overflow");
            std::array<DeferredMidiActionQueue<64>::Output, 64> events {};
            expect (queue.copyTo (events) == 64
                        && events.front().argument == 0
                        && events.front().sequence == 1
                        && events.back().argument == (63 % 12)
                        && events.back().sequence == 64,
                    "overflow alterou decisoes acionaveis ja aceitas");
        }
    }

    // ======================================================================
    // BLOCO 2 — despacho de CC por rotas fixas, fila de Learn, mailbox MPSC,
    // comando stale e identidade estavel de linha do painel.

    // A1) Tabela publicada + latest-wins: rajada maior que a capacidade
    // fisica coalesce na celula da rota; o consumidor ve apenas o valor final
    // e a fila nao cresce.
    {
        ChurchDAWMidiMapping::FixedControllerRouteDispatcher<4, 32> routes;
        using Result = ChurchDAWMidiMapping::ControllerRoutePublishResult;
        const char* fader = "midi:fader-a";
        const auto faderLen = std::strlen (fader);

        routes.beginRouteUpdate();
        const auto faderGen =
            routes.publishRoute (0, fader, faderLen, 1, 7, false);
        expect (faderGen != 0, "rota valida de fader nao publicou");
        routes.finishRouteUpdate (42, 1, 1);

        for (int value = 0; value <= 99; ++value)
            expect (routes.tryPublish (fader, faderLen, 1, 7, false,
                                       value, 42)
                        == Result::published,
                    "rajada de CC continuo foi recusada com tabela atual");
        expect (routes.coalescedCount() == 99,
                "rajada nao coalesceu na celula latest-wins");

        ChurchDAWMidiMapping::ControllerRouteEvent event;
        expect (routes.tryConsumeNext (event)
                    && event.routeSlot == 0
                    && event.value == 99
                    && event.routeGeneration == faderGen
                    && event.mappingGeneration == 42,
                "consumidor nao recebeu apenas o ultimo valor da rota");
        expect (! routes.tryConsumeNext (event),
                "fila cresceu alem da celula unica por rota");

        // CC sem rota nao publica nem conta como mapeado.
        const char* estranho = "midi:desconhecido";
        expect (routes.tryPublish (estranho, std::strlen (estranho), 1, 7,
                                   false, 10, 42)
                    == Result::notMapped
                    && ! routes.matchesActiveRoute (
                           estranho, std::strlen (estranho), 1, 7, false),
                "CC sem mapeamento foi aceito pela tabela de rotas");
    }

    // A2) Ordem global entre rotas distintas: o consumidor drena pela
    // publicationSequence, nao pela posicao do slot.
    {
        ChurchDAWMidiMapping::FixedControllerRouteDispatcher<4, 32> routes;
        using Result = ChurchDAWMidiMapping::ControllerRoutePublishResult;
        const char* teclado = "midi:teclado";
        const auto tecladoLen = std::strlen (teclado);

        routes.beginRouteUpdate();
        expect (routes.publishRoute (0, teclado, tecladoLen, 1, 7, false) != 0
                    && routes.publishRoute (1, teclado, tecladoLen, 1, 11,
                                            false) != 0,
                "duas rotas nao publicaram");
        routes.finishRouteUpdate (7, 2, 2);

        // Publica primeiro na rota do slot MAIOR para provar que a ordem vem
        // da sequencia global e nao do indice.
        expect (routes.tryPublish (teclado, tecladoLen, 1, 11, false, 40, 7)
                        == Result::published
                    && routes.tryPublish (teclado, tecladoLen, 1, 7, false,
                                          80, 7)
                        == Result::published,
                "publicacao em duas rotas falhou");

        ChurchDAWMidiMapping::ControllerRouteEvent first, second, none;
        expect (routes.tryConsumeNext (first)
                    && routes.tryConsumeNext (second)
                    && ! routes.tryConsumeNext (none),
                "drenagem de duas rotas nao devolveu exatamente dois eventos");
        expect (first.routeSlot == 1 && first.value == 40
                    && second.routeSlot == 0 && second.value == 80
                    && first.publicationSequence < second.publicationSequence,
                "eventos de rotas distintas sairam fora da ordem global");
    }

    // A3) Geracao stale nunca publica, mas o membership sobrevive a janela de
    // republicacao: um fader mapeado continua cedendo ao mapa do usuario
    // enquanto a tabela e reconstruida, e some depois que a rota e removida.
    {
        ChurchDAWMidiMapping::FixedControllerRouteDispatcher<4, 32> routes;
        using Result = ChurchDAWMidiMapping::ControllerRoutePublishResult;
        const char* fader = "midi:fader-b";
        const auto faderLen = std::strlen (fader);

        routes.beginRouteUpdate();
        expect (routes.publishRoute (0, fader, faderLen, 2, 21, false) != 0,
                "rota do fader-b nao publicou");
        routes.finishRouteUpdate (5, 1, 1);

        expect (routes.tryPublish (fader, faderLen, 2, 21, false, 3, 4)
                    == Result::staleTable,
                "geracao de mapa divergente publicou mesmo assim");

        routes.beginRouteUpdate(); // republicacao em andamento
        expect (routes.tryPublish (fader, faderLen, 2, 21, false, 3, 6)
                    == Result::staleTable,
                "publicou durante a republicacao da tabela");
        expect (routes.matchesActiveRoute (fader, faderLen, 2, 21, false),
                "membership do fader morreu durante a republicacao — a "
                "superficie de fabrica dispararia no meio do culto");
        expect (! routes.hasRoute (fader, faderLen, 2, 21, false, 6),
                "hasRoute ignorou a geracao da tabela");

        // A republicacao termina SEM a rota: o membership entao desaparece.
        routes.finishRouteUpdate (6, 0, 0);
        expect (! routes.matchesActiveRoute (fader, faderLen, 2, 21, false),
                "rota removida continuou respondendo apos a republicacao");
        expect (routes.routeGeneration (0) == 0,
                "slot da rota removida nao foi aposentado");
    }

    // A4) Evento pendente que atravessa uma republicacao carrega a geracao
    // antiga da rota: o consumidor real compara com a geracao atual do slot e
    // descarta, entao um valor velho nunca e aplicado como atual.
    {
        ChurchDAWMidiMapping::FixedControllerRouteDispatcher<4, 32> routes;
        using Result = ChurchDAWMidiMapping::ControllerRoutePublishResult;
        const char* fader = "midi:fader-c";
        const auto faderLen = std::strlen (fader);

        routes.beginRouteUpdate();
        const auto oldGen =
            routes.publishRoute (0, fader, faderLen, 1, 30, false);
        routes.finishRouteUpdate (9, 1, 1);
        expect (routes.tryPublish (fader, faderLen, 1, 30, false, 55, 9)
                    == Result::published,
                "publicacao pre-republicacao falhou");

        routes.beginRouteUpdate();
        const auto newGen =
            routes.publishRoute (0, fader, faderLen, 1, 30, false);
        routes.finishRouteUpdate (10, 1, 1);
        expect (newGen != 0 && newGen != oldGen,
                "republicar a mesma rota nao trocou a geracao do slot");

        ChurchDAWMidiMapping::ControllerRouteEvent event;
        expect (routes.tryConsumeNext (event)
                    && event.routeGeneration == oldGen
                    && event.routeGeneration
                           != routes.routeGeneration (event.routeSlot)
                    && event.mappingGeneration != 10,
                "evento pendente antigo passaria na validacao do "
                "consumidor apos a republicacao");

        // Slot republicado segue utilizavel na geracao nova.
        expect (routes.tryPublish (fader, faderLen, 1, 30, false, 77, 10)
                    == Result::published,
                "slot republicado nao aceitou publicacao nova");
        expect (routes.tryConsumeNext (event)
                    && event.value == 77
                    && event.routeGeneration == newGen,
                "publicacao pos-republicacao nao chegou ao consumidor");
    }

    // A5) Overflow fisico: mais rotas que a capacidade arma o latch e conta
    // um episodio; as rotas que couberam seguem publicando (fail-open).
    {
        ChurchDAWMidiMapping::FixedControllerRouteDispatcher<2, 32> routes;
        using Result = ChurchDAWMidiMapping::ControllerRoutePublishResult;
        const char* base = "midi:overflow";
        const auto baseLen = std::strlen (base);

        routes.beginRouteUpdate();
        size_t published = 0;
        for (int cc = 1; cc <= 3; ++cc)
            if (published < 2
                 && routes.publishRoute (published, base, baseLen, 1, cc,
                                         false) != 0)
                ++published;
        routes.finishRouteUpdate (3, 3, published);

        expect (routes.overflowed()
                    && routes.overflowEpisodeCount() == 1,
                "overflow de capacidade fisica nao armou o latch/episodio");
        expect (routes.tryPublish (base, baseLen, 1, 1, false, 9, 3)
                    == Result::published,
                "rota dentro da capacidade parou de publicar no overflow");
        expect (routes.tryPublish (base, baseLen, 1, 3, false, 9, 3)
                    == Result::notMapped,
                "rota excedente publicou alem da capacidade fisica");

        // Republicar dentro da capacidade desarma o latch sem novo episodio.
        routes.beginRouteUpdate();
        expect (routes.publishRoute (0, base, baseLen, 1, 1, false) != 0,
                "republicacao pos-overflow falhou");
        routes.finishRouteUpdate (4, 1, 1);
        expect (! routes.overflowed()
                    && routes.overflowEpisodeCount() == 1,
                "latch de overflow nao desarmou apos caber na capacidade");
    }

    // A6) Prova estrutural: a publicacao do callback e POD puro.
    static_assert (
        std::is_trivially_copyable<
            ChurchDAWMidiMapping::ControllerRouteEvent>::value,
        "evento de rota publicado pelo callback MIDI deve permanecer POD");

    // A7) Soak com dois produtores e um consumidor: sequencias consumidas
    // estritamente crescentes (ordem global) e contabilidade exata. Uma
    // tentativa mais nova que falha por contencao invalida o pending antigo:
    // o valor cru mais novo passa e nenhum valor velho atrasado e aplicado.
    {
        ChurchDAWMidiMapping::FixedControllerRouteDispatcher<2, 32> routes;
        using Result = ChurchDAWMidiMapping::ControllerRoutePublishResult;
        const char* fonte = "midi:soak";
        const auto fonteLen = std::strlen (fonte);

        routes.beginRouteUpdate();
        expect (routes.publishRoute (0, fonte, fonteLen, 1, 7, false) != 0
                    && routes.publishRoute (1, fonte, fonteLen, 1, 11,
                                            false) != 0,
                "rotas do soak nao publicaram");
        routes.finishRouteUpdate (1, 2, 2);

        constexpr int publishesPerProducer = 4000;
        std::atomic<juce::uint64> acceptedTotal { 0 };
        std::atomic<int> finishedProducers { 0 };

        auto producer = [&] (int cc)
        {
            for (int i = 0; i < publishesPerProducer; ++i)
            {
                const int value = i % 128;
                if (routes.tryPublish (fonte, fonteLen, 1, cc, false,
                                       value, 1)
                        == Result::published)
                    acceptedTotal.fetch_add (1, std::memory_order_relaxed);
            }
            finishedProducers.fetch_add (1, std::memory_order_release);
        };

        juce::uint64 consumed = 0;
        juce::uint64 lastSequence = 0;
        int lastValueCC7 = -1;
        int lastValueCC11 = -1;
        bool orderBroken = false;
        auto drain = [&]
        {
            ChurchDAWMidiMapping::ControllerRouteEvent event;
            while (routes.tryConsumeNext (event))
            {
                if (event.publicationSequence <= lastSequence)
                    orderBroken = true;
                lastSequence = event.publicationSequence;
                (event.routeSlot == 0 ? lastValueCC7 : lastValueCC11) =
                    event.value;
                ++consumed;
            }
        };

        std::thread first (producer, 7);
        std::thread second (producer, 11);
        while (finishedProducers.load (std::memory_order_acquire) < 2)
        {
            drain();
            std::this_thread::yield();
        }
        first.join();
        second.join();
        drain();

        expect (! orderBroken,
                "sequencias consumidas sairam fora da ordem global");
        expect (consumed + routes.coalescedCount()
                    + routes.invalidatedPendingCount()
                    == acceptedTotal.load (std::memory_order_relaxed),
                "contabilidade consumido+coalescido+invalidado != aceito");

        // Mesmo depois de contencao/invalidation, as duas celulas precisam ser
        // reutilizaveis e latest-wins para uma publicacao confirmada.
        expect (routes.tryPublish (
                    fonte, fonteLen, 1, 7, false, 123, 1)
                        == Result::published
                    && routes.tryPublish (
                        fonte, fonteLen, 1, 11, false, 124, 1)
                        == Result::published,
                "rotas nao foram reutilizaveis depois do soak");
        drain();
        expect (lastValueCC7 == 123 && lastValueCC11 == 124,
                "sentinelas finais nao respeitaram latest-wins");
    }

    // L1) Fila fixa de Learn: FIFO exato, overflow com latch fail-open,
    // reutilizacao apos clear e recusa de origem acima do limite fisico.
    {
        ChurchDAWMidiMapping::FixedMidiLearnControlQueue<4, 16, 16> learnQueue;
        using LearnEvent =
            ChurchDAWMidiMapping::FixedMidiLearnControlQueue<4, 16, 16>::Event;

        const char* origem = "midi:learn";
        const auto origemLen = std::strlen (origem);
        const char* nome = "Teclado";
        const auto nomeLen = std::strlen (nome);

        for (int i = 0; i < 4; ++i)
            expect (learnQueue.tryPublish (origem, origemLen, nome, nomeLen,
                                           1 + i, 20 + i, 64, i % 2 == 0,
                                           i == 3, 77),
                    "fila de Learn recusou antes da capacidade");
        expect (! learnQueue.tryPublish (origem, origemLen, nome, nomeLen,
                                         5, 24, 64, false, false, 77)
                    && learnQueue.overflowed()
                    && learnQueue.droppedCount() == 1,
                "quinto gesto nao caiu em fail-open com latch de overflow");

        LearnEvent event;
        for (int i = 0; i < 4; ++i)
        {
            expect (learnQueue.tryConsume (event),
                    "fila de Learn perdeu um gesto aceito");
            expect (static_cast<int> (event.channel) == 1 + i
                        && static_cast<int> (event.number) == 20 + i
                        && event.isNote == (i % 2 == 0)
                        && event.unmapOnly == (i == 3)
                        && event.targetGeneration == 77
                        && static_cast<size_t> (event.sourceIdLength)
                               == origemLen
                        && std::memcmp (event.sourceId.data(), origem,
                                        origemLen) == 0,
                    "gesto de Learn saiu corrompido ou fora de ordem");
        }
        expect (! learnQueue.tryConsume (event),
                "fila de Learn devolveu gesto inexistente");

        learnQueue.clear();
        expect (! learnQueue.overflowed(),
                "clear nao desarmou o latch de overflow do Learn");
        expect (learnQueue.tryPublish (origem, origemLen, nome, nomeLen,
                                       2, 40, 10, false, false, 78)
                    && learnQueue.tryConsume (event)
                    && static_cast<int> (event.number) == 40,
                "fila de Learn nao reutilizou apos clear");

        const char* longa = "midi:um-source-id-bem-maior-que-16-bytes";
        expect (! learnQueue.tryPublish (longa, std::strlen (longa), nome,
                                         nomeLen, 1, 21, 64, false, false, 78),
                "origem acima do limite fisico entrou na fila de Learn");
        expect (ChurchDAWMidiMapping::
                    midiLearnPublicationConsumesRawInput (
                        true, true, false)
                 && ChurchDAWMidiMapping::
                    midiLearnPublicationConsumesRawInput (
                        true, false, true)
                 && ! ChurchDAWMidiMapping::
                    midiLearnPublicationConsumesRawInput (
                        true, false, false)
                 && ! ChurchDAWMidiMapping::
                    midiLearnPublicationConsumesRawInput (
                        false, true, true),
                "overflow global virou membership de uma nota desconhecida");
    }

    // G1) Mailbox MPSC de parametro: ordem deterministica, full fail-open,
    // reutilizacao, filtro de geracao stale e "ultimo aceito" observavel.
    {
        ChurchDAWMidiMapping::ParameterTouchMailbox<4> mailbox;
        int fakeProcessorA = 0;
        int fakeProcessorB = 0;

        juce::uint64 accepted = 0;
        expect (mailbox.tryPublish (&fakeProcessorA, 3, 10, 20, &accepted)
                    && accepted == 1
                    && mailbox.tryPublish (&fakeProcessorB, 5, 10, 20,
                                           &accepted)
                    && accepted == 2,
                "mailbox nao aceitou publicacoes iniciais em ordem");

        ChurchDAWMidiMapping::ParameterTouchEvent event;
        expect (mailbox.tryConsume (event)
                    && event.processor
                           == reinterpret_cast<std::uintptr_t> (
                                  &fakeProcessorA)
                    && event.parameterIndex == 3
                    && event.publicationSequence == 1,
                "mailbox inverteu a ordem de consumo");
        expect (mailbox.tryConsume (event)
                    && event.parameterIndex == 5
                    && event.publicationSequence == 2,
                "segundo toque nao saiu na ordem de aceitacao");
        expect (! mailbox.tryConsume (event),
                "mailbox devolveu toque inexistente");

        expect (ChurchDAWMidiMapping::parameterTouchBelongsToGeneration (
                    event, 10, 20)
                    && ! ChurchDAWMidiMapping::parameterTouchBelongsToGeneration (
                           event, 11, 20)
                    && ! ChurchDAWMidiMapping::parameterTouchBelongsToGeneration (
                           event, 10, 21),
                "filtro de geracao stale do mailbox falhou");

        for (int i = 0; i < 4; ++i)
            expect (mailbox.tryPublish (&fakeProcessorA, i, 10, 20, nullptr),
                    "mailbox recusou antes da capacidade");
        expect (! mailbox.tryPublish (&fakeProcessorA, 9, 10, 20, nullptr)
                    && mailbox.droppedCount() == 1,
                "mailbox cheio nao caiu em fail-open");
        expect (mailbox.tryConsume (event) && event.parameterIndex == 0,
                "drenagem apos full perdeu o toque mais antigo");
        expect (mailbox.tryPublish (&fakeProcessorA, 9, 10, 20, nullptr),
                "slot liberado nao foi reutilizado");
        mailbox.clear();
        expect (! mailbox.tryConsume (event),
                "clear nao drenou o mailbox");
        expect (! mailbox.tryPublish (nullptr, 1, 10, 20, nullptr)
                    && ! mailbox.tryPublish (&fakeProcessorA, -1, 10, 20,
                                             nullptr),
                "mailbox aceitou processador nulo ou indice invalido");
    }

    // G2) Dois produtores concorrentes: nenhum toque aceito e perdido, o
    // consumo respeita a ordem global de aceitacao (sequencias estritamente
    // crescentes) e a ordem de cada produtor e preservada.
    {
        ChurchDAWMidiMapping::ParameterTouchMailbox<64> mailbox;
        constexpr int touchesPerProducer = 3000;
        int processorA = 0;
        int processorB = 0;
        std::atomic<juce::uint64> acceptedCount { 0 };
        std::atomic<int> finishedProducers { 0 };

        auto producer = [&] (int* processor, std::vector<juce::uint64>& mine)
        {
            for (int i = 0; i < touchesPerProducer; ++i)
            {
                juce::uint64 sequence = 0;
                if (mailbox.tryPublish (processor, i, 1, 1, &sequence))
                {
                    mine.push_back (sequence);
                    acceptedCount.fetch_add (1, std::memory_order_relaxed);
                }
                if ((i & 63) == 0)
                    std::this_thread::yield();
            }
            finishedProducers.fetch_add (1, std::memory_order_release);
        };

        std::vector<juce::uint64> sequencesA, sequencesB;
        sequencesA.reserve (touchesPerProducer);
        sequencesB.reserve (touchesPerProducer);

        juce::uint64 consumedCount = 0;
        juce::uint64 lastSequence = 0;
        bool globalOrderBroken = false;
        int lastIndexA = -1;
        int lastIndexB = -1;
        bool producerOrderBroken = false;
        auto drain = [&]
        {
            ChurchDAWMidiMapping::ParameterTouchEvent event;
            while (mailbox.tryConsume (event))
            {
                if (event.publicationSequence <= lastSequence)
                    globalOrderBroken = true;
                lastSequence = event.publicationSequence;
                auto& lastIndex = event.processor
                        == reinterpret_cast<std::uintptr_t> (&processorA)
                    ? lastIndexA : lastIndexB;
                if (event.parameterIndex <= lastIndex)
                    producerOrderBroken = true;
                lastIndex = event.parameterIndex;
                ++consumedCount;
            }
        };

        std::thread first (producer, &processorA, std::ref (sequencesA));
        std::thread second (producer, &processorB, std::ref (sequencesB));
        while (finishedProducers.load (std::memory_order_acquire) < 2)
        {
            drain();
            std::this_thread::yield();
        }
        first.join();
        second.join();
        drain();

        expect (consumedCount == acceptedCount.load(),
                "toque aceito pelo mailbox foi perdido no consumo");
        expect (! globalOrderBroken,
                "consumo do mailbox violou a ordem global de aceitacao");
        expect (! producerOrderBroken,
                "ordem por produtor foi invertida no mailbox");
        expect (std::is_sorted (sequencesA.begin(), sequencesA.end())
                    && std::is_sorted (sequencesB.begin(), sequencesB.end()),
                "sequencias aceitas de um produtor nao sao crescentes");
    }

    // B1) Politica stale de comando de parametro: qualquer divergencia entre o
    // que o painel mostrou e o estado atual falha FECHADO antes de gravar.
    {
        namespace M = ChurchDAWMidiMapping;
        using Validation = M::ParameterCommandValidation;

        const M::PersistedCCTarget alvoA { 2, 7, -1, "Cutoff", "cutoff-a",
                                           "vst3:kontakt:111" };
        const M::ControllerKey chave { "midi:teclado", 1, 74, false,
                                       "Teclado" };

        M::ParameterCommandSnapshot esperado;
        esperado.selectedTarget = alvoA;
        esperado.expectedCurrentPluginId = "vst3:kontakt:111";
        esperado.expectedEntryPresent = true;
        esperado.persistedControl = chave;
        esperado.persistedTarget = alvoA;

        M::ParameterCommandObservation atual;
        atual.selectedTarget = alvoA;
        atual.currentPluginId = "vst3:kontakt:111";
        atual.entryPresent = true;
        atual.persistedControl = chave;
        atual.persistedTarget = alvoA;

        expect (M::validateParameterCommandSnapshot (esperado, atual)
                    == Validation::valid,
                "comando identico ao estado atual foi rejeitado");

        // Plugin C ocupou o mesmo slot com o MESMO parameterId antes do save.
        auto pluginTrocado = atual;
        pluginTrocado.currentPluginId = "vst3:outro:999";
        expect (M::validateParameterCommandSnapshot (esperado, pluginTrocado)
                    == Validation::currentProcessorChanged,
                "troca de plugin no mesmo slot passou pela validacao stale");

        // Snapshot sem identidade esperada nunca autoriza mutacao.
        auto semIdentidade = esperado;
        semIdentidade.expectedCurrentPluginId.clear();
        expect (M::validateParameterCommandSnapshot (semIdentidade, atual)
                    == Validation::currentProcessorChanged,
                "snapshot sem identidade esperada foi aceito (fail-open)");

        auto alvoTrocado = atual;
        alvoTrocado.selectedTarget.param = 8;
        expect (M::validateParameterCommandSnapshot (esperado, alvoTrocado)
                    == Validation::selectedTargetChanged,
                "alvo selecionado divergente passou pela validacao");

        auto entradaSumiu = atual;
        entradaSumiu.entryPresent = false;
        expect (M::validateParameterCommandSnapshot (esperado, entradaSumiu)
                    == Validation::entryPresenceChanged,
                "entrada persistida ausente nao foi detectada");

        auto chaveTrocada = atual;
        chaveTrocada.persistedControl.cc = 75;
        expect (M::validateParameterCommandSnapshot (esperado, chaveTrocada)
                    == Validation::persistedEntryChanged,
                "chave fisica trocada passou pela validacao");

        auto payloadTrocado = atual;
        payloadTrocado.persistedTarget.pluginId = "vst3:outro:999";
        expect (M::validateParameterCommandSnapshot (esperado, payloadTrocado)
                    == Validation::persistedEntryChanged,
                "payload persistido trocado passou pela validacao");
    }

    // C1) Identidade estavel de linha do painel: labels ficam de fora, duas
    // instancias com o mesmo nome continuam distintas, e a selecao restaurada
    // pelo algoritmo do painel (primeiro match) encontra a linha certa mesmo
    // apos reordenacao.
    {
        using Command = MidiMappingPanel::Command;

        auto kontaktRow = [] (int slot, const juce::String& pluginId)
        {
            Command row;
            row.name = "Cutoff";
            row.category = "Plugins";
            row.target = MidiMappingPanel::TargetKind::processorParameter;
            row.parameterSlot = slot;
            row.parameterIndex = 4;
            row.parameterId = "cutoff";
            row.parameterName = "Cutoff";
            row.expectedCurrentPluginId = pluginId;
            row.persistedParameterSlot = slot;
            row.persistedPluginId = pluginId;
            return row;
        };

        const auto primeira = kontaktRow (2, "vst3:kontakt:111");
        const auto segunda = kontaktRow (3, "vst3:kontakt:222");
        expect (! Command::sameStableIdentity (primeira, segunda),
                "duas instancias Kontakt com o mesmo rotulo colapsaram");
        expect (Command::sameStableIdentity (segunda, kontaktRow (
                    3, "vst3:kontakt:222")),
                "a mesma linha reconstruida nao casou consigo mesma");

        // Rotulo diferente com semantica igual continua a MESMA linha.
        auto renomeada = segunda;
        renomeada.name = "Cutoff (renomeado)";
        renomeada.device = "Outro texto de tela";
        renomeada.control = "Knob 9";
        renomeada.category = "Outra categoria";
        expect (Command::sameStableIdentity (segunda, renomeada),
                "rotulos de tela entraram na identidade semantica");

        // Algoritmo de restauracao do painel: primeiro match estavel.
        juce::Array<Command> reordenadas;
        reordenadas.add (kontaktRow (9, "vst3:kontakt:999"));
        reordenadas.add (primeira);
        reordenadas.add (segunda);
        int restaurada = -1;
        for (int i = 0; i < reordenadas.size(); ++i)
            if (Command::sameStableIdentity (reordenadas.getReference (i),
                                             segunda))
            {
                restaurada = i;
                break;
            }
        expect (restaurada == 2,
                "apos reordenacao a selecao saltou para outra instancia");

        // Placeholders do Church Live sem motor: mesmos campos semanticos,
        // distinguidos apenas pelo ordinal da linha.
        auto placeholder = [] (int ordinal)
        {
            Command row;
            row.target = MidiMappingPanel::TargetKind::processorParameter;
            row.argument = 0;
            row.parameterSlot = -1;
            row.rowDisambiguator = ordinal;
            return row;
        };
        expect (! Command::sameStableIdentity (placeholder (0),
                                               placeholder (1)),
                "linhas placeholder do Church Live colapsaram em uma so");
        expect (Command::sameStableIdentity (placeholder (1),
                                             placeholder (1)),
                "o ordinal quebrou a igualdade da propria linha");
    }

    std::cout << "MIDI_MAPPING_SAFETY_OK" << std::endl;
    return 0;
}
