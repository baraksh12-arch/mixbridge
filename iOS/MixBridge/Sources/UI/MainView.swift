// MainView.swift
// Primary UI for MixBridge iOS app.
// Professional dark theme, real-time meters, connection management.

import SwiftUI

// MARK: - Colour Palette

private extension Color {
    static let mbBackground   = Color(red: 0.09, green: 0.09, blue: 0.12)
    static let mbSurface      = Color(red: 0.12, green: 0.12, blue: 0.16)
    static let mbSurface2     = Color(red: 0.16, green: 0.16, blue: 0.21)
    static let mbAccent       = Color(red: 0.04, green: 0.52, blue: 1.00)
    static let mbGreen        = Color(red: 0.19, green: 0.82, blue: 0.35)
    static let mbAmber        = Color(red: 1.00, green: 0.58, blue: 0.00)
    static let mbRed          = Color(red: 1.00, green: 0.23, blue: 0.19)
    static let mbTextPrimary  = Color(red: 0.93, green: 0.93, blue: 0.93)
    static let mbTextSecondary = Color(red: 0.53, green: 0.53, blue: 0.60)
    static let mbDivider      = Color(white: 0.20)
}

// MARK: - Main View

struct MainView: View {
    @EnvironmentObject var session: SessionManager
    @State private var showingSettings     = false
    @State private var showingSourcePicker = false

    var body: some View {
        ZStack {
            Color.mbBackground.ignoresSafeArea()

            ScrollView {
                VStack(spacing: 0) {
                    headerSection
                    connectionSection
                    metersSection
                    statsSection
                    sourcesSection
                    Spacer(minLength: 24)
                }
            }
        }
        .sheet(isPresented: $showingSettings) {
            SettingsView()
                .environmentObject(session)
        }
        .sheet(isPresented: $showingSourcePicker) {
            SourcePickerView()
                .environmentObject(session)
        }
    }

    // MARK: - Header

    private var headerSection: some View {
        HStack(alignment: .center) {
            VStack(alignment: .leading, spacing: 2) {
                Text("MIXBRIDGE")
                    .font(.system(size: 22, weight: .black, design: .default))
                    .foregroundColor(.mbTextPrimary)
                    .tracking(3)

                Text("Professional Audio Streaming")
                    .font(.system(size: 11, weight: .medium))
                    .foregroundColor(.mbTextSecondary)
            }

            Spacer()

            Button {
                showingSettings = true
            } label: {
                Image(systemName: "gearshape.fill")
                    .font(.system(size: 20))
                    .foregroundColor(.mbTextSecondary)
                    .frame(width: 44, height: 44)
            }
        }
        .padding(.horizontal, 20)
        .padding(.top, 16)
        .padding(.bottom, 12)
    }

    // MARK: - Connection Status

    private var connectionSection: some View {
        VStack(spacing: 0) {
            HStack(spacing: 14) {
                ConnectionDotView(state: session.sessionState)
                    .frame(width: 12, height: 12)

                VStack(alignment: .leading, spacing: 2) {
                    Text(session.sessionState.displayString)
                        .font(.system(size: 15, weight: .semibold))
                        .foregroundColor(connectionColor)

                    if session.sessionState.isStreaming {
                        Label {
                            Text("\(formattedSampleRate) • Lossless PCM")
                                .font(.system(size: 11))
                                .foregroundColor(.mbTextSecondary)
                        } icon: {
                            Image(systemName: "waveform")
                                .font(.system(size: 10))
                                .foregroundColor(.mbGreen)
                        }
                    }
                }

                Spacer()

                if session.sessionState.isStreaming {
                    Button("Disconnect") {
                        session.disconnect()
                    }
                    .font(.system(size: 12, weight: .medium))
                    .foregroundColor(.mbRed)
                    .padding(.horizontal, 12)
                    .padding(.vertical, 6)
                    .background(Color.mbRed.opacity(0.12))
                    .clipShape(Capsule())
                } else {
                    Button("Find Sources") {
                        showingSourcePicker = true
                    }
                    .font(.system(size: 12, weight: .medium))
                    .foregroundColor(.mbAccent)
                    .padding(.horizontal, 12)
                    .padding(.vertical, 6)
                    .background(Color.mbAccent.opacity(0.12))
                    .clipShape(Capsule())
                }
            }
            .padding(.horizontal, 20)
            .padding(.vertical, 16)
            .background(Color.mbSurface)
        }
        .padding(.bottom, 1)
    }

