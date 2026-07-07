#pragma once

namespace MCC::Splitscreen {
    bool Initialize();
    void ImGuiContext();

    // keeps Halo 1's internal local-player counter in sync with the roster so
    // the splitscreen view layout matches the joined players; call periodically.
    // `source` tags the caller in the trace file; safe from multiple threads
    void SyncHalo1PlayerCount(const char* source);
}