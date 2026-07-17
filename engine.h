#pragma once

#include <SKSE/SKSE.h>
#include "helper.h"

// Forward-declare HIGGS API types to avoid including the full header in this public header.
namespace HiggsPluginAPI { struct IHiggsInterface001; extern IHiggsInterface001* g_higgsInterface; }

namespace BitingAxesVR
{
 // Trampoline provided by CommonLibSSE-NG; pointer placeholder set after creation
 extern SKSE::Trampoline* g_trampoline;

 // Entry point called once init is complete and dependent APIs are available.
 void StartMod();

 // Store SKSE load interface for GetPluginInfo-based plugin detection.
 void SetSkseLoadInterface(const SKSE::LoadInterface* loadInterface);

 // Detect False Edge VR (FalseEdgeVR.dll / legacy FakeEdgeVR.dll).
 bool IsFalseEdgeVRPresent();

 // Log False Edge presence to SKSE + plugin log (call after all plugins load).
 void LogFalseEdgeVRStatus();

 // Log whether SpellInteractionsVR.esp is loaded when game data is ready
 void LogSpellInteractionsVRLoaded();
}
