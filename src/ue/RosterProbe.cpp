#include "RosterProbe.h"

#include "UeObject.h"

#include "common.h"
#include "mcc/mcc.h"

#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <set>

namespace AlphaRing::UE::RosterProbe {
    namespace {
        std::vector<std::string> g_last;

        // Verified offsets (Dumper-7 SDK dump, MCC 1.3528 Steam):
        constexpr uintptr_t kGSVM_Players          = 0x00E8; // TArray<MCCSessionPlayerViewModel*>
        constexpr uintptr_t kGSVM_LocalSlotItemVM  = 0x00B8;
        constexpr uintptr_t kGSVM_Friends          = 0x00F8;
        constexpr uintptr_t kRVM_GameSessionVM     = 0x00E8;
        constexpr uintptr_t kGameInstance_LocalPlayers = 0x0038;

        bool IsReflectionClass(const std::string& cn) {
            if (cn == "Class" || cn == "Enum" || cn == "ScriptStruct" ||
                cn == "Package" || cn == "Function" || cn == "DelegateFunction" ||
                cn == "ArrayProperty" || cn == "BlueprintGeneratedClass")
                return true;
            if (cn.size() > 8 && cn.compare(cn.size() - 8, 8, "Property") == 0)
                return true;
            return false;
        }

        void WriteFile(const std::vector<std::string>& lines) {
            try {
                auto p = std::filesystem::temp_directory_path() / "alpharing_roster_probe.txt";
                std::ofstream f(p, std::ios::app);
                for (auto& l : lines) f << l << "\n";
                f << "\n";
            } catch (...) {}
        }

        // Single consistent pass. Fills `lines` (also stored in g_last) and
        // returns a short signature describing the current screen/roster state
        // (used for change detection).
        std::string RunPass(std::vector<std::string>& lines) {
            char buf[256];
            auto add = [&](const std::string& s) { lines.push_back(s); };

            auto objs = GObjects();
            snprintf(buf, sizeof(buf), "==== probe ==== InGame=%d GObjects.Num=%d",
                     MCC::IsInGame() ? 1 : 0, objs ? objs->num() : -1);
            add(buf);

            Object gi, gsvm, rvm;
            int cnt_session_player = 0, cnt_local_slot = 0, cnt_nameplate = 0, cnt_roster_w = 0;
            std::set<std::string> screen_classes;          // for signature
            std::vector<std::string> screen_instances;     // for display

            const int32_t count = objs ? objs->num() : 0;
            for (int32_t i = 0; i < count; ++i) {
                auto o = objs->get(i);
                if (!o.valid()) continue;
                auto cn = ClassNameOf(o);
                if (IsReflectionClass(cn)) continue;
                auto on = ObjectNameOf(o);
                if (on.rfind("Default__", 0) == 0) continue;

                if (!gi.valid() && cn.find("GameInstance") != std::string::npos) gi = o;
                if (!gsvm.valid() && cn == "MCCGameSessionViewModel") gsvm = o;
                if (!rvm.valid() && cn == "MCCRosterViewModel") rvm = o;

                if (cn == "MCCSessionPlayerViewModel") ++cnt_session_player;
                else if (cn == "WBP_MCCLocalSlotItem_C") ++cnt_local_slot;
                else if (cn == "WBP_Nameplate_C") ++cnt_nameplate;
                else if (cn == "WBP_Roster_C") ++cnt_roster_w;

                // Track the screen(s)/menus currently constructed. Broad match
                // (no MCC-prefix req) so we catch WBP_* blueprint screens too.
                {
                    static const char* kScreenNeedles[] = {
                        "Screen", "MainMenu", "Lobby", "CustomGames", "Campaign",
                        "Roster", "Party", "GameSession", "Matchmaking"
                    };
                    for (auto n : kScreenNeedles) {
                        if (cn.find(n) != std::string::npos) {
                            if (screen_classes.insert(cn).second && screen_instances.size() < 30)
                                screen_instances.push_back(on + " : " + cn);
                            break;
                        }
                    }
                }
            }

            if (gi.valid()) {
                auto lp = gi.read<PtrArray>(kGameInstance_LocalPlayers);
                snprintf(buf, sizeof(buf), "GameInstance [%s] LocalPlayers.Num=%d",
                         ClassNameOf(gi).c_str(), lp.num);
                add(buf);
            }

            snprintf(buf, sizeof(buf), "counts: SessionPlayerVM=%d LocalSlotItem(W)=%d Nameplate(W)=%d Roster(W)=%d",
                     cnt_session_player, cnt_local_slot, cnt_nameplate, cnt_roster_w);
            add(buf);

            add("-- MCC screens/overlays live --");
            for (auto& s : screen_instances) add("  " + s);

            if (gsvm.valid()) {
                snprintf(buf, sizeof(buf), ">> MCCGameSessionViewModel LIVE @ %p", gsvm.ptr());
                add(buf);
                auto players = gsvm.read<PtrArray>(kGSVM_Players);
                snprintf(buf, sizeof(buf), "   Players.Num=%d Max=%d", players.num, players.max);
                add(buf);
                for (int i = 0; i < players.num && i < 16 && players.data; ++i) {
                    Object e(players.data[i]);
                    snprintf(buf, sizeof(buf), "     Players[%d] %s : %s", i,
                             ObjectNameOf(e).c_str(), ClassNameOf(e).c_str());
                    add(buf);
                }
                auto fr = gsvm.read<PtrArray>(kGSVM_Friends);
                snprintf(buf, sizeof(buf), "   Friends.Num=%d", fr.num);
                add(buf);
                Object slot(gsvm.read<void*>(kGSVM_LocalSlotItemVM));
                snprintf(buf, sizeof(buf), "   LocalSlotItemViewModel: %s : %s",
                         ObjectNameOf(slot).c_str(), ClassNameOf(slot).c_str());
                add(buf);
            }
            add("==== end ====");

            // Signature: roster-live + session count + the set of live screens.
            std::string sig = (gsvm.valid() ? "R1:" : "R0:");
            sig += std::to_string(cnt_session_player) + ":";
            for (auto& s : screen_classes) sig += s + ",";
            return sig;
        }
    }

    std::string Dump() {
        std::vector<std::string> lines;
        if (!UE::Init()) {
            lines.push_back("UE reflection unavailable.");
            g_last = lines;
            return {};
        }
        RunPass(lines);
        g_last = lines;
        WriteFile(lines);
        try { return (std::filesystem::temp_directory_path() / "alpharing_roster_probe.txt").string(); }
        catch (...) { return {}; }
    }

    void AutoTick() {
        static unsigned long long s_lastTick = 0, s_lastWrite = 0;
        static std::string s_sig;
        static int s_writes = 0;
        if (s_writes >= 300) return;                 // bound the log
        unsigned long long now = GetTickCount64();
        if (s_lastTick != 0 && now - s_lastTick < 2000) return;
        s_lastTick = now;

        if (!UE::Init()) return;

        std::vector<std::string> lines;
        std::string sig = RunPass(lines);
        g_last = lines;

        // Persist when the screen/roster state changes, OR every ~10s as a
        // heartbeat, so we never miss a transition the signature didn't catch.
        bool changed = (sig != s_sig);
        bool heartbeat = (s_lastWrite == 0) || (now - s_lastWrite >= 10000);
        if (changed || heartbeat) {
            s_sig = sig;
            s_lastWrite = now;
            ++s_writes;
            WriteFile(lines);
        }
    }

    const std::vector<std::string>& LastReport() { return g_last; }
}
