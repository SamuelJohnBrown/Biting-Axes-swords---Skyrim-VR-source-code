#pragma once

#include <string>
#include <fstream>
#include <cstdarg>
#include "helper.h"

namespace BitingAxesVR
{
 // General
 extern int logging; // log level threshold (0 = errors only)
 extern int leftHandedMode;

 // Embed eligibility and release
 extern double biteMinSpeed;         // u/s: minimum weapon speed for a bite
 extern double biteMaxSpeed;         // u/s: speed for deepest initial bite
 extern double biteGripStrength;     // reserved (stick-slip wedge; not wired yet)
 extern double biteYankSpeed;          // reserved (committed yank pop-free; not wired yet)
 extern double biteReleaseDelay;       // seconds after embed before pull-back can release
 extern double bitePullDistance;       // Skyrim units of backward pull required to release
 extern double biteLostDistance;       // controller/wound distance that force-releases embed
 extern double biteVictimMaxDistance;  // player-to-NPC distance that force-releases standard embed (0 = off)
 extern double biteSafetySeconds;      // hard cap on embed duration
 extern double biteShakeLoose;         // bone jump in one frame that dislodges embed
 extern double biteVictimSpeedFrac;    // 0..1: embedded victim speed cap; 0 = immobilize
 extern int embedPlayerStaminaDrainEnabled; // 1 = drain player stamina during standard embed
 extern double embedPlayerStaminaDrainPerSec;
 extern int embedStaminaExhaustRelease;     // 1 = unembed when player stamina hits 0
 extern int embedWorldModelEnabled;         // 1 = grip/trigger during NPC embed spawns world weapon
 extern int embedArmsAndHandsEnabled;       // 1 = allow embeds on NPC upper/forearm (not hands)
 extern double embedCooldownSec;            // per-hand cooldown after embed ends before next embed

 // Axe embed depth (fractions of grip->impact length)
 extern double axeMaxInsertFrac;
 extern double axeMinInitialFrac;
 extern double axeMaxInitialFrac;

 // Sword embed depth
 extern double swordMaxInsertFrac;
 extern double swordMinInitialFrac;
 extern double swordMaxInitialFrac;
 extern double swordHiltExcludeFrac;   // proximal blade fraction that cannot bite (0..1)
 extern double swordMinSpeed;          // u/s: minimum speed for sword bite (can be lower than axes)
 extern double swordDepthScale;        // scales sword biteable length for depth (0..1)
 extern double swordEmbedOffsetTighten; // 0..1: reduce swing follow-through lag on stab lock

 // Embedded weapon wound effects (classic hand-held and world-model)
 extern double embedBloodIntervalSec;     // blood FX while player still holds the embed
 extern double lodgedBloodIntervalSec;    // blood FX while world weapon is left in body
 extern double postEmbedBloodDurationSec; // blood FX continues this long after weapon is pulled out
 extern double lodgedHealthDrainPerSec;   // base HP/s; scaled by body region
 extern double lodgedMagickaDrainPerSec; // base magicka/s above legs; scaled by body region
 extern double lodgedStaminaDrainPerSec; // base stamina/s on legs; scaled by body region
 extern int lodgedHoldStaminaDrainEnabled; // master on/off for player stamina while holding lodged NPC
 extern double lodgedHoldStaminaDrainPerSec;
 extern double lodgedHoldStaminaMin;
 extern double lodgedExtractGrabRadius;

 // Victim reactions
 extern double biteDistressFaceSeconds;
 extern double victimMaxDragStep;
 extern double combatHeadshotHealthFrac;  // 0..1: in-combat head embed health threshold

 // Haptics
 extern double hapticBite;
 extern double hapticExtract;

 // Load configuration from Data\SKSE\Plugins\Biting_Axes_&_Swords_VR.ini
 void loadConfig();

 // Simple logging helper (keeps compatibility with old LOG macros)
 void Log(int msgLogLevel, const char* fmt, ...);

 enum eLogLevels
 {
 LOGLEVEL_ERR =0,
 LOGLEVEL_WARN,
 LOGLEVEL_INFO,
 };
}

// Convenience macros matching original project
#define LOG(fmt, ...) BitingAxesVR::Log(BitingAxesVR::LOGLEVEL_WARN, fmt, ##__VA_ARGS__)
#define LOG_ERR(fmt, ...) BitingAxesVR::Log(BitingAxesVR::LOGLEVEL_ERR, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...) BitingAxesVR::Log(BitingAxesVR::LOGLEVEL_INFO, fmt, ##__VA_ARGS__)
