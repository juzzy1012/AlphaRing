#pragma once

#include "CModule.h"
#include "./utils.h"

namespace MCC::Module {
    bool Initialize();

    // true while any game DLL (halo1..haloreach) is loaded, i.e. a game
    // session is running — including loading screens and cutscenes
    bool AnyGameModuleLoaded();

    bool IsWS();

    FileVersion Version();

    __int64 GetBaseAddress();

    bool ReloadPatch(const char *xml_path = "./alpha_ring/patch.xml");

    void ImGuiContext();
}
