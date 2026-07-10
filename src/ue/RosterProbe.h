#pragma once

#include <string>
#include <vector>

// Read-only investigation probe for the native MCC roster (Track B).
// Answers: is MCCGameSessionViewModel.Players fed by local ULocalPlayers, or by
// a separate session model? Dumps findings to a file + keeps the last report
// for on-screen display. Pure reflection — safe on any thread.

namespace AlphaRing::UE::RosterProbe {
    // Run a probe pass; appends a timestamped report to the probe log file and
    // refreshes LastReport(). Returns the path written (or empty on failure).
    std::string Dump();

    // Call every frame (render thread). Auto-runs Dump() on a time interval for
    // a bounded number of passes — lets us capture the live roster unattended.
    void AutoTick();

    const std::vector<std::string>& LastReport();
}
