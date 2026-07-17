#pragma once

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

// Axe-bite simulation: an axe swung into an NPC bites into the flesh and stays
// lodged there. The virtual hand is locked to the embedded axe, and the player
// must pull with real force (overdrive past the stick threshold) to work it free.
//
// Adapted from the ImmersiveWeaponPenetrationVR stab model (stick-slip friction,
// hand-attach via HIGGS pre/post-VRIK callbacks, PLANCK extended hit data), with
// an axe-specific feel: bites trigger on swings, only the head sinks in, and the
// wedge grips much harder on the way out than a blade does.
namespace BitingAxesVR::Bite
{
    // Register the TESHitEvent sink (call at kDataLoaded).
    void RegisterHitSink();

    // Attach the per-frame simulation to HIGGS (call once the HIGGS interface is up).
    void AttachHiggsCallbacks();

    // Drop all embeds silently (call on kPreLoadGame / kNewGame).
    void ResetAllEmbeds();

    // Preload assets that require game data (call at kDataLoaded).
    void PreloadAssets();
}
