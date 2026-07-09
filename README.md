# MixBridge

**Professional real-time audio streaming from any DAW to iPhone.**  
PCM Float32 · Lossless · Zero-config · Sub-10ms latency on local network.

---

## Overview

MixBridge captures the stereo master output of any VST3-capable DAW and streams it losslessly over UDP to the iPhone app. Discovery is fully automatic via Bonjour/mDNS — no IP addresses, no pairing, no QR codes.

```
DAW Master Bus
      │
  [MixBridge VST3]
      │  PCM Float32 stereo
      │  Lock-free ring buffer
      │
  [Network Thread]
      │  UDP packets · sequence numbers · timestamps
      │  Wi-Fi 5/6 or Ethernet
      │
  [iPhone App]
      │  Jitter buffer · clock sync
      │
  [AVAudioEngine]
      │
  Headphones / Speaker
```

---

## Requirements

### Plugin (macOS / Windows)
- **JUCE** 7.x or later
- **CMake** 3.22+
- **macOS**: Xcode 14+, macOS 11.0+ target, Apple Silicon or Intel
- **Windows**: Visual Studio 2022, Windows 10+ SDK, Bonjour SDK (optional)
- Any VST3-compatible DAW (Ableton Live, Cubase, Reaper, Studio One, Logic via AU)

### iOS App
- **Xcode** 14+
- **iOS** 15.0+
- **Swift** 5.9+
- iPhone and Mac must be on the **same Wi-Fi network**

---

## Build — Plugin

```bash
# 1. Clone JUCE (if not already present)
git clone https://github.com/juce-framework/JUCE.git

# 2. Clone MixBridge
git clone https://github.com/yourorg/MixBridge.git

# 3. Build (macOS)
cd MixBridge
cmake -B build -DJUCE_DIR=../JUCE -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel

# 4. The plugin is automatically copied to:
#    /Library/Audio/Plug-Ins/VST3/MixBridge.vst3  (macOS)
#    C:\Program Files\Common Files\VST3\MixBridge.vst3  (Windows)
```

**Windows with Bonjour SDK:**
```bat
cmake -B build -DJUCE_DIR=../JUCE ^
  -DBONJOUR_INCLUDE_DIR="C:\Program Files\Bonjour SDK\Include" ^
  -DBONJOUR_LIB="C:\Program Files\Bonjour SDK\Lib\x64\dnssd.lib" ^
  -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

**Without Bonjour (Windows fallback):**  
Discovery falls back to UDP broadcast on port 51235. The iPhone and plugin must be on the same subnet.

**Windows (Visual Studio 2022, x64):**
```bat
cmake -B build -DJUCE_DIR=../JUCE -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target MixBridge_VST3 --parallel
```

---

## CI / Windows Artifact

GitHub Actions builds a Release x64 Windows VST3 automatically via [`.github/workflows/windows-vst3-build.yml`](.github/workflows/windows-vst3-build.yml).

### Trigger a build

1. Push this repository to GitHub.
2. The workflow runs on every **push** or **pull request** to `main` (or `master`).
3. To run manually: open **Actions** → **Windows VST3 Build** → **Run workflow**.

The CI runner checks out JUCE **8.0.12** and builds without the Bonjour SDK (UDP discovery fallback). This does not affect the VST3 binary or audio processing.

### Download the artifact

1. Open the completed workflow run on the **Actions** tab.
2. Under **Artifacts**, download **MixBridge-Windows-VST3-x64** (zip).
3. Extract the zip; you should see `MixBridge.vst3/` containing `Contents/x86_64-win/MixBridge.vst3`.

### Install on Windows

1. Copy the entire `MixBridge.vst3` folder to:
   `C:\Program Files\Common Files\VST3\`
2. Rescan plugins in your DAW:
   - **Cubase:** Studio Setup → VST Plug-in Manager → Update
   - **Ableton Live:** Preferences → Plug-Ins → Rescan
   - **FL Studio:** Options → Manage plugins → Find plugins / Rescan

Windows Firewall may prompt for UDP access on first launch (ports 51234–51235). Allow on private networks for iPhone discovery and streaming.

---

## Build — iOS App

1. Open `iOS/MixBridge/MixBridge.xcodeproj` in Xcode
2. Select your development team in **Signing & Capabilities**
3. Set deployment target to **iOS 15.0**
4. Build and run on device (simulator does not support local network audio)

**Required capabilities** (already configured in project):
- Background Modes → Audio, AirPlay, and Picture in Picture
- Local Network Access (prompted at runtime on first launch)

---

## Usage

1. **Install** the VST3 plugin on your macOS or Windows machine
2. **Install** the iOS app on your iPhone
3. **Open** your DAW and create a new project
4. **Insert** MixBridge on the **Master Bus** (or any stereo bus you want to monitor)
5. **Open** the MixBridge iOS app
6. **Connection is automatic** — the plugin and app discover each other via Bonjour/mDNS

The plugin UI shows:
- Green dot: streaming to iPhone
- Amber dot: searching/connecting
- Signal meters with dBFS levels
- Latency, packet loss, network quality

The iOS app shows:
- Full-screen signal meters (L/R)
- Latency, jitter, packet loss, network quality
- Jitter buffer depth
- Auto-reconnect status

---

## Architecture

### Audio Thread (Plugin)

The `processBlock()` method is called by the DAW on its real-time audio thread.  
**Rules strictly enforced:**
- No heap allocations
- No mutex locks
- No I/O operations
- No system calls
- No logging

```cpp
void processBlock(AudioBuffer<float>& buffer, MidiBuffer&) {
    // 1. Compute peak meters (trivial arithmetic)
    updatePeakMeters(buffer);

    // 2. Write to lock-free ring buffer (atomic stores only)
    ringBuffer_.write(left, right, numSamples);

    // Audio passes through unmodified
}
```

### Network Thread (Plugin)

A dedicated `std::thread` runs the `NetworkStreamer::runLoop()`:
- Reads from the lock-free ring buffer
- Packetizes audio into `AudioFrame` UDP packets
- Handles all protocol state (Hello/SessionConfig/Heartbeat/ClockSync)
- Non-blocking: uses 1ms `SO_RCVTIMEO` on receive socket

### Lock-Free Ring Buffer

```
Audio Thread                    Network Thread
    │                                │
    │ write(L[], R[], N)             │ read(interleaved[], N)
    │                                │
    ▼                                ▼
