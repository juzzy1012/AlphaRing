#include "Splitscreen.h"

#include "mcc/settings/Settings.h"

#include "common.h"

#include "global/Global.h"

#include <offset_mcc.h>
#include <offset_halo1.h>

#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "../CGameManager.h"
#include "mcc/module/Module.h"

namespace MCC::Splitscreen {
    DefDetourFunction(__int64, __fastcall, get_index_by_xuid, void* a1, __int64 xuid) {
        auto p_setting = AlphaRing::Global::MCC::Splitscreen();

        if (!p_setting->b_override)
            return ppOriginal_get_index_by_xuid(a1, xuid);

        return CGameManager::get_index(xuid);
    }

    // todo:: let other players have the ability to pause the game

    bool Initialize() {
        bool result;

        // fix: changing team freeze the game
        result = AlphaRing::Hook::Detour({
            {0x38A09C/*0x2D01DC*/, 0x374164/*0x2BD620*/, get_index_by_xuid, (void**)&ppOriginal_get_index_by_xuid},
        });

        assertm(result, "MCC:Splitscreen: failed to hook");

        return true;
    }

    // Traces to %TEMP%\alpharing_h1sync.txt so a failing run can be diagnosed
    // from the file alone (the console isn't always visible/kept).
    static void SyncLog(const char* fmt, ...) {
        char buffer[256];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buffer, sizeof(buffer), fmt, args);
        va_end(args);

        LOG_INFO("[h1sync] {}", buffer);
        try {
            auto path = std::filesystem::temp_directory_path() / "alpharing_h1sync.txt";
            std::ofstream file(path, std::ios::app);
            file << GetTickCount64() << " " << buffer << "\n";
        } catch (...) {}
    }

    // Halo 1 spawns everyone the roster reports, but builds its splitscreen
    // window layout from an internal local-player counter — launching with 3/4
    // players gave 3/4 bipeds on 2 screens. This counter is what the old manual
    // "Halo1 page" InputInt wrote during map load; keep it synced automatically
    // instead. The value must be right while the map loads (views are built
    // then); changing it mid-mission needs a restart. A value of 0 just means
    // the game hasn't seeded it yet — pre-seeding during load is the point —
    // but a wild value means the offset doesn't hold the counter on this build,
    // so leave it alone.
    void SyncHalo1PlayerCount() {
        static __int64 s_base = 0;
        static int s_seen = -100;

        auto p_setting = AlphaRing::Global::MCC::Splitscreen();
        auto info = Module::GetSubModule(Module::MODULE_HALO1)->info();

        if (info.hModule == 0 || info.errorCode != 0) {
            if (s_base) {
                SyncLog("halo1.dll unloaded");
                s_base = 0;
                s_seen = -100;
            }
            return;
        }

        if (info.hModule != s_base) {
            s_base = info.hModule;
            s_seen = -100;
            SyncLog("halo1.dll loaded @ 0x%llx", (unsigned long long)s_base);
        }

        auto p_count = (__int16*)(info.hModule + OFFSET_HALO1_PV_PLAYER_COUNT);
        int current = *p_count;

        if (current != s_seen) {
            SyncLog("counter=%d override=%d players=%d",
                    current, (int)p_setting->b_override, p_setting->player_count);
            s_seen = current;
        }

        if (!p_setting->b_override)
            return;

        int count = p_setting->player_count;

        if (count < 1 || count > 4)
            return;

        if (current < 0 || current > 4 || current == count)
            return;

        *p_count = (__int16)count;
        s_seen = count;
        SyncLog("counter %d -> %d", current, count);
    }
}

#include "imgui.h"
#include "mcc/mcc.h"
#include "input/Input.h"

#include <string>

namespace MCC::Splitscreen {
    void RealContext();

    // Detect which controller (0-3) has any button/trigger pressed, returns -1 if none
    static int DetectActiveController() {
        for (int i = 0; i < 4; i++) {
            XINPUT_STATE state;
            if (AlphaRing::Input::GetXInputGetState(i, &state)) {
                if (state.Gamepad.wButtons != 0 ||
                    state.Gamepad.bLeftTrigger > 30 ||
                    state.Gamepad.bRightTrigger > 30) {
                    return i;
                }
            }
        }
        return -1;
    }

    // Binding state: which player slot is waiting for controller input (-1 = none)
    static int s_binding_player = -1;

    void ImGuiContext() {
        static bool show_splitscreen;

        if (ImGui::BeginMainMenuBar()) {
            ImGui::MenuItem("Splitscreen", nullptr, &show_splitscreen);
            ImGui::EndMainMenuBar();
        }

        if (show_splitscreen) {
            if (ImGui::Begin("Splitscreen", &show_splitscreen, ImGuiWindowFlags_MenuBar))
                RealContext();
            ImGui::End();
        }
    }