    // MARK: - Signal Meters

    private var metersSection: some View {
        VStack(spacing: 0) {
            SectionHeader(title: "SIGNAL")

            VStack(spacing: 12) {
                HStack(spacing: 10) {
                    MeterChannelView(
                        label: "L",
                        level: session.signalPeakL,
                        isActive: session.sessionState.isStreaming)

                    MeterChannelView(
                        label: "R",
                        level: session.signalPeakR,
                        isActive: session.sessionState.isStreaming)
                }
                .frame(height: 160)

                // dB reference scale
                HStack(spacing: 0) {
                    ForEach(["-∞", "-48", "-24", "-12", "-6", "-3", "0"], id: \.self) { label in
                        Text(label)
                            .font(.system(size: 8, weight: .medium, design: .monospaced))
                            .foregroundColor(.mbTextSecondary)
                            .frame(maxWidth: .infinity)
                    }
                }
            }
            .padding(.horizontal, 20)
            .padding(.vertical, 16)
            .background(Color.mbSurface)
        }
        .padding(.bottom, 1)
    }

    // MARK: - Network Stats

    private var statsSection: some View {
        VStack(spacing: 0) {
            SectionHeader(title: "NETWORK")

            LazyVGrid(columns: [
                GridItem(.flexible()),
                GridItem(.flexible()),
                GridItem(.flexible()),
                GridItem(.flexible())
            ], spacing: 1) {
                StatCell(
                    title: "LATENCY",
                    value: formattedLatency,
                    accent: latencyColor)

                StatCell(
                    title: "PACKET LOSS",
                    value: formattedLoss,
                    accent: lossColor)

                StatCell(
                    title: "JITTER",
                    value: formattedJitter,
                    accent: .mbTextPrimary)

                StatCell(
                    title: "QUALITY",
                    value: session.networkQuality.displayString,
                    accent: qualityColor)
            }

            HStack(spacing: 1) {
                StatCell(
                    title: "BUFFER",
                    value: formattedBuffer,
                    accent: .mbTextPrimary)

                StatCell(
                    title: "SAMPLE RATE",
                    value: formattedSampleRate,
                    accent: .mbTextPrimary)
            }
            .padding(.top, 1)
        }
        .background(Color.mbBackground)
        .padding(.bottom, 1)
    }

    // MARK: - Discovered Sources

    private var sourcesSection: some View {
        VStack(spacing: 0) {
            SectionHeader(title: "AVAILABLE SOURCES")

            if session.discoveredSources.isEmpty {
                HStack {
                    Image(systemName: "antenna.radiowaves.left.and.right")
                        .foregroundColor(.mbTextSecondary)
                    Text("Searching for MixBridge plugins…")
                        .font(.system(size: 13))
                        .foregroundColor(.mbTextSecondary)
                    Spacer()
                }
                .padding(.horizontal, 20)
                .padding(.vertical, 20)
                .background(Color.mbSurface)
            } else {
                VStack(spacing: 1) {
                    ForEach(session.discoveredSources) { source in
                        SourceRowView(source: source) {
                            session.connectToSource(source)
                        }
                    }
                }
            }
        }
        .padding(.bottom, 1)
    }

    // MARK: - Computed

    private var connectionColor: Color {
        switch session.sessionState {
        case .streaming:   return .mbGreen
        case .searching,
             .connecting,
             .configuring,
             .reconnecting: return .mbAmber
        case .idle:         return .mbTextSecondary
        }
    }

    private var formattedSampleRate: String {
        guard session.sampleRate > 0 else { return "—" }
        let khz = Double(session.sampleRate) / 1000.0
        return khz.truncatingRemainder(dividingBy: 1) == 0
            ? "\(Int(khz)) kHz"
            : String(format: "%.1f kHz", khz)
    }

