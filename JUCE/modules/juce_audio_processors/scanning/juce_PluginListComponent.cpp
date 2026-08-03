/*
  ==============================================================================

   This file is part of the JUCE framework.
   Copyright (c) Raw Material Software Limited

   JUCE is an open source framework subject to commercial or open source
   licensing.

   By downloading, installing, or using the JUCE framework, or combining the
   JUCE framework with any other source code, object code, content or any other
   copyrightable work, you agree to the terms of the JUCE End User Licence
   Agreement, and all incorporated terms including the JUCE Privacy Policy and
   the JUCE Website Terms of Service, as applicable, which will bind you. If you
   do not agree to the terms of these agreements, we will not license the JUCE
   framework to you, and you must discontinue the installation or download
   process and cease use of the JUCE framework.

   JUCE End User Licence Agreement: https://juce.com/legal/juce-9-licence/
   JUCE Privacy Policy: https://juce.com/juce-privacy-policy
   JUCE Website Terms of Service: https://juce.com/juce-website-terms-of-service/

   Or:

   You may also use this code under the terms of the AGPLv3:
   https://www.gnu.org/licenses/agpl-3.0.en.html

   THE JUCE FRAMEWORK IS PROVIDED "AS IS" WITHOUT ANY WARRANTY, AND ALL
   WARRANTIES, WHETHER EXPRESSED OR IMPLIED, INCLUDING WARRANTY OF
   MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE, ARE DISCLAIMED.

  ==============================================================================
*/

namespace juce
{

class PluginListComponent::TableModel  : public TableListBoxModel
{
public:
    TableModel (PluginListComponent& c, KnownPluginList& l)  : owner (c), list (l) {}

    int getNumRows() override
    {
        return list.getNumTypes() + list.getBlacklistedFiles().size();
    }

    void paintRowBackground (Graphics& g, int /*rowNumber*/, int /*width*/, int /*height*/, bool rowIsSelected) override
    {
        const auto defaultColour = owner.findColour (ListBox::backgroundColourId);
        const auto c = rowIsSelected ? defaultColour.interpolatedWith (owner.findColour (ListBox::textColourId), 0.5f)
                                     : defaultColour;

        g.fillAll (c);
    }

    enum
    {
        nameCol = 1,
        typeCol = 2,
        categoryCol = 3,
        manufacturerCol = 4,
        descCol = 5
    };

    void paintCell (Graphics& g, int row, int columnId, int width, int height, bool /*rowIsSelected*/) override
    {
        const auto types = list.getTypes();
        const auto blacklisted = list.getBlacklistedFiles();
        const int numTypes = types.size();
        String text;
        const bool isBlacklisted = row >= numTypes;

        if (isBlacklisted)
        {
            const int blacklistIndex = row - numTypes;

            if (! isPositiveAndBelow (blacklistIndex, blacklisted.size()))
                return;   // a lista mudou entre getNumRows() e este repaint

            if (columnId == nameCol)
                text = blacklisted[blacklistIndex];
            else if (columnId == descCol)
                text = TRANS ("Deactivated after failing to initialise correctly");
        }
        else
        {
            if (! isPositiveAndBelow (row, types.size()))
                return;   // snapshot defensivo durante a varredura em worker

            const auto& desc = types.getReference (row);

            switch (columnId)
            {
                case nameCol:         text = desc.name; break;
                case typeCol:         text = desc.pluginFormatName; break;
                case categoryCol:     text = desc.category.isNotEmpty() ? desc.category : "-"; break;
                case manufacturerCol: text = desc.manufacturerName; break;
                case descCol:         text = getPluginDescription (desc); break;

                default: jassertfalse; break;
            }
        }

        if (text.isNotEmpty())
        {
            const auto defaultTextColour = owner.findColour (ListBox::textColourId);
            g.setColour (isBlacklisted ? Colours::red
                                       : columnId == nameCol ? defaultTextColour
                                                             : defaultTextColour.interpolatedWith (Colours::transparentBlack, 0.3f));
            g.setFont (owner.withDefaultMetrics (FontOptions ((float) height * 0.7f, Font::bold)));
            g.drawFittedText (text, 4, 0, width - 6, height, Justification::centredLeft, 1, 0.9f);
        }
    }

