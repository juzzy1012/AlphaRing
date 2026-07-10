#include "NameplateInjector.h"

#include "UeObject.h"

#include "common.h"
#include "mcc/mcc.h"
#include "mcc/CGameManager.h"
#include "mcc/CGameEngine.h"
#include "mcc/splitscreen/Splitscreen.h"
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
#include <utility>
#include <vector>

namespace AlphaRing::UE::NameplateInjector {
    namespace {
        // Layouts (Dumper-7 SDK, MCC 1.3528 Steam):
        constexpr uintptr_t kGameInstance_LocalPlayers = 0x0038;
        constexpr uintptr_t kUPlayer_PlayerController  = 0x0030;
        constexpr uintptr_t kSDUserWidget_ViewModel    = 0x02D8;
        constexpr uintptr_t kNPWidget_OverlayVM        = 0x0378; // MCCNameplateOverlayWidget.NameplateOverlayViewModel
        constexpr uintptr_t kNPOVM_OwnPlayerViewModel  = 0x01C8; // MCCNameplateOverlayViewModel.OwnPlayerViewModel
        constexpr uintptr_t kConfigTextBlock_PropName  = 0x0280; // SDConfigurableTextBlock.ConfigPropertyName (FName)
        constexpr uintptr_t kSessionVM_PlayerNameValue = 0x00C8; // MCCSessionPlayerViewModel.PlayerName(0x78).Value(0x50)

        // Visual-parity layouts (UMG / MCC SDK):
        constexpr uintptr_t kNameplate_RosterButtonIcon= 0x0388; // WBP_Nameplate_C.RosterButtonIcon (UImage*)
        constexpr uintptr_t kNPWidget_RosterButton     = 0x0358; // MCCNameplateOverlayWidget.RosterButton (USDTile*)
        constexpr uintptr_t kNPWidget_RosterInputPanel = 0x0360; // MCCNameplateOverlayWidget.RosterInputPanel (UPanelWidget*)
        constexpr uintptr_t kImage_Brush               = 0x0160; // UImage.Brush (FSlateBrush, 0x88)
        constexpr uintptr_t kImage_ColorAndOpacity     = 0x01F8; // UImage.ColorAndOpacity (FLinearColor) — the tint
        constexpr uintptr_t kBrush_ResourceObject      = 0x0048; // FSlateBrush.ResourceObject (UObject*)
        constexpr uintptr_t kDynImage_ImageUri         = 0x0260; // MCCDynamicImage.ImageUri (FString)
        constexpr uintptr_t kGSVM_Players              = 0x00E8; // MCCGameSessionViewModel.Players (TArray) — Num>=1 in a lobby

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
        struct FBrushBlob { uint8_t bytes[0x88]; };                     // FSlateBrush (UE 4.21)
        struct SetBrushParams { FBrushBlob brush; };                    // Image.SetBrush(FSlateBrush)
        struct FLinColor { float r, g, b, a; };                         // FLinearColor
        struct SetColorParams { FLinColor color; };                     // Image.SetColorAndOpacity

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
        void* c_setBrushFn  = nullptr; // Image.SetBrush(FSlateBrush)
        void* c_setColorFn  = nullptr; // Image.SetColorAndOpacity(FLinearColor)
        void* c_setJustifyFn= nullptr; // TextBlock.SetJustification(ETextJustify)
        void* c_setMinWidthFn = nullptr; // TextBlock.SetMinDesiredWidth(float)
        void* c_getDesiredFn  = nullptr; // Widget.GetDesiredSize()->FVector2D
        void* c_setImageUriFn = nullptr;   // MCCDynamicImage.SetImageUri(FString)

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
        // The join-prompt plate is a bare transparent box (no blue bar / emblem /
        // rank) — only joined PLAYER plates get full visual parity. Tracked per
        // slot so the mirror skips prompt plates and collapses their content.
        bool  g_is_prompt[4] = {};
        int   g_content_vis[4] = { -1, -1, -1, -1 }; // plate body (bar/emblem/rank) cache
        int   g_justify[4] = { -1, -1, -1, -1 };     // name-text justification cache

        // Cached primary-player gamertag (read from MCC's own nameplate).
        std::wstring g_gamertag;
        // Cached blue-bar image URI read off P1's "Nameplate" MCCDynamicImage.
        std::wstring g_plate_uri;