    private var formattedLatency: String {
        guard session.latencyMs > 0 else { return "—" }
        return String(format: "%.1f ms", session.latencyMs)
    }

    private var formattedLoss: String {
        return String(format: "%.2f%%", session.packetLossPercent)
    }

    private var formattedJitter: String {
        guard session.jitterMs > 0 else { return "—" }
        return String(format: "%.1f ms", session.jitterMs)
    }

    private var formattedBuffer: String {
        guard session.jitterBufferMs > 0 else { return "—" }
        return String(format: "%.1f ms", session.jitterBufferMs)
    }

    private var latencyColor: Color {
        let ms = session.latencyMs
        if ms == 0 { return .mbTextPrimary }
        if ms < 5  { return .mbGreen }
        if ms < 15 { return .mbAmber }
        return .mbRed
    }

    private var lossColor: Color {
        let pct = session.packetLossPercent
        if pct == 0    { return .mbGreen }
        if pct < 0.5   { return .mbAmber }
        return .mbRed
    }

    private var qualityColor: Color {
        switch session.networkQuality {
        case .excellent, .good: return .mbGreen
        case .fair:              return .mbAmber
        case .poor:              return .mbRed
        case .unknown:           return .mbTextSecondary
        }
    }
}

// MARK: - Connection Dot

struct ConnectionDotView: View {
    let state: SessionState
    @State private var blinking = false

    private var isAnimating: Bool {
        switch state {
        case .idle, .streaming: return false
        default:                return true
        }
    }

    private var dotColor: Color {
        switch state {
        case .idle:        return .mbTextSecondary
        case .streaming:   return .mbGreen
        default:           return .mbAmber
        }
    }

    var body: some View {
        Circle()
            .fill(dotColor)
            .opacity(isAnimating && blinking ? 0.3 : 1.0)
            .overlay {
                if case .streaming = state {
                    Circle()
                        .stroke(Color.mbGreen.opacity(0.3), lineWidth: 4)
                        .scaleEffect(1.5)
                }
            }
            .onAppear {
                guard isAnimating else { return }
                withAnimation(.easeInOut(duration: 0.7).repeatForever()) {
                    blinking = true
                }
            }
            .onChange(of: isAnimating) { animate in
                if animate {
                    withAnimation(.easeInOut(duration: 0.7).repeatForever()) {
                        blinking = true
                    }
                } else {
                    blinking = false
                }
            }
    }
}

// MARK: - Meter Channel

struct MeterChannelView: View {
    let label: String
    let level: Float
    let isActive: Bool

    @State private var displayLevel: Float = 0

    private var dBLevel: Float {
        guard level > 0 else { return -96 }
        return 20 * log10(level)
    }

    private var normalizedLevel: CGFloat {
        let clamped = max(-60, min(0, dBLevel))
        return CGFloat((clamped + 60) / 60)
    }

    var body: some View {
        GeometryReader { geo in
            VStack(spacing: 4) {
                Text(label)
                    .font(.system(size: 10, weight: .bold, design: .monospaced))
                    .foregroundColor(.mbTextSecondary)

                ZStack(alignment: .bottom) {
                    // Track
                    RoundedRectangle(cornerRadius: 3)
                        .fill(Color.mbSurface2)

                    // Level bar
                    if isActive && normalizedLevel > 0 {
                        GeometryReader { barGeo in
                            let barH = barGeo.size.height * normalizedLevel
                            LinearGradient(
                                stops: [
                                    .init(color: .mbGreen, location: 0),
                                    .init(color: .mbGreen, location: 0.75),
                                    .init(color: .mbAmber, location: 0.85),
                                    .init(color: .mbRed,   location: 1.0),
                                ],
                                startPoint: .bottom,
                                endPoint: .top
                            )
                            .frame(height: barH)
                            .frame(maxHeight: .infinity, alignment: .bottom)
                            .clipShape(RoundedRectangle(cornerRadius: 3))
                        }
                    }
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                .animation(.linear(duration: 0.05), value: normalizedLevel)

                // dB value
                Text(isActive && level > 0.00001
                     ? String(format: "%.0f", dBLevel)
                     : "—")
                    .font(.system(size: 9, weight: .medium, design: .monospaced))
                    .foregroundColor(.mbTextSecondary)
            }
        }
    }
}

// MARK: - Section Header

struct SectionHeader: View {
    let title: String

