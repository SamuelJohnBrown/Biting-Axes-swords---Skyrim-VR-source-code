#pragma once

#include <cstdint>
#include <string>
#include <optional>
#include <format>

#include <SKSE/SKSE.h>
#include <RE/Skyrim.h>

namespace BitingAxesVR
{
 // Filesystem slug (DLL / INI / log basename). Ampersand is intentional in the product name.
 inline constexpr const char* kPluginFileSlug = "Biting_Axes_&_Swords_VR";
 // SKSEPlugin_Query name and mod-API messaging recipient (must match BitingAxesAPI::kInterfaceRecipient).
 inline constexpr const char* kPluginQueryName = "BitingAxesVR";
 inline constexpr const char* kPluginDisplayName = "Biting Axes & Swords VR";

 // INI Logging= threshold (defined in config.cpp). 0=errors, 1=+warnings, 2=+info.
 extern int logging;
 inline constexpr int kLogLevelErr = 0;
 inline constexpr int kLogLevelWarn = 1;
 inline constexpr int kLogLevelInfo = 2;

 // Logging macros forwarding to SKSE::log and also appending to plugin-specific log file
 void AppendToPluginLog(const char* level, const char* fmt, ...);
 std::string GetPluginLogPath();

 // Format with std::format ({} placeholders) so the plugin log matches the SKSE log,
 // then hand the finished string to the printf-style file appender via "%s".
 #define IW_LOG_INFO(fmt, ...)                                                                                        \
     do {                                                                                                             \
         if (BitingAxesVR::logging >= BitingAxesVR::kLogLevelInfo) {                                                 \
             SKSE::log::info(fmt, ##__VA_ARGS__);                                                                    \
             BitingAxesVR::AppendToPluginLog("INFO", "%s", std::format(fmt, ##__VA_ARGS__).c_str());                  \
         }                                                                                                            \
     } while (0)
 #define IW_LOG_WARN(fmt, ...)                                                                                        \
     do {                                                                                                             \
         if (BitingAxesVR::logging >= BitingAxesVR::kLogLevelWarn) {                                                 \
             SKSE::log::warn(fmt, ##__VA_ARGS__);                                                                    \
             BitingAxesVR::AppendToPluginLog("WARN", "%s", std::format(fmt, ##__VA_ARGS__).c_str());                \
         }                                                                                                            \
     } while (0)
 #define IW_LOG_ERROR(fmt, ...)                                                                                       \
     do {                                                                                                             \
         if (BitingAxesVR::logging >= BitingAxesVR::kLogLevelErr) {                                                  \
             SKSE::log::error(fmt, ##__VA_ARGS__);                                                                   \
             BitingAxesVR::AppendToPluginLog("ERROR", "%s", std::format(fmt, ##__VA_ARGS__).c_str());                \
         }                                                                                                            \
     } while (0)

 // Backwards-compatible helper used by existing code to write simple plugin messages to SKSE log
 inline void WritePluginLog(const char* a_msg)
 {
 if (a_msg) {
 IW_LOG_INFO("{}", a_msg);
 }
 }

 // Replace a5-byte call at a_src with a trampoline-installed call to a_dst.
 // Returns the original call target address (the address the original call would have invoked).
 std::uintptr_t Write5Call(std::uintptr_t a_src, std::uintptr_t a_dst) noexcept;

 // Show a modal error box and terminate the process (used for fatal errors).
 void ShowErrorBoxAndTerminate(const char* a_errorString) noexcept;

 // Resolve a full form id (combined with mod index) from an esp/plugin name and base form id.
 // Returns0 on failure.
 std::uint32_t GetFullFormIdMine(const char* a_espName, std::uint32_t a_baseFormId) noexcept;

 // Helper: lookup a form by plugin name and form id, cast it to T and log if missing.
 template <class T>
 T* LoadFormAndLog(const std::string& a_pluginName, std::uint32_t& a_outFullFormId, std::uint32_t a_baseFormId,
 const char* a_formName) noexcept
 {
 a_outFullFormId = GetFullFormIdMine(a_pluginName.c_str(), a_baseFormId);
 if (a_outFullFormId ==0) {
 IW_LOG_WARN("LoadFormAndLog: {} not found (formid:0x{:08X})", a_formName, a_baseFormId);
 return nullptr;
 }

 auto form = RE::TESForm::LookupByID(a_outFullFormId);
 if (!form) {
 IW_LOG_WARN("LoadFormAndLog: {} not found (full formid:0x{:08X})", a_formName, a_outFullFormId);
 return nullptr;
 }

 if (!form->Is(T::FORMTYPE)) {
 IW_LOG_WARN("LoadFormAndLog: {} has wrong type (full formid:0x{:08X})", a_formName, a_outFullFormId);
 return nullptr;
 }

 return static_cast<T*>(form);
 }
}
