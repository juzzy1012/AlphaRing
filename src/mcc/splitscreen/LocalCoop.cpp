#include "LocalCoop.h"

#include "common.h"

#include "global/Global.h"
#include "mcc/mcc.h"
#include "mcc/settings/Settings.h"
#include "input/Input.h"

#include "../CGameManager.h"
#include "ue/RosterProbe.h"
#include "ue/NameplateInjector.h"

#include <imgui.h>

#include <cstdio>

namespace MCC::LocalCoop {
    // ---- MCC-inspired theme -------------------------------------------------
    namespace Theme {
        const ImVec4 kBackground   = ImVec4(0.04f, 0.06f, 0.09f, 0.96f);
        const ImVec4 kCardEmpty    = ImVec4(0.09f, 0.12f, 0.16f, 0.90f);
        const ImVec4 kCardJoined   = ImVec4(0.10f, 0.16f, 0.22f, 0.95f);
        const ImVec4 kAccent       = ImVec4(0.20f, 0.62f, 0.92f, 1.00f); // steel blue
        const ImVec4 kAccentGold   = ImVec4(0.96f, 0.76f, 0.22f, 1.00f); // primary/active
        const ImVec4 kText         = ImVec4(0.92f, 0.95f, 0.98f, 1.00f);
        const ImVec4 kTextDim      = ImVec4(0.55f, 0.62f, 0.70f, 1.00f);
        const ImVec4 kError        = ImVec4(0.95f, 0.40f, 0.35f, 1.00f);
    }

    static bool s_show = false;

    // Rising-edge tracking per XInput user index (0-3).
    static bool s_prev_a[4] = {};
    static bool s_prev_b[4] = {};

    bool& Visible() { return s_show; }

    // Is controller `c` already owned by an active player slot?
    static bool ControllerAssigned(int c, int count, bool p0_km) {
        for (int slot = 0; slot < count; ++slot) {
            if (slot == 0 && p0_km)
                continue;
            auto profile = CGameManager::get_profile(slot);
            if (profile && profile->controller_index == c)
                return true;
        }
        return false;
    }

    // Which slot owns controller `c` (-1 = none / KM).
    static int SlotForController(int c, int count, bool p0_km) {
        for (int slot = 0; slot < count; ++slot) {
            if (slot == 0 && p0_km)
                continue;
            auto profile = CGameManager::get_profile(slot);
            if (profile && profile->controller_index == c)
                return slot;
        }
        return -1;
    }

    static void Persist() {
        MCC::Settings::Splitscreen::CaptureFromRuntime();
        MCC::Settings::Splitscreen::Save();
    }

    // Poll controllers and apply join / leave intent to the backend.
    // Returns true if any setting changed.
    static bool ProcessControllers() {
        auto ss = AlphaRing::Global::MCC::Splitscreen();
        bool dirty = false;

        for (int c = 0; c < 4; ++c) {
            XINPUT_STATE state{};
            bool connected = AlphaRing::Input::GetXInputGetState(c, &state);

            bool a_down = connected && (state.Gamepad.wButtons & XINPUT_GAMEPAD_A);
            bool b_down = connected && (state.Gamepad.wButtons & XINPUT_GAMEPAD_B);

            bool a_pressed = a_down && !s_prev_a[c];
            bool b_pressed = b_down && !s_prev_b[c];

            s_prev_a[c] = a_down;
            s_prev_b[c] = b_down;

            if (!connected)
                continue;

            int count = ss->player_count;

            // JOIN: an unassigned controller presses A -> take the next slot.
            if (a_pressed && count < 4 && !ControllerAssigned(c, count, ss->b_player0_use_km)) {
                int slot = count;
                auto profile = CGameManager::get_profile(slot);
                if (profile) {
                    profile->controller_index = c;
                    ss->player_count = count + 1;
                    ss->b_override = true;
                    dirty = true;
                    LOG_INFO("LocalCoop: controller {} joined as Player {}", c, slot + 1);
                }
            }
            // LEAVE (LIFO): the controller owning the last slot presses B.
            else if (b_pressed && count > 1) {
                int slot = SlotForController(c, count, ss->b_player0_use_km);
                if (slot == count - 1) {
                    ss->player_count = count - 1;
                    if (ss->player_count <= 1)
                        ss->b_override = false;
                    dirty = true;
                    LOG_INFO("LocalCoop: controller {} (Player {}) left", c, slot + 1);
                }
            }
        }

        return dirty;
    }

