#pragma once

#if defined(_WIN32) && ! defined(NOMINMAX)
 #define NOMINMAX 1
#endif

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_cryptography/juce_cryptography.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <map>
#include <limits>
#include <memory>
#include <set>
#include <type_traits>
#include <utility>
#include <vector>

#if JUCE_ANDROID || JUCE_MAC || JUCE_IOS || JUCE_LINUX || JUCE_BSD
 #include <fcntl.h>
 #include <sys/stat.h>
 #include <unistd.h>
#endif

#if JUCE_WINDOWS
 #include <windows.h>
#endif

namespace ChurchDAWMidiMapping
{
    inline juce::String normalizeMidiSourceId (juce::String sourceId)
    {
        sourceId = sourceId.trim();
        if (sourceId.isEmpty())
            return {};

        // Builds anteriores chegaram a persistir a porta virtual do Launchkey
        // depois de passar duas vezes pelo prefixador generico. Sem esta migracao
        // o mapa continua valido no XML, mas nunca casa com o callback DAW.
        if (sourceId.startsWithIgnoreCase ("midi:churchdaw:"))
            return "churchdaw:" + sourceId.substring (15);

        if (sourceId.startsWithIgnoreCase ("midi:"))
            return "midi:" + sourceId.substring (5);

        if (sourceId.startsWithIgnoreCase ("churchdaw:"))
            return "churchdaw:" + sourceId.substring (10);

        return "midi:" + sourceId;
    }

    struct MidiSourceDescriptor
    {
        juce::String identifier;
        juce::String name;
    };

    inline bool midiSourceNamesEqual (const juce::String& first,
                                      const juce::String& second)
    {
        const auto normalisedFirst = first.trim();
        return normalisedFirst.isNotEmpty()
            && normalisedFirst.equalsIgnoreCase (second.trim());
    }

    template <typename MappingMap>
    inline bool hasUnresolvedUnnamedMidiSource (
        const MappingMap& mappings,
        const std::vector<MidiSourceDescriptor>& available)
    {
        for (const auto& entry : mappings)
        {
            if (entry.first.sourceName.trim().isNotEmpty())
                continue;

            bool availableById = false;
            for (const auto& source : available)
                if (normalizeMidiSourceId (source.identifier)
                        == normalizeMidiSourceId (entry.first.sourceId))
                {
                    availableById = true;
                    break;
                }

            if (! availableById)
                return true;
        }

        return false;
    }

    inline juce::String sourceNameForPersistence (
        const juce::String& preservedName,
        const juce::String& currentlyAvailableName) noexcept
    {
        const auto trimmedPreserved = preservedName.trim();
        return trimmedPreserved.isNotEmpty()
            ? trimmedPreserved : currentlyAvailableName.trim();
    }

    inline juce::String resolveUniqueMidiSourceId (
        const juce::String& savedSourceId,
        const juce::String& sourceName,
        const std::vector<MidiSourceDescriptor>& available)
    {
        const auto normalizedSaved = normalizeMidiSourceId (savedSourceId);

        // Virtual ChurchDAW sources are logical endpoints, never aliases for
        // an enumerated OS device. A physical port with the same display name
        // must not steal Launchkey DAW mappings during re-key.
        if (normalizedSaved.startsWith ("churchdaw:"))
            return normalizedSaved;

        for (const auto& source : available)
            if (normalizeMidiSourceId (source.identifier) == normalizedSaved)
                return normalizedSaved;

        if (sourceName.trim().isEmpty())
            return normalizedSaved;

        juce::String unique;
        int matches = 0;
        for (const auto& source : available)
            if (midiSourceNamesEqual (source.name, sourceName))
            {
                unique = normalizeMidiSourceId (source.identifier);
                ++matches;
            }

        return matches == 1 ? unique : normalizedSaved;
    }

    struct MidiRekeyResult
    {
        int moved = 0;
        int ambiguous = 0;
        int collisions = 0;
    };

    template <typename MappingMap>
    inline bool hasMappingForSourceName (const MappingMap& mappings,
                                         const juce::String& sourceName,
                                         int channel,
                                         int cc,
                                         bool isNote)
    {
        for (const auto& entry : mappings)
            if (midiSourceNamesEqual (entry.first.sourceName, sourceName)
                 && entry.first.channel == channel
                 && entry.first.cc == cc
                 && entry.first.isNote == isNote)
                return true;
        return false;
    }

    template <typename PrimaryMap, typename SecondaryMap>
    inline bool shouldAddAutomaticMapping (
        const PrimaryMap& primary,
        const SecondaryMap& secondary,
        const juce::String& sourceId,
        const juce::String& sourceName,
        int channel,
        int cc,
        bool isNote)
    {
        const auto normalized = normalizeMidiSourceId (sourceId);
        const auto hasMapping = [&] (const auto& mappings)
        {
            for (const auto& entry : mappings)
                if (entry.first.channel == channel
                     && entry.first.cc == cc
                     && entry.first.isNote == isNote
                     && (normalizeMidiSourceId (entry.first.sourceId) == normalized
                         || (sourceName.isNotEmpty()
                             && midiSourceNamesEqual (
                                 entry.first.sourceName, sourceName))))
                    return true;
            return false;
        };

        return ! hasMapping (primary) && ! hasMapping (secondary);
    }

    // Reindexes a complete map from the old source id to the current id. The
    // input is never modified until the rebuilt copy is complete; collisions
    // retain the stale entry instead of dropping either mapping.
    template <typename MappingMap>
    inline MidiRekeyResult rekeyMidiMapByUniqueSourceName (
        MappingMap& mappings,
        const std::vector<MidiSourceDescriptor>& available)
    {
        MappingMap rebuilt;
        MidiRekeyResult result;

        const auto destinationToken = [] (const auto& key)
        {
            return key.sourceId + "|" + juce::String (key.channel) + "|"
                 + juce::String (key.cc) + "|" + juce::String (key.isNote ? 1 : 0);
        };
        std::map<juce::String, int> destinationUse;
        for (const auto& entry : mappings)
        {
            auto destination = entry.first;
            destination.sourceId = resolveUniqueMidiSourceId (
                destination.sourceId, destination.sourceName, available);
            if (destination.sourceId.isEmpty())
                destination.sourceId = normalizeMidiSourceId (entry.first.sourceId);
            ++destinationUse[destinationToken (destination)];
        }

        for (const auto& entry : mappings)
        {
            auto key = entry.first;
            const auto resolved = resolveUniqueMidiSourceId (
                key.sourceId, key.sourceName, available);

            if (! key.sourceName.isEmpty())
            {
                int matches = 0;
                for (const auto& source : available)
                    if (midiSourceNamesEqual (source.name, key.sourceName))
                        ++matches;
                if (matches > 1)
                    ++result.ambiguous;
            }

            auto destination = key;
            destination.sourceId = resolved.isNotEmpty()
                                 ? resolved
                                 : normalizeMidiSourceId (key.sourceId);
            const bool shouldMove = destination.sourceId != key.sourceId;
            const bool destinationCollides = shouldMove
                && destinationUse[destinationToken (destination)] > 1;
            if (destinationCollides)
            {
                ++result.collisions;
                key.sourceId = entry.first.sourceId;
            }
            else if (shouldMove)
                key.sourceId = destination.sourceId;
            else if (key.sourceId.isNotEmpty())
                key.sourceId = normalizeMidiSourceId (key.sourceId);

            if (rebuilt.find (key) != rebuilt.end())
            {
                // Keep the original stale key as a second, non-destructive
                // entry. A normalized-id collision can only be represented by
                // retaining the old spelling, which is intentionally stable.
                auto stale = entry.first;
                if (rebuilt.find (stale) == rebuilt.end())
                    rebuilt.emplace (std::move (stale), entry.second);
                continue;
            }

            if (shouldMove && ! destinationCollides)
                ++result.moved;
            rebuilt.emplace (std::move (key), entry.second);
        }

        mappings.swap (rebuilt);
        return result;
    }

    struct MidiMapLifecycleGate
    {
        bool ccLoaded = false;
        bool noteLoaded = false;

        bool canSave() const noexcept { return ccLoaded && noteLoaded; }
    };

    class MidiDirtyEpoch
    {
    public:
        juce::uint64 markDirty() noexcept { return ++epoch; }
        juce::uint64 snapshot() const noexcept { return epoch; }

        bool finish (juce::uint64 captured, bool writeSucceeded) noexcept
        {
            if (! writeSucceeded || epoch != captured)
                return false;
            cleanEpoch = captured;
            return true;
        }

        bool isDirty() const noexcept { return epoch != cleanEpoch; }

    private:
        std::atomic<juce::uint64> epoch { 0 };
        std::atomic<juce::uint64> cleanEpoch { 0 };
    };

    enum class SuppressedNoteRelease
    {
        notSuppressed,
        stillSuppressed,
        releasedLast
    };

    inline bool mayWriteMidiMapSnapshot (bool previousReadFailed,
                                         bool mapIsEmpty,
                                         bool explicitEmptyAuthorised) noexcept
    {
        return ! previousReadFailed || ! mapIsEmpty || explicitEmptyAuthorised;
    }

    // Empty-autosave protection belongs to each inspected side of a committed
    // CC + NOTE generation. Half-save success and rollback leave it untouched;
    // even after journal commit, an unchanged side keeps its guard because its
    // bytes were not replaced/verified by this transaction.
    inline void applyMidiMapPairCommitToEmptySaveProtection (
        bool pairCommitted,
        bool ccWasWritten,
        bool notesWereWritten,
        bool& blockedCC,
        bool& blockedNotes) noexcept
    {
        if (pairCommitted && ccWasWritten)
            blockedCC = false;

        if (pairCommitted && notesWereWritten)
            blockedNotes = false;
    }

    inline bool shouldBlockEmptySaveAfterFailedRecovery (
        bool mainFileExists,
        bool lastGoodFileExists) noexcept
    {
        return mainFileExists || lastGoodFileExists;
    }

    inline bool hasOnlyAllowedMidiMapChildren (
        const juce::XmlElement& xml,
        const juce::String& childTag,
        const juce::String& secondChildTag = {})
    {
        if (childTag.isEmpty())
            return false;

        for (auto* child : xml.getChildIterator())
            if (! child->hasTagName (childTag)
                 && (secondChildTag.isEmpty()
                     || ! child->hasTagName (secondChildTag)))
                return false;

        return true;
    }

    inline bool hasFutureMidiMapSchema (const juce::XmlElement& xml,
                                        const juce::String& root) noexcept
    {
        return xml.hasTagName (root)
            && xml.getIntAttribute ("schema", 1) > 1;
    }

    // Recheck the files at save time, not only during startup restore. Another
    // ChurchDAW build may remain open against the same settings directory and
    // install a newer schema after this process has already loaded its maps.
    // In that case both the primary and last-good snapshot belong to the newer
    // writer and must remain untouched.
    inline bool midiMapFilesHaveFutureSchema (const juce::File& mainFile,
                                               const juce::File& lastGood,
                                               const juce::String& root);

    inline bool hasStrictIntegerAttribute (const juce::XmlElement& xml,
                                           const juce::String& name)
    {
        if (! xml.hasAttribute (name))
            return false;

        const auto text = xml.getStringAttribute (name);
        if (text.isEmpty())
            return false;

        int firstDigit = 0;
        if (text[0] == '-')
            firstDigit = 1;
        if (firstDigit == text.length())
            return false;

        const int digitCount = text.length() - firstDigit;
        if (digitCount > 10)
            return false;

        for (int i = firstDigit; i < text.length(); ++i)
            if (text[i] < '0' || text[i] > '9')
                return false;

        const auto parsed = text.getLargeIntValue();
        return parsed >= std::numeric_limits<int>::min()
            && parsed <= std::numeric_limits<int>::max();
    }

    inline bool isMappableController (int cc) noexcept;
    inline bool isMappableAction (int cc) noexcept;
    inline bool isSafeStoredTarget (int slot,
                                    const juce::String& stableId,
                                    const juce::String& legacyName) noexcept;

    inline bool isPersistedMidiActionValid (int action, int argument) noexcept
    {
        switch (action)
        {
            case 1:  return argument >= 0 && argument < 12; // padKey
            case 2:  return argument == -1;                 // padOff
            case 3:  return argument >= 0 && argument < 10; // scene
            case 4:  // panic
            case 5:  // nextScene
            case 6:  // previousScene
            case 8:  // tapTempo
            case 9:  // sustainToggle
            case 10: // padSynthEngineToggle
                return argument == -1;
            case 7:  return argument >= 0 && argument < 16; // padSynthProgram
            default: return false;
        }
    }

    inline bool isPersistedMidiMapChildValid (
        const juce::XmlElement& child,
        const juce::String& root)
    {
        const auto validSourceAndChannel = [&child]
        {
            return child.hasAttribute ("source")
                && child.getStringAttribute ("source").trim().isNotEmpty()
                && hasStrictIntegerAttribute (child, "channel")
                && child.getIntAttribute ("channel", 0) >= 1
                && child.getIntAttribute ("channel", 0) <= 16;
        };

        if (root == "NOTEMAP")
        {
            if (! child.hasTagName ("NOTE")
                 || ! validSourceAndChannel()
                 || ! hasStrictIntegerAttribute (child, "note")
                 || ! hasStrictIntegerAttribute (child, "action")
                 || (child.hasAttribute ("arg")
                     && ! hasStrictIntegerAttribute (child, "arg")))
                return false;

            const int note = child.getIntAttribute ("note", -1);
            return note >= 0 && note <= 127
                && isPersistedMidiActionValid (
                    child.getIntAttribute ("action", 0),
                    child.getIntAttribute ("arg", -1));
        }

        if (root != "MIDIMAP" || ! validSourceAndChannel())
            return false;

        if (child.hasTagName ("CCACAO"))
        {
            if (! hasStrictIntegerAttribute (child, "cc")
                 || ! hasStrictIntegerAttribute (child, "action")
                 || (child.hasAttribute ("arg")
                     && ! hasStrictIntegerAttribute (child, "arg"))
                 || (child.hasAttribute ("modo")
                     && ! hasStrictIntegerAttribute (child, "modo")))
                return false;

            const int mode = child.getIntAttribute ("modo", 0);
            return isMappableAction (child.getIntAttribute ("cc", -1))
                && (mode == 0 || mode == 1)
                && isPersistedMidiActionValid (
                    child.getIntAttribute ("action", 0),
                    child.getIntAttribute ("arg", -1));
        }

        if (! child.hasTagName ("MAP")
             || ! hasStrictIntegerAttribute (child, "cc")
             || ! hasStrictIntegerAttribute (child, "slot")
             || (child.hasAttribute ("insert")
                 && ! hasStrictIntegerAttribute (child, "insert"))
             || (child.hasAttribute ("param")
                 && ! hasStrictIntegerAttribute (child, "param")))
            return false;

        const auto isNoteText = child.getStringAttribute ("isNote", "0");
        if (isNoteText != "0" && isNoteText != "1")
            return false;

        const bool isNote = isNoteText == "1";
        const int controller = child.getIntAttribute ("cc", -1);
        if (controller < 0 || controller > 127
             || (! isNote && ! isMappableController (controller)))
            return false;

        const int slot = child.getIntAttribute ("slot", -2);
        const int insert = child.getIntAttribute ("insert", -1);
        const bool validSlot = slot == -4 || slot == -3 || slot == -1
                            || (slot >= 0 && slot < 4);
        const bool validInsert = insert == -1
                              || ((slot >= 0 && slot < 4)
                                  && insert >= 0 && insert < 2);
        return validSlot && validInsert
            && isSafeStoredTarget (
                slot, child.getStringAttribute ("id"),
                child.getStringAttribute ("name"));
    }

    inline bool hasValidUniqueMidiMapChildren (
        const juce::XmlElement& xml,
        const juce::String& root)
    {
        std::set<juce::String> identities;
        for (auto* child : xml.getChildIterator())
        {
            if (! isPersistedMidiMapChildValid (*child, root))
                return false;

            const auto numberAttribute = root == "NOTEMAP" ? "note" : "cc";
            const bool isNoteTarget = root == "NOTEMAP"
                || (child->hasTagName ("MAP")
                    && child->getIntAttribute ("isNote", 0) != 0);
            const auto identity = normalizeMidiSourceId (
                                      child->getStringAttribute ("source"))
                                + "|" + juce::String (
                                      child->getIntAttribute ("channel", 0))
                                + "|" + juce::String (
                                      child->getIntAttribute (numberAttribute, -1))
                                + "|" + juce::String (isNoteTarget ? 1 : 0);
            if (! identities.insert (identity).second)
                return false;
        }
        return true;
    }

    inline bool isMidiMapXmlValid (const juce::XmlElement& xml,
                                   const juce::String& root,
                                   const juce::String& childTag,
                                   const juce::String& secondChildTag = {})
    {
        if (! xml.hasTagName (root)
             || ! hasStrictIntegerAttribute (xml, "schema")
             || ! hasStrictIntegerAttribute (xml, "complete")
             || ! hasStrictIntegerAttribute (xml, "entryCount")
             || ! hasStrictIntegerAttribute (xml, "explicitEmpty")
             || xml.getIntAttribute ("schema", 0) != 1
             || xml.getIntAttribute ("complete", 0) != 1)
            return false;

        if (! hasOnlyAllowedMidiMapChildren (xml, childTag, secondChildTag))
            return false;

        if (! hasValidUniqueMidiMapChildren (xml, root))
            return false;

        const int count = xml.getIntAttribute ("entryCount", -1);
        const int actual = xml.getNumChildElements();
        if (count < 0 || count != actual)
            return false;

        const int explicitEmpty = xml.getIntAttribute ("explicitEmpty", -1);
        return explicitEmpty == (count == 0 ? 1 : 0);
    }

    // Files written before complete/entryCount existed remain readable, but
    // their child set is still strict. If any snapshot metadata is present,
    // require the complete modern contract instead of accepting a truncated
    // half-upgraded file as legacy.
    inline bool isMidiMapXmlReadable (const juce::XmlElement& xml,
                                      const juce::String& root,
                                      const juce::String& childTag,
                                      const juce::String& secondChildTag = {})
    {
        if (! xml.hasTagName (root)
             || (xml.hasAttribute ("schema")
                 && ! hasStrictIntegerAttribute (xml, "schema"))
             || xml.getIntAttribute ("schema", 1) != 1
             || ! hasOnlyAllowedMidiMapChildren (
                    xml, childTag, secondChildTag)
             || ! hasValidUniqueMidiMapChildren (xml, root))
            return false;

        const bool hasSnapshotMetadata =
            xml.hasAttribute ("complete")
            || xml.hasAttribute ("entryCount")
            || xml.hasAttribute ("explicitEmpty");

        // A legacy file had no completeness metadata, but every legitimate
        // legacy snapshot contained at least one mapping. Treating a bare
        // <MIDIMAP/> / <NOTEMAP/> as a valid legacy snapshot is dangerous:
        // it can replace a populated last-good file on the next autosave.
        // Empty maps are supported only by the modern explicitEmpty contract.
        return hasSnapshotMetadata
            ? isMidiMapXmlValid (xml, root, childTag, secondChildTag)
            : xml.getNumChildElements() > 0;
    }

    inline bool midiMapPairHasConflictingNoteControls (
        const juce::XmlElement& ccMap,
        const juce::XmlElement& noteMap)
    {
        std::set<juce::String> noteIdentities;
        for (auto* note : noteMap.getChildWithTagNameIterator ("NOTE"))
            noteIdentities.insert (
                normalizeMidiSourceId (note->getStringAttribute ("source"))
                + "|" + juce::String (note->getIntAttribute ("channel", 0))
                + "|" + juce::String (note->getIntAttribute ("note", -1)));

        for (auto* mapping : ccMap.getChildWithTagNameIterator ("MAP"))
        {
            if (mapping->getIntAttribute ("isNote", 0) == 0)
                continue;

            const auto identity =
                normalizeMidiSourceId (mapping->getStringAttribute ("source"))
                + "|" + juce::String (mapping->getIntAttribute ("channel", 0))
                + "|" + juce::String (mapping->getIntAttribute ("cc", -1));
            if (noteIdentities.count (identity) != 0)
                return true;
        }

        return false;
    }

    template <typename ParameterMap, typename NoteActionMap>
    inline bool midiMapCollectionsHaveConflictingNoteControls (
        const ParameterMap& parameterMappings,
        const NoteActionMap& noteMappings)
    {
        std::set<juce::String> noteIdentities;
        for (const auto& note : noteMappings)
            noteIdentities.insert (
                normalizeMidiSourceId (note.first.sourceId)
                + "|" + juce::String (note.first.channel)
                + "|" + juce::String (note.first.cc));

        for (const auto& mapping : parameterMappings)
        {
            if (! mapping.first.isNote)
                continue;

            const auto identity =
                normalizeMidiSourceId (mapping.first.sourceId)
                + "|" + juce::String (mapping.first.channel)
                + "|" + juce::String (mapping.first.cc);
            if (noteIdentities.count (identity) != 0)
                return true;
        }

        return false;
    }

    template <typename ParameterMap, typename NoteActionMap>
    inline int midiMapCollectionNoteConflictCount (
        const ParameterMap& parameterMappings,
        const NoteActionMap& noteMappings)
    {
        std::set<juce::String> noteIdentities;
        for (const auto& note : noteMappings)
            noteIdentities.insert (
                normalizeMidiSourceId (note.first.sourceId)
                + "|" + juce::String (note.first.channel)
                + "|" + juce::String (note.first.cc));

        int conflicts = 0;
        for (const auto& mapping : parameterMappings)
            if (mapping.first.isNote)
            {
                const auto identity =
                    normalizeMidiSourceId (mapping.first.sourceId)
                    + "|" + juce::String (mapping.first.channel)
                    + "|" + juce::String (mapping.first.cc);
                if (noteIdentities.count (identity) != 0)
                    ++conflicts;
            }

        return conflicts;
    }

    // A CC action and a continuous parameter share the same physical CC
    // namespace. Android source IDs can change after reconnect; when both old
    // IDs re-key to one current ID, keeping both entries would make the action
    // shadow the parameter and would also serialize duplicate XML identities.
    template <typename ParameterMap, typename CCActionMap,
              typename IsActiveAction>
    inline bool midiMapCollectionsHaveConflictingCCControls (
        const ParameterMap& parameterMappings,
        const CCActionMap& actionMappings,
        IsActiveAction&& isActiveAction)
    {
        for (const auto& parameter : parameterMappings)
        {
            if (parameter.first.isNote)
                continue;

            for (const auto& action : actionMappings)
                if (isActiveAction (action.second)
                     && ! action.first.isNote
                     && parameter.first.channel == action.first.channel
                     && parameter.first.cc == action.first.cc
                     && normalizeMidiSourceId (parameter.first.sourceId)
                          == normalizeMidiSourceId (action.first.sourceId))
                    return true;
        }

        return false;
    }

