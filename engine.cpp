#include "engine.h"
#include <SKSE/SKSE.h>
#include "higgsinterface.h"
#include "helper.h"

#include <Windows.h>

namespace BitingAxesVR
{
 SKSE::Trampoline* g_trampoline = nullptr;

 namespace
 {
     const SKSE::LoadInterface* g_skseLoadInterface = nullptr;

     bool IsSksePluginDllLoaded(const char* dllFileName)
     {
         HMODULE module = GetModuleHandleA(dllFileName);
         return module && GetProcAddress(module, "SKSEPlugin_Query");
     }
 }

 void SetSkseLoadInterface(const SKSE::LoadInterface* loadInterface)
 {
     g_skseLoadInterface = loadInterface;
 }

 bool IsFalseEdgeVRPresent()
 {
     static const char* kDllNames[] = {
         "FalseEdgeVR.dll",
         "FakeEdgeVR.dll",
     };

     for (const char* dllName : kDllNames) {
         if (IsSksePluginDllLoaded(dllName)) {
             return true;
         }
     }

     if (g_skseLoadInterface) {
         static const char* kPluginNames[] = {
             "FalseEdgeVR",
             "FakeEdgeVR",
         };

         for (const char* pluginName : kPluginNames) {
             if (g_skseLoadInterface->GetPluginInfo(pluginName)) {
                 return true;
             }
         }
     }

     return false;
 }

 void LogFalseEdgeVRStatus()
 {
     if (IsFalseEdgeVRPresent()) {
         IW_LOG_INFO("FalseEdgeVR.dll FOUND in load order — world-model embed uses TRIGGER (release grip to leave weapon in body)");
         SKSE::log::info("{}: FalseEdgeVR.dll detected; world-model embed input=trigger", kPluginDisplayName);
     } else {
         IW_LOG_INFO("FalseEdgeVR.dll NOT found in load order — world-model embed uses GRIP");
         SKSE::log::info("{}: FalseEdgeVR.dll not detected; world-model embed input=grip", kPluginDisplayName);
     }
 }

 void LogSpellInteractionsVRLoaded()
 {
 auto handler = RE::TESDataHandler::GetSingleton();
 if (!handler) {
 IW_LOG_WARN("LogSpellInteractionsVRLoaded: TESDataHandler not available");
 return;
 }

 const auto mod = handler->LookupLoadedModByName("SpellInteractionsVR.esp");
 if (mod) {
 // mod->GetModIndex() isn't universally available; use handler->GetLoadedModIndex
 auto idx = handler->GetLoadedModIndex("SpellInteractionsVR.esp");
 if (idx && *idx !=0xFF) {
 IW_LOG_INFO("SpellInteractionsVR.esp is loaded. Mod index:0x{:02X}", static_cast<unsigned int>(*idx));
 } else {
 IW_LOG_WARN("SpellInteractionsVR.esp is loaded but mod index invalid");
 }
 } else {
 IW_LOG_WARN("SpellInteractionsVR.esp is NOT loaded");
 }
 }

 void StartMod()
 {
 // Implement module startup here: install hooks, initialize state, use helper functions.
 IW_LOG_INFO("{}: StartMod called", kPluginDisplayName);
 // Example placeholder: if HIGGS available, log build
 if (g_higgsInterface) {
 IW_LOG_INFO("{}: HIGGS build {}", kPluginDisplayName, g_higgsInterface->GetBuildNumber());
 }
 }
}
