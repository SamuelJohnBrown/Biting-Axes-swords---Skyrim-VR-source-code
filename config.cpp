#include "config.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <cstdarg>
#include <windows.h>

namespace BitingAxesVR
{
 int logging =0;
 int leftHandedMode =0;

 double biteMinSpeed = 160.0;
 double biteMaxSpeed = 650.0;
 double biteGripStrength = 1.0;
 double biteYankSpeed = 140.0;
 double biteReleaseDelay = 1.75;
 double bitePullDistance = 6.0;
 double biteLostDistance = 90.0;
 double biteVictimMaxDistance = 75.0;
 double biteSafetySeconds = 30.0;
 double biteShakeLoose = 55.0;
 double biteVictimSpeedFrac = 0.0;
 int embedPlayerStaminaDrainEnabled = 1;
 double embedPlayerStaminaDrainPerSec = 2.0;
 int embedStaminaExhaustRelease = 1;
 int embedWorldModelEnabled = 1;
 int embedArmsAndHandsEnabled = 0;
 double embedCooldownSec = 0.5;

 double axeMaxInsertFrac = 0.40;
 double axeMinInitialFrac = 0.19;
 double axeMaxInitialFrac = 0.37;

 double swordMaxInsertFrac = 0.08;
 double swordMinInitialFrac = 0.03;
 double swordMaxInitialFrac = 0.06;
 double swordHiltExcludeFrac = 0.35;
 double swordMinSpeed = 115.0;
 double swordDepthScale = 0.50;
 double swordEmbedOffsetTighten = 0.80;

 double embedBloodIntervalSec = 1.0;
 double lodgedBloodIntervalSec = 3.0;
 double postEmbedBloodDurationSec = 10.0;
 double lodgedHealthDrainPerSec = 0.35;
 double lodgedMagickaDrainPerSec = 0.35;
 double lodgedStaminaDrainPerSec = 0.35;
 int lodgedHoldStaminaDrainEnabled = 1;
 double lodgedHoldStaminaDrainPerSec = 12.0;
 double lodgedHoldStaminaMin = 0.5;
 double lodgedExtractGrabRadius = 42.0;

 double biteDistressFaceSeconds = 1.0;
 double victimMaxDragStep = 45.0;
 double combatHeadshotHealthFrac = 0.50;

 double hapticBite = 1.0;
 double hapticExtract = 0.6;