    template <typename ParameterMap, typename CCActionMap>
    inline bool midiMapCollectionsHaveConflictingCCControls (
        const ParameterMap& parameterMappings,
        const CCActionMap& actionMappings)
    {
        return midiMapCollectionsHaveConflictingCCControls (
            parameterMappings, actionMappings,
            [] (const auto&) { return true; });
    }

    template <typename ParameterMap, typename CCActionMap,
              typename IsActiveAction>
    inline int midiMapCollectionCCConflictCount (
        const ParameterMap& parameterMappings,
        const CCActionMap& actionMappings,
        IsActiveAction&& isActiveAction)
    {
        int conflicts = 0;
        for (const auto& parameter : parameterMappings)
        {
            if (parameter.first.isNote)
                continue;

            for (const auto& action : actionMappings)
                if (isActiveAction (action.second)
                     && ! action.first.isNote
                     && parameter.first.channel == action.first.channel
                     && parameter.first.cc == action.first.cc
                     && normalizeMidiSourceId (parameter.first.sourceId)
                          == normalizeMidiSourceId (action.first.sourceId))
                    ++conflicts;
        }

        return conflicts;
    }

    static constexpr juce::int64 maxMidiMapFileBytes = 8 * 1024 * 1024;
    static constexpr juce::int64 maxMidiMapJournalBytes = 48 * 1024 * 1024;
    static constexpr juce::int64 maxMidiMapBundleBytes = 16 * 1024 * 1024;

    struct BoundedXmlLimits
    {
        juce::int64 maxBytes = maxMidiMapFileBytes;
        int maxDepth = 32;
        int maxElements = 16384;
        int maxAttributes = 131072;
        int maxStringBytes = 65536;
        juce::int64 maxTextNodeBytes = maxMidiMapFileBytes;
    };

    inline BoundedXmlLimits midiMapXmlLimits()
    {
        return {};
    }

    inline BoundedXmlLimits midiMapJournalXmlLimits()
    {
        BoundedXmlLimits limits;
        limits.maxBytes = maxMidiMapJournalBytes;
        limits.maxDepth = 8;
        limits.maxElements = 16;
        limits.maxAttributes = 64;
        limits.maxStringBytes = 4096;
        limits.maxTextNodeBytes = maxMidiMapJournalBytes;
        return limits;
    }

    inline BoundedXmlLimits midiMapBundleXmlLimits()
    {
        BoundedXmlLimits limits;
        limits.maxBytes = maxMidiMapBundleBytes;
        limits.maxDepth = 40;
        limits.maxElements = 32768;
        limits.maxAttributes = 196608;
        limits.maxTextNodeBytes = maxMidiMapBundleBytes;
        return limits;
    }

    inline BoundedXmlLimits midiMapMarkerXmlLimits()
    {
        BoundedXmlLimits limits;
        limits.maxBytes = 64 * 1024;
        limits.maxDepth = 4;
        limits.maxElements = 4;
        limits.maxAttributes = 16;
        limits.maxStringBytes = 4096;
        limits.maxTextNodeBytes = 4096;
        return limits;
    }

    inline bool isValidUtf8 (const std::uint8_t* bytes, size_t size) noexcept
    {
        size_t i = 0;
        while (i < size)
        {
            const auto first = bytes[i++];
            if (first == 0)
                return false;
            if (first < 0x80)
                continue;

            int continuationCount = 0;
            std::uint32_t value = 0;
            std::uint32_t minimum = 0;
            if ((first & 0xe0) == 0xc0)
            {
                continuationCount = 1;
                value = first & 0x1f;
                minimum = 0x80;
            }
            else if ((first & 0xf0) == 0xe0)
            {
                continuationCount = 2;
                value = first & 0x0f;
                minimum = 0x800;
            }
            else if ((first & 0xf8) == 0xf0)
            {
                continuationCount = 3;
                value = first & 0x07;
                minimum = 0x10000;
            }
            else
            {
                return false;
            }

            if (i + (size_t) continuationCount > size)
                return false;
            for (int n = 0; n < continuationCount; ++n)
            {
                const auto next = bytes[i++];
                if ((next & 0xc0) != 0x80)
                    return false;
                value = (value << 6) | (next & 0x3f);
            }

            if (value < minimum || value > 0x10ffff
                 || (value >= 0xd800 && value <= 0xdfff))
                return false;
        }

        return true;
    }

    inline bool startsWithAscii (const std::uint8_t* bytes, size_t size,
                                 size_t offset, const char* token) noexcept
    {
        for (size_t i = 0; token[i] != 0; ++i)
            if (offset + i >= size
                 || bytes[offset + i] != (std::uint8_t) token[i])
                return false;
        return true;
    }

    inline size_t findAsciiTerminator (const std::uint8_t* bytes, size_t size,
                                       size_t offset, const char* token) noexcept
    {
        for (size_t i = offset; i < size; ++i)
            if (startsWithAscii (bytes, size, i, token))
                return i;
        return size;
    }

    // Reject declarations/entities before JUCE's XML parser sees them and put
    // hard bounds on nesting and node count. This scan is deliberately small:
    // it is a resource/safety gate, while XmlDocument remains the syntax parser.
    inline bool xmlBytesPassSafetyScan (const juce::MemoryBlock& data,
                                        const BoundedXmlLimits& limits) noexcept
    {
        if ((juce::int64) data.getSize() > limits.maxBytes)
            return false;

        const auto* bytes = static_cast<const std::uint8_t*> (data.getData());
        const auto size = data.getSize();
        if (size == 0 || ! isValidUtf8 (bytes, size))
            return false;

        int depth = 0;
        int elements = 0;
        for (size_t i = 0; i < size;)
        {
            if (bytes[i] != '<')
            {
                ++i;
                continue;
            }

            if (startsWithAscii (bytes, size, i, "<!--"))
            {
                const auto end = findAsciiTerminator (bytes, size, i + 4, "-->");
                if (end == size)
                    return false;
                i = end + 3;
                continue;
            }
            if (startsWithAscii (bytes, size, i, "<![CDATA["))
            {
                const auto end = findAsciiTerminator (bytes, size, i + 9, "]]>");
                if (end == size)
                    return false;
                i = end + 3;
                continue;
            }
            if (startsWithAscii (bytes, size, i, "<?"))
            {
                const auto end = findAsciiTerminator (bytes, size, i + 2, "?>");
                if (end == size)
                    return false;
                i = end + 2;
                continue;
            }
            if (startsWithAscii (bytes, size, i, "<!"))
                return false; // DOCTYPE, ENTITY and every other declaration

            const bool closing = startsWithAscii (bytes, size, i, "</");
            size_t end = i + (closing ? 2u : 1u);
            std::uint8_t quote = 0;
            for (; end < size; ++end)
            {
                const auto current = bytes[end];
                if (quote != 0)
                {
                    if (current == quote)
                        quote = 0;
                    continue;
                }
                if (current == '\'' || current == '"')
                {
                    quote = current;
                    continue;
                }
                if (current == '>')
                    break;
                if (current == '<')
                    return false;
            }
            if (end >= size || quote != 0)
                return false;

            if (closing)
            {
                if (--depth < 0)
                    return false;
            }
            else
            {
                size_t last = end;
                while (last > i && (bytes[last - 1] == ' '
                                     || bytes[last - 1] == '\t'
                                     || bytes[last - 1] == '\r'
                                     || bytes[last - 1] == '\n'))
                    --last;
                const bool selfClosing = last > i && bytes[last - 1] == '/';
                if (++elements > limits.maxElements)
                    return false;
                if (! selfClosing && ++depth > limits.maxDepth)
                    return false;
            }

            i = end + 1;
        }

        return elements > 0 && depth == 0;
    }

    inline bool xmlTreePassesSafetyLimits (const juce::XmlElement& xml,
                                           const BoundedXmlLimits& limits,
                                           int depth = 1,
                                           int* elementCount = nullptr,
                                           int* attributeCount = nullptr)
    {
        int localElements = 0;
        int localAttributes = 0;
        if (elementCount == nullptr)
            elementCount = &localElements;
        if (attributeCount == nullptr)
            attributeCount = &localAttributes;

        if (depth > limits.maxDepth || ++*elementCount > limits.maxElements)
            return false;
        if (xml.isTextElement())
            return xml.getText().getNumBytesAsUTF8() <= limits.maxTextNodeBytes;
        if (xml.getTagName().getNumBytesAsUTF8() > limits.maxStringBytes)
            return false;

        const int attributes = xml.getNumAttributes();
        if (attributes < 0 || *attributeCount > limits.maxAttributes - attributes)
            return false;
        *attributeCount += attributes;
        for (int i = 0; i < attributes; ++i)
            if (xml.getAttributeName (i).getNumBytesAsUTF8()
                    > limits.maxStringBytes
                 || xml.getAttributeValue (i).getNumBytesAsUTF8()
                    > limits.maxStringBytes)
                return false;

        for (auto* child : xml.getChildIterator())
            if (! xmlTreePassesSafetyLimits (*child, limits, depth + 1,
                                             elementCount, attributeCount))
                return false;
        return true;
    }

