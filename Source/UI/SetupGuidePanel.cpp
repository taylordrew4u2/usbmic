#include "SetupGuidePanel.h"
#include "AppLookAndFeel.h"

namespace mma {

namespace {
constexpr int kMargin       = 12;
constexpr int kTopRow       = 36;
constexpr int kAfterTopRow  = 18;
constexpr int kAfterTitle   = 8;
constexpr int kAfterBody    = 12;
constexpr int kAfterStatus  = 12;
constexpr int kMicRow       = 34; // a name and the hardware's own name under it
constexpr int kNameRow      = 36; // a 28px Name button with room around it
constexpr int kToggleRow    = 30;
constexpr int kFolderRow    = 32;
constexpr int kAfterContent = 18;
constexpr int kButtonRow    = 36;
constexpr int kMaxTextWidth = 560; // the drawer's widest, less its margins

juce::Font titleFont()   { return juce::Font (20.0f, juce::Font::bold); }
juce::Font bodyFont()    { return juce::Font (13.0f); }
juce::Font counterFont() { return juce::Font (10.0f, juce::Font::bold); }
} // namespace

SetupGuidePanel::SetupGuidePanel()
{
    // None of these take the keyboard on a click. The guide sits beside a live
    // main screen whose spacebar is the room's mute; a click on Next that left
    // the keyboard on Next would make the next Space press Next again instead.
    // Tab still reaches every one of them.
    for (auto* b : { &skipButton, &backButton, &nextButton, &changeFolderButton })
    {
        b->setMouseClickGrabsKeyboardFocus (false);
        addAndMakeVisible (b);
    }

    skipButton.setTooltip ("Close the guide. Settings and Help can open it again.");
    skipButton.onClick = [this] { if (onSkipClicked) onSkipClicked(); };
    backButton.onClick = [this] { if (onBackClicked) onBackClicked(); };

    // The one colour that acts, on the one button that moves the reader on.
    nextButton.setColour (juce::TextButton::buttonColourId, AppLookAndFeel::accent);
    nextButton.setColour (juce::TextButton::textColourOffId, AppLookAndFeel::background);
    nextButton.onClick = [this] { if (onNextClicked) onNextClicked(); };

    changeFolderButton.setTitle ("Change where recordings are saved");
    changeFolderButton.onClick = [this] { if (onChangeFolderClicked) onChangeFolderClicked(); };

    updateChildVisibility();
}

SetupGuidePanel::~SetupGuidePanel() = default;

void SetupGuidePanel::setPage (const SetupGuidePage& newPage)
{
    const bool changed = newPage.step != page.step || newPage.number != page.number
                      || newPage.total != page.total || newPage.title != page.title
                      || newPage.body != page.body || newPage.status != page.status
                      || newPage.statusIsWarning != page.statusIsWarning
                      || newPage.nextLabel != page.nextLabel || newPage.canGoBack != page.canGoBack;

    // Restated on every tick from live state, so only a real change relays out.
    if (! changed)
        return;

    page = newPage;
    nextButton.setButtonText (juce::String (page.nextLabel));
    backButton.setEnabled (page.canGoBack);
    updateChildVisibility();
    resized();
    repaint();
}

void SetupGuidePanel::setMicrophones (const std::vector<MicRow>& rows)
{
    bool same = rows.size() == mics.size();
    for (size_t i = 0; same && i < rows.size(); ++i)
        same = rows[i].name == mics[i].name && rows[i].product == mics[i].product;

    if (same)
        return;

    mics = rows;

    // One Name button per microphone, rebuilt only when the count changes so
    // a button being pressed is never deleted under the click.
    if (nameButtons.size() != mics.size())
    {
        nameButtons.clear();

        for (size_t i = 0; i < mics.size(); ++i)
        {
            auto b = std::make_unique<juce::TextButton> ("Name");
            b->setMouseClickGrabsKeyboardFocus (false);
            const int index = static_cast<int> (i);
            b->onClick = [this, index] { if (onNameMicClicked) onNameMicClicked (index); };
            addChildComponent (*b);
            nameButtons.push_back (std::move (b));
        }
    }

    // Two identical buttons read the same to a screen reader; the title says
    // which microphone each one names.
    for (size_t i = 0; i < mics.size(); ++i)
        nameButtons[i]->setTitle ("Name " + mics[i].name);

    updateChildVisibility();
    resized();
    repaint();
}

void SetupGuidePanel::setHighlightedMic (int index)
{
    if (index == highlightedMic)
        return;

    highlightedMic = index;

    if (page.step == SetupGuideStep::Names)
        repaint (listArea);
}

void SetupGuidePanel::setHeadphones (const std::vector<HeadphoneRow>& rows)
{
    bool sameRows = rows.size() == headphones.size();
    for (size_t i = 0; sameRows && i < rows.size(); ++i)
        sameRows = rows[i].key == headphones[i].key && rows[i].label == headphones[i].label;

    headphones = rows;

    if (! sameRows)
    {
        headphoneToggles.clear();

        for (const auto& row : headphones)
        {
            auto t = std::make_unique<juce::ToggleButton> (row.label);
            t->setMouseClickGrabsKeyboardFocus (false);
            const auto key = row.key;
            auto* raw = t.get();
            t->onClick = [this, key, raw] { if (onHeadphonesToggled) onHeadphonesToggled (key, raw->getToggleState()); };
            addChildComponent (*t);
            headphoneToggles.push_back (std::move (t));
        }

        updateChildVisibility();
        resized();
    }

    // The state is restated every time: it can change in Settings while the
    // guide is open, and the two lists must not disagree.
    for (size_t i = 0; i < headphones.size(); ++i)
        headphoneToggles[i]->setToggleState (headphones[i].on, juce::dontSendNotification);
}

void SetupGuidePanel::setDestinationFolder (const juce::String& path)
{
    if (path == destination)
        return;

    destination = path;

    if (page.step == SetupGuideStep::SaveFolder)
        repaint (folderArea);
}

void SetupGuidePanel::updateChildVisibility()
{
    backButton.setEnabled (page.canGoBack);
    changeFolderButton.setVisible (page.step == SetupGuideStep::SaveFolder);

    for (auto& b : nameButtons)
        b->setVisible (page.step == SetupGuideStep::Names);

    for (auto& t : headphoneToggles)
        t->setVisible (page.step == SetupGuideStep::Headphones);
}

int SetupGuidePanel::listRowCount() const
{
    switch (page.step)
    {
        case SetupGuideStep::Microphones:
        case SetupGuideStep::Names:      return static_cast<int> (mics.size());
        case SetupGuideStep::Headphones: return static_cast<int> (headphones.size());
        case SetupGuideStep::SaveFolder: return 1;
        case SetupGuideStep::Cameras:
        case SetupGuideStep::TestTake:   break;
    }

    return 0;
}

int SetupGuidePanel::listRowHeight() const
{
    switch (page.step)
    {
        case SetupGuideStep::Microphones: return kMicRow;
        case SetupGuideStep::Names:       return kNameRow;
        case SetupGuideStep::Headphones:  return kToggleRow;
        case SetupGuideStep::SaveFolder:  return kFolderRow + 22; // the path, then its button
        case SetupGuideStep::Cameras:
        case SetupGuideStep::TestTake:    break;
    }

    return 0;
}

int SetupGuidePanel::textWidth() const
{
    // A floor for the moment before a container has sized it, as in HelpPanel:
    // measuring at width 1 would report one word per line.
    return juce::jmin (kMaxTextWidth, juce::jmax (300, getWidth() - kMargin * 2));
}

juce::AttributedString SetupGuidePanel::titleText() const
{
    juce::AttributedString s;
    s.setJustification (juce::Justification::topLeft);
    s.append (juce::String (page.title), titleFont(), AppLookAndFeel::bone);
    return s;
}

juce::AttributedString SetupGuidePanel::bodyText() const
{
    juce::AttributedString s;
    s.setJustification (juce::Justification::topLeft);
    s.setLineSpacing (3.0f);
    s.append (juce::String (page.body), bodyFont(), AppLookAndFeel::bone);
    return s;
}

juce::AttributedString SetupGuidePanel::statusText() const
{
    juce::AttributedString s;
    s.setJustification (juce::Justification::topLeft);
    s.setLineSpacing (3.0f);
    s.append (juce::String (page.status), juce::Font (13.0f, juce::Font::bold),
              page.statusIsWarning ? AppLookAndFeel::warning : AppLookAndFeel::accent);
    return s;
}

int SetupGuidePanel::measure (const juce::AttributedString& text, int width)
{
    if (text.getText().isEmpty())
        return 0;

    juce::TextLayout layout;
    layout.createLayout (text, static_cast<float> (width));
    return static_cast<int> (std::ceil (layout.getHeight())) + 2;
}

int SetupGuidePanel::getRequiredHeight() const
{
    const int width = textWidth();
    int y = kMargin + kTopRow + kAfterTopRow;

    y += measure (titleText(), width) + kAfterTitle;
    y += measure (bodyText(), width) + kAfterBody;

    if (! page.status.empty())
        y += measure (statusText(), width) + kAfterStatus;

    y += listRowCount() * listRowHeight();
    y += kAfterContent + kButtonRow + kMargin;
    return y;
}

void SetupGuidePanel::resized()
{
    const int width = textWidth();
    const int x = kMargin;
    int y = kMargin;

    // Skip where Settings and Help keep their Close: top left, the first place
    // anyone looks for the way out. The page count sits opposite.
    skipButton.setBounds (x, y, 110, kTopRow);
    counterArea = { x + 120, y, width - 120, kTopRow };
    y += kTopRow + kAfterTopRow;

    titleArea = { x, y, width, measure (titleText(), width) };
    y = titleArea.getBottom() + kAfterTitle;

    bodyArea = { x, y, width, measure (bodyText(), width) };
    y = bodyArea.getBottom() + kAfterBody;

    statusArea = { x, y, width, 0 };
    if (! page.status.empty())
    {
        statusArea.setHeight (measure (statusText(), width));
        y = statusArea.getBottom() + kAfterStatus;
    }

    const int rowHeight = listRowHeight();
    listArea = { x, y, width, listRowCount() * rowHeight };
    folderArea = {};

    if (page.step == SetupGuideStep::Names)
        for (size_t i = 0; i < nameButtons.size(); ++i)
            nameButtons[i]->setBounds (x + width - 90, y + static_cast<int> (i) * rowHeight + 4, 90, rowHeight - 8);

    if (page.step == SetupGuideStep::Headphones)
        for (size_t i = 0; i < headphoneToggles.size(); ++i)
            headphoneToggles[i]->setBounds (x, y + static_cast<int> (i) * rowHeight, width, rowHeight - 2);

    if (page.step == SetupGuideStep::SaveFolder)
    {
        folderArea = { x, y, width, 22 };
        changeFolderButton.setBounds (x, y + 22 + 2, 130, kFolderRow - 4);
    }

    y = listArea.getBottom() + kAfterContent;

    // Back on the left, Next on the right, under the page rather than pinned
    // to the bottom of the drawer: on a tall window that kept them a long way
    // from the words they answer.
    backButton.setBounds (x, y, 100, kButtonRow);
    nextButton.setBounds (x + width - 120, y, 120, kButtonRow);
}

void SetupGuidePanel::paint (juce::Graphics& g)
{
    g.fillAll (AppLookAndFeel::surface);

    const auto draw = [&g] (const juce::AttributedString& text, juce::Rectangle<int> area)
    {
        if (area.isEmpty())
            return;

        juce::TextLayout layout;
        layout.createLayout (text, static_cast<float> (area.getWidth()));
        layout.draw (g, area.toFloat());
    };

    // In the small capitals the Settings and Help headings use, so the guide
    // reads as another room in the same house.
    g.setColour (AppLookAndFeel::tertiary);
    g.setFont (counterFont());
    g.drawText ("SETUP GUIDE  " + juce::String (page.number) + " OF " + juce::String (page.total),
                counterArea, juce::Justification::centredRight);

    draw (titleText(), titleArea);
    draw (bodyText(), bodyArea);
    draw (statusText(), statusArea);

    const int rowHeight = listRowHeight();

    if (page.step == SetupGuideStep::Microphones || page.step == SetupGuideStep::Names)
    {
        for (size_t i = 0; i < mics.size(); ++i)
        {
            auto row = juce::Rectangle<int> (listArea.getX(), listArea.getY() + static_cast<int> (i) * rowHeight,
                                             listArea.getWidth(), rowHeight).reduced (0, 2);

            // The row of the microphone heard alone, lit in the accent the
            // strip lights in, so tap and row can be matched at a glance.
            const bool lit = page.step == SetupGuideStep::Names && static_cast<int> (i) == highlightedMic;

            g.setColour (lit ? AppLookAndFeel::accent.withAlpha (0.22f) : AppLookAndFeel::surfaceHigh);
            g.fillRoundedRectangle (row.toFloat(), 6.0f);

            if (lit)
            {
                g.setColour (AppLookAndFeel::accent);
                g.drawRoundedRectangle (row.toFloat().reduced (0.5f), 6.0f, 1.0f);
            }

            auto text = row.reduced (10, 0);
            if (page.step == SetupGuideStep::Names)
                text.removeFromRight (100); // the Name button

            const auto& mic = mics[i];
            const bool showProduct = mic.product.isNotEmpty() && mic.product != mic.name
                                  && row.getHeight() >= 28;

            g.setColour (AppLookAndFeel::bone);
            g.setFont (juce::Font (13.0f, juce::Font::bold));
            g.drawFittedText (mic.name, showProduct ? text.removeFromTop (text.getHeight() / 2 + 1) : text,
                              juce::Justification::centredLeft, 1, 0.8f);

            if (showProduct)
            {
                g.setColour (AppLookAndFeel::secondary);
                g.setFont (juce::Font (11.0f));
                g.drawFittedText (mic.product, text, juce::Justification::topLeft, 1, 0.8f);
            }
        }
    }

    if (page.step == SetupGuideStep::SaveFolder && ! folderArea.isEmpty())
    {
        g.setColour (AppLookAndFeel::bone);
        g.setFont (juce::Font (juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain));
        // A long path shrinks rather than losing its end, which is the part
        // that names the folder.
        g.drawFittedText (destination.isNotEmpty() ? destination : juce::String ("(not chosen yet)"),
                          folderArea, juce::Justification::centredLeft, 1, 0.6f);
    }
}

} // namespace mma
