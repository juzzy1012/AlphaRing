#include "roster_probe.h"

#include "log/Log.h"

#include <Windows.h>

#include <cstdint>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <utility>

using namespace AlphaRing;

namespace roster_probe {

    namespace {

        std::mutex g_mutex;
        // key: (function name, absolute caller return address) -> hit count
        std::map<std::pair<std::string, uintptr_t>, uint64_t> g_sites;
        std::ofstream g_file;
        bool g_file_tried = false;

        std::ofstream& file() {
            if (!g_file_tried) {
                g_file_tried = true;
                char tmp[MAX_PATH];
                DWORD n = GetTempPathA(MAX_PATH, tmp);
                std::string path = (n ? std::string(tmp, n) : std::string()) + "alpharing_probe.log";
                // truncate on first open this process so each run is clean
                g_file.open(path, std::ios::out | std::ios::trunc);
            }
            return g_file;
        }

        std::string narrow(const std::wstring& w) {
            if (w.empty()) return {};
            int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
            std::string out(n, '\0');
            WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), out.data(), n, nullptr, nullptr);
            return out;
        }

        void resolve(void* addr, std::string& mod, uintptr_t& rva) {
            HMODULE h = nullptr;
            if (GetModuleHandleExW(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(addr), &h) && h) {
                rva = reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(h);
                wchar_t buf[MAX_PATH];
                DWORD len = GetModuleFileNameW(h, buf, MAX_PATH);
                std::wstring full(buf, len);
                auto pos = full.find_last_of(L"\\/");
                mod = narrow(pos == std::wstring::npos ? full : full.substr(pos + 1));
            } else {
                rva = reinterpret_cast<uintptr_t>(addr);
                mod = "?";
            }
        }

        // Records a call; logs (file + console) only the FIRST time a (fn, caller) pair is seen,
        // so high-frequency callers (e.g. per-frame input) register their site once without spam.
        void record(const char* fn, void* ret, long long arg, const char* arg_label) {
            std::string mod;
            uintptr_t rva = 0;
            resolve(ret, mod, rva);

            bool is_new;
            uint64_t hits;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                auto key = std::make_pair(std::string(fn), reinterpret_cast<uintptr_t>(ret));
                auto it = g_sites.find(key);
                is_new = (it == g_sites.end());
                hits = (is_new ? 1 : ++it->second);
                if (is_new) g_sites.emplace(key, 1);
            }
            if (!is_new) return;

            char line[512];
            _snprintf_s(line, sizeof(line), _TRUNCATE,
                "[roster_probe] NEW fn=%s %s=%lld caller=%s+0x%llx",
                fn, arg_label, arg, mod.c_str(), (unsigned long long)rva);

            LOG_INFO("{}", line);

            auto& f = file();
            if (f.is_open()) { f << line << "\n"; f.flush(); }
        }

    } // namespace

    void on_get_xbox_user_id(void* ret, int index) {
        record("get_xbox_user_id", ret, index, "index");
    }
    void on_get_player_profile(void* ret, long long xid) {
        record("get_player_profile", ret, xid, "xid");
    }
    void on_retrive_gamepad_mapping(void* ret, long long xid) {
        record("retrive_gamepad_mapping", ret, xid, "xid");
    }
    void on_get_key_state(void* ret, unsigned int index) {
        record("get_key_state", ret, (long long)index, "index");
    }

} // namespace roster_probe
