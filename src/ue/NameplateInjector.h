#pragma once

// Native local-coop nameplates + controller join, driven on the GAME thread.
//
// Hooks UObject::ProcessEvent (fires in every UI state) and, on a throttled
// tick: polls XInput, joins unassigned controllers on "A" (and drops on "B"),
// and reconciles a stack of real native WBP_Nameplate_C widgets under the
// primary player's nameplate — one per joined player, plus a "press A to join"
// slot for each connected-but-unjoined controller. No debug menu required.
//
// The render/present hook must NOT be used for this (ProcessEvent there recurses
// and stack-overflows the GPU driver).

namespace AlphaRing::UE::NameplateInjector {
    // Hook ProcessEvent + start the coop tick. Call once at init.
    void Install();
}