    void cellClicked (int rowNumber, int columnId, const MouseEvent& e) override
    {
        TableListBoxModel::cellClicked (rowNumber, columnId, e);

        if (rowNumber >= 0 && rowNumber < getNumRows() && e.mods.isPopupMenu())
        {
            owner.createMenuForRow (rowNumber)
                 .showMenuAsync (PopupMenu::Options().withTargetComponent (e.originalComponent)
                                                     .withMousePosition()
                                                     .withDeletionCheck (owner));
        }
    }

    void deleteKeyPressed (int) override
    {
        owner.removeSelectedPlugins();
    }

    void sortOrderChanged (int newSortColumnId, bool isForwards) override
    {
        switch (newSortColumnId)
        {
            case nameCol:         list.sort (KnownPluginList::sortAlphabetically, isForwards); break;
            case typeCol:         list.sort (KnownPluginList::sortByFormat, isForwards); break;
            case categoryCol:     list.sort (KnownPluginList::sortByCategory, isForwards); break;
            case manufacturerCol: list.sort (KnownPluginList::sortByManufacturer, isForwards); break;
            case descCol:         break;

            default: jassertfalse; break;
        }
    }

    static String getPluginDescription (const PluginDescription& desc)
    {
        StringArray items;

        if (desc.descriptiveName != desc.name)
            items.add (desc.descriptiveName);

        items.add (desc.version);

        items.removeEmptyStrings();
        return items.joinIntoString (" - ");
    }

    PluginListComponent& owner;
    KnownPluginList& list;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TableModel)
};

//==============================================================================
class PluginListComponent::Scanner final : private Timer
{
public:
    static std::shared_ptr<Scanner> make (PluginListComponent& plc,
                                          AudioPluginFormat& format,
                                          const StringArray& filesOrIdentifiers,
                                          PropertiesFile* properties,
                                          bool allowPluginsWhichRequireAsynchronousInstantiation,
                                          int threads,
                                          const String& title,
                                          const String& text)
    {
        std::shared_ptr<Scanner> strong { new Scanner (plc,
                                                       format,
                                                       filesOrIdentifiers,
                                                       properties,
                                                       allowPluginsWhichRequireAsynchronousInstantiation,
                                                       threads,
                                                       title,
                                                       text) };
        strong->doScan (strong);
        return strong;
    }

    ~Scanner() override
    {
        if (pool != nullptr)
        {
            pool->removeAllJobs (true, 60000);
            pool.reset();
        }
    }

private:
    enum Flags
    {
        stopRequested   = 1 << 0,   // Set to indicate to the background scanner that it should stop asap
        finished        = 1 << 1,   // Set by the scanner to indicate that it's done
    };

    PluginListComponent& owner;
    AudioPluginFormat& formatToScan;
    StringArray filesOrIdentifiersToScan;
    PropertiesFile* propertiesToUse;
    std::unique_ptr<PluginDirectoryScanner> scanner;
    AlertWindow pathChooserWindow, progressWindow;
    FileSearchPathListComponent pathList;
    String pluginBeingScanned;
    double progress = 0;
    const int numThreads;
    bool allowAsync, timerReentrancyCheck = false;
    std::atomic<int> flags { 0 };
    std::unique_ptr<ThreadPool> pool;
    std::set<String> initiallyBlacklistedFiles = std::invoke ([&]
    {
        const auto blacklisted = owner.list.getBlacklistedFiles();
        return std::set<String> (blacklisted.begin(), blacklisted.end());
    });
    ScopedMessageBox messageBox;

