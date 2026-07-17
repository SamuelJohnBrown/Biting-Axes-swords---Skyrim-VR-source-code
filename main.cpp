#include "pch.h"
#include <SKSE/SKSE.h>
#include <SKSE/Trampoline.h>
#include <RE/Skyrim.h>
#include "higgsinterface.h"
#include "planckinterface.h"
#include "helper.h"
#include "engine.h"
#include "config.h"
#include "bite.h"
#include <cstdint>
#include <fstream>
#include <cstdlib>
#include <cstdio>

// Minimal plugin info struct used by many SKSE loaders
struct SKSEPluginInfo {
	std::uint32_t infoVersion;
	const char* name;
	std::uint32_t version;
};

// Messaging callback: called for SKSE messages
static void OnSKSEMessage(SKSE::MessagingInterface::Message* msg)
{
	if (!msg) {
		return;
	}

	switch (msg->type) {
	case SKSE::MessagingInterface::kPostPostLoad: {
		BitingAxesVR::LogFalseEdgeVRStatus();

		// Try to obtain HIGGS interface via SKSE messaging now that PostPostLoad occurred
		auto pluginHandle = SKSE::GetPluginHandle();
		auto messaging = SKSE::GetMessagingInterface();
		auto higgs = HiggsPluginAPI::GetHiggsInterface001(pluginHandle, const_cast<SKSE::MessagingInterface*>(messaging));
		if (higgs) {
			SKSE::log::info("{}: obtained HIGGS interface, build {}", BitingAxesVR::kPluginDisplayName,
			                higgs->GetBuildNumber());
			IW_LOG_INFO("{}: obtained HIGGS interface", BitingAxesVR::kPluginDisplayName);
			BitingAxesVR::Bite::AttachHiggsCallbacks();
		} else {
			SKSE::log::info("{}: HIGGS interface not available on PostPostLoad", BitingAxesVR::kPluginDisplayName);
			IW_LOG_WARN("{}: HIGGS interface not available on PostPostLoad", BitingAxesVR::kPluginDisplayName);
		}
		// PLANCK provides the extended melee hit data (hit bone, weapon velocity)
		if (auto* planck = PlanckAPI::GetPlanckInterface()) {
			IW_LOG_INFO("{}: obtained PLANCK interface, build {}", BitingAxesVR::kPluginDisplayName,
			            planck->GetBuildNumber());
		} else {
			IW_LOG_WARN("{}: PLANCK interface not available - is PLANCK installed?", BitingAxesVR::kPluginDisplayName);
		}
		break;
	}
	case SKSE::MessagingInterface::kDataLoaded: {
		// Game data has been loaded; load settings and start listening for melee hits
		IW_LOG_INFO("{}: received kDataLoaded message", BitingAxesVR::kPluginDisplayName);
		BitingAxesVR::loadConfig();
		BitingAxesVR::Bite::PreloadAssets();
		BitingAxesVR::Bite::RegisterHitSink();
		break;
	}
	// The scene we hold pointers into is about to be torn down -- drop the embeds
	// silently (and restore weapon collision) or we'd write to stale nodes after load.
	case SKSE::MessagingInterface::kPreLoadGame:
	case SKSE::MessagingInterface::kNewGame: {
		BitingAxesVR::Bite::ResetAllEmbeds();
		break;
	}
	default:
		break;
	}
}

// Minimal Query export used by the SKSE loader to identify the plugin
extern "C" __declspec(dllexport) bool SKSEPlugin_Query(const void* /*a_skse*/, SKSEPluginInfo* a_info)
{
	if (a_info) {
		a_info->infoVersion = 1; // standard SKSE info version
		a_info->name = BitingAxesVR::kPluginFileSlug; // shown in loader log
		a_info->version = 1;                          // plugin version
	}
	return true;
}

// Load export called after Query
extern "C" __declspec(dllexport) bool SKSEPlugin_Load(const SKSE::LoadInterface* a_skse)
{
	BitingAxesVR::SetSkseLoadInterface(a_skse);
	SKSE::Init(a_skse);

	SKSE::log::info("{} loaded", BitingAxesVR::kPluginDisplayName);

	// Remove old plugin log so we replace it on each load
	const std::string path = BitingAxesVR::GetPluginLogPath();
	if (!path.empty()) {
		std::remove(path.c_str());
	}

	// Defer trampoline creation to CommonLibSSE-NG API init callback for compatibility
	SKSE::RegisterForAPIInitEvent([]()
	{
		try {
			auto& trampoline = SKSE::GetTrampoline();
			if (trampoline.empty()) {
				constexpr std::size_t TrampolineSize = 64; // adjust if you need more
				trampoline.create(TrampolineSize);
			}

			// Wire the global trampoline pointer so other modules can reference it
			BitingAxesVR::g_trampoline = &trampoline;

			// Confirmation logging
			SKSE::log::info("{}: trampoline created (capacity = {} bytes)", BitingAxesVR::kPluginDisplayName,
			                trampoline.capacity());
			IW_LOG_INFO("{}: trampoline created", BitingAxesVR::kPluginDisplayName);

			// Register listener for SKSE messages so we can fetch HIGGS on PostPostLoad and detect DataLoaded
			auto messaging = SKSE::GetMessagingInterface();
			if (messaging) {
				bool reg = messaging->RegisterListener("SKSE", OnSKSEMessage);
				SKSE::log::info("{}: registered SKSE messaging listener: {}", BitingAxesVR::kPluginDisplayName, reg);
				if (reg) {
					IW_LOG_INFO("{}: registered SKSE messaging listener", BitingAxesVR::kPluginDisplayName);
				} else {
					IW_LOG_ERROR("{}: failed to register SKSE messaging listener", BitingAxesVR::kPluginDisplayName);
				}
			} else {
				SKSE::log::warn("{}: messaging interface not available during API init",
				                BitingAxesVR::kPluginDisplayName);
				IW_LOG_WARN("{}: messaging interface not available during API init", BitingAxesVR::kPluginDisplayName);
			}

		} catch (const std::exception& e) {
			SKSE::log::error("{}: trampoline creation failed: {}", BitingAxesVR::kPluginDisplayName, e.what());
			IW_LOG_ERROR("{}: trampoline creation failed", BitingAxesVR::kPluginDisplayName);
		} catch (...) {
			SKSE::log::error("{}: trampoline creation failed: unknown error", BitingAxesVR::kPluginDisplayName);
			IW_LOG_ERROR("{}: trampoline creation failed", BitingAxesVR::kPluginDisplayName);
		}
	});

	return true;
}


//THIS IS THE PATH WHERE THE DLL IS SENT
// C:\Users\user\Downloads\commonlibsse-ng-template-main\commonlibsse-ng-template-main\build\windows\x64\release


// TO REBUILD THE SKSE PLUGIN, RUN THIS COMMAND
// &"C:\Program Files\\xmake\\xmake.exe" build skse_plugin
