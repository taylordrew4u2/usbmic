#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <algorithm>
#include <functional>
#include <vector>

namespace mma {

/// Says when something in the window is drawn where the camera previews are.
///
/// On macOS a live preview is a native NSView added on top of the window's own
/// view, so it is composited above everything JUCE paints -- the save prompt,
/// the saved-take card, the red take-alert card and the full-window take
/// banner included. No z-order among JUCE components can put a card over it.
/// The owner therefore hides the preview hosts while any of those overlays is
/// visible, and this watches them so that happens the moment one goes up and
/// is undone the moment the last one comes down, whatever path shows or hides
/// it (a button, a tick, the banner's own timer).
///
/// Hiding only toggles [NSView setHidden:] through JUCE's NSViewAttachment; it
/// never reparents the preview or touches the capture session.
class CameraPreviewCover final : private juce::ComponentListener
{
public:
    CameraPreviewCover() = default;

    ~CameraPreviewCover() override
    {
        for (auto& overlay : overlays)
            if (auto* component = overlay.getComponent())
                component->removeComponentListener (this);
    }

    /// Called with the new state whenever it changes; never on a repeat.
    std::function<void (bool covered)> onCoverChanged;

    /// Watches one overlay for the rest of this object's life.
    void watch (juce::Component& overlay)
    {
        overlays.emplace_back (&overlay);
        overlay.addComponentListener (this);
        update();
    }

    bool isCovered() const noexcept { return covered; }

private:
    void componentVisibilityChanged (juce::Component&) override { update(); }

    void componentBeingDeleted (juce::Component& component) override
    {
        component.removeComponentListener (this);
        // Still reachable through its SafePointer while it is being deleted,
        // so drop it by hand or it would keep the previews covered.
        overlays.erase (std::remove_if (overlays.begin(), overlays.end(),
                                        [&component] (const auto& overlay)
                                        { return overlay.getComponent() == &component; }),
                        overlays.end());
        update();
    }

    void update()
    {
        bool anyVisible = false;

        for (auto& overlay : overlays)
            if (auto* component = overlay.getComponent())
                anyVisible = anyVisible || component->isVisible();

        if (anyVisible == covered)
            return;

        covered = anyVisible;

        if (onCoverChanged)
            onCoverChanged (covered);
    }

    std::vector<juce::Component::SafePointer<juce::Component>> overlays;
    bool covered = false;

    JUCE_DECLARE_NON_COPYABLE (CameraPreviewCover)
};

} // namespace mma
