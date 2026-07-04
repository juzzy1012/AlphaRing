#include "UeObject.h"

#include "common.h"
#include "hook/Hook.h"

#include <Windows.h>

namespace AlphaRing::UE {
    namespace {
        ObjectArray* g_objects = nullptr;
        t_append_string g_append_string = nullptr;

        std::string WideToUtf8(const wchar_t* w, int len) {
            if (!w || len <= 0) return {};
            int size = WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
            if (size <= 0) return {};
            std::string out(size, '\0');
            WideCharToMultiByte(CP_UTF8, 0, w, len, out.data(), size, nullptr, nullptr);
            return out;
        }
    }

    bool Init() {
        if (Ready())
            return true;

        // Steam-only: these RVAs are for MCC-Win64-Shipping.exe (UE 4.21.1).
        if (AlphaRing::Hook::IsWS())
            return false;

        // Hook::Offset sets each pointer to (module_base + steam_offset).
        AlphaRing::Hook::Offset({
            { 0x03E389C0, 0x0, (void**)&g_objects },        // GObjects (chunked)
            { 0x00D29C48, 0x0, (void**)&g_append_string },  // FName::AppendString
        });

        return Ready();
    }

    bool Ready() { return g_objects != nullptr && g_append_string != nullptr; }

    ObjectArray* GObjects() { return g_objects; }

    std::string NameToString(const FName& name) {
        if (!g_append_string) return {};
        wchar_t buffer[1024];
        FString out{ buffer, 0, 1024 };
        g_append_string(&name, &out);
        auto s = WideToUtf8(out.data, out.num);
        // AppendString can leave a trailing space / NUL — trim so EXACT-match
        // lookups (FindObjectByClass/FindFunction/...) work, not just substring.
        while (!s.empty() && (unsigned char)s.back() <= ' ')
            s.pop_back();
        return s;
    }

    std::string ObjectNameOf(Object o) {
        if (!o.valid()) return {};
        return NameToString(o.name());
    }

    std::string ClassNameOf(Object o) {
        if (!o.valid()) return {};
        auto c = o.get_class();
        if (!c.valid()) return {};
        return NameToString(c.name());
    }

    Object FindObjectByClass(const std::string& class_name) {
        if (!g_objects) return {};
        const int32_t count = g_objects->num();
        for (int32_t i = 0; i < count; ++i) {
            auto o = g_objects->get(i);
            if (!o.valid()) continue;
            if (ClassNameOf(o) != class_name) continue;
            auto name = ObjectNameOf(o);
            if (name.rfind("Default__", 0) == 0) continue; // skip CDO
            return o;
        }
        return {};
    }

    Object FindObjectByName(const std::string& name) {
        if (!g_objects) return {};
        const int32_t count = g_objects->num();
        for (int32_t i = 0; i < count; ++i) {
            auto o = g_objects->get(i);
            if (!o.valid()) continue;
            if (ObjectNameOf(o) == name) return o;
        }
        return {};
    }

    Object FindClassObject(const std::string& name) {
        if (!g_objects) return {};
        const int32_t count = g_objects->num();
        for (int32_t i = 0; i < count; ++i) {
            auto o = g_objects->get(i);
            if (!o.valid()) continue;
            if (ObjectNameOf(o) != name) continue;
            auto cn = ClassNameOf(o);
            // class objects: UClass / (Widget)BlueprintGeneratedClass
            if (cn == "Class" || cn.find("BlueprintGeneratedClass") != std::string::npos)
                return o;
        }
        return {};
    }

    Object FindFunction(const std::string& owning_class, const std::string& func) {
        if (!g_objects) return {};
        const int32_t count = g_objects->num();
        for (int32_t i = 0; i < count; ++i) {
            auto o = g_objects->get(i);
            if (!o.valid()) continue;
            if (ClassNameOf(o) != "Function") continue;
            if (ObjectNameOf(o) != func) continue;
            if (ObjectNameOf(o.outer()) == owning_class) return o;
        }
        return {};
    }

    void ProcessEvent(Object target, Object func, void* params) {
        if (!target.valid() || !func.valid()) return;
        using t_pe = void(*)(void*, void*, void*);
        auto vt = *reinterpret_cast<void***>(target.ptr());
        auto fn = reinterpret_cast<t_pe>(vt[k_process_event_vtable_index]);
        fn(target.ptr(), func.ptr(), params);
    }
}