    var body: some View {
        HStack {
            Text(title)
                .font(.system(size: 10, weight: .bold))
                .foregroundColor(.mbTextSecondary)
                .tracking(1.5)
            Spacer()
        }
        .padding(.horizontal, 20)
        .padding(.vertical, 8)
        .background(Color.mbBackground)
    }
}

// MARK: - Stat Cell

struct StatCell: View {
    let title: String
    let value: String
    let accent: Color

    var body: some View {
        VStack(spacing: 3) {
            Text(value)
                .font(.system(size: 16, weight: .bold, design: .monospaced))
                .foregroundColor(accent)
                .minimumScaleFactor(0.6)
                .lineLimit(1)

            Text(title)
                .font(.system(size: 9, weight: .medium))
                .foregroundColor(.mbTextSecondary)
                .tracking(0.8)
        }
        .frame(maxWidth: .infinity)
        .padding(.vertical, 14)
        .background(Color.mbSurface)
    }
}

// MARK: - Source Row

struct SourceRowView: View {
    let source: DiscoveredSource
    let onTap: () -> Void

    var body: some View {
        Button(action: onTap) {
            HStack(spacing: 14) {
                Image(systemName: "desktopcomputer")
                    .font(.system(size: 22))
                    .foregroundColor(.mbAccent)
                    .frame(width: 36)

                VStack(alignment: .leading, spacing: 2) {
                    Text(source.computerName)
                        .font(.system(size: 15, weight: .semibold))
                        .foregroundColor(.mbTextPrimary)

                    Text("\(source.address):\(source.port)")
                        .font(.system(size: 11, design: .monospaced))
                        .foregroundColor(.mbTextSecondary)
                }

                Spacer()

                Image(systemName: "chevron.right")
                    .font(.system(size: 13, weight: .semibold))
                    .foregroundColor(.mbTextSecondary)
            }
            .padding(.horizontal, 20)
            .padding(.vertical, 14)
            .background(Color.mbSurface)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
    }
}

// MARK: - Settings View

struct SettingsView: View {
    @EnvironmentObject var session: SessionManager
    @Environment(\.dismiss) var dismiss
    @State private var selectedLatencyMode: JitterBuffer.Mode = .auto

