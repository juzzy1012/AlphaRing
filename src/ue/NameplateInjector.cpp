#include "NameplateInjector.h"

#include "UeObject.h"

#include "common.h"
#include "mcc/mcc.h"
#include "mcc/CGameManager.h"
#include "hook/Hook.h"
#include "global/Global.h"
#include "input/Input.h"

#include <Windows.h>
#include <atomic>
#include <filesystem>
#include <fstream>

namespace AlphaRing::UE::NameplateInjector {
    namespace {
        // Layouts (Dumper-7 SDK, MCC 1.3528 Steam):
        constexpr uintptr_t kGameInstance_LocalPlayers = 0x0038;
        constexpr uintptr_t kUPlayer_PlayerController  = 0x0030;
        constexpr uintptr_t kSDUserWidget_ViewModel    = 0x02D8;
        constexpr uintptr_t kNPOVM_OwnPlayerViewModel  = 0x01C8; // MCCNameplateOverlayViewModel.OwnPlayerViewModel

        constexpr __int64 kProcessEventRVA = 0x00E8E7D8;
        void (__fastcall* g_orig_process_event)(void*, void*, void*) = nullptr;

        struct CreateParams { void* world; void* type; void* owning_player; void* ret; };
        struct AddToViewportParams { int32_t z_order; };
        struct FVector2D { float x, y; };

        // Vertical pitch between stacked nameplates (a touch more than the bar
        // height so there's a small gap).
        constexpr float kRowPitch = 104.0f;

        bool g_in_hook = false;

        // --- cached, stable reflection objects (resolved once) ---------------
        bool g_resolved = false;
        void* c_widgetClass = nullptr;
        void* c_createFn = nullptr;
        void* c_wblCDO = nullptr;
        void* c_occFn = nullptr;
        void* c_addFn = nullptr;
        void* c_visFn = nullptr;
        void* c_transFn = nullptr;

        // spawned nameplate per row (1..3); g_vis caches visibility to avoid
        // redundant ProcessEvent calls each tick.
        void* g_widgets[4] = {};
        int   g_vis[4] = { -1, -1, -1, -1 };

        // Per-controller "A held since" timestamp for hold-to-join (0 = not held).
        unsigned long long g_a_hold[4] = {};

        void Log(const std::string& s) {
            LOG_INFO("[coop] {}", s.c_str());
            try {
                auto p = std::filesystem::temp_directory_path() / "alpharing_nameplate.txt";
                std::ofstream f(p, std::ios::app);
                f << s << "\n";
            } catch (...) {}
        }

        bool ResolveStatics() {
            if (g_resolved) return true;
            if (!UE::Init()) return false;
            c_widgetClass = FindClassObject("WBP_Nameplate_C").ptr();
            c_createFn    = FindFunction("WidgetBlueprintLibrary", "Create").ptr();
            c_wblCDO      = FindObjectByName("Default__WidgetBlueprintLibrary").ptr();
            c_occFn       = FindFunction("WBP_Nameplate_C", "OnContextChanged").ptr();
            c_addFn       = FindFunction("UserWidget", "AddToViewport").ptr();
            c_visFn       = FindFunction("Widget", "SetVisibility").ptr();
            c_transFn     = FindFunction("Widget", "SetRenderTranslation").ptr();
            g_resolved = c_widgetClass && c_createFn && c_wblCDO && c_addFn;
            if (g_resolved) Log("statics resolved");
            return g_resolved;
        }

        Object CreateNameplate(int slot) {
            Object gi = FindObjectByClass("BP_MCCGameInstance_C");
            if (!gi.valid()) return {};

            // Require a live nameplate context with the owner player loaded —
            // without it the widget renders a single frame then collapses, and
            // it exists too early (press-start) before the real nameplate. Defer
            // (retry next tick) until the shell nameplate is actually populated.
            Object context = FindObjectByClass("MCCNameplateOverlayViewModel");
            if (!context.valid()) return {};
            if (!Object(context.read<void*>(kNPOVM_OwnPlayerViewModel)).valid()) return {};

            auto lp = gi.read<PtrArray>(kGameInstance_LocalPlayers);
            Object pc;
            if (lp.num > 0 && lp.data)
                pc = Object(Object(lp.data[0]).read<void*>(kUPlayer_PlayerController));

            CreateParams cp{ gi.ptr(), c_widgetClass, pc.ptr(), nullptr };
            ProcessEvent(Object(c_wblCDO), Object(c_createFn), &cp);
            Object widget(cp.ret);
            if (!widget.valid()) return {};

            *reinterpret_cast<void**>(widget.address() + kSDUserWidget_ViewModel) = context.ptr();
            if (c_occFn) ProcessEvent(widget, Object(c_occFn), nullptr);

            AddToViewportParams ap{ 500 };
            ProcessEvent(widget, Object(c_addFn), &ap);

            // Force-visible (the nameplate BP can default to collapsed until its
            // context drives it).
            if (c_visFn) {
                uint8_t vis = 0; // ESlateVisibility::Visible
                ProcessEvent(widget, Object(c_visFn), &vis);
            }

            if (c_transFn) {
                // Stack under the primary (top-right) nameplate, one row per slot.
                FVector2D t{ 0.0f, slot * kRowPitch };
                ProcessEvent(widget, Object(c_transFn), &t);
            }

            char buf[160];
            snprintf(buf, sizeof(buf), "CreateNameplate slot=%d widget=%p gi=%p pc=%p context=%p",
                     slot, widget.ptr(), gi.ptr(), pc.ptr(), context.ptr());
            Log(buf);
            return widget;
        }

