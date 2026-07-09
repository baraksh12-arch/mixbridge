// MixBridgeIOS.xcodeproj configuration notes
// The iOS app is a standard SwiftUI app project.
// This file documents the required Xcode settings.

// ============================================================
// Target Settings Required:
// ============================================================
//
// Bundle Identifier:    audio.mixbridge.iOSApp
// Deployment Target:    iOS 15.0
// Swift Version:        5.9
// Device Family:        iPhone, iPad
//
// ============================================================
// Capabilities Required:
// ============================================================
//
// 1. Background Modes:
//    ✓ Audio, AirPlay, and Picture in Picture
//    (Enables audio playback when app is backgrounded)
//
// 2. Network Extensions: NOT required
//    (We use standard POSIX sockets, no entitlement needed)
//
// 3. Bonjour Services (Info.plist):
//    NSBonjourServices = ["_mixbridge._udp."]
//    (Required for NWBrowser to find mDNS services)
//
// ============================================================
// Info.plist Keys:
// ============================================================
//
// NSLocalNetworkUsageDescription:
//   "MixBridge needs local network access to stream audio
//    from your DAW to this device."
//
// NSBonjourServices:
//   _mixbridge._udp.
//
// UIBackgroundModes:
//   audio
//
// ============================================================
// Build Settings:
// ============================================================
//
// SWIFT_VERSION = 5.9
// IPHONEOS_DEPLOYMENT_TARGET = 15.0
// ENABLE_BITCODE = NO
// ALWAYS_EMBED_SWIFT_STANDARD_LIBRARIES = YES
// GCC_OPTIMIZATION_LEVEL = s      (Release)
// SWIFT_OPTIMIZATION_LEVEL = -O   (Release)
//
// ============================================================
// Linked Frameworks (automatic with SwiftUI template):
// ============================================================
//
// AVFoundation.framework
// Network.framework
// Foundation.framework
// SwiftUI.framework
// UIKit.framework
