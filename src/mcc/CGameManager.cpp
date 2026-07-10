#include "CGameManager.h"

#include "common.h"

#include <cstdio>
#include <guiddef.h>
#include <combaseapi.h>

static struct ProfileContainer_t {CGameManager::Profile_t profiles[4]; ProfileContainer_t();} container;

CGameManager::Profile_t* CGameManager::get_profile(int index) {
    // Bounds check to prevent out-of-bounds access
    if (index < 0 || index >= 4)
        return nullptr;
    return container.profiles + index;
}

// Initialize default Xbox controller mapping for standard Halo controls
static void InitializeDefaultMapping(CGamepadMapping& mapping) {
    // Set all to None (unbound) first
    for (int i = 0; i < 66; i++) {
        mapping.actions[i] = CGamepadMapping::None;
    }

    // Standard Xbox Halo controls - only bind the essential actions
    mapping.actions[0]  = CGamepadMapping::A;             // Jump
    mapping.actions[1]  = CGamepadMapping::LeftShoulder;  // Switch Grenades
    mapping.actions[2]  = CGamepadMapping::X;             // Action/Interact
    mapping.actions[3]  = CGamepadMapping::RightShoulder; // Reload Right Weapon
    mapping.actions[4]  = CGamepadMapping::Y;             // Change Weapon
    mapping.actions[5]  = CGamepadMapping::B;             // Melee
    mapping.actions[6]  = CGamepadMapping::DpadUp;        // Toggle Flashlight
    mapping.actions[7]  = CGamepadMapping::LeftTrigger;   // Throw Grenade
    mapping.actions[8]  = CGamepadMapping::RightTrigger;  // Use Right Weapon (Shoot)
    mapping.actions[9]  = CGamepadMapping::LeftThumb;     // Crouch
    mapping.actions[10] = CGamepadMapping::RightThumb;    // Player Zoom
    mapping.actions[20] = CGamepadMapping::Back;          // Multiplayer Scoreboard
}

ProfileContainer_t::ProfileContainer_t() {
    __int64 guid[2];
    const int controller_map[4] {0, 1, 2, 3};
    memset(this, 0, sizeof(ProfileContainer_t));

    CoCreateGuid((GUID*)guid);
    auto id = guid[0] ^ guid[1];

    for (int i = 0; i < 4; i++) {
        profiles[i].controller_index = controller_map[i];
        profiles[i].id = id + i;
        swprintf(profiles[i].name, L"Player %d", i + 1);

        // Initialize with standard Xbox Halo controls
        InitializeDefaultMapping(profiles[i].mapping);
    }
}

CGameManager* pGameManager;
CGameManager::FunctionTable CGameManager::ppOriginal;

bool CGameManager::Initialize(CGameManager* mng) {
    pGameManager = mng;
    return AlphaRing::Hook::Detour({
        {pGameManager->table->get_player_profile, get_player_profile, (void**)&ppOriginal.get_player_profile},
        {pGameManager->table->get_key_state, get_key_state, (void**)&ppOriginal.get_key_state},
        {pGameManager->table->get_xbox_user_id, get_xbox_user_id, (void**)&ppOriginal.get_xbox_user_id},
        {pGameManager->table->set_vibration, set_vibration, (void**)&ppOriginal.set_vibration},
        {pGameManager->table->retrive_gamepad_mapping, retrive_gamepad_mapping, (void**)&ppOriginal.retrive_gamepad_mapping},
        {pGameManager->table->set_state, set_state, (void**)&ppOriginal.set_state},
        {pGameManager->table->game_restart, game_restart, (void**)&ppOriginal.game_restart},
        {pGameManager->table->game_setup, game_setup, (void**)&ppOriginal.game_setup},
    });
}

__int64 CGameManager::get_xuid(int index) {
    __int64 result;

    // Bounds check
    if (index < 0 || index >= 4)
        return 0;

    if (index)
        return container.profiles[index].id;
    else
        return pGameManager->ppOriginal.get_xbox_user_id(pGameManager, &result, nullptr, 0, index) ? result : 0;
}

bool CGameManager::get_name(int index, wchar_t* out, int count) {
    if (!out || count <= 0 || index < 0 || index >= 4 || pGameManager == nullptr ||
        pGameManager->ppOriginal.get_xbox_user_id == nullptr)
        return false;
    out[0] = 0;
    __int64 id = 0;
    // `count` is the buffer length in wchars. We pass it straight through as the
    // size arg: whether the original treats it as a char count or a byte count,
    // the most it can write (count wchars == count*2 bytes) still fits a
    // wchar_t[count] buffer. SEH guards against any internal fault when called
    // outside a normal in-game context (e.g. at the menu).
    __try {
        if (!pGameManager->ppOriginal.get_xbox_user_id(pGameManager, &id, out, count, index))
            return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = 0;
        return false;
    }
    out[count - 1] = 0;
    return out[0] != 0;
}

// A player slot is KBM if it's slot 0 with the legacy keyboard flag, OR any
// slot whose profile carries the controller_index==4 sentinel (4 indexes the
// keyboard device in CDeviceManager::p_input_device). This generalizes "the
// keyboard belongs to player 0" to "the keyboard belongs to whichever slot
// claimed it", so P1 can be on a gamepad while a later player joins on KBM.
bool CGameManager::SlotUsesKbm(int index) {
    auto setting = AlphaRing::Global::MCC::Splitscreen();
    if (setting == nullptr)
        return false;
    if (index == 0 && setting->b_player0_use_km)
        return true;
    auto profile = get_profile(index);
    return profile != nullptr && profile->controller_index == 4;
}

CInputDevice *CGameManager::get_controller(int index) {
    auto mng = DeviceManager();
    auto setting = AlphaRing::Global::MCC::Splitscreen();
    auto profile = get_profile(index);

    // Null checks to prevent crashes
    if (mng == nullptr || setting == nullptr || profile == nullptr)
        return nullptr;

    auto controller_index = profile->controller_index;

    // KBM slots have no XInput device (routed via the KBM branch in
    // get_key_state); an out-of-range controller_index means "unassigned".
    if (SlotUsesKbm(index) || controller_index >= 4 || controller_index < 0)
        return nullptr;

    return mng->p_input_device[controller_index];
}

int CGameManager::get_index(__int64 xuid) {
    for (int i = 1; i < 4; ++i)
        if (container.profiles[i].id == xuid)
            return i;
    return 0;
}

void CGameManager::set_state(CGameManager *self, eState state) {
    auto state_name = "Unknown";
    if (state == Exiting)
        state_name = "Exiting";
    LOG_INFO("Set Game State[{}]: {}", state, state_name);
    return ppOriginal.set_state(self, state);
}

void *CGameManager::game_restart(CGameManager *self, int type, const char *reason) {
    auto final_reason = reason ? reason : "NoReason";
    LOG_INFO("Game Restart[{}]: {}", type, final_reason);
    return ppOriginal.game_restart(self, type, reason);
}

char __fastcall CGameManager::game_setup(CGameManager* self, void* a2) {
//    LOG_INFO("game setup"); player init/add
    return ppOriginal.game_setup(self, a2);
}
