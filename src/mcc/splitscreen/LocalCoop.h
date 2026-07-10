#pragma once

// Native-looking "Local Co-op" join screen.
//
// This is the user-facing frontend for AlphaRing's splitscreen. It drives the
// EXISTING, proven backend (Global::MCC::Splitscreen() player_count + each
// player's CGameManager profile controller_index) — MCC's own UE shell then
// materialises the splitscreen viewports. We do NOT spawn players ourselves.
//
// Design goal (see docs/2026-05-29_*native-local-player-ui.md): a controller-
// first, MCC-styled roster — "Press A to Join" — rather than a debug window.

namespace MCC::LocalCoop {
    // Menu entry + the screen itself. Called from CMCCContext::render()
    // (render thread). Only mutates settings — never calls game/UE functions.
    void ImGuiContext();

    // Toggle for the screen's visibility, so other menus can open it.
    bool& Visible();
}