        void SetRowVisible(int slot, bool visible) {
            if (!g_widgets[slot] || !c_visFn) return;
            int v = visible ? 1 : 0;
            if (g_vis[slot] == v) return;
            uint8_t vis = visible ? 0 : 1; // ESlateVisibility: Visible=0, Collapsed=1
            ProcessEvent(Object(g_widgets[slot]), Object(c_visFn), &vis);
            g_vis[slot] = v;
        }

        // Show `rows` nameplates (slots 1..rows), hide the rest.
        void Reconcile(int rows) {
            if (rows > 3) rows = 3;
            for (int slot = 1; slot <= 3; ++slot) {
                bool want = slot <= rows;
                if (want && !g_widgets[slot]) {
                    g_widgets[slot] = CreateNameplate(slot).ptr();
                    g_vis[slot] = g_widgets[slot] ? 1 : -1;
                } else {
                    SetRowVisible(slot, want);
                }
            }
        }

        // Is MCC's OWN nameplate widget present yet? (Skips ours.) Used to gate
        // until we're past the press-start screen where the shell nameplate
        // isn't shown.
        bool RealNameplateExists() {
            auto objs = GObjects();
            if (!objs) return false;
            const int32_t n = objs->num();
            for (int32_t i = 0; i < n; ++i) {
                auto o = objs->get(i);
                if (!o.valid()) continue;
                if (ClassNameOf(o) != "WBP_Nameplate_C") continue;
                bool ours = false;
                for (int s = 1; s < 4; ++s) if (g_widgets[s] == o.ptr()) { ours = true; break; }
                if (!ours) return true;
            }
            return false;
        }

        void CoopTick() {
            if (!ResolveStatics()) return;

            // The shell nameplate roster + "hold A to join" belong on the
            // menu/lobby. In a live match A means "jump" — never join there, and
            // hide the roster (the game has its own splitscreen HUD).
            if (MCC::IsInGame()) {
                Reconcile(0);
                for (int c = 0; c < 4; ++c) g_a_hold[c] = 0;
                return;
            }

            // Wait until MCC's own nameplate is on screen (past press-start),
            // so ours appear in sync with it. Cache once ready.
            static bool g_shell_ready = false;
            if (!g_shell_ready) {
                if (!RealNameplateExists()) { Reconcile(0); return; }
                g_shell_ready = true;
            }

            auto ss = AlphaRing::Global::MCC::Splitscreen();
            int count = ss->player_count;
            if (count < 1) count = 1;
            if (count > 4) count = 4;

            // Controllers already assigned to an active player slot.
            bool assigned[4] = {};
            for (int slot = 0; slot < count; ++slot) {
                if (slot == 0 && ss->b_player0_use_km) continue;
                auto prof = CGameManager::get_profile(slot);
                if (prof && prof->controller_index >= 0 && prof->controller_index < 4)
                    assigned[prof->controller_index] = true;
            }

            constexpr unsigned long long kHoldMs = 900;
            unsigned long long now = GetTickCount64();
            int connected_unjoined = 0;

            for (int c = 0; c < 4; ++c) {
                XINPUT_STATE st{};
                bool conn = AlphaRing::Input::GetXInputGetState(c, &st);
                if (!conn) { g_a_hold[c] = 0; continue; }

                if (assigned[c]) { g_a_hold[c] = 0; continue; } // already a player

                ++connected_unjoined;

                bool a = (st.Gamepad.wButtons & XINPUT_GAMEPAD_A) != 0;
                // HOLD A (~1s) to join — a quick tap is left for menu navigation,
                // so this never fights MCC's menu A=select / B=back.
                if (a && count < 4) {
                    if (g_a_hold[c] == 0) {
                        g_a_hold[c] = now;
                    } else if (now - g_a_hold[c] >= kHoldMs) {
                        int slot = count;
                        auto prof = CGameManager::get_profile(slot);
                        if (prof) {
                            prof->controller_index = c;
                            ss->player_count = ++count;
                            ss->b_override = true;
                            assigned[c] = true;
                            g_a_hold[c] = 0;
                            --connected_unjoined;
                            Log("join (held A): controller joined as a player");
                        }
                    }
                } else {
                    g_a_hold[c] = 0;
                }
            }

            // Rows = joined extras + a SINGLE "press A to join" slot when any
            // unjoined controller is connected and there's room (one open slot,
            // not one per detected pad — XInput may report virtual pads).
            int prompts = (connected_unjoined > 0 && count < 4) ? 1 : 0;
            int rows = (count - 1) + prompts;
            Reconcile(rows);

            static int s_last_rows = -1, s_last_count = -1;
            if (rows != s_last_rows || count != s_last_count) {
                char buf[96];
                snprintf(buf, sizeof(buf), "state: count=%d rows=%d unjoined=%d", count, rows, connected_unjoined);
                Log(buf);
                s_last_rows = rows; s_last_count = count;
            }
        }

        void __fastcall hk_process_event(void* This, void* Function, void* Parms) {
            g_orig_process_event(This, Function, Parms);

            static unsigned long long s_last = 0;
            unsigned long long now = GetTickCount64();
            if (g_in_hook || now - s_last < 120) return;
            s_last = now;

            g_in_hook = true;
            CoopTick();
            g_in_hook = false;
        }
    }

    void Install() {
        if (AlphaRing::Hook::IsWS()) { Log("WinStore - injector disabled"); return; }
        bool ok = AlphaRing::Hook::Detour({
            { kProcessEventRVA, 0x0, (void*)&hk_process_event, (void**)&g_orig_process_event },
        });
        Log(ok ? "ProcessEvent hooked" : "ProcessEvent hook FAILED");
    }
}