    Scanner (PluginListComponent& plc,
             AudioPluginFormat& format,
             const StringArray& filesOrIdentifiers,
             PropertiesFile* properties,
             bool allowPluginsWhichRequireAsynchronousInstantiation,
             int threads,
             const String& title,
             const String& text)
        : owner (plc),
          formatToScan (format),
          filesOrIdentifiersToScan (filesOrIdentifiers),
          propertiesToUse (properties),
          pathChooserWindow (TRANS ("Select folders to scan..."), String(), MessageBoxIconType::NoIcon, &plc),
          progressWindow (title, text, MessageBoxIconType::NoIcon, &plc),
          numThreads (threads),
          allowAsync (allowPluginsWhichRequireAsynchronousInstantiation)
    {
    }

    void doScan (std::weak_ptr<Scanner> weakSelf)
    {
        FileSearchPath path (formatToScan.getDefaultLocationsToSearch());

        // You need to use at least one thread when scanning plug-ins asynchronously
        jassert (! allowAsync || (numThreads > 0));

        // If the filesOrIdentifiersToScan argument isn't empty, we should only scan these
        // If the path is empty, then paths aren't used for this format.
        if (filesOrIdentifiersToScan.isEmpty() && path.getNumPaths() > 0)
        {
           #if ! JUCE_IOS
            if (propertiesToUse != nullptr)
                path = getLastSearchPath (*propertiesToUse, formatToScan);
           #endif

            pathList.setSize (500, 300);
            pathList.setPath (path);

            pathChooserWindow.addCustomComponent (&pathList);
            pathChooserWindow.addButton (TRANS ("Scan"),   1, KeyPress (KeyPress::returnKey));
            pathChooserWindow.addButton (TRANS ("Cancel"), 0, KeyPress (KeyPress::escapeKey));

            pathChooserWindow.enterModalState (true,
                                               ModalCallbackFunction::create ([this, weakSelf] (auto result)
                                               {
                                                   if (result != 0)
                                                       warnUserAboutUnsuitablePaths (weakSelf);
                                                   else
                                                   {
                                                       requestStop();
                                                       finishedScan();
                                                   }
                                               }));
        }
        else
        {
            startScan (weakSelf);
        }
    }

    // Try to dissuade people from to scanning their entire C: drive, or other system folders.
    void warnUserAboutUnsuitablePaths (std::weak_ptr<Scanner> weakSelf)
    {
        for (int i = 0; i < pathList.getPath().getNumPaths(); ++i)
        {
            auto f = pathList.getPath().getRawString (i);

            if (File::isAbsolutePath (f) && isUnsuitablePath (File (f)))
            {
                auto options = MessageBoxOptions::makeOptionsOkCancel (MessageBoxIconType::WarningIcon,
                                                                       TRANS ("Plugin Scanning"),
                                                                       TRANS ("If you choose to scan folders that contain non-plugin files, "
                                                                              "then scanning may take a long time, and can cause crashes when "
                                                                              "attempting to load unsuitable files.")
                                                                         + newLine
                                                                         + TRANS ("Are you sure you want to scan the folder \"XYZ\"?")
                                                                            .replace ("XYZ", f),
                                                                       TRANS ("Scan"));
                messageBox = AlertWindow::showScopedAsync (options, [this, weakSelf] (int result)
                {
                    if (result != 0)
                        startScan (weakSelf);
                    else
                    {
                        requestStop();
                        finishedScan();
                    }
                });

                return;
            }
        }

        startScan (weakSelf);
    }

    static bool isUnsuitablePath (const File& f)
    {
        Array<File> roots;
        File::findFileSystemRoots (roots);

        if (roots.contains (f))
            return true;

        File::SpecialLocationType pathsThatWouldBeUnsuitableToScan[]
            = { File::globalApplicationsDirectory,
                File::userHomeDirectory,
                File::userDocumentsDirectory,
                File::userDesktopDirectory,
                File::tempDirectory,
                File::userMusicDirectory,
                File::userMoviesDirectory,
                File::userPicturesDirectory };

        for (auto location : pathsThatWouldBeUnsuitableToScan)
        {
            auto sillyFolder = File::getSpecialLocation (location);

            if (f == sillyFolder || sillyFolder.isAChildOf (f))
                return true;
        }

        return false;
    }

