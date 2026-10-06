#import <AppKit/AppKit.h>
#include "WindowOcclusion.h"

namespace mma {

bool isNativeWindowVisible (void* nativeView)
{
    if (nativeView == nullptr)
        return true;

    // JUCE's peer hands out its NSView. Borrowed, not retained: the peer owns it.
    NSView* view = (__bridge NSView*) nativeView;
    NSWindow* window = [view window];

    // Not in a window yet (or any more): nothing to judge, so do not stop
    // painting on the strength of it.
    if (window == nil)
        return true;

    // NSWindowOcclusionStateVisible is set while any part of the window is
    // on screen; it clears when the window is completely covered, on another
    // Space, minimised, or on a display that is asleep.
    return ([window occlusionState] & NSWindowOcclusionStateVisible) != 0;
}

} // namespace mma
