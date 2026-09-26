import SwiftUI

@main
struct MadeiraApp: App {
    init() {
        // Opt-in: no-op unless --metal4-selftest or the default is set, so a
        // normal launch never enters it.
        Metal4Diagnostics.runIfRequested()
    }

    var body: some Scene {
        WindowGroup {
            ContentView()
        }
    }
}