    void startScan (std::weak_ptr<Scanner> weakSelf)
    {
        pathChooserWindow.setVisible (false);

        scanner.reset (new PluginDirectoryScanner (owner.list, formatToScan, pathList.getPath(),
                                                   true, owner.deadMansPedalFile, allowAsync));

        if (! filesOrIdentifiersToScan.isEmpty())
        {
            scanner->setFilesOrIdentifiersToScan (filesOrIdentifiersToScan);
        }
        else if (propertiesToUse != nullptr)
        {
            setLastSearchPath (*propertiesToUse, formatToScan, pathList.getPath());
            propertiesToUse->saveIfNeeded();
        }

        progressWindow.addButton (TRANS ("Cancel"), 0, KeyPress (KeyPress::escapeKey));
        progressWindow.addProgressBarComponent (progress);
        progressWindow.enterModalState (true, ModalCallbackFunction::create ([weakSelf] (auto)
        {
            if (const auto strong = weakSelf.lock())
                strong->requestStop();
        }));

        if (numThreads > 0)
        {
            pool.reset (new ThreadPool (ThreadPoolOptions{}.withNumberOfThreads (numThreads)));

            for (int i = numThreads; --i >= 0;)
                pool->addJob (new ScanJob (*this), true);
        }

        startTimer (20);
    }

    void requestStop()
    {
        flags |= stopRequested;

        // KnownPluginList::CustomScanner::shouldExit() observes the exit flag
        // of the current ThreadPoolJob, not Scanner::flags. ThreadPool owns the
        // lock/lifetime while it signals jobs; getJob()+raw pointer aqui teria
        // corrida com o worker apagando o job. Se todos ainda estavam apenas
        // na fila, removeAllJobs os retira sem executar runJob, portanto somos
        // nos que precisamos marcar `finished` nesse caso.
        if (pool != nullptr)
            if (pool->removeAllJobs (true, 0))
                flags |= finished;
    }

    void finishedScan()
    {
        const auto blacklisted = owner.list.getBlacklistedFiles();
        std::set<String> allBlacklistedFiles (blacklisted.begin(), blacklisted.end());

        std::vector<String> newBlacklistedFiles;
        std::set_difference (allBlacklistedFiles.begin(), allBlacklistedFiles.end(),
                             initiallyBlacklistedFiles.begin(), initiallyBlacklistedFiles.end(),
                             std::back_inserter (newBlacklistedFiles));

        owner.scanFinished (scanner != nullptr ? scanner->getFailedFiles() : StringArray(),
                            newBlacklistedFiles,
                            (flags & stopRequested) != 0);
    }

    void timerCallback() override
    {
        if (timerReentrancyCheck)
            return;

        progress = scanner->getProgress();

        if (pool == nullptr)
        {
            const ScopedValueSetter<bool> setter (timerReentrancyCheck, true);

            if (doNextScan())
                startTimer (20);
        }

        if ((flags & finished) != 0)
            finishedScan();
        else
            progressWindow.setMessage (TRANS ("Testing") + ":\n\n" + pluginBeingScanned);
    }

    bool doNextScan()
    {
        if ((flags & stopRequested) == 0 && scanner->scanNextFile (true, pluginBeingScanned))
            return true;

        flags |= finished;
        return false;
    }

    struct ScanJob final : public ThreadPoolJob
    {
        explicit ScanJob (Scanner& s)  : ThreadPoolJob ("pluginscan"), scanner (s) {}

        JobStatus runJob() override
        {
            while (! shouldExit() && scanner.doNextScan())
            {}

            // removeAllJobs(true, ...) also signals the job when the owning
            // dialog is closed. Mark the shared scanner as cancelled/finished
            // here so no second file starts and the UI cannot remain waiting.
            if (shouldExit())
                scanner.flags |= stopRequested | finished;

            return jobHasFinished;
        }

        Scanner& scanner;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ScanJob)
    };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Scanner)
};

