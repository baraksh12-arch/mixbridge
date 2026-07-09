// MixBridgeApp.swift
// Entry point for the MixBridge iOS app.

import SwiftUI

@main
struct MixBridgeApp: App {

    @StateObject private var sessionManager = SessionManager()

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(sessionManager)
                .preferredColorScheme(.dark)
        }
    }
}

// MARK: - ContentView

struct ContentView: View {
    @EnvironmentObject var session: SessionManager

    var body: some View {
        NavigationStack {
            MainView()
                .navigationBarHidden(true)
        }
        .onAppear {
            session.startDiscovery()
        }
    }
}
