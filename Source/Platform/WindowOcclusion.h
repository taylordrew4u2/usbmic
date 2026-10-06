#pragma once

namespace mma {

/// Whether any part of the native window holding this view can be seen: not
/// fully covered by other windows, not on another Space, not on a sleeping
/// display. Used only to skip repainting what nobody can see -- never to
/// decide anything about audio, recording or alarms.
///
/// nativeView is ComponentPeer::getNativeHandle(): an NSView* on macOS.
/// macOS asks [NSWindow occlusionState]; everywhere else there is no cheap,
/// reliable answer, so the window is reported visible (minimising is checked
/// separately, through the peer, on every platform). A null handle is visible.
#if defined (__APPLE__)
bool isNativeWindowVisible (void* nativeView);
#else
inline bool isNativeWindowVisible (void*) { return true; }
#endif

} // namespace mma