//==============================================================================
PluginListComponent::PluginListComponent (AudioPluginFormatManager& manager, KnownPluginList& listToEdit,
                                          const File& deadMansPedal, PropertiesFile* const props,
                                          bool allowPluginsWhichRequireAsynchronousInstantiation)
    : formatManager (manager),
      list (listToEdit),
      deadMansPedalFile (deadMansPedal),
      optionsButton (TRANS ("Options...")),
      propertiesToUse (props),
      allowAsync (allowPluginsWhichRequireAsynchronousInstantiation),
      numThreads (allowAsync ? 1 : 0)
{
    tableModel.reset (new TableModel (*this, listToEdit));

    TableHeaderComponent& header = table.getHeader();

    header.addColumn (TRANS ("Name"),         TableModel::nameCol,         200, 100, 700, TableHeaderComponent::defaultFlags | TableHeaderComponent::sortedForwards);
    header.addColumn (TRANS ("Format"),       TableModel::typeCol,         80, 80, 80,    TableHeaderComponent::notResizable);
    header.addColumn (TRANS ("Category"),     TableModel::categoryCol,     100, 100, 200);
    header.addColumn (TRANS ("Manufacturer"), TableModel::manufacturerCol, 200, 100, 300);
    header.addColumn (TRANS ("Description"),  TableModel::descCol,         300, 100, 500, TableHeaderComponent::notSortable);

    table.setHeaderHeight (22);
    table.setRowHeight (20);
    table.setModel (tableModel.get());
    table.setMultipleSelectionEnabled (true);
    addAndMakeVisible (table);

    addAndMakeVisible (optionsButton);
    optionsButton.onClick = [this]
    {
        createOptionsMenu().showMenuAsync (PopupMenu::Options()
                                              .withDeletionCheck (*this)
                                              .withTargetComponent (optionsButton));
    };

    optionsButton.setTriggeredOnMouseDown (true);

    setSize (400, 600);
    list.addChangeListener (this);
    updateList();
    table.getHeader().reSortTable();

    PluginDirectoryScanner::applyBlacklistingsFromDeadMansPedal (list, deadMansPedalFile);
    // Este arquivo e o journal duravel de crash/timeout. Apaga-lo ao apenas
    // reabrir a janela cria uma janela em que o ChangeBroadcaster ainda nao
    // persistiu a blacklist e uma queda reabilita o plugin culpado. O host o
    // limpa somente depois de persistir a quarentena, na revalidacao explicita.
}

PluginListComponent::~PluginListComponent()
{
    list.removeChangeListener (this);
}

void PluginListComponent::setOptionsButtonText (const String& newText)
{
    optionsButton.setButtonText (newText);
    resized();
}

void PluginListComponent::setScanDialogText (const String& title, const String& content)
{
    dialogTitle = title;
    dialogText = content;
}

void PluginListComponent::setNumberOfThreadsForScanning (int num)
{
    numThreads = num;
}

void PluginListComponent::resized()
{
    auto r = getLocalBounds().reduced (2);

    if (optionsButton.isVisible())
    {
        optionsButton.setBounds (r.removeFromBottom (24));
        optionsButton.changeWidthToFitText (24);
        r.removeFromBottom (3);
    }

    table.setBounds (r);
}

void PluginListComponent::changeListenerCallback (ChangeBroadcaster*)
{
    table.getHeader().reSortTable();
    updateList();
}

void PluginListComponent::updateList()
{
    table.updateContent();
    table.repaint();
}

void PluginListComponent::removeSelectedPlugins()
{
    auto selected = table.getSelectedRows();

    for (int i = table.getNumRows(); --i >= 0;)
        if (selected.contains (i))
            removePluginItem (i);
}

void PluginListComponent::setTableModel (TableListBoxModel* model)
{
    table.setModel (nullptr);
    tableModel.reset (model);
    table.setModel (tableModel.get());

    table.getHeader().reSortTable();
    table.updateContent();
    table.repaint();
}

