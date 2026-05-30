#pragma once

#include <cstdint>
#include <string>

// Minimal, read-only UE 4.21.1 object-model primitives for MCC (Steam 1.3528).
// Used by the native-roster investigation probe. Reflection/reads only — no
// ProcessEvent, so it is safe to call from any thread.
//
// Offsets/layouts hard-cited against the Dumper-7 SDK dump
// (C:\Dumper-7\4.21.1-1003+__depot_MCC-MCC).

namespace AlphaRing::UE {
    // FName { int32 ComparisonIndex; int32 Number } @ UObject+0x18
    struct FName { int32_t comparison_index; int32_t number; };

    // FString = TArray<wchar_t> { wchar_t* Data; int32 Num; int32 Max }
    struct FString { wchar_t* data; int32_t num; int32_t max; };

    using t_append_string = void(*)(const FName*, FString*);

    namespace uobject {
        constexpr uintptr_t k_flags = 0x08;
        constexpr uintptr_t k_index = 0x0C;
        constexpr uintptr_t k_class = 0x10;
        constexpr uintptr_t k_name  = 0x18;
        constexpr uintptr_t k_outer = 0x20;
    }

    // Non-owning view over a UObject*.
    class Object {
    public:
        Object(void* ptr = nullptr) : m_ptr(reinterpret_cast<uintptr_t>(ptr)) {}
        bool valid() const { return m_ptr != 0; }
        void* ptr() const { return reinterpret_cast<void*>(m_ptr); }
        uintptr_t address() const { return m_ptr; }

        template<typename T> T read(uintptr_t off) const { return *reinterpret_cast<T*>(m_ptr + off); }

        Object get_class() const { return Object(read<void*>(uobject::k_class)); }
        FName name() const { return read<FName>(uobject::k_name); }
        Object outer() const { return Object(read<void*>(uobject::k_outer)); }

    private:
        uintptr_t m_ptr;
    };

    // FChunkedFixedUObjectArray view (Objects** @0x0, Num @0x14, NumChunks @0x1C).
    class ObjectArray {
    public:
        static constexpr int32_t k_elements_per_chunk = 0x10000;
        struct Item { void* object; uint8_t pad[0x10]; }; // 0x18

        int32_t num() const { return *reinterpret_cast<int32_t*>(base() + 0x14); }
        int32_t num_chunks() const { return *reinterpret_cast<int32_t*>(base() + 0x1C); }

        Object get(int32_t index) const {
            if (index < 0 || index >= num()) return {};
            const int32_t chunk = index / k_elements_per_chunk;
            const int32_t in = index % k_elements_per_chunk;
            if (chunk >= num_chunks()) return {};
            auto chunks = *reinterpret_cast<Item***>(base() + 0x00);
            auto c = chunks[chunk];
            if (!c) return {};
            return Object(c[in].object);
        }

    private:
        uintptr_t base() const { return reinterpret_cast<uintptr_t>(this); }
    };

    // TArray<T*> { T** data; int32 num; int32 max } — generic pointer-array view.
    struct PtrArray { void** data; int32_t num; int32_t max; };

    // Resolve GObjects + AppendString (Steam only). Returns false on WinStore /
    // if resolution fails. Safe to call repeatedly.
    bool Init();
    bool Ready();

    std::string NameToString(const FName& name);
    std::string ObjectNameOf(Object o);
    std::string ClassNameOf(Object o); // name of o's UClass

    ObjectArray* GObjects();

    // First live instance whose UClass is named `class_name` (skips CDO).
    Object FindObjectByClass(const std::string& class_name);

    // First object whose OWN name == `name` (e.g. a class "WBP_Nameplate_C" or
    // a CDO "Default__WidgetBlueprintLibrary").
    Object FindObjectByName(const std::string& name);

    // The UClass / WidgetBlueprintGeneratedClass object named `name`.
    Object FindClassObject(const std::string& name);

    // A UFunction named `func` declared on (Outer ==) the class `owning_class`.
    Object FindFunction(const std::string& owning_class, const std::string& func);

    // UObject::ProcessEvent(func, params) via vtable[64]. GAME THREAD ONLY.
    void ProcessEvent(Object target, Object func, void* params);

    constexpr int k_process_event_vtable_index = 64;
}
