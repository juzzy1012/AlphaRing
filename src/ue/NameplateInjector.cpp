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
#include <cctype>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace AlphaRing::UE::NameplateInjector {
    namespace {
        // Layouts (Dumper-7 SDK, MCC 1.3528 Steam):
        constexpr uintptr_t kGameInstance_LocalPlayers = 0x0038;
        constexpr uintptr_t kUPlayer_PlayerController  = 0x0030;
        constexpr uintptr_t kSDUserWidget_ViewModel    = 0x02D8;
        constexpr uintptr_t kNPOVM_OwnPlayerViewModel  = 0x01C8; // MCCNameplateOverlayViewModel.OwnPlayerViewModel
        constexpr uintptr_t kConfigTextBlock_PropName  = 0x0280; // SDConfigurableTextBlock.ConfigPropertyName (FName)
        constexpr uintptr_t kSessionVM_PlayerNameValue = 0x00C8; // MCCSessionPlayerViewModel.PlayerName(0x78).Value(0x50)

        constexpr __int64 kProcessEventRVA = 0x00E8E7D8;
        void (__fastcall* g_orig_process_event)(void*, void*, void*) = nullptr;

        struct CreateParams { void* world; void* type; void* owning_player; void* ret; };
        struct AddToViewportParams { int32_t z_order; };
        struct FVector2D { float x, y; };

        // FText is 0x18 on UE 4.21 (TSharedPtr<ITextData> + flags). We never own
        // one — we build it via KismetTextLibrary and hand it straight to SetText
        // (which copies it), so a raw byte blob is enough.
        struct FTextBlob { uint8_t bytes[0x18]; };
        struct FStringView { wchar_t* data; int32_t num; int32_t max; };
        struct ConvParams  { FStringView in; FTextBlob ret; };          // string -> text
        struct ToStrParams { FTextBlob in; FStringView ret; };          // text -> string
        struct SetTextParams { FTextBlob text; };
        struct GetTextParams { FTextBlob ret; };                        // TextBlock.GetText()

        // UTextBlock::Text FText lives at +0x180 (UMG SDK).
        constexpr uintptr_t kTextBlock_Text = 0x0180;

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
        // Text population (optional — null if the engine libs aren't resolvable):
        void* c_setTextFn   = nullptr; // TextBlock.SetText(FText)
        void* c_convCDO     = nullptr; // Default__KismetTextLibrary
        void* c_convFn      = nullptr; // KismetTextLibrary.Conv_StringToText(FString)->FText
        void* c_toStrFn     = nullptr; // KismetTextLibrary.Conv_TextToString(FText)->FString
        void* c_getTextFn   = nullptr; // TextBlock.GetText()->FText (live Slate text)

        // spawned nameplate per row (1..3); g_vis caches visibility to avoid
        // redundant ProcessEvent calls each tick.
        void* g_widgets[4] = {};
        int   g_vis[4] = { -1, -1, -1, -1 };
        // The name + clan-tag TextBlocks inside each spawned widget (resolved
        // once on creation) + the last text we wrote, so we only SetText on
        // change.
        void* g_name_tb[4] = {};
        std::vector<void*> g_tag_group[4]; // the "[", "UNSC", "]" blocks together
        std::wstring g_text[4];
        int   g_tag_vis[4] = { -1, -1, -1, -1 }; // clan-tag visibility cache

        // Cached primary-player gamertag (read from MCC's own nameplate).
        std::wstring g_gamertag;

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
            // Optional text-population path (don't gate g_resolved on these).
            c_setTextFn   = FindFunction("TextBlock", "SetText").ptr();
            c_convCDO     = FindObjectByName("Default__KismetTextLibrary").ptr();
            c_convFn      = FindFunction("KismetTextLibrary", "Conv_StringToText").ptr();
            c_toStrFn     = FindFunction("KismetTextLibrary", "Conv_TextToString").ptr();
            c_getTextFn   = FindFunction("TextBlock", "GetText").ptr();
            g_resolved = c_widgetClass && c_createFn && c_wblCDO && c_addFn;
            if (g_resolved) {
                char b[160];
                snprintf(b, sizeof(b), "statics resolved (setText=%p conv=%p convCDO=%p)",
                         c_setTextFn, c_convFn, c_convCDO);
                Log(b);
            }
            return g_resolved;
        }

        std::wstring FStringToWide(const FStringView& s) {
            if (!s.data || s.num <= 0) return {};
            int len = s.num;
            // FString.Num includes the null terminator — drop it.
            while (len > 0 && s.data[len - 1] == 0) --len;
            return std::wstring(s.data, s.data + len);
        }

        // Is `ancestor` somewhere up `o`'s Outer chain?
        bool HasAncestor(Object o, void* ancestor, int depth = 12) {
            Object p = o.outer();
            for (int i = 0; i < depth && p.valid(); ++i) {
                if (p.ptr() == ancestor) return true;
                p = p.outer();
            }
            return false;
        }

        // MCC's own (non-injected) local nameplate widget, if present. Skips the
        // blueprint CDO (Default__WBP_Nameplate_C), whose child text is the
        // design-time placeholder.
        Object FindRealNameplate() {
            auto objs = GObjects();
            if (!objs) return {};
            const int32_t n = objs->num();
            for (int32_t i = 0; i < n; ++i) {
                auto o = objs->get(i);
                if (!o.valid()) continue;
                if (ClassNameOf(o) != "WBP_Nameplate_C") continue;
                if (ObjectNameOf(o).rfind("Default__", 0) == 0) continue; // skip CDO
                bool ours = false;
                for (int s = 1; s < 4; ++s) if (g_widgets[s] == o.ptr()) { ours = true; break; }
                if (!ours) return o;
            }
            return {};
        }

        // The LIVE displayed string of a UTextBlock-derived widget. We call the
        // GetText() UFunction (which returns the Slate widget's current text)
        // rather than reading the Text property — SDConfigurableTextBlock binds
        // its display via config and never writes the Text property (that stays
        // the design-time placeholder). Falls back to the raw property if
        // GetText isn't resolvable.
        std::wstring TextBlockString(Object tb) {
            if (!c_toStrFn || !tb.valid()) return {};
            FTextBlob ft{};
            if (c_getTextFn) {
                GetTextParams gp{};
                ProcessEvent(tb, Object(c_getTextFn), &gp);
                ft = gp.ret;
            } else {
                ft = tb.read<FTextBlob>(kTextBlock_Text);
            }
            ToStrParams tp{};
            tp.in = ft;
            ProcessEvent(tb, Object(c_toStrFn), &tp);
            std::wstring s = FStringToWide(tp.ret);
            while (!s.empty() && s.back() <= L' ') s.pop_back();
            return s;
        }

        std::string Utf8(const std::wstring& s) {
            int k = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
            std::string u(k > 0 ? k : 0, '\0');
            if (k > 0) WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), u.data(), k, nullptr, nullptr);
            return u;
        }

        // Read an FText property at `off` on `o` and convert it to a string.
        std::wstring FTextValueAt(Object o, uintptr_t off) {
            if (!c_toStrFn || !o.valid()) return {};
            ToStrParams tp{};
            tp.in = o.read<FTextBlob>(off);
            ProcessEvent(o, Object(c_toStrFn), &tp);
            std::wstring s = FStringToWide(tp.ret);
            while (!s.empty() && s.back() <= L' ') s.pop_back();
            return s;
        }

        // The Xbox gamertag the game shows on Player 1's nameplate ("LA1VE").
        // The nameplate's PlayerName text is a config binding into a viewmodel
        // (the rendered string isn't on the TextBlock itself), so we read the
        // viewmodel's PlayerName FTextBindable.Value directly. Cached.
        std::wstring PrimaryGamertag() {
            if (!g_gamertag.empty()) return g_gamertag;
            auto objs = GObjects();
            if (!objs) return {};

            // --- diagnostics: what does the real nameplate's PlayerName bind to?
            static bool s_diag = false;
            if (!s_diag) {
                Object real = FindRealNameplate();
                if (real.valid()) {
                    const int32_t n = objs->num();
                    for (int32_t i = 0; i < n; ++i) {
                        auto o = objs->get(i);
                        if (!o.valid()) continue;
                        if (ObjectNameOf(o) != "PlayerName") continue;
                        if (ClassNameOf(o) != "SDConfigurableTextBlock") continue;
                        if (!HasAncestor(o, real.ptr())) continue;
                        auto fn = o.read<FName>(kConfigTextBlock_PropName);
                        Object vm(real.read<void*>(kSDUserWidget_ViewModel));
                        Object own = vm.valid() ? Object(vm.read<void*>(kNPOVM_OwnPlayerViewModel)) : Object();
                        char b[256];
                        snprintf(b, sizeof(b), "diag: PlayerName.ConfigProp='%s' vm=%s own=%s",
                                 NameToString(fn).c_str(), ClassNameOf(vm).c_str(), ClassNameOf(own).c_str());
                        Log(b);
                        s_diag = true;
                        break;
                    }
                }
            }

            // --- source: the (local) session player's PlayerName bindable.
            const int32_t n = objs->num();
            for (int32_t i = 0; i < n; ++i) {
                auto o = objs->get(i);
                if (!o.valid()) continue;
                if (ClassNameOf(o) != "MCCSessionPlayerViewModel") continue;
                if (ObjectNameOf(o).rfind("Default__", 0) == 0) continue;
                std::wstring s = FTextValueAt(o, kSessionVM_PlayerNameValue);
                Log(("diag: sessionVM PlayerName.Value='" + Utf8(s) + "'").c_str());
                if (s.empty() || s == L"PLAYER NAME") continue;
                g_gamertag = s;
                Log(("gamertag resolved: '" + Utf8(s) + "'").c_str());
                return g_gamertag;
            }
            return {};
        }

        // Primary player's display name (the base for "Name(N)").
        std::wstring BaseName() {
            std::wstring gt = PrimaryGamertag();
            if (!gt.empty()) return gt;
            auto prof = CGameManager::get_profile(0);
            if (prof && prof->name[0]) return std::wstring(prof->name);
            return L"Player";
        }

        // Build an FText from a wide string via KismetTextLibrary. The returned
        // blob is handed to SetText (which copies it). Returns false if the
        // engine libs weren't resolvable.
        bool MakeText(const std::wstring& w, FTextBlob& out) {
            out = {};
            if (!c_convCDO || !c_convFn) return false;
            static wchar_t buf[256];
            int len = (int)w.size();
            if (len > 254) len = 254;
            wmemcpy(buf, w.c_str(), len);
            buf[len] = 0;
            ConvParams cp{};
            cp.in = { buf, len + 1, len + 1 }; // FString.Num counts the null
            ProcessEvent(Object(c_convCDO), Object(c_convFn), &cp);
            out = cp.ret;
            return true;
        }

        // Find the player-name block and the clan-tag GROUP among `widget`'s
        // children. The "[UNSC]" tag is THREE separate SDConfigurableTextBlocks:
        // "[" (SDConfigurableTextBlock_0), "UNSC" (ClanTag), "]"
        // (SDConfigurableTextBlock_1) — so the prompt must collapse all three.
        // The name block is "PlayerName"; all are SDConfigurableTextBlock :
        // UTextBlock (safe cast for TextBlock.SetText).
        void ResolveBlocks(Object widget, int slot) {
            g_name_tb[slot] = nullptr;
            g_tag_group[slot].clear();
            auto objs = GObjects();
            if (!objs) return;
            const int32_t n = objs->num();
            void* nameFallback = nullptr;
            int found = 0;
            for (int32_t i = 0; i < n; ++i) {
                auto o = objs->get(i);
                if (!o.valid()) continue;
                auto cn = ClassNameOf(o);
                // UTextBlock-derived only (safe for TextBlock.SetText).
                if (cn != "TextBlock" && cn != "SDConfigurableTextBlock") continue;
                if (!HasAncestor(o, widget.ptr())) continue;

                ++found;
                auto on = ObjectNameOf(o);
                std::wstring cur = TextBlockString(o);
                char b[256];
                snprintf(b, sizeof(b), "  slot=%d textblock '%s' : %s = '%s'",
                         slot, on.c_str(), cn.c_str(), Utf8(cur).c_str());
                Log(b);

                if (on == "PlayerName") g_name_tb[slot] = o.ptr();
                // Tag group = the ClanTag value plus its surrounding brackets.
                else if (on == "ClanTag" || cur == L"[" || cur == L"]")
                    g_tag_group[slot].push_back(o.ptr());

                std::string low = on;
                for (auto& ch : low) ch = (char)tolower((unsigned char)ch);
                if (!nameFallback && low.find("name") != std::string::npos &&
                    low.find("tag") == std::string::npos)
                    nameFallback = o.ptr();
            }
            if (!g_name_tb[slot]) g_name_tb[slot] = nameFallback;

            char b[160];
            snprintf(b, sizeof(b), "  slot=%d textblocks=%d nameBlock=%p tagGroup=%d",
                     slot, found, g_name_tb[slot], (int)g_tag_group[slot].size());
            Log(b);
        }

        void SetBlockText(void* block, const std::wstring& w) {
            if (!block || !c_setTextFn) return;
            SetTextParams p{};
            if (!MakeText(w, p.text)) return;
            ProcessEvent(Object(block), Object(c_setTextFn), &p);
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

            // Locate name + clan-tag TextBlocks for text population.
            ResolveBlocks(widget, slot);
            g_text[slot].clear();
            g_tag_vis[slot] = -1;
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

        // showTag=true keeps the native "[UNSC]" clan tag (joined players);
        // false collapses it (the join prompt).
        struct RowSpec { std::wstring name; bool showTag; };

        void SetTagVisible(int slot, bool show) {
            if (g_tag_group[slot].empty() || !c_visFn) return;
            int v = show ? 1 : 0;
            if (g_tag_vis[slot] == v) return;
            uint8_t vis = show ? 0 : 1; // ESlateVisibility: Visible=0, Collapsed=1
            for (void* block : g_tag_group[slot])
                ProcessEvent(Object(block), Object(c_visFn), &vis);
            g_tag_vis[slot] = v;
        }

        // Show one nameplate per entry in `specs` (slots 1..N), each populated
        // with its name; hide the rest. The clan tag keeps its native default
        // text ("[UNSC]") — we only toggle its visibility.
        void Reconcile(const RowSpec* specs, int rows) {
            if (rows > 3) rows = 3;
            for (int slot = 1; slot <= 3; ++slot) {
                int idx = slot - 1;
                bool want = idx < rows;
                if (want) {
                    if (!g_widgets[slot]) {
                        g_widgets[slot] = CreateNameplate(slot).ptr();
                        g_vis[slot] = g_widgets[slot] ? 1 : -1;
                    } else {
                        SetRowVisible(slot, true);
                    }
                    if (!g_widgets[slot]) continue;
                    if (specs[idx].name != g_text[slot]) {
                        SetBlockText(g_name_tb[slot], specs[idx].name);
                        g_text[slot] = specs[idx].name;
                    }
                    SetTagVisible(slot, specs[idx].showTag);
                } else {
                    SetRowVisible(slot, false);
                }
            }
        }

        void Reconcile(int rows) {
            RowSpec none[3] = {};
            Reconcile(none, rows < 0 ? 0 : rows);
        }

        // Is MCC's OWN nameplate widget present yet? (Skips ours.) Used to gate
        // until we're past the press-start screen where the shell nameplate
        // isn't shown.
        bool RealNameplateExists() {
            return FindRealNameplate().valid();
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

            // Rows = joined extras (labelled "Name(N)" console-style) + a SINGLE
            // "press A to join" slot when any unjoined controller is connected
            // and there's room (one open slot, not one per detected pad — XInput
            // may report virtual pads).
            std::wstring base = BaseName();
            RowSpec specs[3];
            int rows = 0;
            for (int j = 1; j <= count - 1 && rows < 3; ++j) {          // joined extras
                specs[rows].name = base + L"(" + std::to_wstring(j) + L")";
                specs[rows].showTag = true; // keep native "[UNSC]" like the primary
                ++rows;
            }
            int prompts = (connected_unjoined > 0 && count < 4 && rows < 3) ? 1 : 0;
            if (prompts) {
                specs[rows].name = L"Press A to Join";
                specs[rows].showTag = false; // no clan tag on the join prompt
                ++rows;
            }
            Reconcile(specs, rows);

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