static bool canShowFolderForPlugin (KnownPluginList& list, int index)
{
    const auto types = list.getTypes();
    return isPositiveAndBelow (index, types.size())
        && File::createFileWithoutCheckingPath (
               types.getReference (index).fileOrIdentifier).exists();
}

static void showFolderForPlugin (KnownPluginList& list, int index)
{
    const auto types = list.getTypes();

    if (isPositiveAndBelow (index, types.size()))
    {
        const File folder (types.getReference (index).fileOrIdentifier);

        if (folder.exists())
            folder.revealToUser();
    }
}

void PluginListComponent::removeMissingPlugins()
{
    auto types = list.getTypes();

    for (int i = types.size(); --i >= 0;)
    {
        auto type = types.getUnchecked (i);

        if (! formatManager.doesPluginStillExist (type))
            list.removeType (type);
    }
}

void PluginListComponent::removePluginItem (int index)
{
    const auto types = list.getTypes();
    const auto blacklisted = list.getBlacklistedFiles();

    if (isPositiveAndBelow (index, types.size()))
        list.removeType (types.getReference (index));
    else if (isPositiveAndBelow (index - types.size(), blacklisted.size()))
        list.removeFromBlacklist (blacklisted[index - types.size()]);
}

PopupMenu PluginListComponent::createOptionsMenu()
{
    PopupMenu menu;
    menu.addItem (PopupMenu::Item (TRANS ("Clear list"))
                    .setAction ([this] { list.clear(); }));

    menu.addSeparator();

    for (auto format : formatManager.getFormats())
        if (format->canScanForPlugins())
            menu.addItem (PopupMenu::Item (TRANS ("Remove all XFMTX plug-ins").replace ("XFMTX", format->getName()))
                            .setEnabled (! list.getTypesForFormat (*format).isEmpty())
                            .setAction ([this, format]
                                        {
                                            for (auto& pd : list.getTypesForFormat (*format))
                                                list.removeType (pd);
                                        }));

    menu.addSeparator();

    menu.addItem (PopupMenu::Item (TRANS ("Remove selected plug-in from list"))
                    .setEnabled (table.getNumSelectedRows() > 0)
                    .setAction ([this] { removeSelectedPlugins(); }));

    menu.addItem (PopupMenu::Item (TRANS ("Remove any plug-ins whose files no longer exist"))
                    .setAction ([this] { removeMissingPlugins(); }));

    menu.addSeparator();

    auto selectedRow = table.getSelectedRow();

    menu.addItem (PopupMenu::Item (TRANS ("Show folder containing selected plug-in"))
                    .setEnabled (canShowFolderForPlugin (list, selectedRow))
                    .setAction ([this, selectedRow] { showFolderForPlugin (list, selectedRow); }));

    menu.addSeparator();

    for (auto format : formatManager.getFormats())
        if (format->canScanForPlugins())
            menu.addItem (PopupMenu::Item (TRANS ("Scan for new or updated XFMTX plug-ins").replace ("XFMTX", format->getName()))
                            .setAction ([this, format]  { scanFor (*format); }));

    return menu;
}

PopupMenu PluginListComponent::createMenuForRow (int rowNumber)
{
    PopupMenu menu;
    const auto types = list.getTypes();
    const auto blacklisted = list.getBlacklistedFiles();

    if (isPositiveAndBelow (rowNumber, types.size()))
    {
        const auto description = types.getReference (rowNumber);
        const auto pluginFile = File::createFileWithoutCheckingPath (
            description.fileOrIdentifier);

        menu.addItem (PopupMenu::Item (TRANS ("Remove plug-in from list"))
                        .setAction ([this, description]
                        {
                            list.removeType (description);
                        }));

        menu.addItem (PopupMenu::Item (TRANS ("Show folder containing plug-in"))
                        .setEnabled (pluginFile.exists())
                        .setAction ([pluginFile]
                        {
                            if (pluginFile.exists())
                                pluginFile.revealToUser();
                        }));
    }
    else
    {
        const int blacklistIndex = rowNumber - types.size();

        if (isPositiveAndBelow (blacklistIndex, blacklisted.size()))
        {
            const auto identifier = blacklisted[blacklistIndex];
            menu.addItem (PopupMenu::Item (TRANS ("Remove plug-in from list"))
                            .setAction ([this, identifier]
                            {
                                list.removeFromBlacklist (identifier);
                            }));
        }
    }

    return menu;
}