    // ---- rendering ----------------------------------------------------------
    static void DrawCard(int slot, int count, bool p0_km) {
        char id[32];
        sprintf(id, "##coop_card_%d", slot);

        const bool joined = slot < count;
        const bool primary = slot == 0;

        ImGui::PushStyleColor(ImGuiCol_ChildBg, joined ? Theme::kCardJoined : Theme::kCardEmpty);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 12));
        ImGui::BeginChild(id, ImVec2(168, 188), true);

        // accent bar along the top
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImVec4 accent = !joined ? Theme::kTextDim : (primary ? Theme::kAccentGold : Theme::kAccent);
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2(p0.x - 6, p0.y - 6),
            ImVec2(p0.x + 156, p0.y - 2),
            ImGui::ColorConvertFloat4ToU32(accent), 2.0f);

        ImGui::Dummy(ImVec2(0, 4));

        ImGui::PushStyleColor(ImGuiCol_Text, joined ? Theme::kText : Theme::kTextDim);
        ImGui::SetWindowFontScale(1.25f);
        ImGui::Text("PLAYER %d", slot + 1);
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();

        ImGui::Dummy(ImVec2(0, 6));

        if (!joined) {
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::kAccent);
            ImGui::TextWrapped("Press A to Join");
            ImGui::PopStyleColor();
            ImGui::Dummy(ImVec2(0, 6));
            ImGui::TextColored(Theme::kTextDim, "Empty slot");
        } else {
            // Device line.
            char device[32];
            if (primary && p0_km) {
                snprintf(device, sizeof(device), "Keyboard / Mouse");
            } else {
                auto profile = CGameManager::get_profile(slot);
                int ci = profile ? profile->controller_index : -1;
                if (ci >= 0 && ci < 4)
                    snprintf(device, sizeof(device), "Controller %d", ci + 1);
                else
                    snprintf(device, sizeof(device), "Unassigned");
            }

            ImGui::TextColored(primary ? Theme::kAccentGold : Theme::kAccent, "%s", device);

            // Name / service tag from the profile.
            auto profile = CGameManager::get_profile(slot);
            if (profile) {
                char name[256];
                String::convert(name, profile->name, sizeof(name));
                ImGui::Dummy(ImVec2(0, 4));
                ImGui::TextColored(Theme::kTextDim, "%s", name);
            }

            ImGui::Dummy(ImVec2(0, 8));
            ImGui::TextColored(Theme::kTextDim, primary ? "Primary" : "Ready");

            // Leave hint only on the last-joined, non-primary slot (LIFO).
            if (!primary && slot == count - 1) {
                ImGui::Dummy(ImVec2(0, 6));
                ImGui::TextColored(Theme::kTextDim, "Press B to Leave");
            }
        }

        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
    }

    static void RenderScreen() {
        auto ss = AlphaRing::Global::MCC::Splitscreen();
        bool dirty = false;

        // Controller join/leave + native nameplates are driven globally on the
        // game thread by NameplateInjector (works without this menu open). This
        // screen is display + mouse config only.

        ImVec2 ds = ImGui::GetIO().DisplaySize;
        ImGui::SetNextWindowSize(ImVec2(760, 420), ImGuiCond_Appearing);
        ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

        ImGui::PushStyleColor(ImGuiCol_WindowBg, Theme::kBackground);
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::kText);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);

        if (ImGui::Begin("Local Co-op", &s_show, ImGuiWindowFlags_NoCollapse)) {
            ImGui::SetWindowFontScale(1.4f);
            ImGui::TextColored(Theme::kText, "LOCAL CO-OP");
            ImGui::SetWindowFontScale(1.0f);
            ImGui::SameLine();
            ImGui::TextColored(Theme::kTextDim, "   %d / 4 players", ss->player_count);

            if (!MCC::IsInGame())
                ImGui::TextColored(Theme::kTextDim, "Load a campaign or firefight map, then have players press A.");
            ImGui::Separator();
            ImGui::Dummy(ImVec2(0, 4));

            // Player 1 device toggle.
            bool km = ss->b_player0_use_km;
            if (ImGui::Checkbox("Player 1 uses Keyboard / Mouse", &km)) {
                ss->b_player0_use_km = km;
                dirty = true;
            }

            ImGui::Dummy(ImVec2(0, 6));

            // Roster: four cards in a row.
            for (int slot = 0; slot < 4; ++slot) {
                DrawCard(slot, ss->player_count, ss->b_player0_use_km);
                if (slot < 3)
                    ImGui::SameLine();
            }

            ImGui::Dummy(ImVec2(0, 10));
            ImGui::Separator();

            // Footer controller hints.
            ImGui::TextColored(Theme::kAccentGold, "(A)");
            ImGui::SameLine(); ImGui::TextColored(Theme::kText, "Join");
            ImGui::SameLine(0, 24);
            ImGui::TextColored(Theme::kAccentGold, "(B)");
            ImGui::SameLine(); ImGui::TextColored(Theme::kText, "Leave (last player)");

            // Diagnostics.
            bool any_controller = false;
            for (int c = 0; c < 4; ++c) {
                XINPUT_STATE st{};
                if (AlphaRing::Input::GetXInputGetState(c, &st)) { any_controller = true; break; }
            }
            if (!any_controller) {
                ImGui::Dummy(ImVec2(0, 6));
                ImGui::TextColored(Theme::kError, "No controllers detected. Connect a controller to join.");
            }

            // --- Native-roster investigation probe (Track B) ----------------
            ImGui::Dummy(ImVec2(0, 8));
            if (ImGui::CollapsingHeader("Native roster probe (dev)")) {
                if (ImGui::Button("Dump native roster -> log + file")) {
                    auto path = AlphaRing::UE::RosterProbe::Dump();
                    LOG_INFO("LocalCoop: roster probe written to {}", path.c_str());
                }
                ImGui::SameLine();
                ImGui::TextColored(Theme::kTextDim, "%%TEMP%%\\alpharing_roster_probe.txt");

                const auto& report = AlphaRing::UE::RosterProbe::LastReport();
                if (!report.empty()) {
                    ImGui::BeginChild("##probe_out", ImVec2(0, 180), true);
                    for (const auto& line : report)
                        ImGui::TextUnformatted(line.c_str());
                    ImGui::EndChild();
                }
            }
        }
        ImGui::End();

        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);

        if (dirty)
            Persist();
    }

    void ImGuiContext() {
        if (ImGui::BeginMainMenuBar()) {
            ImGui::MenuItem("Local Co-op", nullptr, &s_show);
            ImGui::EndMainMenuBar();
        }

        if (s_show)
            RenderScreen();
    }
}
