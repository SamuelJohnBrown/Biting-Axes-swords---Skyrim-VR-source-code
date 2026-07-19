#pragma once

// Mod-support API for Biting Axes & Swords VR.
// Copy this header (and bitingaxesinterface001.cpp fetcher pattern) into consuming mods
// such as False Edge VR. Matches the HIGGS / PLANCK SKSE messaging pattern.

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <cstdint>

namespace BitingAxesAPI
{
    // Must match Biting Axes & Swords VR SKSEPlugin_Query::name exactly (not the DLL filename).
    inline constexpr const char* kInterfaceRecipient = "BitingAxesVR";

    // Provider: call RegisterBitingAxesInterface() from kPostPostLoad (registers with RegisterListener(nullptr, ...)).
    // Consumer: Dispatch(..., kInterfaceRecipient) after PostPostLoad.

    enum class EmbedBodyRegion : std::uint8_t
    {
        Unknown = 0,
        Head,
        Neck,
        Torso,
        Shoulder,
        UpperArm,
        Forearm,
        Hand,
        Thigh,
        Calf,
        Foot,
    };

    // Snapshot of one hand's embed state. Valid when active == true unless noted.
    struct EmbedSnapshot
    {
        bool active = false;
        bool isLeft = false;
        bool useWorldAxe = false;
        bool npcEmbedEligible = false;
        bool pendingWorldAxeGrab = false;
        RE::ActorHandle victimHandle{};
        RE::FormID equippedWeaponFormId = 0;
        RE::FormID worldAxeRefFormId = 0;
        EmbedBodyRegion bodyRegion = EmbedBodyRegion::Unknown;
        RE::BSFixedString embedBoneName;
    };

    struct IBitingAxesInterface001
    {
        virtual std::uint32_t GetBuildNumber() = 0;

        // Query current embed state for the given hand.
        virtual bool IsHandEmbedded(bool isLeft) = 0;
        virtual bool GetHandEmbedSnapshot(bool isLeft, EmbedSnapshot& outSnapshot) = 0;
        virtual bool IsAnyHandEmbedded() = 0;

        // True while the hand holds an active world-model embed (spawned ref, still in embed sim).
        virtual bool IsHandWorldAxeEmbed(bool isLeft) = 0;

        // True when grip can spawn / leave a world-model weapon on a living NPC (standard embed).
        virtual bool CanLeaveWeaponInBody(bool isLeft) = 0;

        // True if the actor has a world weapon left lodged from this mod.
        virtual bool VictimHasLodgedWeapon(RE::Actor* victim) = 0;

        using EmbedCallback = void (*)(bool isLeft, const EmbedSnapshot& snapshot);

        virtual void AddEmbedStartedCallback(EmbedCallback callback) = 0;
        virtual void AddEmbedEndedCallback(EmbedCallback callback) = 0;
        virtual void AddWorldAxeEmbedCallback(EmbedCallback callback) = 0;
        virtual void AddWeaponLodgedInBodyCallback(EmbedCallback callback) = 0;
        // Fired after a lodged world-model weapon is pulled out and re-equipped to the player hand.
        virtual void AddWeaponExtractedFromBodyCallback(EmbedCallback callback) = 0;
    };

    IBitingAxesInterface001* GetBitingAxesInterface001(const SKSE::PluginHandle& pluginHandle,
                                                       SKSE::MessagingInterface* messagingInterface);

    // Provider-side registration (call during plugin startup).
    void RegisterBitingAxesInterface();

    // Provider-side event dispatch (called from embed simulation).
    void NotifyEmbedStarted(const EmbedSnapshot& snapshot);
    void NotifyEmbedEnded(const EmbedSnapshot& snapshot);
    void NotifyWorldAxeEmbed(const EmbedSnapshot& snapshot);
    void NotifyWeaponLodgedInBody(const EmbedSnapshot& snapshot);
    void NotifyWeaponExtractedFromBody(const EmbedSnapshot& snapshot);
}

namespace BitingAxesVR::EmbedApi
{
    bool BuildHandEmbedSnapshot(bool isLeft, BitingAxesAPI::EmbedSnapshot& outSnapshot);
    bool VictimHasLodgedWeapon(RE::Actor* victim);
}