bool PluginListComponent::isInterestedInFileDrag (const StringArray& /*files*/)
{
    return true;
}

void PluginListComponent::filesDropped (const StringArray& files, int, int)
{
    OwnedArray<PluginDescription> typesFound;
    list.scanAndAddDragAndDroppedFiles (formatManager, files, typesFound);
}

FileSearchPath PluginListComponent::getLastSearchPath (PropertiesFile& properties, AudioPluginFormat& format)
{
    auto key = "lastPluginScanPath_" + format.getName();

    if (properties.containsKey (key) && properties.getValue (key, {}).trim().isEmpty())
        properties.removeValue (key);

    return FileSearchPath (properties.getValue (key, format.getDefaultLocationsToSearch().toString()));
}

void PluginListComponent::setLastSearchPath (PropertiesFile& properties, AudioPluginFormat& format,
                                             const FileSearchPath& newPath)
{
    auto key = "lastPluginScanPath_" + format.getName();

    if (newPath.getNumPaths() == 0)
        properties.removeValue (key);
    else
        properties.setValue (key, newPath.toString());
}

//==============================================================================
void PluginListComponent::scanFor (AudioPluginFormat& format)
{
    scanFor (format, StringArray());
}

void PluginListComponent::scanFor (AudioPluginFormat& format, const StringArray& filesOrIdentifiersToScan)
{
    lastScanWasCancelled = false;
    lastScanWasInconclusive = false;
    currentScanner = Scanner::make (*this,
                                    format,
                                    filesOrIdentifiersToScan,
                                    propertiesToUse,
                                    allowAsync,
                                    numThreads,
                                    dialogTitle.isNotEmpty() ? dialogTitle : TRANS ("Scanning for plug-ins..."),
                                    dialogText.isNotEmpty()  ? dialogText  : TRANS ("Searching for all possible plug-in files..."));
}

bool PluginListComponent::isScanning() const noexcept
{
    return currentScanner != nullptr;
}

bool PluginListComponent::consumeLastScanWasCancelled() noexcept
{
    const bool result = lastScanWasCancelled;
    lastScanWasCancelled = false;
    return result;
}

bool PluginListComponent::consumeLastScanWasInconclusive() noexcept
{
    const bool result = lastScanWasInconclusive;
    lastScanWasInconclusive = false;
    return result;
}

void PluginListComponent::scanFinished (const StringArray& failedFiles,
                                        const std::vector<String>& newBlacklistedFiles,
                                        bool wasCancelled)
{
    lastScanWasCancelled = wasCancelled;
    lastScanWasInconclusive = list.consumeLastScanWasInconclusive();
    StringArray warnings;

    const auto addWarningText = [&warnings] (const auto& range, const auto& prefix)
    {
        if (range.size() == 0)
            return;

        StringArray names;

        for (auto& f : range)
            names.add (File::createFileWithoutCheckingPath (f).getFileName());

        warnings.add (prefix + ":\n\n" + names.joinIntoString (", "));
    };

    if (! wasCancelled)
    {
        addWarningText (newBlacklistedFiles,  TRANS ("The following files encountered fatal errors during validation"));
        addWarningText (failedFiles,          TRANS ("The following files appeared to be plugin files, but failed to load correctly"));
    }

    currentScanner.reset(); // mustn't delete this before using the failed files array

    if (! warnings.isEmpty())
    {
        auto options = MessageBoxOptions::makeOptionsOk (MessageBoxIconType::InfoIcon,
                                                         TRANS ("Scan complete"),
                                                         warnings.joinIntoString ("\n\n"));
        messageBox = AlertWindow::showScopedAsync (options, nullptr);
    }
}

} // namespace juce
