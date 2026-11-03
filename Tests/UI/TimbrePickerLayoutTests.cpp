#include "TimbrePicker.h"
#include "PluginGalleryLayout.h"
#include "LibraryCatalogStatus.h"
#include <iostream>

int runTimbrePickerLayoutTests()
{
    int failures = 0;
    const auto expect = [&] (bool ok, const char* message)
    {
        if (! ok) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
    };
    juce::Array<TimbrePicker::Item> items;
    for (int i = 0; i < 40; ++i)
    {
        TimbrePicker::Item item;
        item.id = i + 2;
        item.name = i == 7 ? "Pearl Upright" : "Instrument " + juce::String (i);
        item.section = "Church Live";
        item.category = i < 20 ? "PIANOS" : "PADS";
        item.subtitle = "Library instrument";
        item.key = "key" + juce::String (i);
        items.add (item);
    }
    NordLookAndFeel theme;
    // Inactive graphite buttons must remain distinct from colour-selected filters.
    juce::TextButton filter;
    filter.setBounds (0, 0, 120, 36);
    const auto buttonFill = [&] (juce::Colour configured)
    {
        juce::Image surface (juce::Image::ARGB, 120, 36, true);
        {
            juce::Graphics g (surface);
            theme.drawButtonBackground (g, filter, configured, false, false);
        } // Direct2D commits drawing when the Graphics context is released.
        return surface.getPixelAt (40, 18);
    };
    const auto neutral = buttonFill (juce::Colour (0xff14191f));
    const auto selected = buttonFill (juce::Colour (0xff3a1821));
    expect (neutral.getAlpha() > 0 && selected.getAlpha() > 0,
            "button render must finish before reading pixels");
    expect (neutral.getRed() <= neutral.getGreen(), "inactive graphite must not look selected");
    expect (selected.getRed() > selected.getGreen(), "selected filter must retain its accent");
    // Removal is a real action only when the channel contains an instrument.
    for (const int current : { 0, 1, 9 })
    {
        TimbrePicker state (items, current);
        state.setLookAndFeel (&theme);
        state.setSize (1280, 800);
        juce::Component* remove = nullptr;
        for (auto* child : state.getChildren())
            if (child->getComponentID() == "timbrePickerRemove") remove = child;
        expect (remove != nullptr && remove->isVisible() == (current >= 2),
                "empty channels must not show Remove after adding the control to the picker");
        state.replaceItems (items, current >= 2 ? 1 : 9);
        expect (remove != nullptr && remove->isVisible() == (current < 2),
                "Remove visibility must follow replacement channel state");
        state.setLookAndFeel (nullptr);
    }

    {
        auto piano = items.getFirst();
        piano.key = "real-instrument";
        TimbrePicker::Item import;
        import.id = 901;
        import.name = "Importar arquivo .sf2...";
        import.section = "SOUNDFONTS";
        import.key = "legacy-import-favourite";
        import.importAction = true;
        // Old preferences may contain the command from the previous UI bug.
        TimbrePicker actionPicker ({ piano, import }, 1, { piano.key, import.key }, { piano.key, import.key });
        actionPicker.setLookAndFeel (&theme);
        actionPicker.setSize (1280, 800);
        juce::ListBox* actionList = nullptr;
        juce::TextEditor* actionSearch = nullptr;
        juce::TextButton* load = nullptr;
        juce::TextButton* preview = nullptr;
        for (auto* child : actionPicker.getChildren())
        {
            if (auto* value = dynamic_cast<juce::ListBox*> (child)) actionList = value;
            if (auto* value = dynamic_cast<juce::TextEditor*> (child)) actionSearch = value;
            if (auto* value = dynamic_cast<juce::TextButton*> (child))
            {
                if (value->getButtonText() == "Carregar timbre") load = value;
                if (value->getTitle() == "Ouvir amostra do timbre") preview = value;
            }
        }
        expect (actionPicker.getVisibleInstrumentCount() == 1, "import action must not inflate the instrument count");
        expect (actionList != nullptr && actionSearch != nullptr && load != nullptr && preview != nullptr,
                "action picker must expose its existing controls");
        if (actionList != nullptr && actionSearch != nullptr && load != nullptr && preview != nullptr)
        {
            int favoriteEvents = 0, pickedAction = 0;
            actionPicker.onFavoriteChanged = [&] (const juce::String&, bool) { ++favoriteEvents; };
            actionPicker.onPick = [&] (int id) { pickedAction = id; };
            actionPicker.canPreview = [] (int) { return true; };
            actionSearch->setText ("Importar", false);
            actionSearch->onTextChange();
            expect (actionList->getListBoxModel()->getNumRows() == 1
                     && actionPicker.getVisibleInstrumentCount() == 0,
                    "search must find the import command without counting it as an instrument");
            expect (load->getButtonText() == "Importar arquivo" && load->isEnabled() && ! preview->isEnabled(),
                    "import selection must offer a file action and disable instrument audition");
            const auto point = juce::Point<float> ((float) actionList->getWidth() - 10.0f, 30.0f);
            const auto now = juce::Time::getCurrentTime();
            const juce::MouseEvent click (juce::Desktop::getInstance().getMainMouseSource(), point,
                juce::ModifierKeys::leftButtonModifier, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                actionList, actionList, now, point, now, 1, false);
            actionList->getListBoxModel()->listBoxItemClicked (0, click);
            expect (favoriteEvents == 0 && pickedAction == 0,
                    "clicking the former star area must only select an import action");
            actionList->keyPressed (juce::KeyPress (juce::KeyPress::returnKey));
            expect (pickedAction == import.id, "Enter must still invoke the original import callback");
            actionPicker.selectCategory (1);
            expect (actionList->getListBoxModel()->getNumRows() == 1,
                    "legacy import favorite must not appear among favorite instruments");
            actionPicker.selectCategory (2);
            expect (actionList->getListBoxModel()->getNumRows() == 1,
                    "legacy import recent must not appear among recent instruments");
        }
        actionPicker.setLookAndFeel (nullptr);
    }

    {
        using Failure = ChurchDAWDistribution::Failure;
        const auto offline = ChurchDAWDistribution::failedResponse (false, 0);
        const auto server = ChurchDAWDistribution::failedResponse (true, 503);
        const auto signature = ChurchDAWDistribution::failedResponse (true, 200);
        const auto malformedCatalog = ChurchDAWDistribution::failedResponse (true, 200);
        expect (offline == Failure::connection && server == Failure::httpResponse,
                "transport and HTTP failures must be distinct presentation categories");
        expect (signature == Failure::validation && malformedCatalog == Failure::validation,
                "rejected signature/parse responses must not be presented as offline");
        expect (ChurchDAWDistribution::retainFailure (signature, offline) == Failure::validation,
                "fallback timeout must not hide an earlier validation failure");
        const auto offlineCopy = ChurchDAWLibraryStatus::unavailable (offline, false);
        const auto serverCopy = ChurchDAWLibraryStatus::unavailable (server, false);
        const auto validationCopy = ChurchDAWLibraryStatus::unavailable (signature, false);
        expect (offlineCopy.message != serverCopy.message && serverCopy.message != validationCopy.message
                 && offlineCopy.message != validationCopy.message,
                "offline, server and invalid response states must provide different guidance");
        expect (validationCopy.message.contains ("validar") && ! validationCopy.empty.contains ("Conecte-se"),
                "validation failure must request retry without claiming the computer is disconnected");
        for (const auto failure : { offline, server, signature, Failure::none })
        {
            const auto copy = ChurchDAWLibraryStatus::unavailable (failure, true);
            expect (copy.message.contains ("instaladas"), "failure must preserve the installed-library reassurance");
            expect (! copy.message.contains ("HTTP") && ! copy.message.contains ("https://")
                     && ! copy.message.contains ("SHA") && ! copy.message.contains ("assinatura"),
                    "catalog UI must not expose distribution diagnostics");
        }
    }
    TimbrePicker picker (items, 9, {"key7"}, {"key30"});
    picker.setLookAndFeel (&theme);
    picker.setVisible (true);
    juce::ListBox* list = nullptr;
    juce::TextEditor* search = nullptr;
    for (auto* child : picker.getChildren())
    {
        if (auto* value = dynamic_cast<juce::ListBox*> (child)) list = value;
        if (auto* value = dynamic_cast<juce::TextEditor*> (child)) search = value;
    }
    expect (list != nullptr && search != nullptr, "picker needs list and search");
    if (list == nullptr || search == nullptr) return 1;

    for (const auto size : {juce::Point<int> (960, 600), {1307, 740}, {1600, 900}})
    {
        picker.setSize (size.x, size.y);
        expect (! list->getBounds().intersects (search->getBounds()), "search must not cover rows");
        for (auto* child : picker.getChildren())
            if (auto* filter = dynamic_cast<TimbrePicker::FilterButton*> (child))
            {
                expect (picker.getLocalBounds().contains (filter->getBounds()), "category must remain in window");
                expect (! filter->getBounds().intersects (list->getBounds()), "category must not cover rows");
            }
        // A modal's final resize can leave the selected row partly above its viewport.
        const int selected = list->getSelectedRow();
        list->getViewport()->setViewPosition (0, selected * list->getRowHeight() + 24);
        picker.focusSearch();
        const auto row = list->getRowPosition (selected, true);
        expect (row.getY() >= 0 && row.getBottom() <= list->getHeight(),
                "opening must reveal the whole selected instrument after resizing");
    }
    int picked = 0;
    picker.onPick = [&] (int id) { picked = id; };
    picker.selectCategory (3);
    expect (list->getListBoxModel()->getNumRows() == 20, "piano category must filter the catalog");
    search->setText ("pearl", false);
    search->onTextChange();
    expect (list->getListBoxModel()->getNumRows() == 1, "case-insensitive search must narrow results");
    int previewed = 0;
    picker.canPreview = [] (int id) { return id == 9; };
    picker.onPreview = [&] (int id) { previewed = id; };
    for (auto* child : picker.getChildren())
        if (auto* button = dynamic_cast<juce::TextButton*> (child))
            if (button->getTitle() == "Ouvir amostra do timbre") button->onClick();
    expect (previewed == 9 && picked == 0, "preview must use the highlighted item without loading it");
    list->keyPressed (juce::KeyPress (juce::KeyPress::returnKey));
    expect (picked == 9, "Enter must pick the visible result's original id");
    search->setText ("does not exist", false);
    search->onTextChange();
    expect (list->getListBoxModel()->getNumRows() == 0, "no matches must leave an empty list");
    picked = 0;
    list->keyPressed (juce::KeyPress (juce::KeyPress::returnKey));
    expect (picked == 0, "Enter on empty results must not pick a stale instrument");
    picker.selectCategory (0);
    picker.selectCategory (3); // External category selection replaces the family.
    picker.selectCategory (1);
    expect (list->getListBoxModel()->getNumRows() == 1, "favorites must remain available with family filtering");
    picker.setLookAndFeel (nullptr);
    if (! failures) std::cout << "TIMBRE_PICKER_LAYOUT_OK\n";
    return failures ? 1 : 0;
}

int runPluginGalleryLayoutTests()
{
    int failures = 0;
    const auto expect = [&] (bool ok, const char* message)
    { if (! ok) { ++failures; std::cerr << "FAIL: " << message << '\n'; } };
    // The same bounds are used by drawing, mouse hit testing and viewport sizing.
    for (const int width : {190, 360, 438, 640, 880, 1100})
    {
        const auto layout = ChurchDAWPluginGallery::forWidth (width);
        expect (layout.height (0) == 0, "an empty search must not retain stale scroll space");
        for (const int count : {1, 2, 7, 31})
        {
            juce::Rectangle<int> content (0, 0, width, layout.height (count));
            for (int i = 0; i < count; ++i)
            {
                const auto card = layout.item (i);
                expect (content.contains (card), "the last plugin must remain reachable by scrolling");
                for (int j = 0; j < i; ++j)
                    expect (! card.intersects (layout.item (j)), "cards must not steal neighbouring mouse clicks");
            }
            expect (layout.item (count - 1).getBottom() == content.getBottom(),
                    "the scroll range must follow the final filtered row");
        }
    }
    if (! failures) std::cout << "PLUGIN_GALLERY_LAYOUT_OK\n";
    return failures ? 1 : 0;
}
