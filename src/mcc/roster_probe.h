#pragma once

// Roster investigation probe (master-chief base).
//
// Logs WHO calls the game-manager local-player enumeration detours, resolving the caller to
// module!+RVA, so we can determine whether MCC's native menu/roster queries these functions
// (and hand the caller addresses to a disassembler). Each detour passes _ReturnAddress()
// (captured as the very first statement) as `ret`. Writes to its own file
// (%TEMP%\alpharing_probe.log) and to the console logger. Always on. See
// docs/native-roster-investigation.md.
namespace roster_probe {
    void on_get_xbox_user_id(void* ret, int index);        // game asks for a local player's XUID/name (roster)
    void on_get_player_profile(void* ret, long long xid);  // game asks for a player's profile
    void on_retrive_gamepad_mapping(void* ret, long long xid);
    void on_get_key_state(void* ret, unsigned int index);  // per-frame input (deduped by call site)
}
