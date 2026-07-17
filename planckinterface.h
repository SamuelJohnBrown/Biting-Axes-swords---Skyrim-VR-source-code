#pragma once

// RE::-adapted redeclaration of PLANCK's (activeragdoll) mod-support interface
// (planckinterface001.h). Vtable order and by-value struct layout are preserved
// exactly; legacy skse64 type names mapped to their RE:: equivalents.
//
// Reference: https://github.com/adamhynek/activeragdoll  include/planckinterface001.h

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

namespace PlanckAPI
{
    // Extended melee-hit information PLANCK attaches to its hit events. Layout must match
    // the original exactly (it is returned by value from GetLastHitData()).
    struct PlanckHitData
    {
        RE::NiPoint3 position;               // world position of the hit
        RE::NiPoint3 velocity;               // weapon velocity at impact
        RE::NiPointer<RE::NiAVObject> node;  // the ragdoll node (bone) that was hit
        RE::BSFixedString nodeName;          // name of that node
        bool isLeft;                         // left- or right-hand weapon
    };

    struct IPlanckInterface001
    {
        virtual std::uint32_t GetBuildNumber() = 0;

        virtual bool Deprecated1(const std::string_view& name, double& out) = 0;
        virtual bool Deprecated2(const std::string& name, double val) = 0;

        virtual void AddIgnoredActor(RE::Actor* actor) = 0;
        virtual void RemoveIgnoredActor(RE::Actor* actor) = 0;

        virtual void AddAggressionIgnoredActor(RE::Actor* actor) = 0;
        virtual void RemoveAggressionIgnoredActor(RE::Actor* actor) = 0;

        virtual void SetAggressionLowTopic(RE::Actor* actor, RE::TESTopic* topic) = 0;
        virtual void SetAggressionHighTopic(RE::Actor* actor, RE::TESTopic* topic) = 0;

        virtual void AddRagdollCollisionIgnoredActor(RE::Actor* actor) = 0;
        virtual void RemoveRagdollCollisionIgnoredActor(RE::Actor* actor) = 0;

        // Extended info of the last melee hit PLANCK processed (deliberate copy).
        virtual PlanckHitData GetLastHitData() = 0;
        // The hit event currently being dispatched; only valid inside a hit event.
        // Compare against the event delivered to our sink to confirm a PLANCK hit.
        virtual RE::TESHitEvent* GetCurrentHitEvent() = 0;

        virtual bool GetSettingDouble(const char* name, double& out) = 0;
        virtual bool SetSettingDouble(const char* name, double val) = 0;
    };

    inline IPlanckInterface001* g_planck = nullptr;

    // Fetch PLANCK's interface. Safe to call once PLANCK has processed SKSE
    // kPostPostLoad, i.e. from our plugin's kPostPostLoad handler or later.
    inline IPlanckInterface001* GetPlanckInterface()
    {
        if (g_planck) {
            return g_planck;
        }
        auto* messaging = SKSE::GetMessagingInterface();
        if (!messaging) {
            return nullptr;
        }
        struct PlanckMessage
        {
            enum : std::uint32_t { kMessage_GetInterface = 0x92F38745u };
            void* (*GetApiFunction)(unsigned int revisionNumber) = nullptr;
        } msg;
        messaging->Dispatch(PlanckMessage::kMessage_GetInterface, &msg, sizeof(void*), "PLANCK");
        if (!msg.GetApiFunction) {
            return nullptr;
        }
        g_planck = static_cast<IPlanckInterface001*>(msg.GetApiFunction(1));
        return g_planck;
    }
}