        // Per-controller "A held since" timestamp for hold-to-join (0 = not held).
        unsigned long long g_a_hold[4] = {};
        // "Enter held since" timestamp for keyboard hold-to-join (0 = not held).
        unsigned long long g_enter_hold = 0;

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
            c_setBrushFn  = FindFunction("Image", "SetBrush").ptr();
            c_setColorFn  = FindFunction("Image", "SetColorAndOpacity").ptr();
            c_setJustifyFn= FindFunction("TextBlock", "SetJustification").ptr();
            c_setMinWidthFn = FindFunction("TextBlock", "SetMinDesiredWidth").ptr();
            c_getDesiredFn  = FindFunction("Widget", "GetDesiredSize").ptr();
            c_setImageUriFn = FindFunction("MCCDynamicImage", "SetImageUri").ptr();
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

        // The LIVE, data-populated P1 nameplate — NOT the empty "WidgetArchetype"
        // template (which has every image/emblem null and was the source of our
        // all-null probes). The live one has a real NameplateOverlayViewModel
        // whose OwnPlayerViewModel is set; the archetype/CDO don't. Falls back to
        // any non-template instance if none looks fully populated yet.
        Object FindPopulatedNameplate() {
            auto objs = GObjects();
            if (!objs) return {};
            const int32_t n = objs->num();
            Object fallback;
            for (int32_t i = 0; i < n; ++i) {
                auto o = objs->get(i);
                if (!o.valid()) continue;
                if (ClassNameOf(o) != "WBP_Nameplate_C") continue;
                auto on = ObjectNameOf(o);
                if (on.rfind("Default__", 0) == 0) continue;          // CDO
                if (on.find("WidgetArchetype") != std::string::npos) continue; // template
                bool ours = false;
                for (int s = 1; s < 4; ++s) if (g_widgets[s] == o.ptr()) { ours = true; break; }
                if (ours) continue;
                Object vm(o.read<void*>(kNPWidget_OverlayVM));
                if (vm.valid() && Object(vm.read<void*>(kNPOVM_OwnPlayerViewModel)).valid())
                    return o; // live + populated
                if (!fallback.valid()) fallback = o;
            }
            return fallback;
        }