    void ProfileContext(int index) {
        char buffer[1024];
        auto p_setting = AlphaRing::Global::MCC::Splitscreen();
        auto p_profile = CGameManager::get_profile(index);
        const char* items[] = {"Controller 1", "Controller 2", "Controller 3", "Controller 4", "NONE"};

        ImGui::PushItemWidth(200);
        String::convert(buffer, p_profile->name, 1024);
        if (ImGui::InputText("Name", buffer, sizeof(buffer)))
            String::convert(p_profile->name, buffer, 1024);
        ImGui::PopItemWidth();

        ImGui::BeginDisabled(!index && p_setting->b_player0_use_km);

        // Check for controller binding completion
        if (s_binding_player == index) {
            int detected = DetectActiveController();
            if (detected >= 0) {
                p_profile->controller_index = detected;
                s_binding_player = -1;
            }
        }

        if (s_binding_player == index) {
            // Show binding prompt
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Press any button on controller...");
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                s_binding_player = -1;
            }
        } else {
            // Show dropdown and bind button
            ImGui::PushItemWidth(200);
            ImGui::Combo("Input", &p_profile->controller_index, items, IM_ARRAYSIZE(items));
            ImGui::PopItemWidth();
            ImGui::SameLine();
            sprintf(buffer, "Bind##ctrl%d", index);
            if (ImGui::Button(buffer) && s_binding_player < 0) {
                s_binding_player = index;
            }
        }

        ImGui::EndDisabled();

        if (ImGui::Button("Apply Profile")) {
            __int64 xuid;
            auto p_mng = GameManager();
            auto p_engine = GameEngine();
            if (MCC::IsInGame() && p_mng && (xuid = CGameManager::get_xuid(0))) {
                memcpy(&p_profile->profile, p_mng->ppOriginal.get_player_profile(p_mng, xuid), sizeof(CUserProfile));
                memcpy(&p_profile->mapping, p_mng->ppOriginal.retrive_gamepad_mapping(p_mng, xuid), sizeof(CGamepadMapping));
                if (p_engine)
                    p_engine->load_setting();
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Use this in game!!!");
        if (ImGui::Button("Save Profile")) {
            MCC::Settings::Profile::CaptureFromRuntime();
            MCC::Settings::Profile::Save();
        }

        bool is_disabled = (!index && !p_setting->b_override_profile) || (index && p_setting->b_use_player0_profile);

        if (ImGui::CollapsingHeader("Gamepad Mapping")) {
            ImGui::Indent();
            ImGui::BeginDisabled(is_disabled);
            p_profile->mapping.ImGuiContext();
            ImGui::EndDisabled();
            ImGui::Unindent();
        }

        if (ImGui::CollapsingHeader("Profile")) {
            ImGui::Indent();
            ImGui::BeginDisabled(is_disabled);
            p_profile->profile.ImGuiContext();
            ImGui::EndDisabled();
            ImGui::Unindent();
        }
    }

    void RealContext() {
        bool dirty = false;
        char buffer[10];
        auto p_setting = AlphaRing::Global::MCC::Splitscreen();

        if (ImGui::BeginMenuBar()) {
            // ImGui::MenuItem(p_setting->b_override ? "Disable" : "Enable", nullptr, &p_setting->b_override);
            dirty |= ImGui::MenuItem(
                p_setting->b_override ? "Disable" : "Enable",
                nullptr,
                &p_setting->b_override
            );
            if (ImGui::BeginMenu("Options")) {
                // ImGui::MenuItem("Use player1's profile", nullptr, &p_setting->b_use_player0_profile);
                dirty |= ImGui::MenuItem(
                    "Use player1's profile", 
                    nullptr, 
                    &p_setting->b_use_player0_profile
                );
                // ImGui::MenuItem("Enable K/M for player1", nullptr, &p_setting->b_player0_use_km);
                dirty |= ImGui::MenuItem(
                    "Enable K/M for player1", 
                    nullptr, 
                    &p_setting->b_player0_use_km
                );
                // ImGui::MenuItem("Override profile", nullptr, &p_setting->b_override_profile);
                dirty |= ImGui::MenuItem(
                    "Override profile", 
                    nullptr, 
                    &p_setting->b_override_profile
                );
                ImGui::EndMenu();
            }
#pragma region player count
            ImGui::PushItemWidth(200);
            // int count = p_setting->player_count;
            // if (ImGui::InputInt("Players", &count) && count >= 1 && count <=4) {
            //     p_setting->player_count = count;
            // }
            int count = p_setting->player_count;
            if (ImGui::InputInt("Players", &count) && count >= 1 && count <= 4) {
                p_setting->player_count = count;
                dirty = true;
            }
            ImGui::PopItemWidth();
            ImGui::EndMenuBar();
#pragma endregion
        }

        if (ImGui::BeginTabBar("Players")) {
            for (int i = 0; i < p_setting->player_count; ++i) {
                sprintf(buffer, "Player %d", i + 1);
                if (ImGui::BeginTabItem(buffer)) {
                    ProfileContext(i);
                    ImGui::EndTabItem();
                }
            }
            ImGui::EndTabBar();
        }

        if (dirty) {
            MCC::Settings::Splitscreen::CaptureFromRuntime();
            // MCC::Settings::Profile::CaptureFromRuntime();
            MCC::Settings::Splitscreen::Save();
            // MCC::Settings::Profile::Save();
        }
    }
}