    var body: some View {
        NavigationStack {
            ZStack {
                Color.mbBackground.ignoresSafeArea()

                List {
                    Section {
                        ForEach([JitterBuffer.Mode.auto, .ultraLow, .low, .normal, .stable],
                                id: \.rawValue) { mode in
                            Button {
                                selectedLatencyMode = mode
                                session.setLatencyMode(mode)
                            } label: {
                                HStack {
                                    VStack(alignment: .leading, spacing: 2) {
                                        Text(mode.displayName)
                                            .foregroundColor(.mbTextPrimary)
                                        Text(latencyDescription(mode))
                                            .font(.caption)
                                            .foregroundColor(.mbTextSecondary)
                                    }
                                    Spacer()
                                    if selectedLatencyMode == mode {
                                        Image(systemName: "checkmark")
                                            .foregroundColor(.mbAccent)
                                    }
                                }
                            }
                            .listRowBackground(Color.mbSurface)
                        }
                    } header: {
                        Text("LATENCY MODE")
                            .foregroundColor(.mbTextSecondary)
                    }

                    Section {
                        Toggle("Auto-Connect", isOn: Binding(
                            get: { session.autoReconnect },
                            set: { session.setAutoReconnect($0) }))
                            .tint(.mbAccent)
                            .listRowBackground(Color.mbSurface)

                        HStack {
                            Text("Audio Quality")
                                .foregroundColor(.mbTextPrimary)
                            Spacer()
                            Text("Lossless PCM Float32")
                                .font(.system(size: 13))
                                .foregroundColor(.mbTextSecondary)
                        }
                        .listRowBackground(Color.mbSurface)
                    } header: {
                        Text("CONNECTION")
                            .foregroundColor(.mbTextSecondary)
                    }

                    Section {
                        HStack {
                            Text("Protocol Version")
                                .foregroundColor(.mbTextPrimary)
                            Spacer()
                            Text("MixBridge v1.0")
                                .foregroundColor(.mbTextSecondary)
                        }
                        .listRowBackground(Color.mbSurface)

                        HStack {
                            Text("Transport")
                                .foregroundColor(.mbTextPrimary)
                            Spacer()
                            Text("UDP (LAN)")
                                .foregroundColor(.mbTextSecondary)
                        }
                        .listRowBackground(Color.mbSurface)

                        HStack {
                            Text("Discovery")
                                .foregroundColor(.mbTextPrimary)
                            Spacer()
                            Text("Bonjour/mDNS")
                                .foregroundColor(.mbTextSecondary)
                        }
                        .listRowBackground(Color.mbSurface)
                    } header: {
                        Text("ABOUT")
                            .foregroundColor(.mbTextSecondary)
                    }
                }
                .scrollContentBackground(.hidden)
                .foregroundColor(.mbTextPrimary)
            }
            .navigationTitle("Settings")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .navigationBarTrailing) {
                    Button("Done") { dismiss() }
                        .foregroundColor(.mbAccent)
                }
            }
        }
        .preferredColorScheme(.dark)
    }

    private func latencyDescription(_ mode: JitterBuffer.Mode) -> String {
        switch mode {
        case .auto:     return "Adapts automatically to network conditions"
        case .ultraLow: return "~2ms — requires excellent Wi-Fi"
        case .low:      return "~5ms — for good local networks"
        case .normal:   return "~10ms — recommended for most setups"
        case .stable:   return "~20ms — tolerates poor network conditions"
        }
    }
}

// MARK: - Source Picker View

struct SourcePickerView: View {
    @EnvironmentObject var session: SessionManager
    @Environment(\.dismiss) var dismiss

    var body: some View {
        NavigationStack {
            ZStack {
                Color.mbBackground.ignoresSafeArea()

                Group {
                    if session.discoveredSources.isEmpty {
                        VStack(spacing: 20) {
                            ProgressView()
                                .tint(.mbAccent)
                            Text("Searching for MixBridge plugins…")
                                .foregroundColor(.mbTextSecondary)
                            Text("Make sure the plugin is inserted on your DAW's master bus.")
                                .font(.caption)
                                .foregroundColor(.mbTextSecondary)
                                .multilineTextAlignment(.center)
                                .padding(.horizontal, 40)
                        }
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                    } else {
                        List(session.discoveredSources) { source in
                            Button {
                                session.connectToSource(source)
                                dismiss()
                            } label: {
                                HStack(spacing: 14) {
                                    Image(systemName: "desktopcomputer")
                                        .font(.system(size: 24))
                                        .foregroundColor(.mbAccent)
                                        .frame(width: 40)

                                    VStack(alignment: .leading, spacing: 3) {
                                        Text(source.computerName)
                                            .font(.system(size: 16, weight: .semibold))
                                            .foregroundColor(.mbTextPrimary)
                                        Text(source.address)
                                            .font(.system(size: 12, design: .monospaced))
                                            .foregroundColor(.mbTextSecondary)
                                    }

                                    Spacer()

                                    Image(systemName: "arrow.right.circle.fill")
                                        .foregroundColor(.mbAccent)
                                }
                                .padding(.vertical, 6)
                            }
                            .listRowBackground(Color.mbSurface)
                        }
                        .scrollContentBackground(.hidden)
                    }
                }
            }
            .navigationTitle("Select Source")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .navigationBarTrailing) {
                    Button("Cancel") { dismiss() }
                        .foregroundColor(.mbAccent)
                }
            }
        }
        .preferredColorScheme(.dark)
    }
}