 static inline void trim(std::string& s)
 {
 s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) { return !std::isspace(ch); }));
 s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), s.end());
 }

 static inline void skipComments(std::string& s)
 {
 auto pos = s.find(';');
 if (pos != std::string::npos) s.erase(pos);
 pos = s.find('#');
 if (pos != std::string::npos) s.erase(pos);
 }

 static std::string GetConfigSettingsStringValue(const std::string& line, std::string& outName)
 {
 auto pos = line.find('=');
 if (pos == std::string::npos) return {};
 outName = line.substr(0, pos);
 std::string val = line.substr(pos +1);
 trim(outName);
 trim(val);
 if (!val.empty() && val.front() == '"' && val.back() == '"') val = val.substr(1, val.size() -2);
 return val;
 }

 static bool TryParseDouble(const std::string& value, double& out)
 {
 if (value.empty()) {
 return false;
 }
 try {
 out = std::stod(value);
 return true;
 } catch (...) {
 return false;
 }
 }

 static bool TryParseInt(const std::string& value, int& out)
 {
 if (value.empty()) {
 return false;
 }
 try {
 out = std::stoi(value);
 return true;
 } catch (...) {
 return false;
 }
 }

 static void ClampUnitFrac(double& val)
 {
 val = std::clamp(val, 0.0, 1.0);
 }

 static void ClampNonNegative(double& val)
 {
 if (val < 0.0) {
 val = 0.0;
 }
 }

 static std::string GetRuntimeDirectory()
 {
 char buf[MAX_PATH];
 const DWORD len = GetModuleFileNameA(nullptr, buf, MAX_PATH);
 if (len == 0 || len >= MAX_PATH) {
 return std::filesystem::current_path().string() + "\\";
 }
 std::string path(buf, len);
 const auto pos = path.find_last_of("\\/");
 if (pos == std::string::npos) {
 return std::filesystem::current_path().string() + "\\";
 }
 return path.substr(0, pos + 1);
 }

 void loadConfig()
 {
 std::string runtimeDirectory = GetRuntimeDirectory();

 if (runtimeDirectory.empty()) {
 IW_LOG_WARN("Config: runtime directory not found, skipping config load");
 return;
 }

 std::string filepath = runtimeDirectory + "Data\\SKSE\\Plugins\\" + std::string(kPluginFileSlug) + ".ini";
 std::ifstream file(filepath);

 if (!file.is_open()) {
 std::string pathLower = filepath;
 std::transform(pathLower.begin(), pathLower.end(), pathLower.begin(), ::tolower);
 file.open(pathLower);
 }

 if (!file.is_open()) {
 IW_LOG_WARN("Config: failed to open config file {}", filepath.c_str());
 return;
 }

 std::string line;
 std::string currentSection;

 while (std::getline(file, line)) {
 trim(line);
 skipComments(line);
 if (line.empty()) continue;

 if (line[0] == '[') {
 auto endBracket = line.find(']');
 if (endBracket != std::string::npos) {
 currentSection = line.substr(1, endBracket -1);
 trim(currentSection);
 }
 continue;
 }

 if (currentSection != "Settings") {
 continue;
 }

 std::string varName;
 auto value = GetConfigSettingsStringValue(line, varName);
 if (varName.empty()) {
 continue;
 }

 double dVal = 0.0;
 int iVal = 0;

 if (varName == "Logging") {
 if (TryParseInt(value, iVal)) logging = iVal;
 } else if (varName == "LeftHandedMode") {
 if (TryParseInt(value, iVal)) leftHandedMode = iVal;
 } else if (varName == "BiteMinSpeed") {
 if (TryParseDouble(value, dVal)) { ClampNonNegative(dVal); biteMinSpeed = dVal; }
 } else if (varName == "BiteMaxSpeed") {
 if (TryParseDouble(value, dVal)) { ClampNonNegative(dVal); biteMaxSpeed = dVal; }
 } else if (varName == "BiteGripStrength") {
 if (TryParseDouble(value, dVal)) { ClampNonNegative(dVal); biteGripStrength = dVal; }
 } else if (varName == "BiteYankSpeed") {
 if (TryParseDouble(value, dVal)) { ClampNonNegative(dVal); biteYankSpeed = dVal; }
 } else if (varName == "BiteReleaseDelay") {
 if (TryParseDouble(value, dVal)) { ClampNonNegative(dVal); biteReleaseDelay = dVal; }
 } else if (varName == "BitePullDistance") {
 if (TryParseDouble(value, dVal)) { ClampNonNegative(dVal); bitePullDistance = dVal; }
 } else if (varName == "BiteLostDistance") {
 if (TryParseDouble(value, dVal)) { ClampNonNegative(dVal); biteLostDistance = dVal; }
 } else if (varName == "BiteVictimMaxDistance") {
 if (TryParseDouble(value, dVal)) { ClampNonNegative(dVal); biteVictimMaxDistance = dVal; }
 } else if (varName == "BiteSafetySeconds") {
 if (TryParseDouble(value, dVal)) { ClampNonNegative(dVal); biteSafetySeconds = dVal; }
 } else if (varName == "BiteShakeLoose") {
 if (TryParseDouble(value, dVal)) { ClampNonNegative(dVal); biteShakeLoose = dVal; }
 } else if (varName == "BiteVictimSpeedFrac") {
 if (TryParseDouble(value, dVal)) { biteVictimSpeedFrac = dVal; ClampUnitFrac(biteVictimSpeedFrac); }
 } else if (varName == "EmbedPlayerStaminaDrainEnabled") {
 if (TryParseInt(value, iVal)) embedPlayerStaminaDrainEnabled = iVal != 0 ? 1 : 0;
 } else if (varName == "EmbedPlayerStaminaDrainPerSec") {
 if (TryParseDouble(value, dVal)) { ClampNonNegative(dVal); embedPlayerStaminaDrainPerSec = dVal; }
 } else if (varName == "EmbedStaminaExhaustRelease") {
 if (TryParseInt(value, iVal)) embedStaminaExhaustRelease = iVal != 0 ? 1 : 0;
 } else if (varName == "EmbedWorldModelEnabled") {
 if (TryParseInt(value, iVal)) embedWorldModelEnabled = iVal != 0 ? 1 : 0;
 } else if (varName == "EmbedArmsAndHandsEnabled") {
 if (TryParseInt(value, iVal)) embedArmsAndHandsEnabled = iVal != 0 ? 1 : 0;
 } else if (varName == "EmbedCooldownSec") {
 if (TryParseDouble(value, dVal)) { ClampNonNegative(dVal); embedCooldownSec = dVal; }
 } else if (varName == "AxeMaxInsertFrac") {
 if (TryParseDouble(value, dVal)) { axeMaxInsertFrac = dVal; ClampUnitFrac(axeMaxInsertFrac); }
 } else if (varName == "AxeMinInitialFrac") {
 if (TryParseDouble(value, dVal)) { axeMinInitialFrac = dVal; ClampUnitFrac(axeMinInitialFrac); }
 } else if (varName == "AxeMaxInitialFrac") {
 if (TryParseDouble(value, dVal)) { axeMaxInitialFrac = dVal; ClampUnitFrac(axeMaxInitialFrac); }
 } else if (varName == "SwordMaxInsertFrac") {
 if (TryParseDouble(value, dVal)) { swordMaxInsertFrac = dVal; ClampUnitFrac(swordMaxInsertFrac); }
 } else if (varName == "SwordMinInitialFrac") {
 if (TryParseDouble(value, dVal)) { swordMinInitialFrac = dVal; ClampUnitFrac(swordMinInitialFrac); }
 } else if (varName == "SwordMaxInitialFrac") {
 if (TryParseDouble(value, dVal)) { swordMaxInitialFrac = dVal; ClampUnitFrac(swordMaxInitialFrac); }
 } else if (varName == "SwordHiltExcludeFrac") {
 if (TryParseDouble(value, dVal)) { swordHiltExcludeFrac = dVal; ClampUnitFrac(swordHiltExcludeFrac); }
 } else if (varName == "SwordMinSpeed") {
 if (TryParseDouble(value, dVal)) { ClampNonNegative(dVal); swordMinSpeed = dVal; }
 } else if (varName == "SwordDepthScale") {
 if (TryParseDouble(value, dVal)) { swordDepthScale = dVal; ClampUnitFrac(swordDepthScale); }
 } else if (varName == "SwordEmbedOffsetTighten") {
 if (TryParseDouble(value, dVal)) { swordEmbedOffsetTighten = dVal; ClampUnitFrac(swordEmbedOffsetTighten); }
 } else if (varName == "EmbedBloodIntervalSec") {
 if (TryParseDouble(value, dVal)) { ClampNonNegative(dVal); embedBloodIntervalSec = dVal; }
 } else if (varName == "LodgedBloodIntervalSec") {
 if (TryParseDouble(value, dVal)) { ClampNonNegative(dVal); lodgedBloodIntervalSec = dVal; }
 } else if (varName == "PostEmbedBloodDurationSec") {
 if (TryParseDouble(value, dVal)) { ClampNonNegative(dVal); postEmbedBloodDurationSec = dVal; }
 } else if (varName == "LodgedHealthDrainPerSec") {
 if (TryParseDouble(value, dVal)) { ClampNonNegative(dVal); lodgedHealthDrainPerSec = dVal; }
 } else if (varName == "LodgedMagickaDrainPerSec") {
 if (TryParseDouble(value, dVal)) { ClampNonNegative(dVal); lodgedMagickaDrainPerSec = dVal; }
 } else if (varName == "LodgedStaminaDrainPerSec") {
 if (TryParseDouble(value, dVal)) { ClampNonNegative(dVal); lodgedStaminaDrainPerSec = dVal; }
 } else if (varName == "LodgedHoldStaminaDrainEnabled") {
 if (TryParseInt(value, iVal)) { lodgedHoldStaminaDrainEnabled = iVal != 0 ? 1 : 0; }
 } else if (varName == "LodgedHoldStaminaDrainPerSec") {
 if (TryParseDouble(value, dVal)) { ClampNonNegative(dVal); lodgedHoldStaminaDrainPerSec = dVal; }
 } else if (varName == "LodgedHoldStaminaMin") {
 if (TryParseDouble(value, dVal)) { ClampNonNegative(dVal); lodgedHoldStaminaMin = dVal; }
 } else if (varName == "LodgedExtractGrabRadius") {
 if (TryParseDouble(value, dVal)) { ClampNonNegative(dVal); lodgedExtractGrabRadius = dVal; }
 } else if (varName == "BiteDistressFaceSeconds") {
 if (TryParseDouble(value, dVal)) { ClampNonNegative(dVal); biteDistressFaceSeconds = dVal; }
 } else if (varName == "VictimMaxDragStep") {
 if (TryParseDouble(value, dVal)) { ClampNonNegative(dVal); victimMaxDragStep = dVal; }
 } else if (varName == "CombatHeadshotHealthFrac") {
 if (TryParseDouble(value, dVal)) { combatHeadshotHealthFrac = dVal; ClampUnitFrac(combatHeadshotHealthFrac); }
 } else if (varName == "HapticBite") {
 if (TryParseDouble(value, dVal)) { ClampNonNegative(dVal); hapticBite = dVal; }
 } else if (varName == "HapticExtract") {
 if (TryParseDouble(value, dVal)) { ClampNonNegative(dVal); hapticExtract = dVal; }
 }
 }

 IW_LOG_INFO("Config: loaded {}", filepath.c_str());
 }

 void Log(int msgLogLevel, const char* fmt, ...)
 {
 if (msgLogLevel > logging) return;

 va_list args;
 va_start(args, fmt);
 char buffer[4096];
 vsnprintf(buffer, sizeof(buffer), fmt, args);
 va_end(args);

 if (msgLogLevel <= LOGLEVEL_ERR) {
 SKSE::log::error("{}", buffer);
 AppendToPluginLog("ERROR", "%s", buffer);
 } else if (msgLogLevel <= LOGLEVEL_WARN) {
 SKSE::log::warn("{}", buffer);
 AppendToPluginLog("WARN", "%s", buffer);
 } else {
 SKSE::log::info("{}", buffer);
 AppendToPluginLog("INFO", "%s", buffer);
 }
 }

} // namespace BitingAxesVR
