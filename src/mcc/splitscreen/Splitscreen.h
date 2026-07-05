#pragma once

namespace MCC::Splitscreen {
    bool Initialize();
    void ImGuiContext();

    // keeps Halo 1's internal local-player counter in sync with the roster so
    // the splitscreen view layout matches the joined players; call periodically
    void SyncHalo1PlayerCount();
}