    // Reads from one already-open handle and never reopens the path while the
    // bytes are being collected. A later parser therefore consumes exactly the
    // bounded snapshot that passed these checks.
    inline bool loadBoundedFileData (const juce::File& file,
                                     juce::MemoryBlock& result,
                                     juce::int64 maxBytes)
    {
        result.reset();
        if (maxBytes < 0 || ! file.existsAsFile())
            return false;

       #if JUCE_ANDROID || JUCE_MAC || JUCE_IOS || JUCE_LINUX || JUCE_BSD
        int flags = O_RDONLY;
       #ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
       #endif
       #ifdef O_NOFOLLOW
        flags |= O_NOFOLLOW;
       #endif
        const int descriptor = ::open (file.getFullPathName().toRawUTF8(), flags);
        if (descriptor < 0)
            return false;

        struct stat before {};
        const bool regular = ::fstat (descriptor, &before) == 0
                          && S_ISREG (before.st_mode)
                          && before.st_size >= 0
                          && (juce::int64) before.st_size <= maxBytes;
        if (! regular)
        {
            (void) ::close (descriptor);
            return false;
        }

        const size_t expected = (size_t) before.st_size;
        result.setSize (expected, false);
        size_t offset = 0;
        while (offset < expected)
        {
            const auto amount = ::read (
                descriptor,
                static_cast<char*> (result.getData()) + offset,
                expected - offset);
            if (amount < 0 && errno == EINTR)
                continue;
            if (amount <= 0)
            {
                (void) ::close (descriptor);
                result.reset();
                return false;
            }
            offset += (size_t) amount;
        }

        std::uint8_t extra = 0;
        ssize_t extraRead = 0;
        do
        {
            extraRead = ::read (descriptor, &extra, 1);
        }
        while (extraRead < 0 && errno == EINTR);

        struct stat after {};
        const bool stable = extraRead == 0
                         && ::fstat (descriptor, &after) == 0
                         && before.st_dev == after.st_dev
                         && before.st_ino == after.st_ino
                         && before.st_size == after.st_size;
        (void) ::close (descriptor);
        if (! stable)
            result.reset();
        return stable;
       #elif JUCE_WINDOWS
        const HANDLE handle = ::CreateFileW (
            file.getFullPathName().toWideCharPointer(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT
                                  | FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            return false;

        BY_HANDLE_FILE_INFORMATION before {};
        const bool regular = ::GetFileInformationByHandle (handle, &before) != 0
            && (before.dwFileAttributes
                & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0;
        ULARGE_INTEGER size {};
        size.HighPart = before.nFileSizeHigh;
        size.LowPart = before.nFileSizeLow;
        if (! regular
             || size.QuadPart > (ULONGLONG) maxBytes
             || size.QuadPart > (ULONGLONG) std::numeric_limits<size_t>::max())
        {
            (void) ::CloseHandle (handle);
            return false;
        }

        const size_t expected = (size_t) size.QuadPart;
        result.setSize (expected, false);
        size_t offset = 0;
        while (offset < expected)
        {
            const auto remaining = expected - offset;
            const DWORD requested = (DWORD) juce::jmin<size_t> (
                remaining, (size_t) std::numeric_limits<DWORD>::max());
            DWORD amount = 0;
            if (::ReadFile (
                    handle,
                    static_cast<char*> (result.getData()) + offset,
                    requested, &amount, nullptr) == 0
                 || amount == 0)
            {
                (void) ::CloseHandle (handle);
                result.reset();
                return false;
            }
            offset += (size_t) amount;
        }

        std::uint8_t extra = 0;
        DWORD extraRead = 0;
        BY_HANDLE_FILE_INFORMATION after {};
        const bool stable = ::ReadFile (
                                handle, &extra, 1, &extraRead, nullptr) != 0
                         && extraRead == 0
                         && ::GetFileInformationByHandle (handle, &after) != 0
                         && before.dwVolumeSerialNumber
                                == after.dwVolumeSerialNumber
                         && before.nFileIndexHigh == after.nFileIndexHigh
                         && before.nFileIndexLow == after.nFileIndexLow
                         && before.nFileSizeHigh == after.nFileSizeHigh
                         && before.nFileSizeLow == after.nFileSizeLow
                         && before.ftLastWriteTime.dwHighDateTime
                                == after.ftLastWriteTime.dwHighDateTime
                         && before.ftLastWriteTime.dwLowDateTime
                                == after.ftLastWriteTime.dwLowDateTime;
        const bool closed = ::CloseHandle (handle) != 0;
        if (! stable || ! closed)
            result.reset();
        return stable && closed;
       #else
        juce::FileInputStream input (file);
        if (! input.openedOk())
            return false;
        const auto read = input.readIntoMemoryBlock (
            result, (ssize_t) juce::jmin<juce::int64> (
                maxBytes + 1, std::numeric_limits<ssize_t>::max()));
        const bool valid = (juce::int64) read <= maxBytes
                        && read == result.getSize()
                        && input.getStatus().wasOk();
        if (! valid)
            result.reset();
        return valid;
       #endif
    }

    inline std::unique_ptr<juce::XmlElement> parseBoundedXmlData (
        const juce::MemoryBlock& data,
        const BoundedXmlLimits& limits = midiMapXmlLimits())
    {
        if (! xmlBytesPassSafetyScan (data, limits))
            return {};

        auto xml = juce::parseXML (juce::String::fromUTF8 (
            static_cast<const char*> (data.getData()),
            (int) data.getSize()));
        if (xml == nullptr || ! xmlTreePassesSafetyLimits (*xml, limits))
            return {};
        return xml;
    }

    inline std::unique_ptr<juce::XmlElement> parseBoundedXmlText (
        const juce::String& text,
        const BoundedXmlLimits& limits = midiMapXmlLimits())
    {
        juce::MemoryBlock data;
        data.append (text.toRawUTF8(), (size_t) text.getNumBytesAsUTF8());
        return parseBoundedXmlData (data, limits);
    }

    // Validate the exact bytes that a writer is about to publish, after every
    // attribute/envelope injected by that writer. Checking the source
    // XmlElement before serialization is insufficient: serialization,
    // transactionId and bundle escaping can all enlarge the final payload.
    inline bool serializedXmlPayloadFitsSafetyLimits (
        const juce::String& payload,
        const BoundedXmlLimits& limits)
    {
        const auto byteCount =
            (juce::int64) payload.getNumBytesAsUTF8();
        return byteCount > 0
            && byteCount <= limits.maxBytes
            && parseBoundedXmlText (payload, limits) != nullptr;
    }

    inline bool midiMapSerializedPayloadFitsSafetyLimits (
        const juce::String& payload)
    {
        return serializedXmlPayloadFitsSafetyLimits (
            payload, midiMapXmlLimits());
    }

    inline bool midiMapSerializedBundleFitsSafetyLimits (
        const juce::String& payload)
    {
        return serializedXmlPayloadFitsSafetyLimits (
            payload, midiMapBundleXmlLimits());
    }

    inline std::unique_ptr<juce::XmlElement> loadBoundedXmlFile (
        const juce::File& file,
        const BoundedXmlLimits& limits = midiMapXmlLimits())
    {
        juce::MemoryBlock data;
        return loadBoundedFileData (file, data, limits.maxBytes)
             ? parseBoundedXmlData (data, limits) : nullptr;
    }

    inline bool midiMapFilesHaveFutureSchema (const juce::File& mainFile,
                                               const juce::File& lastGood,
                                               const juce::String& root)
    {
        const auto hasFutureSchema = [&root] (const juce::File& file)
        {
            const auto xml = loadBoundedXmlFile (file);
            return xml != nullptr && hasFutureMidiMapSchema (*xml, root);
        };

        return hasFutureSchema (mainFile) || hasFutureSchema (lastGood);
    }

    inline bool midiMapPairHasFutureSchema (const juce::File& ccFile,
                                             const juce::File& noteFile)
    {
        return midiMapFilesHaveFutureSchema (
                   ccFile,
                   ccFile.getSiblingFile (ccFile.getFileName() + ".last-good"),
                   "MIDIMAP")
            || midiMapFilesHaveFutureSchema (
                   noteFile,
                   noteFile.getSiblingFile (
                       noteFile.getFileName() + ".last-good"),
                   "NOTEMAP");
    }

    inline bool syncMidiMapParentDirectory (const juce::File& file)
    {
       #if JUCE_ANDROID || JUCE_MAC || JUCE_IOS || JUCE_LINUX || JUCE_BSD
        int flags = O_RDONLY;
       #ifdef O_DIRECTORY
        flags |= O_DIRECTORY;
       #endif
       #ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
       #endif
        const int descriptor = ::open (
            file.getParentDirectory().getFullPathName().toRawUTF8(), flags);
        if (descriptor < 0)
            return false;
        int resultCode = 0;
        do
        {
            resultCode = ::fsync (descriptor);
        }
        while (resultCode != 0 && errno == EINTR);
        const bool closed = ::close (descriptor) == 0;
        return resultCode == 0 && closed;
       #else
        juce::ignoreUnused (file);
        return true; // MoveFileExW(..., MOVEFILE_WRITE_THROUGH) covers Windows.
       #endif
    }

    // FileOutputStream::flush() reaches fsync on POSIX, but macOS requires
    // F_FULLFSYNC to ask the storage device itself to drain volatile caches.
    // Reopen only after the JUCE stream has closed, then establish the durable
    // barrier before the temporary file becomes visible at its final name.
    inline bool syncPreparedMidiMapFile (const juce::File& file)
    {
       #if JUCE_MAC || JUCE_IOS
        int flags = O_RDWR;
       #ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
       #endif
        const int descriptor = ::open (file.getFullPathName().toRawUTF8(), flags);
        if (descriptor < 0)
            return false;

        int resultCode = 0;
        do
        {
            resultCode = ::fcntl (descriptor, F_FULLFSYNC);
        }
        while (resultCode != 0 && errno == EINTR);

        const bool closed = ::close (descriptor) == 0;
        return resultCode == 0 && closed;
       #elif JUCE_ANDROID || JUCE_LINUX || JUCE_BSD
        int flags = O_RDWR;
       #ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
       #endif
        const int descriptor = ::open (file.getFullPathName().toRawUTF8(), flags);
        if (descriptor < 0)
            return false;

        int resultCode = 0;
        do
        {
            resultCode = ::fsync (descriptor);
        }
        while (resultCode != 0 && errno == EINTR);

        const bool closed = ::close (descriptor) == 0;
        return resultCode == 0 && closed;
       #elif JUCE_WINDOWS
        const HANDLE handle = ::CreateFileW (
            file.getFullPathName().toWideCharPointer(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            return false;
        const bool flushed = ::FlushFileBuffers (handle) != 0;
        const bool closed = ::CloseHandle (handle) != 0;
        return flushed && closed;
       #else
        juce::ignoreUnused (file);
        return true;
       #endif
    }

    inline bool replaceDurablyPreparedFile (const juce::File& temporary,
                                             const juce::File& destination)
    {
       #if JUCE_WINDOWS
        const bool renamed = ::MoveFileExW (
            temporary.getFullPathName().toWideCharPointer(),
            destination.getFullPathName().toWideCharPointer(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
       #else
        const bool renamed = ::rename (
            temporary.getFullPathName().toRawUTF8(),
            destination.getFullPathName().toRawUTF8()) == 0;
       #endif
        return renamed && syncMidiMapParentDirectory (destination);
    }

    inline bool replaceFileWithDataAtomically (const juce::File& destination,
                                                const juce::MemoryBlock& contents)
    {
        if (! destination.getParentDirectory().createDirectory())
            return false;

        juce::TemporaryFile temporary (destination);
        {
            juce::FileOutputStream output (temporary.getFile());
            if (! output.openedOk()
                 || (contents.getSize() != 0
                     && ! output.write (contents.getData(), contents.getSize())))
                return false;
            output.flush();
            if (output.getStatus().failed())
                return false;
        }

        return syncPreparedMidiMapFile (temporary.getFile())
            && replaceDurablyPreparedFile (temporary.getFile(), destination);
    }

    inline bool replaceFileWithTextAtomically (const juce::File& destination,
                                                const juce::String& contents)
    {
        juce::MemoryBlock data;
        data.append (contents.toRawUTF8(),
                     (size_t) contents.getNumBytesAsUTF8());
        return replaceFileWithDataAtomically (destination, data);
    }

    inline bool atomicCopyFile (const juce::File& source,
                                const juce::File& destination)
    {
        if (! source.existsAsFile())
            return false;

        if (! destination.getParentDirectory().createDirectory())
            return false;
        juce::TemporaryFile temporary (destination);
        {
            juce::FileInputStream input (source);
            juce::FileOutputStream output (temporary.getFile());
            if (! input.openedOk() || ! output.openedOk()
                 || output.writeFromInputStream (input, -1) < 0)
                return false;
            output.flush();
            if (input.getStatus().failed() || output.getStatus().failed())
                return false;
        }
        return syncPreparedMidiMapFile (temporary.getFile())
            && replaceDurablyPreparedFile (temporary.getFile(), destination);
    }

    // Removing a path is also a persistent namespace update. A plain
    // File::deleteFile() has no write-through contract on Windows, so first
    // publish the absence by renaming to a same-directory tombstone. Once that
    // rename is durable, tombstone cleanup is best-effort: its reappearance
    // after power loss cannot resurrect the original map name.
    inline bool removeMidiMapFileDurably (const juce::File& target)
    {
        if (! target.exists())
            return syncMidiMapParentDirectory (target);
        if (! target.existsAsFile())
            return false;

        const auto tombstone = target.getParentDirectory()
            .getNonexistentChildFile (
                ".churchdaw-delete-" + juce::Uuid().toString(), {}, false);

       #if JUCE_WINDOWS
        const bool renamed = ::MoveFileExW (
            target.getFullPathName().toWideCharPointer(),
            tombstone.getFullPathName().toWideCharPointer(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
       #elif JUCE_ANDROID || JUCE_MAC || JUCE_IOS || JUCE_LINUX || JUCE_BSD
        const bool renamed = ::rename (
            target.getFullPathName().toRawUTF8(),
            tombstone.getFullPathName().toRawUTF8()) == 0;
       #else
        const bool renamed = target.moveFileTo (tombstone);
       #endif

        if (! renamed || target.exists()
             || ! syncMidiMapParentDirectory (target))
            return false;

        // Correctness is already established at the original pathname. Failure
        // here leaves only a hidden, uniquely named cleanup artifact.
        if (tombstone.deleteFile())
            (void) syncMidiMapParentDirectory (tombstone);
        return ! target.exists();
    }

    inline std::array<juce::File, 4> midiMapPairTargets (
        const juce::File& ccFile,
        const juce::File& noteFile)
    {
        return {{
            ccFile,
            noteFile,
            ccFile.getSiblingFile (ccFile.getFileName() + ".last-good"),
            noteFile.getSiblingFile (noteFile.getFileName() + ".last-good")
        }};
    }

    enum class MidiMapTargetInspection
    {
        absent,
        readable,
        unknown
    };

    // Absence is accepted only when the operating system positively reports
    // ENOENT/not-found. Permission errors, directories, symlinks/reparse points,
    // oversized files and short/I/O reads are all "unknown", never "corrupt".
    inline MidiMapTargetInspection inspectMidiMapTarget (
        const juce::File& file,
        juce::MemoryBlock& contents)
    {
        contents.reset();

       #if JUCE_ANDROID || JUCE_MAC || JUCE_IOS || JUCE_LINUX || JUCE_BSD
        struct stat metadata {};
        if (::lstat (file.getFullPathName().toRawUTF8(), &metadata) != 0)
            return errno == ENOENT ? MidiMapTargetInspection::absent
                                   : MidiMapTargetInspection::unknown;
        if (! S_ISREG (metadata.st_mode))
            return MidiMapTargetInspection::unknown;
       #elif JUCE_WINDOWS
        const auto attributes = ::GetFileAttributesW (
            file.getFullPathName().toWideCharPointer());
        if (attributes == INVALID_FILE_ATTRIBUTES)
        {
            const auto error = ::GetLastError();
            return (error == ERROR_FILE_NOT_FOUND
                     || error == ERROR_PATH_NOT_FOUND)
                ? MidiMapTargetInspection::absent
                : MidiMapTargetInspection::unknown;
        }
        if ((attributes & (FILE_ATTRIBUTE_DIRECTORY
                           | FILE_ATTRIBUTE_REPARSE_POINT)) != 0)
            return MidiMapTargetInspection::unknown;
       #else
        if (! file.exists())
            return MidiMapTargetInspection::absent;
        if (! file.existsAsFile())
            return MidiMapTargetInspection::unknown;
       #endif

        juce::MemoryBlock verification;
        if (! loadBoundedFileData (file, contents, maxMidiMapFileBytes)
             || ! loadBoundedFileData (
                    file, verification, maxMidiMapFileBytes)
             || verification != contents)
        {
            contents.reset();
            return MidiMapTargetInspection::unknown;
        }

        return MidiMapTargetInspection::readable;
    }

    inline bool midiMapPairTargetsInspectable (
        const std::array<juce::File, 4>& targets)
    {
        for (const auto& target : targets)
        {
            juce::MemoryBlock contents;
            if (inspectMidiMapTarget (target, contents)
                  == MidiMapTargetInspection::unknown)
                return false;
        }
        return true;
    }

    inline bool midiMapPairTargetsInspectable (const juce::File& ccFile,
                                                const juce::File& noteFile)
    {
        return midiMapPairTargetsInspectable (
            midiMapPairTargets (ccFile, noteFile));
    }

    inline juce::String midiMapDataSha256 (const juce::MemoryBlock& data)
    {
        return juce::SHA256 (data.getData(), data.getSize())
            .toHexString().toLowerCase();
    }

    struct MidiMapFileRevision
    {
        bool existed = false;
        juce::int64 byteSize = 0;
        juce::String sha256;

        bool operator== (const MidiMapFileRevision& other) const noexcept
        {
            return existed == other.existed
                && byteSize == other.byteSize
                && sha256 == other.sha256;
        }
    };

    // Reads twice to detect ordinary lock-bypassing changes around the first
    // snapshot. This is best-effort for legacy/external writers; all current
    // ChurchDAW writers get the strong guarantee from MidiMapPairFileLock.
    inline bool readStableMidiMapFileRevision (
        const juce::File& file,
        MidiMapFileRevision& result)
    {
        if (! file.existsAsFile())
        {
            if (file.exists())
                return false;

            result = {};
            result.sha256 = midiMapDataSha256 ({});
            return true;
        }

        juce::MemoryBlock first;
        juce::MemoryBlock second;
        if (! loadBoundedFileData (file, first, maxMidiMapFileBytes)
             || ! loadBoundedFileData (file, second, maxMidiMapFileBytes)
             || first != second)
            return false;

        result.existed = true;
        result.byteSize = (juce::int64) first.getSize();
        result.sha256 = midiMapDataSha256 (first);
        return true;
    }

    // Optimistic revision/CAS guard for long-lived app instances. The process
    // lock prevents simultaneous renames; this digest additionally prevents a
    // stale instance (opened before another instance saved) from overwriting
    // that newer valid map after it eventually acquires the lock.
    class MidiMapPairRevision
    {
    public:
        bool refresh (const juce::File& ccFile,
                      const juce::File& noteFile)
        {
            return refresh (midiMapPairTargets (ccFile, noteFile));
        }

        bool refresh (const std::array<juce::File, 4>& files)
        {
            std::array<MidiMapFileRevision, 4> next;
            for (size_t i = 0; i < files.size(); ++i)
                if (! readStableMidiMapFileRevision (files[i], next[i]))
                {
                    invalidate();
                    return false;
                }

            revisions = std::move (next);
            initialised = true;
            return true;
        }

        bool matches (const std::array<juce::File, 4>& files) const
        {
            if (! initialised)
                return false;

            for (size_t i = 0; i < files.size(); ++i)
            {
                MidiMapFileRevision current;
                if (! readStableMidiMapFileRevision (files[i], current)
                     || ! (current == revisions[i]))
                    return false;
            }

            return true;
        }

        bool matches (const juce::File& ccFile,
                      const juce::File& noteFile) const
        {
            return matches (midiMapPairTargets (ccFile, noteFile));
        }

        bool isInitialised() const noexcept { return initialised; }
        void invalidate() noexcept { initialised = false; }

    private:
        std::array<MidiMapFileRevision, 4> revisions;
        bool initialised = false;
    };

    // One lock name is derived from the complete map-pair location, so every
    // ChurchDAW process that shares these settings also shares the same lock.
    // Keep this contract stable in future schema versions: it is what makes a
    // schema check and the following atomic rename one indivisible operation
    // with respect to another ChurchDAW writer.
    inline juce::String midiMapPairLockName (const juce::File& ccFile,
                                             const juce::File& noteFile)
    {
        const auto identity = ccFile.getFullPathName()
                            + "|" + noteFile.getFullPathName();
        return "ChurchDAW-MidiMapPair-"
             + juce::String::toHexString ((juce::int64) identity.hashCode64());
    }

    inline juce::File midiMapPairJournalFile (const juce::File& ccFile)
    {
        return ccFile.getSiblingFile (
            ccFile.getFileName() + ".pair-transaction");
    }

    inline juce::File midiMapPairLockFile (const juce::File& ccFile,
                                           const juce::File& noteFile)
    {
        const auto identity = ccFile.getFullPathName()
                            + "|" + noteFile.getFullPathName();
        return ccFile.getParentDirectory().getChildFile (
            ".churchdaw-midi-map-pair-"
              + juce::String::toHexString (
                    (juce::int64) identity.hashCode64())
              + ".lock");
    }

    inline juce::String midiMapStringSha256 (const juce::String& text)
    {
        const auto utf8 = text.toRawUTF8();
        return juce::SHA256 (utf8, (size_t) text.getNumBytesAsUTF8())
            .toHexString().toLowerCase();
    }

    inline juce::File midiMapPairAbandonMarkerFile (
        const juce::File& journal,
        const juce::String& transactionId)
    {
        // The transaction text came from disk during recovery, so never place
        // it directly in a path. Its SHA is deterministic and path-safe.
        return journal.getSiblingFile (
            journal.getFileName() + ".abandoned-"
              + midiMapStringSha256 (transactionId));
    }

    inline bool loadStableMidiMapFileData (
        const juce::File& file,
        juce::MemoryBlock& result,
        juce::int64 maxBytes = maxMidiMapJournalBytes)
    {
        if (! file.existsAsFile())
            return false;

        juce::MemoryBlock first;
        juce::MemoryBlock second;
        if (! loadBoundedFileData (file, first, maxBytes)
             || ! loadBoundedFileData (file, second, maxBytes)
             || first != second)
            return false;

        result = std::move (first);
        return true;
    }

    inline bool midiMapPairHasValidAbandonMarker (
        const juce::File& journal,
        const juce::String& transactionId)
    {
        if (transactionId.isEmpty())
            return false;

        juce::MemoryBlock journalBytes;
        if (! loadStableMidiMapFileData (journal, journalBytes))
            return false;

        const auto marker = midiMapPairAbandonMarkerFile (
            journal, transactionId);
        const auto xml = loadBoundedXmlFile (
            marker, midiMapMarkerXmlLimits());
        return xml != nullptr
            && xml->hasTagName (
                "CHURCHDAW_MIDI_MAP_PAIR_ABANDONED")
            && hasStrictIntegerAttribute (*xml, "version")
            && xml->getIntAttribute ("version", 0) == 1
            && xml->getStringAttribute ("transactionId")
                == transactionId
            && xml->getStringAttribute ("journalSha256")
                    .equalsIgnoreCase (
                        midiMapDataSha256 (journalBytes));
    }

    inline bool writeMidiMapPairAbandonMarker (
        const juce::File& journal,
        const juce::String& transactionId)
    {
        juce::MemoryBlock journalBytes;
        if (transactionId.isEmpty()
             || ! loadStableMidiMapFileData (journal, journalBytes))
            return false;

        juce::XmlElement xml (
            "CHURCHDAW_MIDI_MAP_PAIR_ABANDONED");
        xml.setAttribute ("version", 1);
        xml.setAttribute ("transactionId", transactionId);
        xml.setAttribute (
            "journalSha256", midiMapDataSha256 (journalBytes));

        const auto marker = midiMapPairAbandonMarkerFile (
            journal, transactionId);
        return replaceFileWithTextAtomically (
                   marker,
                   xml.toString (
                       juce::XmlElement::TextFormat().singleLine()))
            && midiMapPairHasValidAbandonMarker (
                journal, transactionId);
    }

    // A save nested in a durable pair transaction may see that transaction's
    // pending journal. Standalone saves accept only a verified committed marker
    // or an exact SHA-acknowledged abandoned checkpoint; an unclosed pending
    // generation still blocks every write.
    inline bool midiMapPairJournalAllowsWrite (
        const juce::File& ccFile,
        const juce::String& activeTransactionId);

#if JUCE_ANDROID || JUCE_MAC || JUCE_LINUX || JUCE_BSD
    // JUCE's generic POSIX InterProcessLock uses /var/tmp (then /tmp). Those
    // paths are not writable in a normal Android application sandbox. This
    // backend locks an explicit app-private sibling path and is also compiled
    // by the desktop safety tests so Android does not carry an untested fork.
    class MidiMapSandboxFileLock
    {
    public:
        explicit MidiMapSandboxFileLock (juce::File fileToUse)
            : file (std::move (fileToUse))
        {
        }

        ~MidiMapSandboxFileLock()
        {
            forceClose();
        }

        bool enter (int timeoutMs)
        {
            if (depth > 0)
            {
                ++depth;
                return true;
            }

            if (! file.getParentDirectory().createDirectory())
                return false;

            int flags = O_RDWR | O_CREAT;
           #ifdef O_CLOEXEC
            flags |= O_CLOEXEC;
           #endif
           #ifdef O_NOFOLLOW
            flags |= O_NOFOLLOW;
           #endif
            const int opened = ::open (
                file.getFullPathName().toRawUTF8(), flags, 0600);
            if (opened < 0)
                return false;

           #ifndef O_CLOEXEC
            (void) ::fcntl (opened, F_SETFD, FD_CLOEXEC);
           #endif

            struct flock request {};
            request.l_type = F_WRLCK;
            request.l_whence = SEEK_SET;

            const auto started =
                juce::Time::getMillisecondCounterHiRes();
            for (;;)
            {
                if (::fcntl (opened, F_SETLK, &request) == 0)
                {
                    descriptor = opened;
                    depth = 1;
                    return true;
                }

                const int error = errno;
                if (error != EINTR && error != EACCES
                     && error != EAGAIN)
                    break;
                if (timeoutMs == 0)
                    break;
                if (timeoutMs > 0
                     && juce::Time::getMillisecondCounterHiRes() - started
                            >= (double) timeoutMs)
                    break;
                juce::Thread::sleep (1);
            }

            (void) ::close (opened);
            return false;
        }

        void exit() noexcept
        {
            if (depth <= 0)
                return;
            if (--depth == 0)
                forceClose();
        }

    private:
        void forceClose() noexcept
        {
            if (descriptor >= 0)
            {
                struct flock request {};
                request.l_type = F_UNLCK;
                request.l_whence = SEEK_SET;
                (void) ::fcntl (descriptor, F_SETLK, &request);
                (void) ::close (descriptor);
            }
            descriptor = -1;
            depth = 0;
        }

        juce::File file;
        int descriptor = -1;
        int depth = 0;

        JUCE_DECLARE_NON_COPYABLE (MidiMapSandboxFileLock)
    };
#endif

    inline std::shared_ptr<juce::CriticalSection>
    sharedMidiMapIntraProcessLock (const juce::String& identity)
    {
        static juce::CriticalSection registryLock;
        static std::map<juce::String,
                        std::weak_ptr<juce::CriticalSection>> registry;
        const juce::ScopedLock guard (registryLock);
        if (const auto existing = registry[identity].lock())
            return existing;

        auto created = std::make_shared<juce::CriticalSection>();
        registry[identity] = created;
        return created;
    }

    class MidiMapPairFileLock
    {
    public:
        MidiMapPairFileLock (const juce::File& ccFile,
                             const juce::File& noteFile)
            : intraProcess (sharedMidiMapIntraProcessLock (
                  midiMapPairLockName (ccFile, noteFile)))
           #if JUCE_ANDROID
            , interProcess (midiMapPairLockFile (ccFile, noteFile))
           #else
            , interProcess (midiMapPairLockName (ccFile, noteFile))
           #endif
        {
        }

        class ScopedAccess
        {
        public:
            explicit ScopedAccess (MidiMapPairFileLock& ownerToUse,
                                   int timeoutMs = 2000)
                : owner (&ownerToUse), locked (owner->enter (timeoutMs))
            {
            }

            ~ScopedAccess()
            {
                if (locked)
                    owner->exit();
            }

            bool isLocked() const noexcept { return locked; }

        private:
            MidiMapPairFileLock* owner = nullptr;
            bool locked = false;

            JUCE_DECLARE_NON_COPYABLE (ScopedAccess)
        };

        bool beginTransaction (const juce::String& transactionId,
                               int timeoutMs = 2000)
        {
            if (transactionId.isEmpty() || ! enter (timeoutMs))
                return false;

            {
                const juce::ScopedLock stateGuard (stateLock);
                if (activeTransaction.isNotEmpty())
                {
                    exit();
                    return false;
                }

                activeTransaction = transactionId;
            }

            return true;
        }

        void endTransaction (const juce::String& transactionId) noexcept
        {
            {
                const juce::ScopedLock stateGuard (stateLock);
                if (activeTransaction == transactionId)
                    activeTransaction.clear();
            }

            exit();
        }

        juce::String getActiveTransactionId() const
        {
            const juce::ScopedLock stateGuard (stateLock);
            return activeTransaction;
        }

    private:
        bool enter (int timeoutMs)
        {
            // CriticalSection is recursive for the owning thread. This lets a
            // pair transaction call saveMidiMap/saveNoteMap while still
            // serialising another thread in this process. JUCE's process lock
            // is also recursive when the same object is used.
            const auto started = juce::Time::getMillisecondCounterHiRes();
            if (timeoutMs < 0)
                intraProcess->enter();
            else
            {
                while (! intraProcess->tryEnter())
                {
                    const auto elapsed =
                        juce::Time::getMillisecondCounterHiRes() - started;
                    if (elapsed >= (double) timeoutMs)
                        return false;
                    juce::Thread::sleep (1);
                }
            }

            const auto elapsed = juce::Time::getMillisecondCounterHiRes()
                               - started;
            const int remaining = timeoutMs < 0
                ? timeoutMs
                : juce::jmax (0, timeoutMs - (int) elapsed);
            if (interProcess.enter (remaining))
                return true;

            intraProcess->exit();
            return false;
        }

        void exit() noexcept
        {
            interProcess.exit();
            intraProcess->exit();
        }

        std::shared_ptr<juce::CriticalSection> intraProcess;
       #if JUCE_ANDROID
        MidiMapSandboxFileLock interProcess;
       #else
        juce::InterProcessLock interProcess;
       #endif
        mutable juce::CriticalSection stateLock;
        juce::String activeTransaction;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiMapPairFileLock)
    };

    // CCs and notes each have a primary and a last-good file. All four are
    // checkpointed in a durable write-ahead journal before a pair transaction.
    // If the process or machine stops between the two file writes, startup can
    // replay the pending journal and restore the exact pre-operation pair.
    class MidiMapPairRollback
    {
    public:
        MidiMapPairRollback (const juce::File& ccFile,
                             const juce::File& noteFile)
            : ownedFileLock (
                  std::make_unique<MidiMapPairFileLock> (ccFile, noteFile)),
              fileLock (ownedFileLock.get()),
              targets (midiMapPairTargets (ccFile, noteFile)),
              journal (midiMapPairJournalFile (ccFile))
        {
        }

        MidiMapPairRollback (const juce::File& ccFile,
                             const juce::File& noteFile,
                             MidiMapPairFileLock& sharedFileLock,
                             MidiMapPairRevision* revisionToUse = nullptr)
            : fileLock (&sharedFileLock),
              revision (revisionToUse),
              targets (midiMapPairTargets (ccFile, noteFile)),
              journal (midiMapPairJournalFile (ccFile))
        {
        }

        ~MidiMapPairRollback()
        {
            releaseTransactionLock();
        }

        static bool recoverPendingTransaction (const juce::File& ccFile,
                                               const juce::File& noteFile)
        {
            MidiMapPairRollback recovery (ccFile, noteFile);
            MidiMapPairFileLock::ScopedAccess access (*recovery.fileLock, 5000);
            return access.isLocked()
                && midiMapPairTargetsInspectable (recovery.targets)
                && recovery.recoverPendingJournal();
        }

        static bool recoverPendingTransaction (const juce::File& ccFile,
                                               const juce::File& noteFile,
                                               MidiMapPairFileLock& sharedFileLock)
        {
            MidiMapPairRollback recovery (ccFile, noteFile, sharedFileLock);
            MidiMapPairFileLock::ScopedAccess access (sharedFileLock, 5000);
            return access.isLocked()
                && midiMapPairTargetsInspectable (recovery.targets)
                && recovery.recoverPendingJournal();
        }

        static bool journalAllowsWrite (
            const juce::File& journal,
            const juce::String& activeTransactionId);

        static void failCommitAfterCommittedJournalForTests (bool enabled)
        {
            commitAfterJournalFailpoint().store (enabled);
        }

        static void failRestoreAfterTargetCountForTests (int count)
        {
            restoreAfterTargetFailpoint().store (count);
        }

        static void failRestoreWithForeignTargetAfterPreflightForTests (
            int targetIndex)
        {
            restoreForeignTargetAfterPreflightFailpoint().store (
                targetIndex);
        }

        bool capture (const juce::String& label)
        {
            juce::ignoreUnused (label);

            const auto newTransactionId = juce::Uuid().toString();
            if (fileLock == nullptr
                 || ! fileLock->beginTransaction (newTransactionId))
                return false;

            lockHeld = true;
            lockTransactionId = newTransactionId;

            // Do not let recovery or a new journal reinterpret an existing but
            // unreadable quartet as four ordinary corrupt/absent files.
            if (! midiMapPairTargetsInspectable (targets))
            {
                uninspectableTargetConflict = true;
                releaseTransactionLock();
                return false;
            }

            if (! recoverPendingJournal())
            {
                releaseTransactionLock();
                return false;
            }

            transactionId = newTransactionId;
            // recoverPendingJournal may have accepted an already-committed v1
            // compatibility journal. From this point onward we are creating a
            // brand-new v3 transaction and ownership checks must use its
            // transactionId; never inherit the legacy no-owner semantics.
            loadedJournalVersion = 3;

            if (revision != nullptr && ! revision->matches (targets))
            {
                revisionConflict = true;
                releaseTransactionLock();
                return false;
            }

            for (size_t i = 0; i < targets.size(); ++i)
                if (! captureState (i))
                {
                    releaseTransactionLock();
                    return false;
                }

            // Schema ownership is a property of the complete quartet. If only
            // NOTE (or only CC) belongs to a newer build, touching the opposite
            // half would still create a mixed generation that the newer build
            // never published.
            if (capturedPairHasFutureSchema())
            {
                futureSchemaConflict = true;
                releaseTransactionLock();
                return false;
            }

            captured = writeJournal ("pending")
                && journalHasState ("pending");
            if (! captured)
                releaseTransactionLock();
            return captured;
        }

        bool restore()
        {
            // Only the transaction that still owns the pair lock may undo its
            // checkpoint. In particular, commit() verification failures keep
            // this lock held so no transaction B can replace the journal
            // between commit(false) and the caller's restore().
            if (! captured || ! lockHeld)
                return false;

            // commit() may already have durably changed the marker to
            // committed before a later CAS/ownership verification fails. A
            // rollback is itself a new multi-file transaction: re-arm this
            // exact journal as pending and verify it before touching target 1.
            if (! journalHasState ("pending"))
            {
                if (! journalHasState ("committed")
                     || ! writeJournal ("pending")
                     || ! journalHasState ("pending"))
                {
                    releaseTransactionLock();
                    return false;
                }
            }

            // Best-effort detection for a legacy/external writer that ignored
            // the pair lock. If its generation is already visible, preserve it
            // and acknowledge only this exact pending journal by SHA. Current
            // ChurchDAW writers cannot enter here concurrently because they all
            // hold the pair lock.
            if (hasExternalTargetChanges())
            {
                externalChangesPreserved = true;
                const bool acknowledged =
                    writeMidiMapPairAbandonMarker (
                        journal, transactionId);
                if (revision != nullptr)
                    revision->invalidate();
                if (acknowledged)
                    captured = false;
                releaseTransactionLock();
                return false;
            }

            const bool restored = restoreAllStatesOrLeaveJournal();

            if (restored)
            {
                // Never delete a shared journal path after verification: a
                // non-cooperating process could replace it in the gap. A
                // committed marker is itself the durable close operation and
                // is harmless to the next standalone save/capture.
                if (transactionId.isEmpty())
                    transactionId = juce::Uuid().toString();
                const bool closed = writeJournal ("committed")
                    && journalHasState ("committed");
                if (! closed)
                {
                    releaseTransactionLock();
                    return false;
                }

                if (revision != nullptr && ! revision->refresh (targets))
                {
                    revision->invalidate();
                    releaseTransactionLock();
                    return false;
                }

                if (! journalHasState ("committed")
                     || hasExternalTargetChanges())
                {
                    if (revision != nullptr)
                        revision->invalidate();
                    releaseTransactionLock();
                    return false;
                }

                captured = false;
            }

            releaseTransactionLock();
            return restored;
        }

        // A durable committed marker closes the journal. It deliberately stays
        // in place: deleting a shared path after verification has a TOCTOU gap
        // in which a non-cooperating process could publish a newer pending
        // journal and have it deleted by this transaction.
        bool commit()
        {
            if (! captured || ! lockHeld
                 || hasExternalTargetChanges())
                return false;

            const bool committed = writeJournal ("committed")
                && journalHasState ("committed");

            if (! committed)
            {
                // Deliberately retain ownership. Every mutating caller follows
                // a failed commit with restore(); releasing here would let a
                // second process publish a new journal that the first restore
                // could overwrite or delete.
                return false;
            }

            if (commitAfterJournalFailpoint().exchange (false))
                return false; // retain lock; caller must invoke restore()

            if (revision != nullptr && ! revision->refresh (targets))
            {
                revision->invalidate();
                return false;
            }

            // Refresh must not bless a generation that appeared between the
            // first marker verification and the digest read. Re-check both
            // ownership domains before releasing the transaction lock.
            if (! journalHasState ("committed")
                 || hasExternalTargetChanges())
            {
                if (revision != nullptr)
                    revision->invalidate();
                return false;
            }

            captured = false;
            releaseTransactionLock();
            return true;
        }

        // Close a checkpoint that performed no writes. If all targets still
        // match, this is an ordinary commit and the loaded CAS may advance. If
        // a lock-bypassing change is observed, preserve that generation, leave
        // the loaded revision invalid, and require a reload before another save.
        bool closeWithoutMutation()
        {
            if (! captured || ! lockHeld)
                return false;

            if (targetsUnchangedSinceCapture())
                return commit();

            externalChangesPreserved = true;
            const bool acknowledged = writeMidiMapPairAbandonMarker (
                journal, transactionId);
            if (revision != nullptr)
                revision->invalidate();
            if (acknowledged)
                captured = false;
            releaseTransactionLock();
            return acknowledged;
        }

        bool targetsUnchangedSinceCapture() const
        {
            if (! captured)
                return false;

            for (size_t i = 0; i < targets.size(); ++i)
                if (! targetMatchesState (i))
                    return false;

            return true;
        }

        bool hadRevisionConflict() const noexcept
        {
            return revisionConflict;
        }

        bool preservedExternalChanges() const noexcept
        {
            return externalChangesPreserved;
        }

        bool hadFutureSchemaConflict() const noexcept
        {
            return futureSchemaConflict;
        }

        bool hadUninspectableTargetConflict() const noexcept
        {
            return uninspectableTargetConflict;
        }

    private:
        struct State
        {
            bool existed = false;
            juce::int64 byteSize = 0;
            juce::String sha256;
            juce::MemoryBlock contents;

            bool operator== (const State& other) const noexcept
            {
                return existed == other.existed
                    && byteSize == other.byteSize
                    && sha256 == other.sha256
                    && contents == other.contents;
            }
        };

        bool captureState (size_t index)
        {
            auto& state = states[index];
            const auto& target = targets[index];
            state = {};

            if (! target.existsAsFile())
            {
                if (target.exists())
                    return false;
                state.sha256 = midiMapDataSha256 (state.contents);
                return true;
            }

            juce::MemoryBlock verification;
            if (! loadBoundedFileData (
                    target, state.contents, maxMidiMapFileBytes)
                 || ! loadBoundedFileData (
                    target, verification, maxMidiMapFileBytes)
                 || verification != state.contents)
                return false;

            state.existed = true;
            state.byteSize = (juce::int64) state.contents.getSize();
            state.sha256 = midiMapDataSha256 (state.contents);
            return true;
        }

        bool capturedPairHasFutureSchema() const
        {
            for (size_t i = 0; i < states.size(); ++i)
            {
                if (! states[i].existed)
                    continue;
                const auto xml = parseBoundedXmlData (states[i].contents);
                const auto root = (i == 0 || i == 2)
                                    ? juce::String ("MIDIMAP")
                                    : juce::String ("NOTEMAP");
                if (xml != nullptr && hasFutureMidiMapSchema (*xml, root))
                    return true;
            }
            return false;
        }

        bool targetMatchesState (size_t index) const
        {
            return targetMatchesCapturedState (index, index);
        }

        bool targetMatchesCapturedState (size_t targetIndex,
                                         size_t stateIndex) const
        {
            const auto& target = targets[targetIndex];
            const auto& state = states[stateIndex];
            if (! state.existed)
                return ! target.exists();
            if (! target.existsAsFile()
                 || target.getSize() != state.byteSize)
                return false;

            juce::MemoryBlock current;
            return loadBoundedFileData (
                       target, current, maxMidiMapFileBytes)
                && (juce::int64) current.getSize() == state.byteSize
                && midiMapDataSha256 (current) == state.sha256
                && current == state.contents;
        }

        bool currentTargetBelongsToExternalWriter (size_t index) const
        {
            // Unchanged targets often carry an older committed transactionId.
            // They still belong to this checkpoint and are always safe.
            if (targetMatchesState (index))
                return false;

            // saveMidiMap/saveNoteMap first promote the captured primary to
            // last-good. Those bytes legitimately carry the previous
            // transaction ID, but are still an exact state captured by us.
            if ((index == 2 && targetMatchesCapturedState (2, 0))
                 || (index == 3 && targetMatchesCapturedState (3, 1)))
                return false;

            const auto& target = targets[index];
            if (! target.existsAsFile())
                return true;

            const auto xml = loadBoundedXmlFile (target);
            if (xml == nullptr)
                return true;

            const auto root = (index == 0 || index == 2)
                                ? juce::String ("MIDIMAP")
                                : juce::String ("NOTEMAP");
            if (hasFutureMidiMapSchema (*xml, root))
                return true;

            // Version-1 journals predate transaction IDs. They can only
            // distinguish a future schema; preserving their recovery contract
            // is necessary for clients updating from that build.
            if (loadedJournalVersion == 1)
                return false;

            const auto writer = xml->getStringAttribute ("transactionId");
            return writer.isEmpty() || writer != transactionId;
        }

        bool restoreAllStatesOrLeaveJournal() const
        {
            // The pair lock excludes every current ChurchDAW writer for this
            // whole loop. Legacy tools/editors may ignore it, so perform a
            // best-effort quartet preflight and another ownership check inside
            // restoreState immediately before each mutation. Portable rename
            // APIs do not provide hash-CAS; the tiny check-to-rename window is
            // therefore outside the strong guarantee and failures stay pending.
            if (hasExternalTargetChanges())
                return false;

            const int foreignTarget =
                restoreForeignTargetAfterPreflightFailpoint().exchange (-1);
            if (foreignTarget >= 0
                 && foreignTarget < (int) targets.size())
            {
                juce::XmlElement foreign (
                    foreignTarget == 0 || foreignTarget == 2
                        ? "MIDIMAP" : "NOTEMAP");
                foreign.setAttribute ("schema", 1);
                foreign.setAttribute ("complete", 1);
                foreign.setAttribute (
                    "transactionId", "foreign-after-restore-preflight");
                foreign.setAttribute (
                    "testPayload", "must-be-preserved");
                (void) replaceFileWithTextAtomically (
                    targets[(size_t) foreignTarget],
                    foreign.toString (
                        juce::XmlElement::TextFormat().singleLine()));
            }

            int restoredTargets = 0;
            for (int i = (int) targets.size() - 1; i >= 0; --i)
            {
                if (! restoreState ((size_t) i))
                    return false;
                ++restoredTargets;
                const int failAfter = restoreAfterTargetFailpoint().load();
                if (failAfter > 0 && restoredTargets >= failAfter)
                {
                    restoreAfterTargetFailpoint().store (-1);
                    return false;
                }
            }

            for (size_t i = 0; i < targets.size(); ++i)
                if (! targetMatchesState (i))
                    return false;

            return true;
        }

        bool hasExternalTargetChanges() const
        {
            for (size_t i = 0; i < targets.size(); ++i)
                if (currentTargetBelongsToExternalWriter (i))
                    return true;
            return false;
        }

        bool restoreState (size_t index) const
        {
            const auto& target = targets[index];
            const auto& state = states[index];

            // Avoid an unnecessary rename for an already-restored file. Apart
            // from reducing I/O this is important for a future-schema neighbor:
            // byte-identical snapshots must not receive a new filesystem event.
            if (targetMatchesState (index))
                return state.existed
                    || syncMidiMapParentDirectory (target);

            // Re-check at the last portable point before replace/delete. The
            // strong no-interleaving guarantee comes from the pair lock used by
            // ChurchDAW writers; this additionally catches the deterministic
            // legacy/editor race between the quartet preflight and this target.
            if (currentTargetBelongsToExternalWriter (index))
                return false;

            if (state.existed)
                return replaceFileWithDataAtomically (target, state.contents)
                    && targetMatchesState (index);

            if (! target.exists())
                return true;

            return removeMidiMapFileDurably (target);
        }

        bool writeJournal (const juce::String& state) const
        {
            juce::XmlElement xml ("CHURCHDAW_MIDI_MAP_PAIR_TRANSACTION");
            xml.setAttribute ("version", 3);
            xml.setAttribute ("state", state);
            xml.setAttribute ("transactionId", transactionId);

            for (size_t i = 0; i < states.size(); ++i)
            {
                auto* file = xml.createNewChildElement ("FILE");
                file->setAttribute ("index", (int) i);
                file->setAttribute ("existed", states[i].existed ? 1 : 0);
                file->setAttribute ("byteSize",
                                    juce::String (states[i].byteSize));
                file->setAttribute ("sha256", states[i].sha256);
                auto* contents = file->createNewChildElement ("CONTENTS");
                contents->addTextElement (
                    states[i].contents.toBase64Encoding());
            }

            return replaceFileWithTextAtomically (
                journal, xml.toString (juce::XmlElement::TextFormat().singleLine()));
        }

        bool journalHasState (const juce::String& expected) const
        {
            if (! journal.existsAsFile()
                 || journal.getSize() < 0
                 || journal.getSize() > maxMidiMapJournalBytes)
                return false;

            auto xml = loadBoundedXmlFile (
                journal, midiMapJournalXmlLimits());
            if (xml == nullptr
                 || xml->getStringAttribute ("state") != expected)
                return false;

            std::array<State, 4> decoded;
            juce::String decodedTransaction;
            int decodedVersion = 0;
            return decodeStatesFromJournal (
                       *xml, decoded, decodedTransaction, decodedVersion)
                && decodedVersion == 3
                && decodedTransaction == transactionId
                && decoded == states;
        }

        static bool parseNonNegativeInt64Attribute (
            const juce::XmlElement& xml,
            const juce::String& name,
            juce::int64& result)
        {
            if (! xml.hasAttribute (name))
                return false;
            const auto text = xml.getStringAttribute (name);
            if (text.isEmpty() || text.length() > 18)
                return false;
            for (auto character : text)
                if (character < '0' || character > '9')
                    return false;
            result = text.getLargeIntValue();
            return result >= 0;
        }

        static bool decodeStatesFromJournal (
            const juce::XmlElement& xml,
            std::array<State, 4>& decoded,
            juce::String& decodedTransaction,
            int& decodedVersion)
        {
            constexpr juce::int64 maxEntryBytes = 8 * 1024 * 1024;
            constexpr juce::int64 maxTotalBytes = 32 * 1024 * 1024;
            const int version = xml.getIntAttribute ("version", 0);
            if (! xml.hasTagName ("CHURCHDAW_MIDI_MAP_PAIR_TRANSACTION")
                 || ! hasStrictIntegerAttribute (xml, "version")
                 || (version != 1 && version != 2 && version != 3)
                 || (xml.getStringAttribute ("state") != "pending"
                     && xml.getStringAttribute ("state") != "committed")
                 || xml.getNumChildElements() != (int) decoded.size())
                return false;

            const auto loadedTransactionId =
                xml.getStringAttribute ("transactionId");
            if (version >= 2 && loadedTransactionId.isEmpty())
                return false;

            std::array<bool, 4> seen {{ false, false, false, false }};
            juce::int64 totalBytes = 0;
            for (auto* file : xml.getChildIterator())
            {
                if (! file->hasTagName ("FILE")
                     || ! hasStrictIntegerAttribute (*file, "index")
                     || ! hasStrictIntegerAttribute (*file, "existed")
                     || file->getNumChildElements() != 1)
                    return false;

                const int index = file->getIntAttribute ("index", -1);
                const int existed = file->getIntAttribute ("existed", -1);
                auto* contents = file->getFirstChildElement();
                if (! juce::isPositiveAndBelow (index, (int) decoded.size())
                     || seen[(size_t) index]
                     || (existed != 0 && existed != 1)
                     || contents == nullptr
                     || ! contents->hasTagName ("CONTENTS"))
                    return false;

                seen[(size_t) index] = true;
                auto& target = decoded[(size_t) index];
                target = {};
                target.existed = existed != 0;

                if (version == 3)
                {
                    juce::int64 declaredSize = 0;
                    if (! parseNonNegativeInt64Attribute (
                            *file, "byteSize", declaredSize)
                         || declaredSize > maxEntryBytes
                         || totalBytes > maxTotalBytes - declaredSize
                         || ! file->hasAttribute ("sha256")
                         || ! decodeBoundedBase64 (
                                contents->getAllSubText(),
                                declaredSize,
                                target.contents)
                         || (juce::int64) target.contents.getSize()
                                != declaredSize)
                        return false;

                    totalBytes += declaredSize;
                    target.byteSize = declaredSize;
                    target.sha256 = file->getStringAttribute (
                        "sha256").toLowerCase();
                    if (target.sha256.length() != 64
                         || target.sha256
                                != midiMapDataSha256 (target.contents))
                        return false;
                }
                else
                {
                    const auto legacyText = contents->getAllSubText();
                    const auto legacySize =
                        (juce::int64) legacyText.getNumBytesAsUTF8();
                    if (legacySize > maxEntryBytes
                         || totalBytes > maxTotalBytes - legacySize)
                        return false;
                    target.contents.append (
                        legacyText.toRawUTF8(),
                        (size_t) legacyText.getNumBytesAsUTF8());
                    totalBytes += legacySize;
                    target.byteSize = (juce::int64) target.contents.getSize();
                    target.sha256 = midiMapDataSha256 (target.contents);
                }

                if (! target.existed && target.byteSize != 0)
                    return false;
            }

            for (const bool value : seen)
                if (! value)
                    return false;

            decodedTransaction = loadedTransactionId;
            decodedVersion = version;
            return true;
        }

        static bool decodeBoundedBase64 (
            const juce::String& encoded,
            juce::int64 declaredSize,
            juce::MemoryBlock& decoded)
        {
            if (declaredSize < 0
                 || declaredSize > 8 * 1024 * 1024)
                return false;

            const int dot = encoded.indexOfChar ('.');
            if (dot <= 0)
                return false;

            const auto prefix = encoded.substring (0, dot);
            if (prefix.length() > 18)
                return false;
            for (auto character : prefix)
                if (character < '0' || character > '9')
                    return false;
            if (prefix.getLargeIntValue() != declaredSize)
                return false;

            const juce::int64 encodedPayloadLength =
                ((declaredSize * 8) + 5) / 6;
            if ((juce::int64) encoded.length()
                    != (juce::int64) dot + 1 + encodedPayloadLength)
                return false;

            for (int i = dot + 1; i < encoded.length(); ++i)
            {
                const auto c = encoded[i];
                const bool valid = c == '.' || c == '+'
                    || (c >= 'A' && c <= 'Z')
                    || (c >= 'a' && c <= 'z')
                    || (c >= '0' && c <= '9');
                if (! valid)
                    return false;
            }

            // JUCE's decoder trusts its embedded length prefix. All length,
            // alphabet and encoded-size checks above therefore happen before
            // it is allowed to allocate.
            return decoded.fromBase64Encoding (encoded)
                && (juce::int64) decoded.getSize() == declaredSize;
        }

        bool loadStatesFromJournal (const juce::XmlElement& xml)
        {
            juce::String loadedTransaction;
            int version = 0;
            std::array<State, 4> loaded;
            if (! decodeStatesFromJournal (
                    xml, loaded, loadedTransaction, version))
                return false;

            states = std::move (loaded);
            transactionId = std::move (loadedTransaction);
            loadedJournalVersion = version;
            return true;
        }

        bool recoverPendingJournal()
        {
            if (! journal.existsAsFile())
                return ! journal.exists();

            if (journal.getSize() < 0
                 || journal.getSize() > maxMidiMapJournalBytes)
                return false;

            auto xml = loadBoundedXmlFile (
                journal, midiMapJournalXmlLimits());
            if (xml == nullptr || ! loadStatesFromJournal (*xml))
                return false;

            if (xml->getStringAttribute ("state") == "committed")
                return true;

            if (midiMapPairHasValidAbandonMarker (
                    journal, transactionId))
                return true;

            const bool restored = restoreAllStatesOrLeaveJournal();
            if (! restored)
                return false;

            if (transactionId.isEmpty())
                transactionId = juce::Uuid().toString();
            return writeJournal ("committed")
                && journalHasState ("committed");
        }

        void releaseTransactionLock() noexcept
        {
            if (! lockHeld || fileLock == nullptr)
                return;

            lockHeld = false;
            fileLock->endTransaction (lockTransactionId);
            lockTransactionId.clear();
        }

        std::unique_ptr<MidiMapPairFileLock> ownedFileLock;
        MidiMapPairFileLock* fileLock = nullptr;
        MidiMapPairRevision* revision = nullptr;
        std::array<juce::File, 4> targets;
        std::array<State, 4> states;
        juce::File journal;
        juce::String transactionId;
        juce::String lockTransactionId;
        int loadedJournalVersion = 3;
        bool captured = false;
        bool lockHeld = false;
        bool revisionConflict = false;
        bool externalChangesPreserved = false;
        bool futureSchemaConflict = false;
        bool uninspectableTargetConflict = false;

        static std::atomic<bool>& commitAfterJournalFailpoint()
        {
            static std::atomic<bool> enabled { false };
            return enabled;
        }

        static std::atomic<int>& restoreAfterTargetFailpoint()
        {
            static std::atomic<int> count { -1 };
            return count;
        }

        static std::atomic<int>&
        restoreForeignTargetAfterPreflightFailpoint()
        {
            static std::atomic<int> targetIndex { -1 };
            return targetIndex;
        }
    };

    inline bool MidiMapPairRollback::journalAllowsWrite (
        const juce::File& journal,
        const juce::String& activeTransactionId)
    {
        if (! journal.existsAsFile())
            return ! journal.exists();

        if (journal.getSize() < 0
             || journal.getSize() > maxMidiMapJournalBytes)
            return false;

        const auto xml = loadBoundedXmlFile (
            journal, midiMapJournalXmlLimits());
        if (xml == nullptr)
            return false;

        std::array<State, 4> decoded;
        juce::String owner;
        int version = 0;
        if (! decodeStatesFromJournal (
                *xml, decoded, owner, version))
            return false;

        const auto state = xml->getStringAttribute ("state");
        if (activeTransactionId.isNotEmpty())
        {
            // A nested half-save is legal only while the exact transaction
            // that captured the quartet still owns a pending journal. A
            // foreign committed marker is closed for standalone operations,
            // but must never authorise this transaction to mutate target 1:
            // its later commit/restore would have no journal it can prove.
            return state == "pending"
                && owner == activeTransactionId;
        }

        if (state == "committed")
            return true;

        // A transaction that observed a non-cooperating writer leaves its
        // exact pending journal plus an immutable acknowledgement marker.
        // The marker's SHA prevents a later writer reusing the fixed journal
        // path from being mistaken for the abandoned generation.
        return midiMapPairHasValidAbandonMarker (journal, owner);
    }

    inline bool midiMapPairJournalAllowsWrite (
        const juce::File& ccFile,
        const juce::String& activeTransactionId)
    {
        return MidiMapPairRollback::journalAllowsWrite (
            midiMapPairJournalFile (ccFile),
            activeTransactionId);
    }

    // A future/foreign target makes restore() fail closed and leaves the
    // journal pending. The helper name remains for source compatibility with
    // the call sites that introduced future-schema protection.
    inline bool rollbackMidiMapPairPreservingFutureSchema (
        MidiMapPairRollback& transaction,
        const juce::File& ccFile,
        const juce::File& noteFile)
    {
        juce::ignoreUnused (ccFile, noteFile);
        return transaction.restore();
    }

    inline bool promoteValidMidiMapToLastGood (const juce::File& mainFile,
                                               const juce::File& lastGood,
                                               const juce::String& root,
                                               const juce::String& childTag,
                                               const juce::String& secondChildTag = {})
    {
        juce::MemoryBlock bytes;
        if (! loadStableMidiMapFileData (
                mainFile, bytes, maxMidiMapFileBytes))
            return false;
        auto xml = parseBoundedXmlData (bytes);
        return xml != nullptr
            && isMidiMapXmlValid (*xml, root, childTag, secondChildTag)
            && replaceFileWithDataAtomically (lastGood, bytes);
    }

    inline std::unique_ptr<juce::XmlElement> recoverMidiMapXml (
        const juce::File& mainFile,
        const juce::File& lastGood,
        const juce::String& root,
        const juce::String& childTag,
        const juce::String& secondChildTag = {})
    {
        juce::MemoryBlock mainBytes;
        const auto mainInspection = inspectMidiMapTarget (
            mainFile, mainBytes);
        if (mainInspection == MidiMapTargetInspection::unknown)
            return {};
        auto main = mainInspection == MidiMapTargetInspection::readable
            ? parseBoundedXmlData (mainBytes) : nullptr;
        if (main != nullptr
             && isMidiMapXmlValid (*main, root, childTag, secondChildTag))
            return main;

        juce::MemoryBlock fallbackBytes;
        const auto fallbackInspection = inspectMidiMapTarget (
            lastGood, fallbackBytes);
        if (fallbackInspection == MidiMapTargetInspection::unknown)
            return {};
        auto fallback = fallbackInspection == MidiMapTargetInspection::readable
            ? parseBoundedXmlData (fallbackBytes) : nullptr;
        if (fallback != nullptr
             && isMidiMapXmlValid (*fallback, root, childTag, secondChildTag))
        {
            // Loading the backup only in memory leaves the corrupt main in
            // place, so every launch repeats recovery and a later save may
            // promote the wrong file. Repair it atomically from the validated
            // snapshot. A read-only filesystem may refuse the repair; the
            // caller can still use the in-memory fallback without data loss.
            (void) replaceFileWithDataAtomically (mainFile, fallbackBytes);
            return fallback;
        }

        return {};
    }

    inline juce::File uniqueCorruptCopyTarget (const juce::File& file)
    {
        return file.getParentDirectory().getNonexistentChildFile (
            file.getFileName() + ".corrupt-"
                + juce::String (juce::Time::getMillisecondCounter()),
            {}, false);
    }

    struct ControllerKey
    {
        juce::String sourceId;
        int channel = 0;
        int cc = -1;
        bool isNote = false;
        // Human-readable identity survives an offline Android sourceId.
        // Deliberately excluded from ordering: it is metadata, never lookup.
        juce::String sourceName;

        bool isValid() const noexcept
        {
            return sourceId.isNotEmpty()
                    && channel >= 1 && channel <= 16
                    && cc >= 0 && cc <= 127;
        }

        bool operator< (const ControllerKey& other) const noexcept
        {
            const int sourceOrder = sourceId.compare (other.sourceId);

            if (sourceOrder != 0)
                return sourceOrder < 0;

            if (channel != other.channel)
                return channel < other.channel;

            if (cc != other.cc)
                return cc < other.cc;

            return isNote < other.isNote;
        }
    };

    // Persisted payload shared by MainComponent and the restart/round-trip
    // tests. pluginId binds every new mapping (hosted or internal) to the
    // processor that exposed the parameter. Slot + parameter ID alone is not
    // sufficient because another processor in the same location may publish
    // the same ID. An empty pluginId is accepted only as preserved legacy data
    // and must be explicitly relinked by the user before it can run.
    inline juce::String canonicalizePersistedProcessorIdentity (
        const juce::String& identifier);

    struct PersistedCCTarget
    {
        int slot = -2;
        int param = -1;       // diagnostics/migration only; never resolves alone
        int insert = -1;
        juce::String paramName;
        juce::String paramId;
        juce::String pluginId;
    };

    struct PersistedCCMapping
    {
        ControllerKey control;
        PersistedCCTarget target;
    };

    struct PersistedNoteMapping
    {
        ControllerKey control;
        int action = 0;
        int argument = -1;
    };

    inline void appendPersistedCCMapping (
        juce::XmlElement& root,
        const ControllerKey& control,
        const PersistedCCTarget& target,
        const juce::String& persistedSourceName)
    {
        auto* element = root.createNewChildElement ("MAP");
        element->setAttribute ("source", control.sourceId);
        element->setAttribute ("channel", control.channel);
        element->setAttribute ("cc", control.cc);
        element->setAttribute ("isNote", control.isNote ? 1 : 0);
        element->setAttribute ("slot", target.slot);
        element->setAttribute ("param", target.param);
        element->setAttribute ("insert", target.insert);
        element->setAttribute ("name", target.paramName);
        element->setAttribute ("id", target.paramId);

        // Optional in schema 1 only for backward compatibility. Current code
        // never creates an empty identity, but preserves an old empty value so
        // the panel can offer an explicit, atomic relink.
        if (target.pluginId.isNotEmpty())
            element->setAttribute ("pluginId", target.pluginId);

        element->setAttribute ("sourceName", persistedSourceName);
    }

    inline bool readPersistedCCMapping (const juce::XmlElement& element,
                                        PersistedCCMapping& result)
    {
        if (! element.hasTagName ("MAP"))
            return false;

        result.control = {
            normalizeMidiSourceId (element.getStringAttribute ("source")),
            element.getIntAttribute ("channel", 0),
            element.getIntAttribute ("cc", -1),
            element.getIntAttribute ("isNote", 0) != 0,
            element.getStringAttribute ("sourceName")
        };
        result.target = {
            element.getIntAttribute ("slot", -2),
            element.getIntAttribute ("param", -1),
            element.getIntAttribute ("insert", -1),
            element.getStringAttribute ("name"),
            element.getStringAttribute ("id"),
            canonicalizePersistedProcessorIdentity (
                element.getStringAttribute ("pluginId"))
        };
        return true;
    }

    inline void appendPersistedNoteMapping (
        juce::XmlElement& root,
        const ControllerKey& control,
        int action,
        int argument,
        const juce::String& persistedSourceName)
    {
        auto* element = root.createNewChildElement ("NOTE");
        element->setAttribute ("source", control.sourceId);
        element->setAttribute ("channel", control.channel);
        element->setAttribute ("note", control.cc);
        element->setAttribute ("action", action);
        element->setAttribute ("arg", argument);
        element->setAttribute ("sourceName", persistedSourceName);
    }

    inline bool readPersistedNoteMapping (const juce::XmlElement& element,
                                          PersistedNoteMapping& result)
    {
        if (! element.hasTagName ("NOTE"))
            return false;

        result.control = {
            normalizeMidiSourceId (element.getStringAttribute ("source")),
            element.getIntAttribute ("channel", 0),
            element.getIntAttribute ("note", -1),
            false,
            element.getStringAttribute ("sourceName")
        };
        result.action = element.getIntAttribute ("action", 0);
        result.argument = element.getIntAttribute ("arg", -1);
        return true;
    }

    enum class ProcessorTargetMatch
    {
        ready,
        pending,
        incompatible,
        needsExplicitRelink
    };

    inline const juce::String& masterProcessorIdentity()
    {
        static const juce::String value { "internal:v1:master" };
        return value;
    }

    inline const juce::String& padProcessorIdentity()
    {
        static const juce::String value { "internal:v1:pad-continuous" };
        return value;
    }

    inline const juce::String& padSynthProcessorIdentity()
    {
        static const juce::String value { "internal:v1:pad-synth" };
        return value;
    }

    inline juce::String internalSlotProcessorIdentity (
        const juce::String& internalIdentifier)
    {
        return internalIdentifier.isNotEmpty()
            ? "internal:v1:slot:" + internalIdentifier
            : juce::String();
    }

    // hosted:v1 is the migration form used by the first plug-in identity
    // implementation. It retained JUCE's 32-bit fileOrIdentifier hash, so it
    // is never promoted to hosted:v2 or trusted automatically: distinct paths
    // such as "Aa" and "BB" collide in that hash. It remains readable only so
    // the panel can offer the explicit, transactional relink flow.
    inline juce::String canonicalizePersistedProcessorIdentity (
        const juce::String& identifier)
    {
        if (identifier.isEmpty()
             || identifier.startsWith ("internal:v1:")
             || identifier.startsWith ("hosted:v1:")
             || identifier.startsWith ("hosted:v2:"))
            return identifier;

        const int uidSeparator = identifier.lastIndexOfChar ('-');
        const int pathSeparator = uidSeparator > 0
            ? identifier.substring (0, uidSeparator).lastIndexOfChar ('-')
            : -1;
        if (pathSeparator < 0 || uidSeparator <= pathSeparator + 1
             || uidSeparator >= identifier.length() - 1)
            return identifier;

        const auto pathHash = identifier.substring (
            pathSeparator + 1, uidSeparator);
        const auto uid = identifier.substring (uidSeparator + 1);
        if (! pathHash.containsOnly ("0123456789abcdefABCDEF")
             || ! uid.containsOnly ("0123456789abcdefABCDEF"))
            return identifier;

        return "hosted:v1:" + pathHash.toLowerCase()
             + ":" + uid.toLowerCase();
    }

    inline juce::String hostedPluginIdentityForDescription (
        const juce::PluginDescription& description)
    {
        // Length-prefix every variable-width field before hashing so embedded
        // separators cannot make two descriptions share the same payload.
        // Display name and version are deliberately excluded: a cosmetic
        // rename/update must not break a mapping. The format, complete native
        // file/identifier and both JUCE UIDs describe the hosted installation.
        // hosted:v2 is the SHA-256 of this canonical description payload; it
        // is deliberately not a digest of the plug-in binary bytes.
        juce::String payload { "ChurchDAW-hosted-plugin-v2;" };
        const auto appendField = [&payload] (const char* tag,
                                             const juce::String& value)
        {
            payload += tag;
            payload += juce::String (value.getNumBytesAsUTF8());
            payload += ":";
            payload += value;
            payload += ";";
        };
        appendField ("format=", description.pluginFormatName);
        appendField ("file=", description.fileOrIdentifier);
        appendField ("uid=", juce::String::toHexString (
            static_cast<juce::int64> (
                static_cast<juce::uint32> (description.uniqueId))));
        appendField ("deprecatedUid=", juce::String::toHexString (
            static_cast<juce::int64> (
                static_cast<juce::uint32> (description.deprecatedUid))));

        const auto* utf8 = payload.toRawUTF8();
        return "hosted:v2:"
             + juce::SHA256 (utf8,
                             static_cast<size_t> (
                                 payload.getNumBytesAsUTF8()))
                    .toHexString().toLowerCase();
    }

    inline bool controllerKeysExactlyEqual (const ControllerKey& first,
                                             const ControllerKey& second) noexcept
    {
        return first.sourceId == second.sourceId
            && first.sourceName == second.sourceName
            && first.channel == second.channel
            && first.cc == second.cc
            && first.isNote == second.isNote;
    }

    inline bool persistedTargetsExactlyEqual (
        const PersistedCCTarget& first,
        const PersistedCCTarget& second) noexcept
    {
        return first.slot == second.slot
            && first.param == second.param
            && first.insert == second.insert
            && first.paramName == second.paramName
            && first.paramId == second.paramId
            && first.pluginId == second.pluginId;
    }

    struct ParameterCommandSnapshot
    {
        PersistedCCTarget selectedTarget;
        juce::String expectedCurrentPluginId;
        bool expectedEntryPresent = false;
        ControllerKey persistedControl;
        PersistedCCTarget persistedTarget;
    };

    struct ParameterCommandObservation
    {
        PersistedCCTarget selectedTarget;
        juce::String currentPluginId;
        bool entryPresent = false;
        ControllerKey persistedControl;
        PersistedCCTarget persistedTarget;
    };

    enum class ParameterCommandValidation
    {
        valid,
        currentProcessorChanged,
        selectedTargetChanged,
        entryPresenceChanged,
        persistedEntryChanged
    };

    // One fail-closed stale-command policy shared by the real Save callsite
    // and its tests. It applies equally to active, unmapped and explicit
    // relink rows: the processor and selected parameter must still be exactly
    // what the panel showed, and the persisted entry must still have the same
    // presence and (when present) the same physical key and payload.
    inline ParameterCommandValidation validateParameterCommandSnapshot (
        const ParameterCommandSnapshot& expected,
        const ParameterCommandObservation& current) noexcept
    {
        if (expected.expectedCurrentPluginId.isEmpty()
             || expected.expectedCurrentPluginId != current.currentPluginId)
            return ParameterCommandValidation::currentProcessorChanged;

        if (! persistedTargetsExactlyEqual (
                expected.selectedTarget, current.selectedTarget))
            return ParameterCommandValidation::selectedTargetChanged;

        if (expected.expectedEntryPresent != current.entryPresent)
            return ParameterCommandValidation::entryPresenceChanged;

        if (expected.expectedEntryPresent
             && (! controllerKeysExactlyEqual (
                    expected.persistedControl, current.persistedControl)
                 || ! persistedTargetsExactlyEqual (
                    expected.persistedTarget, current.persistedTarget)))
            return ParameterCommandValidation::persistedEntryChanged;

        return ParameterCommandValidation::valid;
    }

    struct MidiTargetIdentityPolicy
    {
        static bool sameParameterCoordinates (
            const PersistedCCTarget& left,
            const PersistedCCTarget& right) noexcept
        {
            if (left.slot != right.slot || left.insert != right.insert)
                return false;

            // slot -1 is the single dedicated Master-volume target. Historic
            // code used id "master" while the processor parameter exposes
            // "masterdb"; location is the stable, unambiguous coordinate.
            if (left.slot == -1 && left.insert < 0)
                return true;

            if (left.paramId.isNotEmpty() && right.paramId.isNotEmpty())
                return left.paramId == right.paramId;

            return left.paramName.isNotEmpty()
                && right.paramName.isNotEmpty()
                && left.paramName == right.paramName;
        }

        static ProcessorTargetMatch classifyProcessor (
            const juce::String& persistedIdentity,
            const juce::String& currentIdentity,
            bool currentProcessorAvailable) noexcept
        {
            if (! currentProcessorAvailable || currentIdentity.isEmpty())
                return ProcessorTargetMatch::pending;

            if (persistedIdentity.isEmpty())
                return ProcessorTargetMatch::needsExplicitRelink;

            return persistedIdentity == currentIdentity
                ? ProcessorTargetMatch::ready
                : ProcessorTargetMatch::incompatible;
        }

        // The single equality policy used by panel, remap and delete. Empty
        // legacy identities are never equal to a current processor, and two
        // same-slot plug-ins with the same parameter ID never cross.
        static bool sameTarget (const PersistedCCTarget& left,
                                const PersistedCCTarget& right) noexcept
        {
            return left.pluginId.isNotEmpty()
                && right.pluginId.isNotEmpty()
                && left.pluginId == right.pluginId
                && sameParameterCoordinates (left, right);
        }

        // Relinking is deliberately a separate policy. It is only used after
        // the user presses Save for the exact physical control shown as
        // pending. It never participates in apply, ordinary remap or delete.
        static bool mayExplicitlyRelink (
            const PersistedCCTarget& persisted,
            const PersistedCCTarget& selectedCurrentTarget) noexcept
        {
            return selectedCurrentTarget.pluginId.isNotEmpty()
                && persisted.pluginId != selectedCurrentTarget.pluginId
                && sameParameterCoordinates (
                    persisted, selectedCurrentTarget);
        }
    };

    enum class PersistedTargetPanelRow
    {
        consumedByBuiltInRow,
        active,
        unavailable,
        needsRelink,
        unresolvedParameter
    };

    // Shared by MainComponent and tests. A ready external/insert target still
    // needs its own manageable row unless a built-in row consumed that exact
    // mapping. Unavailable/incompatible targets remain visible and deletable.
    inline PersistedTargetPanelRow classifyPersistedTargetPanelRow (
        bool consumedByBuiltInRow,
        ProcessorTargetMatch processorMatch,
        bool parameterResolved) noexcept
    {
        if (consumedByBuiltInRow)
            return PersistedTargetPanelRow::consumedByBuiltInRow;

        if (processorMatch == ProcessorTargetMatch::ready)
            return parameterResolved ? PersistedTargetPanelRow::active
                                     : PersistedTargetPanelRow::unresolvedParameter;

        if (processorMatch == ProcessorTargetMatch::pending)
            return PersistedTargetPanelRow::unavailable;

        return PersistedTargetPanelRow::needsRelink;
    }

    enum class MidiTargetLocationKind
    {
        invalid,
        master,
        pad,
        padSynth,
        slot,
        insert
    };

    inline MidiTargetLocationKind classifyMidiTargetLocation (
        int slot, int insert, int slotCount,
        int padSlot, int padSynthSlot) noexcept
    {
        if (slot == -1 && insert < 0)
            return MidiTargetLocationKind::master;
        if (slot == padSlot && insert < 0)
            return MidiTargetLocationKind::pad;
        if (slot == padSynthSlot && insert < 0)
            return MidiTargetLocationKind::padSynth;
        if (slot >= 0 && slot < slotCount)
            return insert >= 0 ? MidiTargetLocationKind::insert
                               : MidiTargetLocationKind::slot;
        return MidiTargetLocationKind::invalid;
    }

    inline bool currentProcessorIdentityMatchesSelection (
        const juce::String& expectedCurrentIdentity,
        const juce::String& currentIdentity) noexcept
    {
        return expectedCurrentIdentity.isNotEmpty()
            && expectedCurrentIdentity == currentIdentity;
    }

    struct ParameterTouchEvent
    {
        std::uintptr_t processor = 0;
        int parameterIndex = -1;
        juce::uint64 learnGeneration = 0;
        juce::uint64 topologyGeneration = 0;
        juce::uint64 publicationSequence = 0;
    };

    inline bool parameterTouchBelongsToGeneration (
        const ParameterTouchEvent& event,
        juce::uint64 currentLearnGeneration,
        juce::uint64 currentTopologyGeneration) noexcept
    {
        return event.processor != 0
            && event.parameterIndex >= 0
            && event.learnGeneration == currentLearnGeneration
            && event.topologyGeneration == currentTopologyGeneration;
    }

    inline bool parameterLearnTargetStillCurrent (
        juce::uint64 armedTopologyGeneration,
        juce::uint64 currentTopologyGeneration,
        const juce::String& armedPluginId,
        const juce::String& currentPluginId,
        bool processorPresent) noexcept
    {
        return processorPresent
            && armedTopologyGeneration != 0
            && armedTopologyGeneration == currentTopologyGeneration
            && armedPluginId.isNotEmpty()
            && armedPluginId == currentPluginId;
    }

    inline bool midiLearnPublicationConsumesRawInput (
        bool publicationAccepted,
        bool targetArmed,
        bool indexedRouteMatched) noexcept
    {
        return publicationAccepted && (targetArmed || indexedRouteMatched);
    }

    // Non-droppable lifetime gate for AudioProcessorListener callbacks. The
    // high bit closes admission; the low bits count every callback that was
    // admitted, including nested/reentrant callbacks on the same thread.
    // fetch_add is intentional: a one-shot CAS would drop a mandatory action
    // (notably global transpose synchronisation) merely because another
    // callback changed the count concurrently.
    class ParameterCallbackLifetimeGate
    {
    public:
        static constexpr juce::uint32 closingBit = 0x80000000u;

        class Lease
        {
        public:
            Lease() noexcept = default;
            Lease (const Lease&) = delete;
            Lease& operator= (const Lease&) = delete;

            Lease (Lease&& other) noexcept
                : owner (std::exchange (other.owner, nullptr))
            {
            }

            Lease& operator= (Lease&& other) noexcept
            {
                if (this != &other)
                {
                    release();
                    owner = std::exchange (other.owner, nullptr);
                }
                return *this;
            }

            ~Lease() { release(); }

            explicit operator bool() const noexcept
            {
                return owner != nullptr;
            }

        private:
            friend class ParameterCallbackLifetimeGate;
            explicit Lease (ParameterCallbackLifetimeGate* gate) noexcept
                : owner (gate)
            {
            }

            void release() noexcept
            {
                if (owner != nullptr)
                {
                    const auto previous = owner->state.fetch_sub (
                        1, std::memory_order_release);
                    if ((previous & ~closingBit) == 0)
                    {
                        owner->state.fetch_add (
                            1, std::memory_order_relaxed);
                        jassertfalse; // moved/double-released Lease
                    }
                    owner = nullptr;
                }
            }

            ParameterCallbackLifetimeGate* owner = nullptr;
        };

        Lease tryEnter() noexcept
        {
            if (permanentlyClosed.load (std::memory_order_acquire))
                return {};

            const auto previous = state.fetch_add (
                1, std::memory_order_acq_rel);
            const auto previousCount = previous & ~closingBit;
            if ((previous & closingBit) == 0
                 && previousCount != ~closingBit)
                return Lease (this);

            state.fetch_sub (1, std::memory_order_release);
            return {};
        }

        void closeTemporarily() noexcept
        {
            state.fetch_or (closingBit, std::memory_order_acq_rel);
        }

        void closePermanently() noexcept
        {
            permanentlyClosed.store (true, std::memory_order_release);
            state.fetch_or (closingBit, std::memory_order_acq_rel);
        }

        bool reopen() noexcept
        {
            jassert (isDrained());
            if (permanentlyClosed.load (std::memory_order_acquire))
                return false;
            state.fetch_and (~closingBit, std::memory_order_release);
            return true;
        }

        bool isClosed() const noexcept
        {
            return (state.load (std::memory_order_acquire) & closingBit) != 0;
        }

        bool isDrained() const noexcept
        {
            return activeCount() == 0;
        }

        juce::uint32 activeCount() const noexcept
        {
            return state.load (std::memory_order_acquire) & ~closingBit;
        }

    private:
        static_assert (std::atomic<juce::uint32>::is_always_lock_free,
                       "Parameter callback admission must stay lock-free");
        std::atomic<juce::uint32> state { 0 };
        std::atomic<bool> permanentlyClosed { false };
    };

    // Lifetime gate for message-thread frames that may call third-party code.
    // The owner is the application, not the component protected by the gate.
    // Closing is permanent for one owner cycle; resetAfterDrain() is called
    // only after that owner has been destroyed and before a replacement is
    // published. A release only asks the stable owner to schedule work -- it
    // never runs the teardown continuation inline.
    class DeferredOwnerTeardownGate
    {
    public:
        static constexpr juce::uint32 closingBit = 0x80000000u;
        using DrainReadyCallback = void (*) (void*) noexcept;

        class Lease
        {
        public:
            Lease() noexcept = default;
            Lease (const Lease&) = delete;
            Lease& operator= (const Lease&) = delete;

            Lease (Lease&& other) noexcept
                : owner (std::exchange (other.owner, nullptr))
            {
            }

            Lease& operator= (Lease&& other) noexcept
            {
                if (this != &other)
                {
                    release();
                    owner = std::exchange (other.owner, nullptr);
                }
                return *this;
            }

            ~Lease() { release(); }

            explicit operator bool() const noexcept
            {
                return owner != nullptr;
            }

        private:
            friend class DeferredOwnerTeardownGate;
            explicit Lease (DeferredOwnerTeardownGate* gate) noexcept
                : owner (gate)
            {
            }

            void release() noexcept
            {
                if (owner == nullptr)
                    return;

                auto* const gate = std::exchange (owner, nullptr);
                const auto previous = gate->state.fetch_sub (
                    1, std::memory_order_acq_rel);
                if ((previous & ~closingBit) == 0)
                {
                    gate->state.fetch_add (1, std::memory_order_relaxed);
                    jassertfalse;
                    return;
                }

                if (previous == (closingBit | 1u))
                    gate->scheduleDrainReady();
            }

            DeferredOwnerTeardownGate* owner = nullptr;
        };

        void setDrainReadyCallback (void* contextToUse,
                                    DrainReadyCallback callbackToUse) noexcept
        {
            callbackContext = contextToUse;
            drainReadyCallback = callbackToUse;
        }

        // One strong CAS is deliberate. Contention, closure and counter
        // saturation all fail closed; no caller can spin before entering
        // third-party code.
        Lease tryEnter() noexcept
        {
            auto expected = state.load (std::memory_order_acquire);
            const auto count = expected & ~closingBit;
            if ((expected & closingBit) != 0 || count == ~closingBit)
                return {};

            if (! state.compare_exchange_strong (
                    expected, expected + 1,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire))
                return {};

            return Lease (this);
        }

        // Returns true only for the first request in the current owner cycle.
        bool requestClose() noexcept
        {
            const auto previous = state.fetch_or (
                closingBit, std::memory_order_acq_rel);
            if ((previous & ~closingBit) == 0)
                scheduleDrainReady();
            return (previous & closingBit) == 0;
        }

        bool consumeScheduledDrain() noexcept
        {
            return drainScheduled.exchange (false,
                                            std::memory_order_acq_rel);
        }

        bool resetAfterDrain() noexcept
        {
            if (! isClosedAndDrained())
                return false;
            drainScheduled.store (false, std::memory_order_release);
            state.store (0, std::memory_order_release);
            return true;
        }

        bool isClosed() const noexcept
        {
            return (state.load (std::memory_order_acquire) & closingBit) != 0;
        }

        bool isDrained() const noexcept
        {
            return activeCount() == 0;
        }

        bool isClosedAndDrained() const noexcept
        {
            return state.load (std::memory_order_acquire) == closingBit;
        }

        juce::uint32 activeCount() const noexcept
        {
            return state.load (std::memory_order_acquire) & ~closingBit;
        }

    private:
        void scheduleDrainReady() noexcept
        {
            bool expected = false;
            if (! drainScheduled.compare_exchange_strong (
                    expected, true,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire))
                return;

            if (drainReadyCallback != nullptr)
                drainReadyCallback (callbackContext);
        }

        static_assert (std::atomic<juce::uint32>::is_always_lock_free,
                       "External owner-frame admission must stay lock-free");
        std::atomic<juce::uint32> state { 0 };
        std::atomic<bool> drainScheduled { false };
        void* callbackContext = nullptr; // stable application lifetime
        DrainReadyCallback drainReadyCallback = nullptr;
    };

    // A graph slot can retain at most one disconnected node. A second node is
    // refused before publication, so modal replacement cannot turn the graph
    // itself into an unbounded retirement collection.
    class SinglePendingRetirementSlot
    {
    public:
        bool tryReserve (juce::uint32 node) noexcept
        {
            if (node == 0)
                return false;
            if (pendingNode == node)
                return true;
            if (pendingNode != 0)
                return false;
            pendingNode = node;
            return true;
        }

        bool clearIf (juce::uint32 node) noexcept
        {
            if (pendingNode != node || node == 0)
                return false;
            pendingNode = 0;
            return true;
        }

        bool hasPending() const noexcept { return pendingNode != 0; }
        juce::uint32 node() const noexcept { return pendingNode; }

    private:
        juce::uint32 pendingNode = 0; // message thread only
    };

    // One fixed insert-listener route. Writers run on the message thread.
    // Callbacks compare only raw addresses, then revalidate after reading the
    // generation; no plug-in object is cast or dereferenced here.
    class ParameterCallbackRoute
    {
    public:
        struct Snapshot
        {
            juce::uint64 topologyGeneration = 0;
        };

        void publish (const void* newProcessor,
                      juce::uint64 newTopologyGeneration) noexcept
        {
            processor.store (nullptr, std::memory_order_release);
            generation.store (newTopologyGeneration,
                              std::memory_order_release);
            processor.store (newProcessor, std::memory_order_release);
        }

        bool invalidate (const void* expectedProcessor = nullptr) noexcept
        {
            auto* current = processor.load (std::memory_order_acquire);
            if (expectedProcessor != nullptr && current != expectedProcessor)
                return false;

            processor.store (nullptr, std::memory_order_release);
            return current != nullptr;
        }

        bool snapshotIfCurrent (const void* expectedProcessor,
                                Snapshot& result) const noexcept
        {
            if (expectedProcessor == nullptr
                 || processor.load (std::memory_order_acquire)
                        != expectedProcessor)
                return false;

            const auto observedGeneration = generation.load (
                std::memory_order_acquire);
            if (processor.load (std::memory_order_acquire)
                    != expectedProcessor)
                return false;

            result.topologyGeneration = observedGeneration;
            return true;
        }

    private:
        std::atomic<const void*> processor { nullptr };
        std::atomic<juce::uint64> generation { 0 };
    };

    inline int globalTransposeSemitonesFromNormalised (
        float normalisedValue) noexcept
    {
        return juce::jlimit (
            -12, 12,
            juce::roundToInt (juce::jmap (
                juce::jlimit (0.0f, 1.0f, normalisedValue),
                -12.0f, 12.0f)));
    }

    struct ControllerRouteEvent
    {
        juce::uint16 routeSlot = 0;
        juce::uint8 value = 0;
        juce::uint64 routeGeneration = 0;
        juce::uint64 mappingGeneration = 0;
        juce::uint64 publicationSequence = 0;
        juce::uint64 attemptSequence = 0;
    };

    enum class ControllerRoutePublishResult
    {
        published,
        notMapped,
        staleTable,
        contention
    };

    // Fixed route index plus one latest-wins cell per route. Route metadata is
    // published as atomic POD words; CCTarget/ControllerKey strings stay in a
    // parallel owner table that only the message thread reads and writes.
    // The producer API deliberately accepts bytes, integers and booleans only:
    // it cannot construct/copy/destroy a juce::String or grow storage.
    template <size_t Capacity, size_t MaxSourceBytes = 192>
    class FixedControllerRouteDispatcher
    {
    public:
        static_assert (Capacity > 0, "controller routes need storage");
        static_assert (Capacity <= std::numeric_limits<juce::uint16>::max(),
                       "routeSlot must represent every physical slot");
        static_assert (MaxSourceBytes > 0, "source identity needs storage");
        static_assert (std::is_trivially_copyable<ControllerRouteEvent>::value,
                       "callback route publication must remain POD");

        static constexpr size_t capacity() noexcept { return Capacity; }
        static constexpr size_t maximumSourceBytes() noexcept
        {
            return MaxSourceBytes;
        }

        // Message-thread writer: block value publication, overwrite individual
        // slots, then publish the map generation with finishRouteUpdate().
        // Route cells are retired one by one (publishRoute) or at the end
        // (finishRouteUpdate), never zeroed wholesale. The owning integration
        // must stop producer scans across publishRoute calls because each slot
        // is deliberately made inactive while its metadata is overwritten.
        void beginRouteUpdate() noexcept
        {
            tableMappingGeneration.store (0, std::memory_order_release);
        }

        juce::uint64 publishRoute (size_t routeSlot,
                                   const char* sourceBytes,
                                   size_t sourceLength,
                                   int channel, int cc,
                                   bool isNote) noexcept
        {
            if (routeSlot >= Capacity || sourceBytes == nullptr
                 || sourceLength == 0 || sourceLength > MaxSourceBytes
                 || channel < 1 || channel > 16 || cc < 0 || cc > 127)
                return 0;

            auto& route = routes[routeSlot];
            route.activeGeneration.store (0, std::memory_order_release);

            route.sourceHash.store (
                hashSource (sourceBytes, sourceLength),
                std::memory_order_relaxed);
            route.sourceLength.store (
                static_cast<juce::uint64> (sourceLength),
                std::memory_order_relaxed);
            for (size_t chunk = 0; chunk < sourceChunkCount; ++chunk)
                route.sourceChunks[chunk].store (
                    packSourceChunk (sourceBytes, sourceLength, chunk),
                    std::memory_order_relaxed);
            route.channel.store (static_cast<juce::uint64> (channel),
                                 std::memory_order_relaxed);
            route.cc.store (static_cast<juce::uint64> (cc),
                            std::memory_order_relaxed);
            route.isNote.store (isNote ? 1u : 0u,
                                std::memory_order_relaxed);

            auto generation = route.generationCounter.fetch_add (
                                  1, std::memory_order_relaxed) + 1;
            if (generation == 0)
                generation = route.generationCounter.fetch_add (
                                 1, std::memory_order_relaxed) + 1;
            route.activeGeneration.store (
                generation, std::memory_order_release);
            return generation;
        }

        void finishRouteUpdate (juce::uint64 mappingGeneration,
                                size_t requestedRouteCount,
                                size_t publishedRouteCount) noexcept
        {
            // Retire leftovers from the previous table before re-opening
            // publication; slots below publishedRouteCount were overwritten.
            for (size_t index = publishedRouteCount; index < Capacity; ++index)
                routes[index].activeGeneration.store (
                    0, std::memory_order_release);

            const bool overflow = requestedRouteCount > Capacity
                               || publishedRouteCount < requestedRouteCount;
            const bool wasOverflowed = routeOverflow.exchange (
                overflow, std::memory_order_acq_rel);
            if (overflow && ! wasOverflowed)
                overflowEpisodes.fetch_add (1, std::memory_order_relaxed);

            tableMappingGeneration.store (
                mappingGeneration, std::memory_order_release);
        }

        ControllerRoutePublishResult tryPublish (
            const char* sourceBytes, size_t sourceLength,
            int channel, int cc, bool isNote, int value,
            juce::uint64 currentMappingGeneration) noexcept
        {
            if (sourceBytes == nullptr || sourceLength == 0
                 || sourceLength > MaxSourceBytes
                 || channel < 1 || channel > 16 || cc < 0 || cc > 127)
                return ControllerRoutePublishResult::notMapped;

            if (tableMappingGeneration.load (std::memory_order_acquire)
                    != currentMappingGeneration)
                return ControllerRoutePublishResult::staleTable;

            const auto wantedHash = hashSource (sourceBytes, sourceLength);
            for (size_t index = 0; index < Capacity; ++index)
            {
                auto& route = routes[index];
                const auto generation = route.activeGeneration.load (
                    std::memory_order_acquire);
                if (generation == 0
                     || route.sourceHash.load (std::memory_order_relaxed)
                            != wantedHash
                     || route.sourceLength.load (std::memory_order_relaxed)
                            != sourceLength
                     || route.channel.load (std::memory_order_relaxed)
                            != channel
                     || route.cc.load (std::memory_order_relaxed) != cc
                     || route.isNote.load (std::memory_order_relaxed)
                            != (isNote ? 1u : 0u)
                     || ! sourceExactlyMatches (
                            route, sourceBytes, sourceLength)
                     || route.activeGeneration.load (
                            std::memory_order_acquire) != generation)
                    continue;

                // Linearize every matching arrival before contending for the
                // latest-wins cell. If this newer arrival must fail open, the
                // consumer can still prove that an older pending value became
                // obsolete and must not be applied after the raw newer event.
                const auto attemptSequence =
                    route.latestAttemptSequence.fetch_add (
                        1, std::memory_order_seq_cst) + 1;

                // The flag holder (consumer scan or another producer) owns it
                // for a handful of word loads; a bounded spin keeps the NEWEST
                // value (latest-wins) instead of dropping it at the first
                // collision, and 128 iterations stays far below audio budget.
                bool ownsEventCell = false;
                for (int spin = 0; spin < 128; ++spin)
                {
                    if (! route.eventWriter.test_and_set (
                            std::memory_order_acquire))
                    {
                        ownsEventCell = true;
                        break;
                    }
                }
                if (! ownsEventCell)
                {
                    droppedForContention.fetch_add (
                        1, std::memory_order_relaxed);
                    return ControllerRoutePublishResult::contention;
                }

                if (route.activeGeneration.load (
                        std::memory_order_acquire) != generation
                     || tableMappingGeneration.load (
                            std::memory_order_acquire)
                            != currentMappingGeneration)
                {
                    route.eventWriter.clear (std::memory_order_release);
                    return ControllerRoutePublishResult::staleTable;
                }

                // A newer arrival may have linearized before this producer
                // acquired the per-route cell. Never let this delayed older
                // attempt overwrite that newer mapped/raw value.
                if (route.latestAttemptSequence.load (
                        std::memory_order_seq_cst) > attemptSequence)
                {
                    route.eventWriter.clear (std::memory_order_release);
                    droppedForContention.fetch_add (
                        1, std::memory_order_relaxed);
                    return ControllerRoutePublishResult::contention;
                }

                // Accepted-order is assigned only while the route cell is
                // owned. If this producer is preempted afterwards, the
                // consumer observes eventWriter and cannot emit a later route
                // first. attemptSequence remains separate because failed
                // newer arrivals must still invalidate an older pending value.
                const auto publicationSequence =
                    nextPublicationSequence.fetch_add (
                        1, std::memory_order_relaxed) + 1;
                if (route.pending)
                    coalesced.fetch_add (1, std::memory_order_relaxed);
                route.event = {
                    static_cast<juce::uint16> (index),
                    static_cast<juce::uint8> (
                        juce::jlimit (0, 127, value)),
                    generation,
                    currentMappingGeneration,
                    publicationSequence,
                    attemptSequence
                };
                route.pending = true;
                route.eventWriter.clear (std::memory_order_release);
                return ControllerRoutePublishResult::published;
            }

            return ControllerRoutePublishResult::notMapped;
        }

        bool hasRoute (const char* sourceBytes, size_t sourceLength,
                       int channel, int cc, bool isNote,
                       juce::uint64 currentMappingGeneration) const noexcept
        {
            if (tableMappingGeneration.load (std::memory_order_acquire)
                    != currentMappingGeneration)
                return false;

            return matchesActiveRoute (
                sourceBytes, sourceLength, channel, cc, isNote);
        }

        // Membership check that deliberately ignores tableMappingGeneration:
        // answers "was this control part of the last published table (or the
        // one being republished right now)". Suppression decisions use this so
        // a mapped fader/note keeps yielding to the user map during the
        // republication window, even when the value publication itself was
        // refused as stale — the dropped value self-heals on the next event.
        bool matchesActiveRoute (const char* sourceBytes, size_t sourceLength,
                                 int channel, int cc,
                                 bool isNote) const noexcept
        {
            if (sourceBytes == nullptr || sourceLength == 0
                 || sourceLength > MaxSourceBytes)
                return false;

            const auto wantedHash = hashSource (sourceBytes, sourceLength);
            for (const auto& route : routes)
            {
                const auto generation = route.activeGeneration.load (
                    std::memory_order_acquire);
                if (generation != 0
                     && route.sourceHash.load (std::memory_order_relaxed)
                            == wantedHash
                     && route.sourceLength.load (std::memory_order_relaxed)
                            == sourceLength
                     && route.channel.load (std::memory_order_relaxed)
                            == channel
                     && route.cc.load (std::memory_order_relaxed) == cc
                     && route.isNote.load (std::memory_order_relaxed)
                            == (isNote ? 1u : 0u)
                     && sourceExactlyMatches (
                            route, sourceBytes, sourceLength)
                     && route.activeGeneration.load (
                            std::memory_order_acquire) == generation)
                    return true;
            }
            return false;
        }

        // Message-thread-only. A contended producer may own an earlier event,
        // so contention returns false instead of consuming a later route. A
        // producer that updates the chosen route forces a bounded rescan.
        bool tryConsumeNext (ControllerRouteEvent& result) noexcept
        {
            for (size_t attempt = 0; attempt < 4; ++attempt)
            {
                size_t selected = Capacity;
                juce::uint64 selectedSequence =
                    std::numeric_limits<juce::uint64>::max();

                for (size_t index = 0; index < Capacity; ++index)
                {
                    auto& route = routes[index];
                    if (route.eventWriter.test_and_set (
                            std::memory_order_acquire))
                        return false;

                    if (route.pending
                         && route.event.publicationSequence < selectedSequence)
                    {
                        selected = index;
                        selectedSequence = route.event.publicationSequence;
                    }
                    route.eventWriter.clear (std::memory_order_release);
                }

                if (selected == Capacity)
                    return false;

                auto& route = routes[selected];
                if (route.eventWriter.test_and_set (
                        std::memory_order_acquire))
                    return false;

                if (! route.pending
                     || route.event.publicationSequence != selectedSequence)
                {
                    route.eventWriter.clear (std::memory_order_release);
                    continue;
                }

                result = route.event;
                route.pending = false;
                route.eventWriter.clear (std::memory_order_release);

                // A producer records its sequence before testing eventWriter.
                // Thus every producer that observed this consumer's lock is
                // visible here before this older value can escape.
                if (route.latestAttemptSequence.load (
                        std::memory_order_seq_cst)
                      > result.attemptSequence)
                {
                    invalidatedByNewerAttempt.fetch_add (
                        1, std::memory_order_relaxed);
                    continue;
                }
                return true;
            }

            return false;
        }

        bool hasPending() noexcept
        {
            for (auto& route : routes)
            {
                if (route.eventWriter.test_and_set (
                        std::memory_order_acquire))
                    return true;
                const bool pending = route.pending;
                route.eventWriter.clear (std::memory_order_release);
                if (pending)
                    return true;
            }
            return false;
        }

        juce::uint64 routeGeneration (size_t routeSlot) const noexcept
        {
            return routeSlot < Capacity
                ? routes[routeSlot].activeGeneration.load (
                    std::memory_order_acquire)
                : 0;
        }

        bool overflowed() const noexcept
        {
            return routeOverflow.load (std::memory_order_acquire);
        }

        juce::uint64 overflowEpisodeCount() const noexcept
        {
            return overflowEpisodes.load (std::memory_order_relaxed);
        }

        juce::uint64 coalescedCount() const noexcept
        {
            return coalesced.load (std::memory_order_relaxed);
        }

        juce::uint64 contentionDropCount() const noexcept
        {
            return droppedForContention.load (std::memory_order_relaxed);
        }

        juce::uint64 invalidatedPendingCount() const noexcept
        {
            return invalidatedByNewerAttempt.load (
                std::memory_order_relaxed);
        }

    private:
        static constexpr size_t sourceChunkCount =
            (MaxSourceBytes + sizeof (juce::uint64) - 1)
                / sizeof (juce::uint64);

        struct Route
        {
            std::atomic<juce::uint64> generationCounter { 0 };
            std::atomic<juce::uint64> activeGeneration { 0 };
            std::atomic<juce::uint64> sourceHash { 0 };
            std::atomic<juce::uint64> sourceLength { 0 };
            std::array<std::atomic<juce::uint64>, sourceChunkCount>
                sourceChunks {};
            std::atomic<juce::uint64> channel { 0 };
            std::atomic<juce::uint64> cc { 0 };
            std::atomic<juce::uint64> isNote { 0 };
            std::atomic<juce::uint64> latestAttemptSequence { 0 };
            std::atomic_flag eventWriter = ATOMIC_FLAG_INIT;
            bool pending = false; // protected by eventWriter
            ControllerRouteEvent event; // protected by eventWriter
        };

        static juce::uint64 hashSource (const char* bytes,
                                        size_t length) noexcept
        {
            juce::uint64 hash = 1469598103934665603ull;
            for (size_t i = 0; i < length; ++i)
            {
                hash ^= static_cast<juce::uint8> (bytes[i]);
                hash *= 1099511628211ull;
            }
            return hash;
        }

        static juce::uint64 packSourceChunk (const char* bytes,
                                             size_t length,
                                             size_t chunk) noexcept
        {
            juce::uint64 packed = 0;
            const size_t first = chunk * sizeof (juce::uint64);
            for (size_t byte = 0; byte < sizeof (juce::uint64); ++byte)
            {
                const size_t index = first + byte;
                if (index >= length)
                    break;
                packed |= static_cast<juce::uint64> (
                              static_cast<juce::uint8> (bytes[index]))
                          << (byte * 8);
            }
            return packed;
        }

        static bool sourceExactlyMatches (const Route& route,
                                          const char* bytes,
                                          size_t length) noexcept
        {
            const size_t chunks =
                (length + sizeof (juce::uint64) - 1)
                    / sizeof (juce::uint64);
            for (size_t chunk = 0; chunk < chunks; ++chunk)
                if (route.sourceChunks[chunk].load (
                        std::memory_order_relaxed)
                      != packSourceChunk (bytes, length, chunk))
                    return false;
            return true;
        }

        static_assert (std::atomic<juce::uint64>::is_always_lock_free,
                       "MIDI route publication requires lock-free words");
        std::array<Route, Capacity> routes {};
        std::atomic<juce::uint64> tableMappingGeneration { 0 };
        std::atomic<juce::uint64> nextPublicationSequence { 0 };
        std::atomic<juce::uint64> coalesced { 0 };
        std::atomic<juce::uint64> droppedForContention { 0 };
        std::atomic<juce::uint64> invalidatedByNewerAttempt { 0 };
        std::atomic<juce::uint64> overflowEpisodes { 0 };
        std::atomic<bool> routeOverflow { false };
    };

    template <size_t Capacity, size_t MaxSourceIdBytes = 192,
              size_t MaxSourceNameBytes = 128>
    class FixedMidiLearnControlQueue
    {
    public:
        struct Event
        {
            std::array<char, MaxSourceIdBytes> sourceId {};
            std::array<char, MaxSourceNameBytes> sourceName {};
            juce::uint16 sourceIdLength = 0;
            juce::uint16 sourceNameLength = 0;
            juce::uint8 channel = 0;
            juce::uint8 number = 0;
            juce::uint8 value = 0;
            bool isNote = false;
            bool unmapOnly = false;
            juce::uint64 targetGeneration = 0;
            juce::uint64 publicationSequence = 0;
        };

        static_assert (Capacity > 0, "Learn FIFO needs storage");
        static_assert (MaxSourceIdBytes > 0 && MaxSourceNameBytes > 0,
                       "Learn source fields need physical storage");
        static_assert (std::is_trivially_copyable<Event>::value,
                       "MIDI callback Learn publication must remain POD");

        FixedMidiLearnControlQueue() noexcept
        {
            for (size_t index = 0; index < Capacity; ++index)
                slots[index].sequence.store (
                    static_cast<juce::uint64> (index),
                    std::memory_order_relaxed);
        }

        bool tryPublish (const char* sourceId, size_t sourceIdLength,
                         const char* sourceName, size_t sourceNameLength,
                         int channel, int number, int value, bool isNote,
                         bool unmapOnly,
                         juce::uint64 targetGeneration) noexcept
        {
            if (sourceId == nullptr || sourceIdLength == 0
                 || sourceIdLength > MaxSourceIdBytes
                 || sourceNameLength > MaxSourceNameBytes
                 || (sourceNameLength > 0 && sourceName == nullptr)
                 || channel < 1 || channel > 16
                 || number < 0 || number > 127)
            {
                dropped.fetch_add (1, std::memory_order_relaxed);
                overflowLatch.store (true, std::memory_order_release);
                return false;
            }

            for (size_t attempt = 0; attempt < reservationAttemptLimit;
                 ++attempt)
            {
                auto position = enqueuePosition.load (
                    std::memory_order_relaxed);
                auto& slot = slots[static_cast<size_t> (
                    position % Capacity)];
                if (slot.sequence.load (std::memory_order_acquire)
                        != position)
                {
                    dropped.fetch_add (1, std::memory_order_relaxed);
                    overflowLatch.store (true, std::memory_order_release);
                    return false;
                }

                if (! enqueuePosition.compare_exchange_weak (
                        position, position + 1,
                        std::memory_order_acq_rel,
                        std::memory_order_relaxed))
                    continue;

                auto& event = slot.event;
                for (size_t i = 0; i < sourceIdLength; ++i)
                    event.sourceId[i] = sourceId[i];
                for (size_t i = sourceIdLength; i < MaxSourceIdBytes; ++i)
                    event.sourceId[i] = 0;
                for (size_t i = 0; i < sourceNameLength; ++i)
                    event.sourceName[i] = sourceName[i];
                for (size_t i = sourceNameLength; i < MaxSourceNameBytes; ++i)
                    event.sourceName[i] = 0;
                event.sourceIdLength = static_cast<juce::uint16> (
                    sourceIdLength);
                event.sourceNameLength = static_cast<juce::uint16> (
                    sourceNameLength);
                event.channel = static_cast<juce::uint8> (channel);
                event.number = static_cast<juce::uint8> (number);
                event.value = static_cast<juce::uint8> (
                    juce::jlimit (0, 127, value));
                event.isNote = isNote;
                event.unmapOnly = unmapOnly;
                event.targetGeneration = targetGeneration;
                event.publicationSequence = position + 1;
                slot.sequence.store (position + 1,
                                     std::memory_order_release);
                return true;
            }

            dropped.fetch_add (1, std::memory_order_relaxed);
            overflowLatch.store (true, std::memory_order_release);
            return false;
        }

        bool tryConsume (Event& result) noexcept
        {
            const auto position = dequeuePosition;
            auto& slot = slots[static_cast<size_t> (position % Capacity)];
            if (slot.sequence.load (std::memory_order_acquire)
                    != position + 1)
                return false;

            result = slot.event;
            slot.sequence.store (position + Capacity,
                                 std::memory_order_release);
            dequeuePosition = position + 1;
            return true;
        }

        // Contract: MIDI callback producers are stopped/drained.
        void clear() noexcept
        {
            Event ignored;
            while (tryConsume (ignored)) {}
            overflowLatch.store (false, std::memory_order_release);
        }

        bool overflowed() const noexcept
        {
            return overflowLatch.load (std::memory_order_acquire);
        }

        juce::uint64 droppedCount() const noexcept
        {
            return dropped.load (std::memory_order_relaxed);
        }

    private:
        static constexpr size_t reservationAttemptLimit =
            Capacity < 8 ? 8 : Capacity * 2;
        struct Slot
        {
            std::atomic<juce::uint64> sequence { 0 };
            Event event;
        };

        static_assert (std::atomic<juce::uint64>::is_always_lock_free,
                       "Learn FIFO positions must be lock-free");
        std::array<Slot, Capacity> slots {};
        std::atomic<juce::uint64> enqueuePosition { 0 };
        std::atomic<juce::uint64> dropped { 0 };
        std::atomic<bool> overflowLatch { false };
        juce::uint64 dequeuePosition = 0; // message thread only
    };

    // Bounded linearizable MPSC ring for AudioProcessorListener callbacks.
    // The enqueue-position CAS is the global acceptance order. A producer
    // writes only its reserved POD cell and publishes it with release
    // semantics; the single consumer never skips a reserved/unfinished head.
    // Reservation retries are bounded, and full/contention drops only Learn.
    template <size_t Capacity>
    class ParameterTouchMailbox
    {
    public:
        static_assert (Capacity > 0, "ParameterTouchMailbox needs storage");

        ParameterTouchMailbox() noexcept
        {
            for (size_t index = 0; index < Capacity; ++index)
                slots[index].sequence.store (
                    static_cast<juce::uint64> (index),
                    std::memory_order_relaxed);
        }

        ParameterTouchMailbox (const ParameterTouchMailbox&) = delete;
        ParameterTouchMailbox& operator= (
            const ParameterTouchMailbox&) = delete;

        bool tryPublish (const void* processor, int parameterIndex,
                         juce::uint64 learnGeneration,
                         juce::uint64 topologyGeneration,
                         juce::uint64* acceptedSequence = nullptr) noexcept
        {
            if (processor == nullptr || parameterIndex < 0)
                return false;

            for (size_t attempt = 0; attempt < reservationAttemptLimit;
                 ++attempt)
            {
                auto position = enqueuePosition.load (
                    std::memory_order_relaxed);
                auto& slot = slots[static_cast<size_t> (
                    position % Capacity)];
                if (slot.sequence.load (std::memory_order_acquire)
                        != position)
                {
                    dropped.fetch_add (1, std::memory_order_relaxed);
                    return false;
                }

                if (! enqueuePosition.compare_exchange_weak (
                        position, position + 1,
                        std::memory_order_acq_rel,
                        std::memory_order_relaxed))
                    continue;

                slot.event = {
                    reinterpret_cast<std::uintptr_t> (processor),
                    parameterIndex,
                    learnGeneration,
                    topologyGeneration,
                    position + 1
                };
                slot.sequence.store (position + 1,
                                     std::memory_order_release);
                if (acceptedSequence != nullptr)
                    *acceptedSequence = position + 1;
                return true;
            }

            dropped.fetch_add (1, std::memory_order_relaxed);
            return false;
        }

        bool tryConsume (ParameterTouchEvent& result) noexcept
        {
            const auto position = dequeuePosition;
            auto& slot = slots[static_cast<size_t> (position % Capacity)];
            if (slot.sequence.load (std::memory_order_acquire)
                    != position + 1)
                return false;

            result = slot.event;
            slot.sequence.store (position + Capacity,
                                 std::memory_order_release);
            dequeuePosition = position + 1;
            return true;
        }

        // Only call after callback admission has closed and all producers have
        // drained. It intentionally consumes in FIFO order rather than
        // resetting producer positions underneath an in-flight reservation.
        void clear() noexcept
        {
            ParameterTouchEvent ignored;
            while (tryConsume (ignored)) {}
        }

        juce::uint64 droppedCount() const noexcept
        {
            return dropped.load (std::memory_order_relaxed);
        }

    private:
        static constexpr size_t reservationAttemptLimit =
            Capacity < 8 ? 8 : Capacity * 2;

        struct Slot
        {
            std::atomic<juce::uint64> sequence { 0 };
            ParameterTouchEvent event;
        };

        static_assert (std::is_trivially_copyable<ParameterTouchEvent>::value,
                       "parameter callbacks may publish POD only");
        static_assert (std::atomic<juce::uint64>::is_always_lock_free,
                       "parameter callback ring positions must be lock-free");
        std::array<Slot, Capacity> slots {};
        std::atomic<juce::uint64> enqueuePosition { 0 };
        std::atomic<juce::uint64> dropped { 0 };
        juce::uint64 dequeuePosition = 0; // message thread only
    };

    // Fixed storage for source-aware callback state. ControllerKey copies only
    // retain the JUCE String storage prepared by the caller; unlike std::set,
    // inserting a new source/channel never allocates a tree node in the MIDI
    // callback. Capacity exhaustion is explicit so each owner can fail open.
    template <size_t Capacity>
    class FixedControllerKeySet
    {
    public:
        struct Entry
        {
            ControllerKey key;
            bool occupied = false;
        };

        bool insert (const ControllerKey& key)
        {
            if (! key.isValid())
                return false;

            for (const auto& entry : entries)
                if (entry.occupied && keysEqual (entry.key, key))
                    return true;

            for (auto& entry : entries)
                if (! entry.occupied)
                {
                    entry.key = key;
                    entry.occupied = true;
                    ++occupiedCount;
                    return true;
                }

            return false;
        }

        bool contains (const ControllerKey& key) const noexcept
        {
            for (const auto& entry : entries)
                if (entry.occupied && keysEqual (entry.key, key))
                    return true;
            return false;
        }

        bool erase (const ControllerKey& key)
        {
            for (auto& entry : entries)
                if (entry.occupied && keysEqual (entry.key, key))
                {
                    release (entry);
                    return true;
                }
            return false;
        }

        template <typename Predicate>
        void eraseIf (Predicate&& predicate)
        {
            for (auto& entry : entries)
                if (entry.occupied && predicate (entry.key))
                    release (entry);
        }

        template <typename Visitor>
        void forEach (Visitor&& visitor) const
        {
            for (const auto& entry : entries)
                if (entry.occupied)
                    visitor (entry.key);
        }

        void clear() noexcept
        {
            for (auto& entry : entries)
            {
                entry.key = {};
                entry.occupied = false;
            }
            occupiedCount = 0;
        }

        bool empty() const noexcept { return occupiedCount == 0; }
        size_t size() const noexcept { return occupiedCount; }
        static constexpr size_t capacity() noexcept { return Capacity; }

    private:
        static bool keysEqual (const ControllerKey& first,
                               const ControllerKey& second) noexcept
        {
            return first.sourceId == second.sourceId
                && first.channel == second.channel
                && first.cc == second.cc
                && first.isNote == second.isNote;
        }

        void release (Entry& entry)
        {
            entry.key = {};
            entry.occupied = false;
            --occupiedCount;
        }

        std::array<Entry, Capacity> entries {};
        size_t occupiedCount = 0;
    };

    template <size_t Capacity = 256, std::uint16_t MaxDepth = 255>
    class SuppressedMidiNoteLedger
    {
    public:
        struct Entry
        {
            ControllerKey key;
            std::uint16_t depth = 0;
            bool failOpen = false;
            bool occupied = false;
        };

        bool tryAcquire (const ControllerKey& key)
        {
            if (! key.isValid() || capacityFailOpen)
                return false;

            for (auto& entry : entries)
                if (entry.occupied && keysEqual (entry.key, key))
                {
                    // MIDI does not identify which OFF belongs to which one of
                    // several repeated same-key ONs. Once the bounded debt is
                    // exhausted, discard those command-only debts and latch
                    // this key fail-open until an explicit transport reset;
                    // otherwise an old debt could swallow the OFF of the ON
                    // that is now being forwarded to the instrument.
                    if (entry.failOpen)
                        return false;

                    if (entry.depth >= MaxDepth)
                    {
                        entry.depth = 0;
                        entry.failOpen = true;
                        return false;
                    }

                    ++entry.depth;
                    return true;
                }

            for (auto& entry : entries)
                if (! entry.occupied)
                {
                    entry.key = key;
                    entry.depth = 1;
                    entry.failOpen = false;
                    entry.occupied = true;
                    ++occupiedCount;
                    return true;
                }

            // There is no slot in which to remember the key whose ON now has
            // to reach the instrument. Abandon every logical command debt and
            // remain globally fail-open until transport reset; otherwise a
            // later free slot could suppress this raw ON's eventual OFF.
            capacityFailOpen = true;
            return false;
        }

        SuppressedNoteRelease consumeRelease (const ControllerKey& key)
        {
            if (capacityFailOpen)
                return SuppressedNoteRelease::notSuppressed;

            for (auto& entry : entries)
                if (entry.occupied && keysEqual (entry.key, key))
                {
                    if (entry.failOpen)
                        return SuppressedNoteRelease::notSuppressed;

                    if (entry.depth > 1)
                    {
                        --entry.depth;
                        return SuppressedNoteRelease::stillSuppressed;
                    }
                    entry.depth = 0;
                    entry.failOpen = false;
                    entry.occupied = false;
                    --occupiedCount;
                    return SuppressedNoteRelease::releasedLast;
                }
            return SuppressedNoteRelease::notSuppressed;
        }

        // Undo a reservation when the inspected ON was not ultimately mapped.
        SuppressedNoteRelease cancelAcquire (const ControllerKey& key)
        {
            return consumeRelease (key);
        }

        bool contains (const ControllerKey& key) const noexcept
        {
            if (capacityFailOpen)
                return false;

            for (const auto& entry : entries)
                if (entry.occupied && ! entry.failOpen
                     && keysEqual (entry.key, key))
                    return true;
            return false;
        }

        bool shouldSuppressCurrentGesture (
            const ControllerKey& key) const noexcept
        {
            if (capacityFailOpen)
                return false;

            for (const auto& entry : entries)
                if (entry.occupied && keysEqual (entry.key, key))
                    return ! entry.failOpen && entry.depth > 0;
            return false;
        }

        void clearSourceChannel (const juce::String& sourceId, int channel)
        {
            for (auto& entry : entries)
                if (entry.occupied
                     && entry.key.sourceId == sourceId
                     && entry.key.channel == channel)
                {
                    entry.depth = 0;
                    entry.failOpen = false;
                    entry.occupied = false;
                    --occupiedCount;
                }
        }

        void clear() noexcept
        {
            for (auto& entry : entries)
            {
                entry.depth = 0;
                entry.failOpen = false;
                entry.occupied = false;
            }
            occupiedCount = 0;
            capacityFailOpen = false;
        }

        template <typename Visitor>
        void forEach (Visitor&& visitor) const
        {
            if (capacityFailOpen)
                return;

            for (const auto& entry : entries)
                if (entry.occupied)
                    visitor (entry.key, entry.depth);
        }

        size_t size() const noexcept { return occupiedCount; }
        bool failedOpenForCapacity() const noexcept
        {
            return capacityFailOpen;
        }
        static constexpr size_t capacity() noexcept { return Capacity; }
        static constexpr std::uint16_t maxDepth() noexcept { return MaxDepth; }

    private:
        static bool keysEqual (const ControllerKey& first,
                               const ControllerKey& second) noexcept
        {
            return first.sourceId == second.sourceId
                && first.channel == second.channel
                && first.cc == second.cc
                && first.isNote == second.isNote;
        }

        std::array<Entry, Capacity> entries {};
        size_t occupiedCount = 0;
        bool capacityFailOpen = false;
    };

    inline bool samePhysicalMidiControl (const ControllerKey& first,
                                         const ControllerKey& second,
                                         bool compareKind = true)
    {
        if (first.channel != second.channel
             || first.cc != second.cc
             || (compareKind && first.isNote != second.isNote))
            return false;

        if (normalizeMidiSourceId (first.sourceId)
                == normalizeMidiSourceId (second.sourceId))
            return true;

        return midiSourceNamesEqual (first.sourceName, second.sourceName);
    }

    inline bool samePhysicalMidiControl (
        const ControllerKey& first,
        const ControllerKey& second,
        const std::vector<MidiSourceDescriptor>& available,
        bool compareKind = true)
    {
        if (first.channel != second.channel
             || first.cc != second.cc
             || (compareKind && first.isNote != second.isNote))
            return false;
        if (normalizeMidiSourceId (first.sourceId)
                == normalizeMidiSourceId (second.sourceId))
            return true;
        if (! midiSourceNamesEqual (first.sourceName, second.sourceName))
            return false;

        int matches = 0;
        for (const auto& source : available)
            if (midiSourceNamesEqual (source.name, first.sourceName))
                ++matches;
        return matches == 1;
    }

    template <typename MappingMap, typename Predicate>
    auto findPhysicalMidiControlIf (
        MappingMap& mappings,
        const ControllerKey& key,
        const std::vector<MidiSourceDescriptor>& available,
        const Predicate& predicate)
    {
        return std::find_if (
            mappings.begin(), mappings.end(),
            [&] (const auto& mapping)
            {
                return samePhysicalMidiControl (
                           mapping.first, key, available)
                    && predicate (mapping);
            });
    }

    // A map transaction can begin between the high and low halves of a CC
    // gesture. If the high half went to the instrument as raw MIDI and an
    // imported action consumes the low half, sustain (CC64) can remain stuck.
    // This source-aware latch makes the whole gesture keep the routing decision
    // made while the transaction was active.
    class RawCCTransactionGestureLatch
    {
    public:
        static constexpr size_t maxTrackedGestures = 512;

        static bool requiresPairedRawRelease (int cc) noexcept
        {
            // MIDI switch controllers whose ON has lasting meaning inside an
            // instrument. Ordinary pads/knobs are stateless messages; latching
            // a 127-only pad forever would make an imported action unreachable.
            return cc >= 64 && cc <= 69;
        }

        bool observeRawDuringTransaction (const ControllerKey& control,
                                          int value)
        {
            if (! requiresPairedRawRelease (control.cc))
                return true;

            if (value >= 64)
            {
                if (! rawGestures.insert (control))
                    capacityExceeded = true;
                return ! capacityExceeded;
            }

            (void) rawGestures.erase (control);
            return ! capacityExceeded;
        }

        bool shouldContinueRawGesture (const ControllerKey& control,
                                       int value)
        {
            if (! rawGestures.contains (control))
                return capacityExceeded
                    && requiresPairedRawRelease (control.cc);

            if (value < 64)
                (void) rawGestures.erase (control);
            return true;
        }

        bool hasPendingGesture() const noexcept
        {
            return capacityExceeded || ! rawGestures.empty();
        }

        bool overflowed() const noexcept { return capacityExceeded; }
        void markOverflowFailOpen() noexcept { capacityExceeded = true; }

        void clearSourceChannel (const juce::String& sourceId,
                                 int channel)
        {
            const auto normalised = normalizeMidiSourceId (sourceId);
            rawGestures.eraseIf (
                [&] (const ControllerKey& key)
                {
                    return normalizeMidiSourceId (key.sourceId) == normalised
                        && key.channel == channel;
                });
        }

        void clear() noexcept
        {
            rawGestures.clear();
            capacityExceeded = false;
        }

    private:
        FixedControllerKeySet<maxTrackedGestures> rawGestures;
        bool capacityExceeded = false;
    };

    // Sustain do ChurchDAW e global, mas os callbacks chegam por porta MIDI.
    // Sem conservar a origem, um CC64=0 da porta B derruba o pedal que ainda
    // esta em 127 na porta A (o classico "last OFF wins"). Este ledger guarda
    // somente as origens/canais atualmente abaixados e emite transicao apenas
    // quando o OR agregado realmente muda.
    class SourceAwareSustainState
    {
    public:
        static constexpr size_t maxTrackedSources = 256;

        struct Decision
        {
            bool forwardTransition = false;
            bool aggregateDown = false;
        };

        Decision update (const juce::String& sourceId,
                         int channel,
                         bool down)
        {
            const bool before = isDown();

            ControllerKey key {
                sourceId,
                juce::jlimit (1, 16, channel),
                64,
                false
            };

            if (key.isValid())
            {
                if (down)
                {
                    if (! held_.insert (key))
                        capacityExceeded = true;
                }
                else
                    (void) held_.erase (key);
            }

            const bool after = isDown();
            return { before != after, after };
        }

        bool isDown() const noexcept
        {
            return capacityExceeded || ! held_.empty();
        }

        bool overflowed() const noexcept { return capacityExceeded; }

        void clear() noexcept
        {
            held_.clear();
            capacityExceeded = false;
        }

        // sourceIds usa exatamente o mesmo namespace armazenado no callback
        // (por exemplo "midi:{identifier}"). Retorna true se o agregado mudou.
        bool retainOnlySources (const juce::StringArray& sourceIds)
        {
            const bool before = isDown();

            held_.eraseIf (
                [&] (const ControllerKey& key)
                {
                    return ! sourceIds.contains (key.sourceId);
                });

            // An empty physical-source snapshot is an explicit transport
            // boundary and proves that even an untracked overflow entry is no
            // longer held. A non-empty list cannot prove which source it was.
            if (sourceIds.isEmpty())
                capacityExceeded = false;

            return before != isDown();
        }

        void seedRawTransactionGestures (
            RawCCTransactionGestureLatch& destination) const
        {
            held_.forEach (
                [&destination] (const ControllerKey& held)
                {
                    (void) destination.observeRawDuringTransaction (
                        held, 127);
                });
            if (capacityExceeded)
                destination.markOverflowFailOpen();
        }

    private:
        FixedControllerKeySet<maxTrackedSources> held_;
        bool capacityExceeded = false;
    };

    enum class LaunchkeyEncoderTarget
    {
        none,
        reverbReturn,
        padVolume,
        channelComp
    };

    struct LaunchkeyEncoderRoute
    {
        LaunchkeyEncoderTarget target = LaunchkeyEncoderTarget::none;
        int layer = -1;
    };

    inline LaunchkeyEncoderRoute launchkeyEncoderRoute (int index) noexcept
    {
        if (index == 0)
            return { LaunchkeyEncoderTarget::reverbReturn, -1 };

        if (index == 1)
            return { LaunchkeyEncoderTarget::padVolume, -1 };

        if (index >= 2 && index <= 6)
            return { LaunchkeyEncoderTarget::channelComp, index - 2 };

        // Knob 8 fica deliberadamente sem alvo fixo. Em particular, ele nunca
        // volta a ser Transpose global por uma refatoracao acidental.
        return {};
    }

    // Politica pura da porta DAW do Launchkey. Quando o mapeamento automatico
    // esta desligado, TODAS as portas pertencem ao MIDI comum. Quando esta
    // ligado, a porta DAW so pode ser reservada se ainda restar ao menos uma
    // entrada musical; o caso "apenas DAW" falha aberto para nunca calar o
    // teclado por causa de uma heuristica de nome/plataforma.
    struct LaunchkeyCommonInputPolicy
    {
        bool reserveDawPort = false;
        bool failSafeKeepingDawPort = false;

        bool shouldEnableAsCommonInput (bool isDawPort) const noexcept
        {
            return ! isDawPort || ! reserveDawPort;
        }
    };

    inline LaunchkeyCommonInputPolicy launchkeyCommonInputPolicy (
        bool automaticMappingEnabled,
        int totalInputCount,
        int dawPortCount) noexcept
    {
        totalInputCount = juce::jmax (0, totalInputCount);
        dawPortCount = juce::jlimit (0, totalInputCount, dawPortCount);
        const int musicalPortCount = totalInputCount - dawPortCount;

        return {
            automaticMappingEnabled
                && dawPortCount > 0
                && musicalPortCount > 0,
            automaticMappingEnabled
                && dawPortCount > 0
                && musicalPortCount == 0
        };
    }

    inline juce::String stableParameterId (const juce::AudioProcessorParameter* parameter)
    {
        if (const auto* hosted =
                dynamic_cast<const juce::HostedAudioProcessorParameter*> (parameter))
            return hosted->getParameterID();

        if (const auto* withId =
                dynamic_cast<const juce::AudioProcessorParameterWithID*> (parameter))
            return withId->getParameterID();

        return {};
    }

    inline bool isPitchTarget (const juce::String& stableId,
                               const juce::String& legacyName) noexcept
    {
        return stableId.equalsIgnoreCase ("gtransp")
            || stableId.startsWithIgnoreCase ("transp")
            || legacyName.containsIgnoreCase ("transpose")
            || legacyName.containsIgnoreCase ("transpor")
            || legacyName.containsIgnoreCase (
                juce::String::fromUTF8 ("transposi\xc3\xa7"));
    }

    // MAPEAR O TRANSPOSE FOI LIBERADO EM 05/08/2026, A PEDIDO DO CLIENTE.
    // Leia isto antes de mexer — a trava anterior existia por um motivo real.
    //
    // Historico: o transpose era destino PROIBIDO de MIDI Learn porque um CC
    // espurio (muitos controladores varrem CCs no boot) escrevia direto no tom,
    // o valor ficava persistido, e o culto inteiro saia transposto sem ninguem
    // entender a causa. A trava resolvia isso, mas junto tornava impossivel o
    // uso legitimo: o musico que QUER um knob de transpose nao tinha caminho.
    //
    // O que mudou: a trava agora distingue INTENCAO em vez de olhar so o nome
    // do parametro. Um mapa aprendido hoje sempre carrega o stableId ("gtransp")
    // porque o aprendizado grava o ID do parametro. Um mapa legado, criado antes
    // de o ID ser persistido, so tem o nome — e e exatamente essa classe de mapa
    // que causava a transposicao acidental, porque era resolvida por nome ou por
    // indice e podia cair em cima do tom sem o usuario ter escolhido nada.
    //
    // Logo: com ID -> deliberado -> permitido. Sem ID -> legado -> continua
    // bloqueado. Isso atende o pedido sem reabrir o incidente antigo, e o mapa
    // que o usuario criar sobrevive ao restart (o que uma trava no caminho de
    // restauracao teria quebrado).
    //
    // Se ainda assim aparecer salto de tom em palco, a proxima medida NAO e
    // restaurar o bloqueio geral: e exigir "pickup" no transpose (so assume o
    // valor depois que o knob cruza a posicao atual), matando o pulo de ate 12
    // semitons quando o controlador chega numa posicao qualquer.
    inline bool isProtectedPitchTarget (
        const juce::String& stableId,
        const juce::String& legacyName) noexcept
    {
        if (! isPitchTarget (stableId, legacyName))
            return false;

        return stableId.isEmpty();   // sem ID = mapa legado = segue bloqueado
    }

    // Nunca resolve um mapa persistido apenas pelo indice. A ordem dos
    // parametros muda quando o ChurchDAW ganha um controle novo; reutilizar o
    // indice antigo pode transformar, por exemplo, um fader de volume em
    // Transpose. ID e a ancora primaria; nome existe somente para migrar mapas
    // criados antes de o ID ser persistido.
    inline int resolveParameterIndex (
        const juce::Array<juce::AudioProcessorParameter*>& parameters,
        const juce::String& stableId,
        const juce::String& legacyName)
    {
        // Vale tambem para um mapa aprendido na sessao atual: ele pode ate
        // existir temporariamente no arquivo antigo, mas jamais sera aplicado.
        // Na proxima restauracao isSafeStoredTarget() o remove definitivamente.
        if (isProtectedPitchTarget (stableId, legacyName))
            return -1;

        if (stableId.isNotEmpty())
        {
            int uniqueMatch = -1;
            for (int i = 0; i < parameters.size(); ++i)
                if (stableParameterId (parameters[i]) == stableId)
                {
                    if (uniqueMatch >= 0)
                        return -1;
                    uniqueMatch = i;
                }

            if (uniqueMatch >= 0)
                return uniqueMatch;

            // ID presente, mas ausente no processador atual: o plugin/schema
            // mudou. Nao cair para um nome generico e arriscar outro alvo.
            return -1;
        }

        if (legacyName.isNotEmpty())
        {
            int uniqueMatch = -1;
            for (int i = 0; i < parameters.size(); ++i)
                if (parameters[i]->getName (128) == legacyName
                     || parameters[i]->getName (32) == legacyName)
                {
                    if (uniqueMatch >= 0)
                        return -1;
                    uniqueMatch = i;
                }

            return uniqueMatch;
        }

        return -1;
    }

    inline int resolveParameterIndexForTargetLocation (
        const juce::Array<juce::AudioProcessorParameter*>& parameters,
        const juce::String& stableId,
        const juce::String& legacyName,
        MidiTargetLocationKind location)
    {
        // slot -1 is the dedicated Master processor, not a mutable hosted
        // parameter list. Old builds persisted "Master" while the actual
        // parameter is named "Master (dB)"; the dedicated location is the
        // unambiguous migration anchor.
        if (location == MidiTargetLocationKind::master)
            return parameters.isEmpty() ? -1 : 0;

        return resolveParameterIndex (parameters, stableId, legacyName);
    }

    inline bool parameterIdentityIsUnique (
        const juce::Array<juce::AudioProcessorParameter*>& parameters,
        int parameterIndex)
    {
        if (! juce::isPositiveAndBelow (parameterIndex, parameters.size()))
            return false;

        auto* parameter = parameters[parameterIndex];
        const auto stableId = stableParameterId (parameter);
        const auto legacyName = stableId.isEmpty()
            ? parameter->getName (128) : juce::String();
        return resolveParameterIndex (parameters, stableId, legacyName)
            == parameterIndex;
    }

    inline bool isSafeStoredTarget (int slot,
                                    const juce::String& stableId,
                                    const juce::String& legacyName) noexcept
    {
        if (isProtectedPitchTarget (stableId, legacyName))
            return false;

        // Master nao aponta para a lista mutavel de parametros do instrumento.
        return slot == -1 || stableId.isNotEmpty() || legacyName.isNotEmpty();
    }

    inline bool isMappableController (int cc) noexcept
    {
        // Pedais, Bank Select, Data Entry e RPN/NRPN aparecem no boot e na
        // configuracao de pitch-bend de muitos controladores. Channel Mode
        // (120..127) tambem nunca pode controlar parametro salvo.
        if (cc < 0 || cc >= 120)
            return false;

        switch (cc)
        {
            case 0:
            // CC 6 (Data Entry MSB) SAIU da lista em 05/08/2026.
            //
            // Motivo: no Launchkey os faders sao CC 5..13 (LaunchkeyControl.h,
            // faderCC0 = 0x05), entao o CC 6 e o FADER 2 do aparelho do cliente.
            // Com ele bloqueado, esse fader era silenciosamente inmapeavel — o
            // usuario movia e nada acontecia, sem nenhuma mensagem, exatamente
            // a mesma queixa que houve com o knob de transpose.
            //
            // Risco assumido e por que e pequeno: CC 6 carrega o valor de um
            // RPN/NRPN aberto por CC 98..101. Isso ocorre no boot do
            // controlador e ao configurar pitch-bend range — momentos em que
            // ninguem esta com o MIDI Learn armado, porque o aprendizado exige
            // dois gestos deliberados (mover o parametro na tela, depois mover
            // o controle). CC 98..101 seguem bloqueados, entao o RPN em si
            // nunca vira mapa.
            //
            // Se algum dia um controlador aprender CC 6 sozinho, a correcao
            // NAO e rebloquear: e ignorar CC 6 enquanto houver um RPN/NRPN
            // aberto (rastrear 98..101 e fechar com CC 127/RPN null).
            case 32:
            case 38:
            case 64:
            case 66:
            case 67:
            case 96:
            case 97:
            case 98:
            case 99:
            case 100:
            case 101:
                return false;
            default:
                return true;
        }
    }

    // CC11 e o pedal de expressao padrao. Pianos, organs e VSTs costumam
    // interpreta-lo diretamente, entao deixa-lo atravessar sem configuracao
    // faz um pedal destinado ao ChurchDAW alterar o timbre/volume do piano por
    // fora da mesa. Ele continua mapeavel por isMappableController(): apenas o
    // byte cru fica reservado ao host e so age depois de um mapa explicito.
    inline bool shouldForwardRawControllerToInstruments (int cc) noexcept
    {
        return cc != 11;
    }

    //==========================================================================
    // BOTAO POR CC — o "pad de borracha" acionando o Pad Continuo.
    //
    // POR QUE ESTA LOGICA MORA AQUI, E NAO NO MainComponent.
    //
    // A tentativa anterior (Diagnostico-Ruido/toggle-cc-wip.patch) colocou ~1200
    // linhas de heuristica dentro do MainComponent.cpp, onde nao existe como
    // testar nada sem abrir a DAW e sem ter o aparelho na mao. Ninguem conseguiu
    // defender que aquilo funcionava e o patch foi revertido inteiro. Nao ha
    // hardware para testar: os pads sao dos CLIENTES. Entao a unica prova
    // possivel e um teste automatizado, e para haver teste a decisao precisa ser
    // uma funcao PURA — sem juce::Component, sem audio, sem relogio proprio.
    //
    // Esta funcao nao aloca, nao pega lock, nao faz I/O e nao constroi
    // juce::String. Pode ser chamada da thread de MIDI.
    //==========================================================================

    // Lista SEPARADA da isMappableController, e de proposito mais permissiva.
    //
    // isMappableController protege PARAMETRO continuo, e por isso bloqueia
    // 64/66/67 (sustain, sostenuto, soft): um pedal pisado com o Learn armado
    // sequestrava o knob selecionado. Mas e exatamente em CC 64 que sai de
    // fabrica boa parte dos footswitches baratos — com a lista antiga eles eram
    // silenciosamente inmapeaveis como BOTAO, que e o unico uso que faz sentido
    // para eles. Aqui so fica bloqueado o que e estrutural do protocolo e nunca
    // representa um gesto do musico.
    inline bool isMappableAction (int cc) noexcept
    {
        if (cc < 0 || cc >= 120)   // 120..127 = Channel Mode
            return false;

        switch (cc)
        {
            case 0:     // Bank Select MSB
            case 6:     // Data Entry MSB (valor de RPN/NRPN)
            case 32:    // Bank Select LSB
            case 38:    // Data Entry LSB
            case 96:
            case 97:
            case 98:
            case 99:
            case 100:
            case 101:   // RPN / NRPN
                return false;
            default:
                return true;
        }
    }

    enum class ModoDeBotaoCC
    {
        // PADRAO. Alterna no aperto e IGNORA o soltar.
        //
        // Cobre pad momentaneo (manda alto ao apertar e baixo ao soltar), pad de
        // disparo (so manda alto, nunca manda baixo) e pad sensivel a forca.
        // Num pad COM TRAVA ele tambem funciona, so que respondendo a cada dois
        // toques — falha macia, que o musico contorna sozinho apertando de novo.
        //
        // Este e o lado seguro do erro: como a mensagem de "soltar" nunca desliga
        // nada, e IMPOSSIVEL este modo produzir a falha fatal do palco, que e o
        // tom soar apenas enquanto o dedo/pe esta em cima.
        alternar = 0,

        // Escolhido a mao no menu do tom quando o pad TEM TRAVA (fica aceso
        // sozinho). Ai o proprio valor carrega o estado: alto liga, baixo desliga.
        seguirNivel = 1
    };

    enum class DecisaoDeBotaoCC
    {
        nada = 0,
        alternar = 1,   // quem chama decide olhando o estado REAL do Pad
        ligar = 2,
        desligar = 3
    };

    // Fixed-capacity ordered queue for the short interval in which a durable
    // map transaction blocks action publication. padKey actions share one
    // mutually-exclusive state, so composing A/B/A independently is invalid;
    // preserve raw order for toggles and one-shots. On overflow no existing
    // edge is overwritten and the owner must execute its fail-safe path.
    template <size_t Capacity>
    class DeferredMidiActionQueue
    {
    public:
        struct Output
        {
            int actionKind = 0;
            int argument = -1;
            DecisaoDeBotaoCC decision = DecisaoDeBotaoCC::nada;
            std::uint64_t sequence = 0;
        };

        void clear() noexcept
        {
            count = 0;
            sequence = 0;
            didOverflow = false;
        }

        bool push (int actionKind, int argument,
                   DecisaoDeBotaoCC decision) noexcept
        {
            if (count >= Capacity)
            {
                didOverflow = true;
                return false;
            }

            entries[count++] = {
                actionKind, argument, decision, ++sequence };
            return true;
        }

        size_t copyTo (std::array<Output, Capacity>& destination) const noexcept
        {
            for (size_t i = 0; i < count; ++i)
                destination[i] = entries[i];
            return count;
        }

        bool overflowed() const noexcept { return didOverflow; }
        size_t size() const noexcept { return count; }

    private:
        std::array<Output, Capacity> entries {};
        size_t count = 0;
        std::uint64_t sequence = 0;
        bool didOverflow = false;
    };

    // Intervalo minimo entre duas acoes do MESMO controle.
    //
    // ISTO NAO CLASSIFICA NADA e nao decide tipo de aparelho — e so um
    // anti-repique. Nenhum ser humano aperta o mesmo pad duas vezes em 60 ms;
    // um pad que reenvia pressao, sim. O pior que esta guarda pode fazer e
    // engolir uma acao (o musico aperta de novo); ela nunca inverte estado.
    inline constexpr unsigned int kMsMinimoEntreAcoesDeBotao = 60;
    inline constexpr unsigned int kMsJanelaSincronizacaoDeBoot = 2500;

    // Quantos valores DISTINTOS provam que aquilo e um knob/fader/pedal de
    // expressao, e nao um botao. Um botao emite 1 ou 2 valores. Um pad sensivel
    // a forca emite alguns a mais (uma velocity por batida), por isso o limite
    // nao pode ser 3 — desmapear por tres forcas diferentes seria pior que o
    // problema. Um knob varrido passa de 6 em fracao de segundo.
    inline constexpr int kValoresDistintosQueProvamKnob = 6;

    struct EstadoDeBotaoCC
    {
        // Alcance APRENDIDO do proprio controle. Nao ha constante 127 nem
        // constante 0 em lugar nenhum desta decisao: o M-VAVE Chocolate Plus em
        // toggle manda 1 para "ligado", e ha pads configurados com ON=64/OFF=63,
        // onde o zero simplesmente nunca chega.
        int menorValor = -1;
        int maiorValor = -1;

        int nivelAnterior = -1;        // -1 desconhecido | 0 baixo | 1 alto
        bool jaViuMensagem = false;
        bool jaAgiu = false;
        unsigned int msDaUltimaAcao = 0;
        bool sincronizacaoDeBootPendente = false;
        unsigned int msDaSincronizacaoDeBoot = 0;

        // Vigia de "cara de knob". Tamanho fixo: nada de alocar na thread de MIDI.
        int valoresDistintos[8] {};
        int numValoresDistintos = 0;

        bool pareceKnob() const noexcept
        {
            return numValoresDistintos >= kValoresDistintosQueProvamKnob;
        }

        // Volta ao estado "nunca vi este controle". A protecao contra dumps de
        // boot e armada separadamente; esquecer nao sacrifica o proximo gesto.
        void esquecer() noexcept { *this = EstadoDeBotaoCC{}; }

        void armarSincronizacaoDeBoot (unsigned int agoraMs) noexcept
        {
            esquecer();
            sincronizacaoDeBootPendente = true;
            msDaSincronizacaoDeBoot = agoraMs;
        }
    };

    namespace detail
    {
        inline void registrarValorDistinto (EstadoDeBotaoCC& e, int valor) noexcept
        {
            for (int i = 0; i < e.numValoresDistintos; ++i)
                if (e.valoresDistintos[i] == valor)
                    return;

            if (e.numValoresDistintos < (int) (sizeof (e.valoresDistintos)
                                                / sizeof (e.valoresDistintos[0])))
                e.valoresDistintos[e.numValoresDistintos++] = valor;
        }

        // 1 = apertado, 0 = solto.
        //
        // O LIMIAR E O VALOR DE REPOUSO APRENDIDO DO PROPRIO CONTROLE, e nao um
        // numero fixo. Tres aparelhos reais quebram qualquer constante:
        //
        //  - "valor >= 64 liga" deixa o M-VAVE Chocolate Plus MORTO: o toggle
        //    custom dele manda 1 para ligado. Sintoma no telefone: "o pad nao faz
        //    nada" — o pior de todos, porque nao ha o que observar.
        //  - "valor >= 64 liga" tambem mata o toque LEVE de um pad que escala o
        //    valor pela forca: vira "so funciona se eu bater forte".
        //  - "valor > 0 liga" quebra o pad configurado com ON=64 / OFF=63, onde
        //    o zero NUNCA CHEGA: os dois valores acionariam, aperto e soltar se
        //    cancelariam e o Pad nunca ligaria. Falha total e silenciosa.
        //
        // Aprender o repouso resolve os tres com uma regra so. E, de quebra, um
        // pad que manda fluxo de pressao num unico toque (5, 10, 31, 64, 111,
        // 100, 90...) fica TODO acima do repouso: e um aperto so, nao dez.
        inline int classificarNivel (const EstadoDeBotaoCC& e, int valor) noexcept
        {
            if (e.maiorValor <= e.menorValor)
                return valor > 0 ? 1 : 0;   // so um valor visto ate agora

            return valor > e.menorValor ? 1 : 0;
        }
    }

    inline DecisaoDeBotaoCC decidirBotaoCC (EstadoDeBotaoCC& e,
                                            ModoDeBotaoCC modo,
                                            int valor,
                                            unsigned int agoraMs) noexcept
    {
        valor = valor < 0 ? 0 : (valor > 127 ? 127 : valor);

        const bool primeira = ! e.jaViuMensagem;

        if (primeira)
        {
            e.menorValor = valor;
            e.maiorValor = valor;
        }
        else
        {
            if (valor < e.menorValor) e.menorValor = valor;
            if (valor > e.maiorValor) e.maiorValor = valor;
        }

        detail::registrarValorDistinto (e, valor);

        // Aparelho de DISPARO: so manda um valor e ele nao e zero (nunca manda o
        // "soltei"). Sem este ramo ele funcionaria uma vez so, porque nunca mais
        // haveria borda de subida para detectar.
        const bool aparelhoDeDisparo = (e.maiorValor == e.menorValor) && valor > 0;

        const int nivel = detail::classificarNivel (e, valor);
        const int anterior = e.nivelAnterior;   // -1 = ainda nao sabemos
        e.nivelAnterior = nivel;

        // Um mapa restaurado pode receber dump de estado durante a inicializacao.
        // Essa excecao e armada explicitamente e expira; nao transforma todo
        // primeiro toque do musico em uma mensagem perdida.
        if (primeira)
        {
            e.jaViuMensagem = true;

            // Um dump alto e um toque alto sao bytes identicos. Somente mapas
            // restaurados, dentro da curta janela de boot armada explicitamente,
            // silenciam a primeira mensagem; o primeiro gesto normal age agora.
            const bool dumpDeBoot = e.sincronizacaoDeBootPendente
                && (unsigned int) (agoraMs - e.msDaSincronizacaoDeBoot)
                       < kMsJanelaSincronizacaoDeBoot;
            e.sincronizacaoDeBootPendente = false;

            if (dumpDeBoot)
                return DecisaoDeBotaoCC::nada;

            // Baixo no modo alternar ainda e repouso/release. Em seguirNivel e
            // uma ordem legitima de desligar e passa para a decisao normal.
            if (modo == ModoDeBotaoCC::alternar && nivel == 0)
                return DecisaoDeBotaoCC::nada;
        }

        DecisaoDeBotaoCC decisao = DecisaoDeBotaoCC::nada;

        if (modo == ModoDeBotaoCC::alternar)
        {
            const bool toqueNovo = aparelhoDeDisparo || (nivel == 1 && anterior != 1);

            if (! toqueNovo)
                return DecisaoDeBotaoCC::nada;   // a DESCIDA nunca age. Nunca.

            decisao = DecisaoDeBotaoCC::alternar;
        }
        else
        {
            if (nivel == anterior)
                return DecisaoDeBotaoCC::nada;

            decisao = nivel == 1 ? DecisaoDeBotaoCC::ligar
                                 : DecisaoDeBotaoCC::desligar;
        }

        // Anti-repique. Subtracao em unsigned: continua correta quando o
        // contador de milissegundos do JUCE da a volta (a cada ~49 dias).
        if (e.jaAgiu && (unsigned int) (agoraMs - e.msDaUltimaAcao)
                          < kMsMinimoEntreAcoesDeBotao)
            return DecisaoDeBotaoCC::nada;

        e.jaAgiu = true;
        e.msDaUltimaAcao = agoraMs;

        return decisao;
    }

    // Qual tom o Pad passa a ter. -1 = calado.
    //
    // NAO EXISTE "ESTADO DO BOTAO" GUARDADO EM LUGAR NENHUM: o alternar e
    // resolvido a partir do tom que o Pad TEM AGORA. E o que faz o mapa de CC
    // acompanhar automaticamente uma cena, a tela ou uma tecla MIDI que troquem
    // o tom por fora — sem isso, o proximo toque no pad de borracha faria o
    // oposto do esperado. De quebra: com o Pad em Re, apertar o pad de "Sol"
    // LIGA em Sol em vez de desligar, e uma mensagem perdida no cabo nao inverte
    // o estado para o resto do culto.
    inline int tomDepoisDoBotao (int tomAtual, int tomDoBotao,
                                 DecisaoDeBotaoCC decisao) noexcept
    {
        switch (decisao)
        {
            case DecisaoDeBotaoCC::alternar:
                return tomAtual == tomDoBotao ? -1 : tomDoBotao;

            case DecisaoDeBotaoCC::ligar:
                return tomDoBotao;

            case DecisaoDeBotaoCC::desligar:
                return tomAtual == tomDoBotao ? -1 : tomAtual;

            case DecisaoDeBotaoCC::nada:
            default:
                return tomAtual;
        }
    }
}