        // True when a game-session LOBBY is active — MCC's
        // MCCGameSessionViewModel has at least one player. This is the page
        // where local players belong (you're setting up a game). P1's corner
        // nameplate and the roster overlay are present on the main menu too
        // (identical there — that's why gating on them leaked the prompt onto
        // the main screen), but the session is EMPTY on the main menu
        // (Players.Num==0) and fills (>=1) once you enter a lobby. Verified via
        // the page diagnostic: main menu -> 0 players, plate page -> 1 player.
        bool IsSessionLobbyActive() {
            auto objs = GObjects();
            if (!objs) return false;
            const int32_t n = objs->num();
            for (int32_t i = 0; i < n; ++i) {
                auto o = objs->get(i);
                if (!o.valid()) continue;
                if (ClassNameOf(o) != "MCCGameSessionViewModel") continue;
                if (ObjectNameOf(o).rfind("Default__", 0) == 0) continue; // CDO
                PtrArray players = o.read<PtrArray>(kGSVM_Players);
                if (players.num >= 1) return true;
            }
            return false;
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

        bool IsImageClass(const std::string& cn) {
            return cn == "Image" || cn == "MCCDynamicImage" || cn == "MCCUUIImage";
        }

        // The clone is structurally identical to P1 (verified via DumpTree) — all
        // widgets present at full opacity — but its image brushes are empty
        // because the per-player data binding never loaded the textures. So we
        // copy each populated FSlateBrush from P1's image widgets onto the
        // clone's same-named images (the blue "Nameplate" bar, the emblem
        // images, the rank icons). Pure memory read + SetBrush (a UImage method,
        // safe on all three UImage-derived classes) — NO game-data functions
        // (RefreshTextures / OnContextChanged re-drives faulted uncatchably).
        // Runs every tick so async-loaded textures get picked up; SetBrush only
        // fires when the source texture differs from what the clone already has.
        void MirrorBrushes(Object real) {
            if (!c_setBrushFn || !real.valid()) return;
            auto objs = GObjects();
            if (!objs) return;
            const int32_t n = objs->num();

            // One O(n) pass: collect P1's images (name->object) and every clone's
            // images (object + name). Both sets are tiny (~8 each).
            std::vector<std::pair<std::string, void*>> src; // P1 name -> image obj
            std::vector<std::pair<std::string, void*>> dst; // clone image objs
            for (int32_t i = 0; i < n; ++i) {
                auto o = objs->get(i);
                if (!o.valid()) continue;
                if (!IsImageClass(ClassNameOf(o))) continue;
                if (HasAncestor(o, real.ptr(), 16)) {
                    src.push_back({ ObjectNameOf(o), o.ptr() });
                    continue;
                }
                for (int s = 1; s < 4; ++s) {
                    if (g_is_prompt[s]) continue; // prompt plate stays transparent
                    if (g_widgets[s] && HasAncestor(o, g_widgets[s], 16)) {
                        dst.push_back({ ObjectNameOf(o), o.ptr() });
                        break;
                    }
                }
            }
            for (auto& d : dst) {
                for (auto& s : src) {
                    if (s.first != d.first) continue;
                    // Copy when the brushes differ at all — not only when the
                    // source has a texture. P1's blue bar is a TEXTURELESS color
                    // brush (ResourceObject null, blue TintColor), so a
                    // resource-only check skipped it; a full-brush compare
                    // carries the tint across too.
                    FBrushBlob sb = Object(s.second).read<FBrushBlob>(kImage_Brush);
                    FBrushBlob db = Object(d.second).read<FBrushBlob>(kImage_Brush);
                    if (std::memcmp(&sb, &db, sizeof(FBrushBlob)) != 0) {
                        SetBrushParams p{};
                        p.brush = sb;
                        ProcessEvent(Object(d.second), Object(c_setBrushFn), &p);
                    }
                    // Mirror the tint (UImage.ColorAndOpacity) too — the blue
                    // nameplate bar is a greyscale base texture tinted blue via
                    // this field, NOT the brush, so SetBrush alone left it grey.
                    if (c_setColorFn) {
                        FLinColor sc = Object(s.second).read<FLinColor>(kImage_ColorAndOpacity);
                        FLinColor dc = Object(d.second).read<FLinColor>(kImage_ColorAndOpacity);
                        if (std::memcmp(&sc, &dc, sizeof(FLinColor)) != 0) {
                            SetColorParams p{};
                            p.color = sc;
                            ProcessEvent(Object(d.second), Object(c_setColorFn), &p);
                        }
                    }
                    break;
                }
            }
        }

        // Drive the clone's blue-bar "Nameplate" MCCDynamicImage from P1's image
        // URI (its texture comes from ImageUri/LoadedTexture, NOT the brush, so
        // SetBrush can't reach it). Native loader = safe; only fires once per
        // clone (when its uri differs).
        void MirrorBlueBar(Object real) {
            if (!c_setImageUriFn) return;
            auto objs = GObjects();
            if (!objs) return;
            const int32_t n = objs->num();
            // Resolve P1's blue-bar URI lazily (it may load after first spawn).
            if (g_plate_uri.empty() && real.valid()) {
                for (int32_t i = 0; i < n; ++i) {
                    auto o = objs->get(i);
                    if (!o.valid()) continue;
                    if (ClassNameOf(o) != "MCCDynamicImage") continue;
                    if (ObjectNameOf(o) != "Nameplate") continue;
                    if (!HasAncestor(o, real.ptr(), 16)) continue;
                    std::wstring uri = FStringToWide(o.read<FStringView>(kDynImage_ImageUri));
                    if (!uri.empty()) { g_plate_uri = uri; }
                    break;
                }
            }
            if (g_plate_uri.empty()) return;
            for (int32_t i = 0; i < n; ++i) {
                auto o = objs->get(i);
                if (!o.valid()) continue;
                if (ClassNameOf(o) != "MCCDynamicImage") continue;
                if (ObjectNameOf(o) != "Nameplate") continue;
                bool ours = false;
                for (int s = 1; s < 4; ++s) {
                    if (g_is_prompt[s]) continue; // prompt plate stays transparent
                    if (g_widgets[s] && HasAncestor(o, g_widgets[s], 16)) { ours = true; break; }
                }
                if (!ours) continue;
                std::wstring cur = FStringToWide(o.read<FStringView>(kDynImage_ImageUri));
                if (cur == g_plate_uri) continue;
                static wchar_t buf[260];
                int len = (int)g_plate_uri.size();
                if (len > 258) len = 258;
                wmemcpy(buf, g_plate_uri.c_str(), len);
                buf[len] = 0;
                FStringView sv{ buf, len + 1, len + 1 };
                ProcessEvent(o, Object(c_setImageUriFn), &sv);
                Log("blue-bar: SetImageUri applied to a clone");
            }
        }

        // Surface the nameplate's native roster A-button glyph on the prompt
        // plate (RosterButtonIcon / RosterButton / RosterInputPanel are the
        // game's own "press A" prompt elements). show=false collapses them on
        // joined plates.
        void SetRosterGlyphVisible(int slot, bool show) {
            void* w = g_widgets[slot];
            if (!w || !c_visFn) return;
            Object widget(w);
            uint8_t vis = show ? 0 : 1; // Visible=0, Collapsed=1
            for (uintptr_t off : { kNameplate_RosterButtonIcon,
                                   kNPWidget_RosterButton,
                                   kNPWidget_RosterInputPanel }) {
                void* el = widget.read<void*>(off);
                if (el) ProcessEvent(Object(el), Object(c_visFn), &vis);
            }
        }

        // Show/hide the plate BODY — the blue bar ("Nameplate"), emblem
        // ("Emblem"), rank ("AccountLevelIcons"), the left avatar box
        // ("PlayerPreview") and the XP bar ("AccountProgression"/"XPProgress").
        // Collapsed = a bare transparent box (the join prompt); visible = a full
        // player plate. O(n) scan, but cached so it only runs when state flips.
        void SetPlateContentVisible(int slot, bool show) {
            void* w = g_widgets[slot];
            if (!w || !c_visFn) return;
            int v = show ? 1 : 0;
            if (g_content_vis[slot] == v) return;
            auto objs = GObjects();
            if (!objs) return;
            const int32_t n = objs->num();
            uint8_t vis = show ? 0 : 1; // Visible=0, Collapsed=1
            for (int32_t i = 0; i < n; ++i) {
                auto o = objs->get(i);
                if (!o.valid()) continue;
                auto on = ObjectNameOf(o);
                // Only LEAF visuals — NOT containers like PlayerPreview /
                // AccountProgression, which hold the name text (collapsing them
                // hides "Hold A to Join").
                if (on != "Nameplate" && on != "Emblem" && on != "AccountLevelIcons" &&
                    on != "XPProgress")
                    continue;
                if (!HasAncestor(o, w, 16)) continue;
                ProcessEvent(o, Object(c_visFn), &vis);
            }
            g_content_vis[slot] = v;
        }

        // Style the plate's name text: prompt = centered across the full plate,
        // player = left/auto. A text block auto-sizes to its content, so
        // justification alone can't center it — we first give it a min width
        // equal to the plate's width so it spans the row, then center within it.
        void SetNameStyle(int slot, bool center) {
            if (!g_name_tb[slot]) return;
            int v = center ? 1 : 0;
            if (g_justify[slot] == v) return;
            if (c_setMinWidthFn) {
                float w = 0.0f;
                if (center) {
                    // The name block starts AFTER the emblem column (left offset
                    // S ~= 0.16*plateWidth). A block of width plateWidth-2*S,
                    // grown rightward from S and center-justified, lands the text
                    // at the true plate centre. => ~0.68 * plateWidth.
                    float plateW = 640.0f; // fallback
                    if (c_getDesiredFn && g_widgets[slot]) {
                        struct { FVector2D ret; } gp{};
                        ProcessEvent(Object(g_widgets[slot]), Object(c_getDesiredFn), &gp);
                        if (gp.ret.x > 200.0f && gp.ret.x < 4000.0f) plateW = gp.ret.x;
                    }
                    w = plateW * 0.68f;
                }
                struct { float w; } mp{ w };
                ProcessEvent(Object(g_name_tb[slot]), Object(c_setMinWidthFn), &mp);
            }
            if (c_setJustifyFn) {
                uint8_t j = center ? 1 : 0; // ETextJustify: Left=0, Center=1
                ProcessEvent(Object(g_name_tb[slot]), Object(c_setJustifyFn), &j);
            }
            g_justify[slot] = v;
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
            *reinterpret_cast<void**>(widget.address() + kNPWidget_OverlayVM) = context.ptr();

            AddToViewportParams ap{ 500 };
            ProcessEvent(widget, Object(c_addFn), &ap);

            // Bind the context AFTER the Slate tree is constructed (post-add) so
            // the emblem/background/rank bindings actually populate — binding
            // before AddToViewport leaves the clone "ghosted" (text only).
            if (c_occFn) ProcessEvent(widget, Object(c_occFn), nullptr);

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
        // false collapses it (the join prompt). showGlyph surfaces the native
        // roster A-button glyph (the join prompt) and hides it on joined plates.
        struct RowSpec { std::wstring name; bool showTag; bool showGlyph; };

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
        void Reconcile(const RowSpec* specs, int rows, Object real) {
            if (rows > 3) rows = 3;
            bool anyActive = false;
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
                    anyActive = true;

                    if (specs[idx].name != g_text[slot]) {
                        SetBlockText(g_name_tb[slot], specs[idx].name);
                        g_text[slot] = specs[idx].name;
                    }
                    SetTagVisible(slot, specs[idx].showTag);
                    // The roster button belongs on PLAYER plates, not the join
                    // prompt (showGlyph marks the prompt) — hide it there.
                    SetRosterGlyphVisible(slot, !specs[idx].showGlyph);
                    // The prompt (showGlyph) is a transparent box: hide its blue
                    // bar / emblem / rank and skip the visual mirror; player
                    // plates keep the full body.
                    g_is_prompt[slot] = specs[idx].showGlyph;
                    SetPlateContentVisible(slot, !specs[idx].showGlyph);
                    SetNameStyle(slot, specs[idx].showGlyph); // prompt = centered
                } else {
                    SetRowVisible(slot, false);
                }
            }
            // Copy P1's populated visuals onto the clones so they render
            // identically — brush images (rank/XP) via SetBrush, and the blue
            // bar (MCCDynamicImage) via its native ImageUri loader. Runs each
            // tick to pick up async-loaded textures. `real` is the caller's
            // already-validated live source.
            if (anyActive && real.valid()) {
                MirrorBrushes(real);
                MirrorBlueBar(real);
            }
        }