[writeIndex_] → buffer[] ← [readIndex_]
    │                                │
 atomic store                  atomic load
 (release)                     (acquire)
```

Capacity: 65,536 float pairs (~0.7 seconds at 48kHz stereo).  
Type: SPSC (single-producer, single-consumer) — safe without any locks.

### Protocol

All packets share a 16-byte header:

```
 0               1               2               3
 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                    Magic (0x4D585247 "MXRG")                  |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|   Version (1) |  Packet Type  |        Header CRC-16          |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                      Sequence Number                          |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                       Session ID                              |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

**Packet types:**

| Type | Value | Direction | Description |
|------|-------|-----------|-------------|
| Hello | 0x01 | Plugin→App | Announce presence |
| HelloAck | 0x02 | App→Plugin | Accept connection |
| Goodbye | 0x03 | Either | Graceful disconnect |
| Heartbeat | 0x04 | Either | Keep-alive ping |
| HeartbeatAck | 0x05 | Either | Keep-alive pong |
| SessionConfig | 0x06 | Plugin→App | Stream parameters |
| SessionAck | 0x07 | App→Plugin | Ready to receive |
| LatencyMode | 0x08 | App→Plugin | Request mode change |
| AudioFrame | 0x10 | Plugin→App | PCM audio payload |
| ClockSync | 0x20 | Either | NTP t1 timestamp |
| ClockSyncAck | 0x21 | Either | NTP t1+t2+t3 |
| StatsRequest | 0x30 | App→Plugin | Request statistics |
| StatsResponse | 0x31 | Plugin→App | Current statistics |

### Connection Sequence

```
Plugin                              iPhone App
  │                                      │
  │◄──── [mDNS browse: _mixbridge._udp] ─┤ (NWBrowser)
  │                                      │
  ├──── Hello ──────────────────────────►│
  │                                      │
  │◄─── HelloAck ───────────────────────┤
  │                                      │
  ├──── SessionConfig ──────────────────►│ (sampleRate, blockSize, framesPerPacket)
  │                                      │
  │◄─── SessionAck ─────────────────────┤ (jitterBufferMs)
  │                                      │
  ├──── AudioFrame (stream) ────────────►│
  ├──── AudioFrame ─────────────────────►│
  ├──── AudioFrame ─────────────────────►│ (continuous)
  │                                      │
  ├──── Heartbeat ──────────────────────►│ (every 500ms)
  │◄─── HeartbeatAck ───────────────────┤
  │                                      │
  ├──── ClockSync ──────────────────────►│ (every 2s)
  │◄─── ClockSyncAck ───────────────────┤
```

### Clock Synchronization (NTP-style)

```
Plugin sends:   ClockSync(t1)
App receives at t2, sends: ClockSyncAck(t1, t2, t3)
Plugin receives at t4:
  RTT    = (t4-t1) - (t3-t2)
  Offset = ((t2-t1) + (t3-t4)) / 2
```