        // Drop all clone references WITHOUT touching the widgets. Used when the
        // menu nameplate UI is being torn down (entering a match / loading): the
        // engine frees the widgets during the transition, and ProcessEvent on a
        // freed widget is an uncatchable fatal. They re-spawn fresh on return to
        // the menu (g_widgets are null again).
        void ForgetClones() {
            for (int s = 1; s < 4; ++s) {
                g_widgets[s] = nullptr;
                g_vis[s] = -1;
                g_name_tb[s] = nullptr;
                g_tag_group[s].clear();
                g_text[s].clear();
                g_tag_vis[s] = -1;
                g_is_prompt[s] = false;
                g_content_vis[s] = -1;
                g_justify[s] = -1;
            }
        }

        // Is this pad actively being used this tick (button / trigger / stick
        // beyond a generous deadzone)? Mere connection isn't enough — XInput
        // reports idle/virtual pads, and we don't want one to steal P1.
        bool PadActive(const XINPUT_STATE& st) {
            const auto& g = st.Gamepad;
            if (g.wButtons != 0) return true;
            if (g.bLeftTrigger > 40 || g.bRightTrigger > 40) return true;
            constexpr int dz = 12000; // generous — ignores stick drift
            if (g.sThumbLX > dz || g.sThumbLX < -dz) return true;
            if (g.sThumbLY > dz || g.sThumbLY < -dz) return true;
            if (g.sThumbRX > dz || g.sThumbRX < -dz) return true;
            if (g.sThumbRY > dz || g.sThumbRY < -dz) return true;
            return false;
        }

        // Is the keyboard/mouse being used this tick? A small menu-relevant key
        // set plus mouse buttons and cursor movement — mirrors what flips MCC's
        // own menu into KBM mode.
        bool KeyboardActive() {
            static const int keys[] = {
                VK_RETURN, VK_SPACE, VK_ESCAPE, VK_BACK, VK_TAB,
                VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT,
                'W', 'A', 'S', 'D', 'E', 'F',
                VK_LBUTTON, VK_RBUTTON,
            };
            for (int k : keys)
                if (GetAsyncKeyState(k) & 0x8000) return true;
            // Cursor movement (mouse used to navigate). Threshold avoids jitter.
            POINT p{};
            if (GetCursorPos(&p)) {
                static bool s_have = false;
                static POINT s_last{};
                if (s_have) {
                    long dx = p.x - s_last.x, dy = p.y - s_last.y;
                    s_last = p;
                    if (dx * dx + dy * dy > 100) return true; // moved > ~10px
                } else {
                    s_last = p; s_have = true;
                }
            }
            return false;
        }

        // Auto-identify Player 1's input device from how the user is driving the
        // menu — the specific gamepad, or keyboard/mouse — mirroring MCC's own
        // last-used-device auto-switch. Runs ONLY while solo on the main menu;
        // once a lobby is active or extra players have joined the assignment is
        // frozen so P1 can't flip while a second player is joining. b_override
        // stays false here, so this only PRE-CONFIGURES the routing the game
        // reads once a second player joins. Sticky: keep the prior device on a
        // tick with no input. Logs only on change.
        void AutoDetectPlayer1(bool lobbyActive) {
            auto ss = AlphaRing::Global::MCC::Splitscreen();
            if (!ss) return;
            if (lobbyActive || ss->player_count > 1) return; // frozen

            int activePad = -1;
            for (int c = 0; c < 4; ++c) {
                XINPUT_STATE st{};
                if (AlphaRing::Input::GetXInputGetState(c, &st) && PadActive(st)) {
                    activePad = c;
                    break;
                }
            }

            static int s_logged = -2; // -2 none yet, -1 KBM, 0..3 pad index
            if (activePad >= 0) {
                ss->b_player0_use_km = false;
                auto p0 = CGameManager::get_profile(0);
                if (p0) p0->controller_index = activePad;
                if (s_logged != activePad) {
                    char b[64];
                    snprintf(b, sizeof(b), "P1 auto-detect: gamepad %d", activePad);
                    Log(b);
                    s_logged = activePad;
                }
            } else if (KeyboardActive()) {
                ss->b_player0_use_km = true;
                if (s_logged != -1) { Log("P1 auto-detect: keyboard/mouse"); s_logged = -1; }
            }
            // else: nothing active this tick — keep the prior assignment.
        }