Used for: accurately timestamping audio frames relative to the receiver's clock, enabling precise jitter buffer sizing.

### Jitter Buffer (iOS)

The adaptive jitter buffer:
1. Accepts packets indexed by sequence number (handles reordering)
2. Maintains a sorted map `[seqNum → BufferedPacket]`
3. `pull()` called from AVAudioEngine render callback:
   - If `seqNum` exists: copy samples, advance cursor
   - If `seqNum` is missing: insert silence for that block
4. Target depth auto-adapts based on measured inter-arrival jitter (3σ rule)

### Reconnection Logic

```
Connection lost (no packet for 3 seconds)
      │
      ▼
State: Reconnecting
      │
      ▼
Wait 2 seconds
      │
      ▼
Send Hello to last known endpoint
      │
      ├─── Got HelloAck ──► Streaming (session restored)
      │
      └─── Timeout ──► Try again (indefinite retry)
```

Survives: DAW stop/play, buffer size changes, sample rate changes, plugin bypass, iPhone lock, app minimize, computer sleep, router reboot, Wi-Fi interruption.

---

## Audio Quality

- **Format**: PCM Float32, stereo, interleaved (L,R,L,R,…)
- **Sample rates**: 44.1 / 48 / 88.2 / 96 / 176.4 / 192 kHz (auto-detected)
- **Bit depth**: 32-bit float (equivalent to ~144dB dynamic range)
- **Compression**: None. Bit-perfect transmission.
- **Payload CRC32**: Each AudioFrame has a CRC32 over its payload. Corrupted frames are silently dropped and the jitter buffer inserts silence.

---

## Latency Modes

| Mode | Target Buffer | Use Case |
|------|--------------|----------|
| Auto | Adaptive (2–20ms) | Recommended — adjusts to network |
| Ultra Low | ~2ms | Excellent Wi-Fi 6, wired setup |
| Low | ~5ms | Good home network |
| Normal | ~10ms | Standard home/studio Wi-Fi |
| Stable | ~20ms | Noisy or congested networks |

Total end-to-end latency = DAW buffer + network RTT/2 + jitter buffer depth + iOS audio output latency.

---

## Network Requirements

- **Protocol**: UDP (no TCP, no WebSocket, no HTTP)
- **Port**: 51234 (configurable, falls back to any available port)
- **Bandwidth**: ~3 Mbps at 48kHz stereo Float32 (192kHz = ~12 Mbps)
- **Network**: Same LAN (Wi-Fi or Ethernet). Internet not required or used.
- **Wi-Fi**: 802.11n (Wi-Fi 4) minimum, Wi-Fi 5/6 recommended for Ultra Low latency

---

## File Structure

```
MixBridge/
├── CMakeLists.txt                  # Plugin build system
│
├── Plugin/
│   └── Source/
│       ├── Core/
│       │   ├── MixBridgeProcessor.h/cpp  # VST3 AudioProcessor
│       │   └── PluginEditor (via UI/)
│       ├── UI/
│       │   ├── MixBridgeEditor.h/cpp     # Plugin window
│       │   └── components (SignalMeter, StatusDot, etc.)
│       ├── Network/
│       │   ├── NetworkStreamer.h          # Network thread + state machine
│       │   └── UDPSocket.h               # Cross-platform UDP
│       └── Discovery/
│           └── BonjourDiscovery.h         # mDNS registration + browse
│
├── Shared/
│   ├── Protocol/
│   │   └── MixBridgeProtocol.h    # All packet formats, constants
│   ├── RingBuffer/
│   │   └── LockFreeRingBuffer.h   # SPSC ring buffer + AudioRingBuffer
│   └── Common/
│       ├── HighResClock.h         # Nanosecond monotonic clock
│       └── NetworkStats.h         # Packet loss, jitter, quality
│
├── iOS/
│   └── MixBridge/
│       └── Sources/
│           ├── Audio/
│           │   ├── AudioEngine.swift     # AVAudioEngine management
│           │   └── JitterBuffer.swift    # Adaptive jitter buffer
│           ├── Network/
│           │   ├── UDPReceiver.swift     # POSIX UDP receive thread
│           │   └── SessionManager.swift  # Protocol state machine
│           ├── Discovery/
│           │   └── BonjourBrowser.swift  # NWBrowser + UDP fallback
│           └── UI/
│               ├── MixBridgeApp.swift    # App entry point
│               └── MainView.swift        # Full SwiftUI UI
│
└── Tests/
    └── Shared/
        └── MixBridgeTests.cpp     # Unit tests (JUCE UnitTest)
```

---

## License

Copyright © 2024 MixBridge Audio. All rights reserved.  
Commercial product — see LICENSE file for terms.
# mixbridge