        void CoopTick() {
            if (!ResolveStatics()) return;

            // SAFETY GATE — only manage clones while the LIVE menu nameplate is
            // confirmed present THIS tick. In a match (A=jump; the game has its
            // own splitscreen HUD) or while the menu UI is torn down (loading /
            // transition), DROP our clone refs WITHOUT touching them: the engine
            // frees the widgets during the transition and ProcessEvent on a freed
            // widget is an uncatchable fatal. A short grace period avoids
            // forgetting on a one-tick miss. The populated nameplate's presence
            // also replaces the old press-start readiness gate.
            // A game session is "running" while the game ENGINE exists — MCC
            // creates it at level launch, keeps it through loading screens and
            // CUTSCENES, and nulls it on exit to the menus. MCC's in-game flag
            // alone flaps during cinematics; gating on it let the clones be
            // re-created mid-cutscene (populated WBP_Nameplates exist in-game
            // once splitscreen players join) and then abandoned visible by the
            // next ForgetClones — the roster appeared to flash. The game DLL is
            // NOT a usable signal: it stays loaded in the lobby, where the join
            // UI must keep working.
            const bool in_game = MCC::IsInGame() || GameEngine() != nullptr;

            static int s_menu_miss = 0;
            Object real = in_game ? Object() : FindPopulatedNameplate();
            if (in_game || !real.valid()) {
                if (in_game || ++s_menu_miss >= 3) { ForgetClones(); s_menu_miss = 0; }
                for (int c = 0; c < 4; ++c) g_a_hold[c] = 0;
                g_enter_hold = 0;
                return;
            }
            s_menu_miss = 0;

            bool onPage = IsSessionLobbyActive();

            // P1 AUTO-DETECT — runs BEFORE the page gate so it works on the main
            // menu (which the gate early-returns). Identifies P1's device (the
            // gamepad they're using, or KBM) and freezes once a lobby is active.
            AutoDetectPlayer1(onPage);

            // PAGE GATE — the coop plates + join prompt belong on the game-
            // session lobby page (where you set players up), NOT the main menu.
            // P1's nameplate and the roster overlay are present on both, so gate
            // on the session having players: empty (Num==0) on the main menu,
            // >=1 once a lobby is entered. The populated nameplate is confirmed
            // present this tick, so the clone widgets are valid → HIDE them (not
            // forget) when off-page; they re-show instantly on return.
            static int s_page_logged = -1;
            if (s_page_logged != (onPage ? 1 : 0)) {
                Log(onPage ? "page: session lobby active — coop plates ON"
                           : "page: no session — coop plates hidden");
                s_page_logged = onPage ? 1 : 0;
            }
            if (!onPage) {
                for (int slot = 1; slot <= 3; ++slot) SetRowVisible(slot, false);
                for (int c = 0; c < 4; ++c) g_a_hold[c] = 0;
                g_enter_hold = 0;
                return;
            }

            auto ss = AlphaRing::Global::MCC::Splitscreen();
            int count = ss->player_count;
            if (count < 1) count = 1;
            if (count > 4) count = 4;

            // Devices already owned by an active player slot. Pads are tracked
            // in assigned[]; the keyboard (controller_index==4, or P1's legacy
            // KBM flag) is tracked separately so only ONE slot can be KBM.
            bool assigned[4] = {};
            bool kbm_assigned = ss->b_player0_use_km;
            for (int slot = 0; slot < count; ++slot) {
                if (slot == 0 && ss->b_player0_use_km) continue;
                auto prof = CGameManager::get_profile(slot);
                if (!prof) continue;
                if (prof->controller_index == 4) { kbm_assigned = true; continue; }
                if (prof->controller_index >= 0 && prof->controller_index < 4)
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

            // Keyboard joins via HOLD Enter, but only when the keyboard isn't
            // already a player (single-KBM). Symmetric with hold-A; the hold
            // (not a tap) keeps it from firing MCC's menu Enter=select.
            if (!kbm_assigned && count < 4) {
                bool enter = (GetAsyncKeyState(VK_RETURN) & 0x8000) != 0;
                if (enter) {
                    if (g_enter_hold == 0) {
                        g_enter_hold = now;
                    } else if (now - g_enter_hold >= kHoldMs) {
                        int slot = count;
                        auto prof = CGameManager::get_profile(slot);
                        if (prof) {
                            prof->controller_index = 4; // KBM sentinel
                            ss->player_count = ++count;
                            ss->b_override = true;
                            kbm_assigned = true;
                            g_enter_hold = 0;
                            Log("join (held Enter): keyboard joined as a player");
                        }
                    }
                } else {
                    g_enter_hold = 0;
                }
            }

            // Rows = joined extras (labelled "Name(N)" console-style) + a SINGLE
            // join prompt when there's an open slot and a free device. The
            // prompt's label reflects what can still join: a connected unjoined
            // pad ("A"), the free keyboard ("Enter"), or both.
            std::wstring base = BaseName();
            RowSpec specs[3];
            int rows = 0;
            for (int j = 1; j <= count - 1 && rows < 3; ++j) {          // joined extras
                specs[rows].name = base + L"(" + std::to_wstring(j) + L")";
                specs[rows].showTag = true;   // keep native "[UNSC]" like the primary
                specs[rows].showGlyph = false; // no roster prompt on a joined plate
                ++rows;
            }
            bool padCanJoin = connected_unjoined > 0;   // a free, connected pad
            bool kbCanJoin  = !kbm_assigned;            // the keyboard is free
            int prompts = ((padCanJoin || kbCanJoin) && count < 4 && rows < 3) ? 1 : 0;
            if (prompts) {
                specs[rows].name = (padCanJoin && kbCanJoin) ? L"Hold A or Enter to Join"
                                 : kbCanJoin                 ? L"Hold Enter to Join"
                                                             : L"Hold A to Join";
                specs[rows].showTag = false;  // no clan tag on the join prompt
                specs[rows].showGlyph = true; // surface the native Xbox A glyph
                ++rows;
            }
            Reconcile(specs, rows, real);

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
            MCC::Splitscreen::SyncHalo1PlayerCount("ue");
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
