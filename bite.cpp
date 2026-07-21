#include "bite.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "config.h"
#include "engine.h"
#include "helper.h"
#include "higgsinterface.h"
#include "bitingaxesinterface001.h"
#include "openvr_compat.h"
#include "planckinterface.h"

#ifdef GetObject
#undef GetObject
#endif

namespace
{
    // Skyrim world units <-> Havok (meters). Engine constants (~69.99 units/m).
    constexpr float kHavokToSkyrim = 69.99125f;
    constexpr float kSkyrimToHavok = 0.0142875f;

    // Visual lock (legacy stick-slip; unused).
    constexpr float kExitGrace = 3.0f;

    // Stick-slip friction: the axe head's depth is SIMULATED and chases the controller
    // through friction. The wedge holds the axe in place against small hand motion, a
    // deliberate pull works it out in under a second, and a committed yank pops it free.
    constexpr float kStickThreshold = 4.0f;   // hand wanders this far before the head moves at all
    constexpr float kSlipGain = 8.0f;         // 1/s: slip speed per unit of overdrive past the threshold
    constexpr float kMaxSlideSpeed = 55.0f;   // u/s damper cap
    constexpr float kInsertMul = 1.15f;       // chopping deeper is only slightly easier
    constexpr float kDepthSlow = 0.6f;        // deeper = slower (cap shrinks with embedded fraction)
    constexpr float kCatchStart = 0.9f;       // exit-catch: nearly the whole (shallow) channel resists
    constexpr float kCatchSlowMul = 0.6f;     // slide cap multiplier inside the catch

    // Momentum -> instant initial bite (hard chop sinks the whole edge, glancing blow barely bites).
    constexpr float kBodyTugGain = 7.0f;      // 1/s of overdrive -> bone velocity (axes lever hard)
    constexpr float kBodyTugMax = 130.0f;     // u/s cap on the tug

    // Body reaction: friction's reaction force tugs the victim along the bite axis only.
    static constexpr const char* kAxeBiteESP = "Axe_bite_VR.esp";
    static constexpr std::uint32_t kDislodgeSoundLocalFormID = 0x000800u;
    static RE::BGSSoundDescriptorForm* g_dislodgeSound = nullptr;

    // Vanilla blood impact dataset (Skyrim.esm), same as Throat Slit VR.
    static constexpr RE::FormID kBloodImpactDataSetFormId = 0x0001F82Au;
    static RE::BGSImpactDataSet* g_bloodImpactDataSet = nullptr;

    // ESL/light plugins use 0xFE000000 | (smallFileCompileIndex << 12) | localFormId,
    // NOT (modIndex << 24) | localFormId. Load order shifts the middle nibble, so we must
    // resolve the plugin by name instead of hardcoding 0xFE000800.
    RE::BGSSoundDescriptorForm* LookupESLSoundDescriptor(const char* espName, std::uint32_t localFormId)
    {
        auto* handler = RE::TESDataHandler::GetSingleton();
        if (!handler) {
            return nullptr;
        }

        const RE::TESFile* lightMod = handler->LookupLoadedLightModByName(espName);
        if (!lightMod) {
            IW_LOG_WARN("[bite] ESL plugin '{}' not found in loaded light mods", espName);
            return nullptr;
        }

        const auto lightIdx = handler->GetLoadedLightModIndex(espName);
        if (!lightIdx) {
            IW_LOG_WARN("[bite] ESL plugin '{}' has no light-mod index", espName);
            return nullptr;
        }

        const std::uint32_t runtimeId =
            0xFE000000u | (static_cast<std::uint32_t>(*lightIdx) << 12u) | (localFormId & 0xFFFu);

        auto* form = RE::TESForm::LookupByID(runtimeId);
        if (!form) {
            IW_LOG_WARN("[bite] SNDR not in form map: runtimeId=0x{:08X} esp='{}' lightIdx={} local=0x{:03X}",
                        runtimeId, espName, *lightIdx, localFormId & 0xFFFu);
            return nullptr;
        }

        if (!form->Is(RE::FormType::SoundRecord)) {
            IW_LOG_WARN("[bite] form 0x{:08X} is type {} (expected SNDR)", runtimeId,
                        static_cast<std::uint32_t>(form->GetFormType()));
            return nullptr;
        }

        auto* sound = form->As<RE::BGSSoundDescriptorForm>();
        if (sound) {
            IW_LOG_INFO("[bite] loaded ESL dislodge sound '{}' -> runtimeId=0x{:08X} (lightIdx={})",
                        espName, runtimeId, *lightIdx);
        }
        return sound;
    }

    RE::BGSSoundDescriptorForm* EnsureDislodgeSound()
    {
        if (g_dislodgeSound) {
            return g_dislodgeSound;
        }

        // Primary path: resolve ESL plugin by name + local form id 0x800.
        if (auto* sound = LookupESLSoundDescriptor(kAxeBiteESP, kDislodgeSoundLocalFormID)) {
            g_dislodgeSound = sound;
            return g_dislodgeSound;
        }

        // Fallback: xEdit-style id if this ESL happens to be at light index 0.
        const std::uint32_t editorStyleId = 0xFE000000u | kDislodgeSoundLocalFormID;
        if (auto* form = RE::TESForm::LookupByID(editorStyleId)) {
            if (form->Is(RE::FormType::SoundRecord)) {
                g_dislodgeSound = form->As<RE::BGSSoundDescriptorForm>();
                if (g_dislodgeSound) {
                    IW_LOG_INFO("[bite] loaded dislodge sound via editor-style id 0x{:08X}", editorStyleId);
                    return g_dislodgeSound;
                }
            }
        }

        IW_LOG_WARN("[bite] dislodge sound not found ('{}' local=0x{:03X})",
                    kAxeBiteESP, kDislodgeSoundLocalFormID);
        return nullptr;
    }

    void PlayDislodgeSound(const RE::NiPoint3& position, RE::NiAVObject* followNode)
    {
        auto* sound = EnsureDislodgeSound();
        if (!sound) {
            return;
        }

        auto* audio = RE::BSAudioManager::GetSingleton();
        if (!audio) {
            return;
        }

        RE::BSSoundHandle handle;
        if (!audio->BuildSoundDataFromDescriptor(handle, static_cast<RE::BSISoundDescriptor*>(sound), 16)) {
            IW_LOG_WARN("[bite] BuildSoundDataFromDescriptor failed for dislodge sound");
            return;
        }
        if (handle.soundID == static_cast<std::uint32_t>(-1)) {
            return;
        }

        handle.SetPosition(position);
        if (followNode) {
            handle.SetObjectToFollow(followNode);
        }
        handle.SetVolume(1.0f);

        if (handle.Play()) {
            IW_LOG_INFO("[bite] playing dislodge sound");
        }
    }

    void ClearDislodgeSoundCache()
    {
        g_dislodgeSound = nullptr;
    }

    RE::BGSImpactDataSet* EnsureBloodImpactDataSet()
    {
        if (g_bloodImpactDataSet) {
            return g_bloodImpactDataSet;
        }

        auto* form = RE::TESForm::LookupByID(kBloodImpactDataSetFormId);
        if (!form || !form->Is(RE::FormType::ImpactDataSet)) {
            IW_LOG_WARN("[bite] blood impact dataset not found (formId=0x{:08X})", kBloodImpactDataSetFormId);
            return nullptr;
        }

        g_bloodImpactDataSet = form->As<RE::BGSImpactDataSet>();
        if (g_bloodImpactDataSet) {
            IW_LOG_INFO("[bite] loaded blood impact dataset formId=0x{:08X}", kBloodImpactDataSetFormId);
        }
        return g_bloodImpactDataSet;
    }

    void ClearBloodImpactDataCache()
    {
        g_bloodImpactDataSet = nullptr;
    }

    bool PlayWoundBloodImpact(RE::Actor* victim, const RE::BSFixedString& boneName, const RE::NiPoint3& pickDirection,
                              bool applyNodeRotation)
    {
        auto* impactSet = EnsureBloodImpactDataSet();
        auto* impactManager = RE::BGSImpactManager::GetSingleton();
        if (!victim || !impactSet || !impactManager || boneName.empty() || !victim->Get3D()) {
            return false;
        }

        RE::NiPoint3 direction = pickDirection;
        return impactManager->PlayImpactEffect(
            victim,
            impactSet,
            boneName,
            direction,
            100.0f,
            applyNodeRotation,
            false);
    }

    // ---- Weapon classification ---------------------------------------------

    struct WeaponProfile
    {
        bool allowed = false;
        float maxInsertFrac = 0.0f;   // deepest the head can sink: fraction of grip->impact length
        float minInitialFrac = 0.0f;  // fraction buried on the slowest qualifying chop
        float maxInitialFrac = 0.0f;  // fraction buried on the hardest chop
        float slideMul = 1.0f;        // slide-speed multiplier (lower = grips harder)
        float hiltExcludeFrac = 0.0f; // proximal blade fraction that cannot bite (swords: guard/hilt)
        float minBiteSpeed = 0.0f;    // per-weapon min speed gate; 0 = use global BiteMinSpeed
        bool allowsHeadshotKill = false;
        const char* name = "none";
    };

    // Weapon bite profiles: fraction of grip->impact length buried on embed (values from INI).
    WeaponProfile MakeAxeProfile()
    {
        WeaponProfile p{};
        p.allowed = true;
        p.maxInsertFrac = static_cast<float>(BitingAxesVR::axeMaxInsertFrac);
        p.minInitialFrac = static_cast<float>(BitingAxesVR::axeMinInitialFrac);
        p.maxInitialFrac = static_cast<float>(BitingAxesVR::axeMaxInitialFrac);
        p.slideMul = 0.45f;
        p.hiltExcludeFrac = 0.0f;
        p.minBiteSpeed = static_cast<float>(BitingAxesVR::biteMinSpeed);
        p.allowsHeadshotKill = true;
        p.name = "axe";
        return p;
    }

    WeaponProfile MakeSwordProfile()
    {
        WeaponProfile p{};
        p.allowed = true;
        p.maxInsertFrac = static_cast<float>(BitingAxesVR::swordMaxInsertFrac);
        p.minInitialFrac = static_cast<float>(BitingAxesVR::swordMinInitialFrac);
        p.maxInitialFrac = static_cast<float>(BitingAxesVR::swordMaxInitialFrac);
        p.slideMul = 0.45f;
        p.hiltExcludeFrac = static_cast<float>(BitingAxesVR::swordHiltExcludeFrac);
        p.minBiteSpeed = static_cast<float>(BitingAxesVR::swordMinSpeed);
        p.allowsHeadshotKill = false;
        p.name = "sword";
        return p;
    }

    float GetProfileMinBiteSpeed(const WeaponProfile& profile)
    {
        if (profile.minBiteSpeed > 0.0f) {
            return profile.minBiteSpeed;
        }
        return static_cast<float>(BitingAxesVR::biteMinSpeed);
    }

    bool IsSwordProfile(const WeaponProfile& profile)
    {
        return profile.hiltExcludeFrac > 0.0f;
    }

    constexpr WeaponProfile kNoProfile{};

    WeaponProfile ClassifyWeapon(const RE::TESObjectWEAP* weap)
    {
        if (!weap) {
            return kNoProfile;
        }
        switch (weap->GetWeaponType()) {
            case RE::WEAPON_TYPE::kOneHandAxe:
            case RE::WEAPON_TYPE::kTwoHandAxe:
                return MakeAxeProfile();
            case RE::WEAPON_TYPE::kOneHandSword:
            case RE::WEAPON_TYPE::kTwoHandSword:
                return MakeSwordProfile();
            default:
                return kNoProfile;
        }
    }

    const RE::TESObjectWEAP* GetEquippedWeapon(bool isLeft)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return nullptr;
        }
        auto* form = player->GetEquippedObject(isLeft);
        return form ? form->As<RE::TESObjectWEAP>() : nullptr;
    }

    // Skyrim.esm bound conjured weapons — standard embed OK, world-model leave-in-body blocked.
    bool IsBoundConjuredWeapon(const RE::TESObjectWEAP* weap)
    {
        if (!weap) {
            return false;
        }

        switch (weap->GetFormID() & 0xFFFFFFu) {
            case 0x00058f5e: // Bound Battleaxe
            case 0x000424f7: // Bound Mystic Battleaxe
            case 0x00058f5f: // Bound Sword
            case 0x000424f9: // Bound Mystic Sword
            case 0x000ba30e: // Alternative Bound Sword
                return true;
            default:
                return false;
        }
    }

    bool CanLeaveEquippedWeaponInBody(const RE::TESObjectWEAP* weap)
    {
        return weap && !IsBoundConjuredWeapon(weap);
    }

    // ---- math helpers -------------------------------------------------------

    float Dot(const RE::NiPoint3& a, const RE::NiPoint3& b)
    {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    // Shortest-arc rotation taking unit vector a onto unit vector b (Rodrigues).
    RE::NiMatrix3 RotationBetween(const RE::NiPoint3& a, const RE::NiPoint3& b)
    {
        RE::NiMatrix3 out;
        const float c = Dot(a, b);
        if (c > 0.9999f) {  // already aligned -> identity
            out.entry[0][0] = 1.0f; out.entry[0][1] = 0.0f; out.entry[0][2] = 0.0f;
            out.entry[1][0] = 0.0f; out.entry[1][1] = 1.0f; out.entry[1][2] = 0.0f;
            out.entry[2][0] = 0.0f; out.entry[2][1] = 0.0f; out.entry[2][2] = 1.0f;
            return out;
        }
        if (c < -0.9999f) {  // opposite: 180 degrees about any perpendicular axis
            RE::NiPoint3 axis = a.Cross(RE::NiPoint3{ 1.0f, 0.0f, 0.0f });
            if (axis.Length() < 1e-4f) {
                axis = a.Cross(RE::NiPoint3{ 0.0f, 1.0f, 0.0f });
            }
            axis = axis / axis.Length();
            out.entry[0][0] = 2.0f * axis.x * axis.x - 1.0f;
            out.entry[0][1] = 2.0f * axis.x * axis.y;
            out.entry[0][2] = 2.0f * axis.x * axis.z;
            out.entry[1][0] = 2.0f * axis.x * axis.y;
            out.entry[1][1] = 2.0f * axis.y * axis.y - 1.0f;
            out.entry[1][2] = 2.0f * axis.y * axis.z;
            out.entry[2][0] = 2.0f * axis.x * axis.z;
            out.entry[2][1] = 2.0f * axis.y * axis.z;
            out.entry[2][2] = 2.0f * axis.z * axis.z - 1.0f;
            return out;
        }
        const RE::NiPoint3 v = a.Cross(b);
        const float k = 1.0f / (1.0f + c);
        const float vv = Dot(v, v);
        out.entry[0][0] = 1.0f + (v.x * v.x - vv) * k;
        out.entry[0][1] = -v.z + v.x * v.y * k;
        out.entry[0][2] = v.y + v.x * v.z * k;
        out.entry[1][0] = v.z + v.x * v.y * k;
        out.entry[1][1] = 1.0f + (v.y * v.y - vv) * k;
        out.entry[1][2] = -v.x + v.y * v.z * k;
        out.entry[2][0] = -v.y + v.x * v.z * k;
        out.entry[2][1] = v.x + v.y * v.z * k;
        out.entry[2][2] = 1.0f + (v.z * v.z - vv) * k;
        return out;
    }

    // ---- node helpers -------------------------------------------------------

    // The node the equipped weapon hangs from: right-hand weapons attach to "WEAPON",
    // left-hand weapons to "SHIELD" (standard skeleton biped attachment nodes).
    RE::NiAVObject* GetPlayerWeaponNode(bool isLeft)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return nullptr;
        }
        auto* root = player->Get3D();
        if (!root) {
            return nullptr;
        }
        return root->GetObjectByName(isLeft ? "SHIELD" : "WEAPON");
    }

    RE::NiAVObject* GetHandNode(bool isLeft)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return nullptr;
        }
        auto* root = player->Get3D();
        if (!root) {
            return nullptr;
        }
        return root->GetObjectByName(isLeft ? "NPC L Hand [LHnd]" : "NPC R Hand [RHnd]");
    }

    // The Havok rigid body backing a scene node (e.g. an NPC ragdoll bone).
    RE::bhkRigidBody* GetRigidBody(RE::NiAVObject* node)
    {
        if (!node || !node->collisionObject) {
            return nullptr;
        }
        auto* nico = netimmerse_cast<RE::bhkNiCollisionObject*>(node->collisionObject.get());
        if (!nico || !nico->body) {
            return nullptr;
        }
        return netimmerse_cast<RE::bhkRigidBody*>(nico->body.get());
    }

    float HkLinearSpeed(const RE::hkVector4& vel)
    {
        const float* f = reinterpret_cast<const float*>(&vel.quad);
        return std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    }

    void ClampHkLinearSpeed(RE::hkVector4& vel, float maxSpeedHavok)
    {
        float* f = reinterpret_cast<float*>(&vel.quad);
        const float len = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
        if (len > maxSpeedHavok && len > 1e-6f) {
            const float scale = maxSpeedHavok / len;
            f[0] *= scale;
            f[1] *= scale;
            f[2] *= scale;
        }
    }

    void ClampNiPoint3Speed(RE::NiPoint3& vel, float maxSpeed)
    {
        const float len = vel.Length();
        if (len > maxSpeed && len > 1e-4f) {
            const float scale = maxSpeed / len;
            vel.x *= scale;
            vel.y *= scale;
            vel.z *= scale;
        }
    }

    void ClampRigidBodyLinearSpeed(RE::bhkRigidBody* rb, float maxSpeedSkyrim)
    {
        if (!rb || maxSpeedSkyrim <= 0.0f) {
            return;
        }

        auto* hkRb = rb->GetRigidBody();
        if (!hkRb) {
            return;
        }

        auto* entity = static_cast<RE::hkpEntity*>(hkRb);
        auto* motion = static_cast<RE::hkpMotion*>(&entity->motion);
        const float maxHavok = maxSpeedSkyrim * kSkyrimToHavok;

        RE::hkVector4 linVel = motion->linearVelocity;
        if (HkLinearSpeed(linVel) > maxHavok) {
            ClampHkLinearSpeed(linVel, maxHavok);
            rb->SetLinearVelocity(linVel);
        }
    }

    // Body reaction: embedded victims track the embedding controller in 3D (HIGGS-grab feel).
    template <class Fn>
    void ForEachAVObject(RE::NiAVObject* node, Fn&& fn)
    {
        if (!node) {
            return;
        }

        fn(node);

        if (auto* niNode = node->AsNode()) {
            for (auto& child : niNode->GetChildren()) {
                if (child) {
                    ForEachAVObject(child.get(), fn);
                }
            }
        }
    }

    float MeasureWeaponExtentAlongAxis(RE::NiAVObject* weaponRoot, const RE::NiPoint3& originWorld,
                                       const RE::NiPoint3& axisUnit)
    {
        if (!weaponRoot) {
            return 0.0f;
        }

        float maxAlong = 0.0f;
        ForEachAVObject(weaponRoot, [&](RE::NiAVObject* node) {
            const RE::NiPoint3 rel = node->world.translate - originWorld;
            const float nodeAlong = Dot(rel, axisUnit);
            if (nodeAlong > maxAlong) {
                maxAlong = nodeAlong;
            }

            const float centerAlong = Dot(node->worldBound.center - originWorld, axisUnit);
            const float boundAlong = centerAlong + node->worldBound.radius;
            if (boundAlong > maxAlong) {
                maxAlong = boundAlong;
            }
        });
        return maxAlong;
    }

    template <class Fn>
    void ForEachRigidBody(RE::NiAVObject* node, Fn&& fn)
    {
        if (!node) {
            return;
        }

        if (auto* rb = GetRigidBody(node)) {
            fn(rb);
        }

        if (auto* niNode = node->AsNode()) {
            for (auto& child : niNode->GetChildren()) {
                if (child) {
                    ForEachRigidBody(child.get(), fn);
                }
            }
        }
    }

    // TRUE controller position via HIGGS's hand rigid body (tracks the real controller even
    // while we override the virtual hand node). Returns false if unavailable.
    bool GetRealHandPos(bool isLeft, RE::NiPoint3& out)
    {
        if (!g_higgsInterface) {
            return false;
        }
        auto* obj = g_higgsInterface->GetHandRigidBody(isLeft);
        auto* rb = obj ? netimmerse_cast<RE::bhkRigidBody*>(obj) : nullptr;
        if (!rb) {
            return false;
        }
        RE::hkVector4 pos;
        rb->GetPosition(pos);
        const float* p = reinterpret_cast<const float*>(&pos.quad);
        out = { p[0] * kHavokToSkyrim, p[1] * kHavokToSkyrim, p[2] * kHavokToSkyrim };
        return true;
    }

    bool GetWeaponLinearVelocitySkyrim(bool isLeft, RE::NiPoint3& outVel)
    {
        if (!g_higgsInterface) {
            return false;
        }

        auto* obj = g_higgsInterface->GetWeaponRigidBody(isLeft);
        auto* rb = obj ? netimmerse_cast<RE::bhkRigidBody*>(obj) : nullptr;
        if (!rb) {
            return false;
        }

        auto* hkRb = rb->GetRigidBody();
        if (!hkRb) {
            return false;
        }

        auto* entity = static_cast<RE::hkpEntity*>(hkRb);
        auto* motion = static_cast<RE::hkpMotion*>(&entity->motion);
        const float* v = reinterpret_cast<const float*>(&motion->linearVelocity.quad);
        outVel = { v[0] * kHavokToSkyrim, v[1] * kHavokToSkyrim, v[2] * kHavokToSkyrim };
        return true;
    }

    RE::NiPoint3 GetPlayerHorizontalRight()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return { 1.0f, 0.0f, 0.0f };
        }

        auto* root = player->Get3D();
        if (!root) {
            return { 1.0f, 0.0f, 0.0f };
        }

        RE::NiPoint3 right{ root->world.rotate.entry[0][0], root->world.rotate.entry[1][0],
                          root->world.rotate.entry[2][0] };
        right.z = 0.0f;
        const float len = right.Length();
        if (len < 1e-4f) {
            return { 1.0f, 0.0f, 0.0f };
        }
        return right * (1.0f / len);
    }

    RE::NiPoint3 MatrixColumn(const RE::NiMatrix3& m, int col)
    {
        return { m.entry[0][col], m.entry[1][col], m.entry[2][col] };
    }

    bool TryGetWeaponBladeFlatNormal(RE::NiAVObject* weaponRoot, RE::NiPoint3& outFlatNormal)
    {
        if (!weaponRoot) {
            return false;
        }

        const RE::NiMatrix3& rot = weaponRoot->world.rotate;
        const RE::NiPoint3 axes[3] = { MatrixColumn(rot, 0), MatrixColumn(rot, 1), MatrixColumn(rot, 2) };
        const RE::NiPoint3 grip = weaponRoot->world.translate;

        int bladeAxis = 0;
        float bladeLen = 0.0f;
        for (int i = 0; i < 3; ++i) {
            RE::NiPoint3 axis = axes[i];
            const float axisLen = axis.Length();
            if (axisLen < 1e-4f) {
                continue;
            }
            axis = axis * (1.0f / axisLen);
            const float extent = MeasureWeaponExtentAlongAxis(weaponRoot, grip, axis);
            if (extent > bladeLen) {
                bladeLen = extent;
                bladeAxis = i;
            }
        }

        if (bladeLen < 1e-2f) {
            return false;
        }

        int flatAxis = (bladeAxis + 1) % 3;
        float flatLen = 0.0f;
        for (int i = 0; i < 3; ++i) {
            if (i == bladeAxis) {
                continue;
            }
            RE::NiPoint3 axis = axes[i];
            const float axisLen = axis.Length();
            if (axisLen < 1e-4f) {
                continue;
            }
            axis = axis * (1.0f / axisLen);
            const float extent = MeasureWeaponExtentAlongAxis(weaponRoot, grip, axis);
            if (flatLen < 1e-4f || extent < flatLen) {
                flatLen = extent;
                flatAxis = i;
            }
        }

        RE::NiPoint3 flatNormal = axes[flatAxis];
        const float flatNormalLen = flatNormal.Length();
        if (flatNormalLen < 1e-4f) {
            return false;
        }

        outFlatNormal = flatNormal * (1.0f / flatNormalLen);
        return true;
    }

    bool IsControllerPhysicallyFlat(bool isLeft)
    {
        auto* hand = GetHandNode(isLeft);
        if (!hand) {
            return false;
        }

        // When the controller is held paddle-flat, its local up axis is mostly horizontal in world space.
        const RE::NiPoint3 ctrlUp = MatrixColumn(hand->world.rotate, 1);
        return std::abs(ctrlUp.z) < 0.50f;
    }

    constexpr float kFlatSwingHorizMinSpeed = 80.0f;
    constexpr float kFlatSwingHorizMaxVerticalFrac = 0.45f;
    constexpr float kFlatSwingLateralDotMin = 0.55f;
    constexpr float kFlatSwingFaceAlignMin = 0.55f;
    constexpr float kFlatSwingLogCooldownSec = 0.35f;

    struct FlatLateralSwingSample
    {
        bool valid = false;
        const char* dirName = nullptr;
        float speed = 0.0f;
        float flatAlign = 0.0f;
        bool controllerFlat = false;
        const char* weaponName = "none";
    };

    struct FlatSwingDiagState
    {
        bool active = false;
        std::chrono::steady_clock::time_point lastLogTime{};
    };

    FlatSwingDiagState g_flatSwingDiag[2];

    FlatLateralSwingSample AnalyzeFlatLateralSwing(bool isLeft, const RE::NiPoint3& velocity,
                                                     const RE::NiPoint3& playerRightHoriz)
    {
        FlatLateralSwingSample sample{};

        RE::NiPoint3 horizVel{ velocity.x, velocity.y, 0.0f };
        const float horizSpeed = horizVel.Length();
        sample.speed = velocity.Length();
        if (horizSpeed < kFlatSwingHorizMinSpeed ||
            std::abs(velocity.z) > horizSpeed * kFlatSwingHorizMaxVerticalFrac) {
            return sample;
        }

        horizVel = horizVel * (1.0f / horizSpeed);
        const float lateralDot = Dot(horizVel, playerRightHoriz);
        if (lateralDot >= kFlatSwingLateralDotMin) {
            sample.dirName = "right";
        } else if (lateralDot <= -kFlatSwingLateralDotMin) {
            sample.dirName = "left";
        } else {
            return sample;
        }

        auto* weapon = GetPlayerWeaponNode(isLeft);
        RE::NiPoint3 flatNormal{};
        if (!TryGetWeaponBladeFlatNormal(weapon, flatNormal)) {
            return sample;
        }

        sample.flatAlign = std::abs(Dot(flatNormal, horizVel));
        sample.controllerFlat = IsControllerPhysicallyFlat(isLeft);
        if (sample.flatAlign < kFlatSwingFaceAlignMin && !sample.controllerFlat) {
            return sample;
        }

        const WeaponProfile profile = ClassifyWeapon(GetEquippedWeapon(isLeft));
        sample.weaponName = profile.name;
        sample.valid = true;
        return sample;
    }

    void LogFlatLateralSwing(bool isLeft, const FlatLateralSwingSample& sample, const char* trigger)
    {
        IW_LOG_INFO(
            "[bite][FLAT-SWING] trigger={}  hand={}  dir={}  speed={:.0f}  flatAlign={:.2f}  ctrlFlat={}  "
            "weapon={}  (blade face leading — should not embed)",
            trigger, isLeft ? "L" : "R", sample.dirName, sample.speed, sample.flatAlign,
            sample.controllerFlat ? "Y" : "N", sample.weaponName);
    }

    void UpdateFlatSwingDiagnostics()
    {
        if (!g_higgsInterface) {
            return;
        }

        const RE::NiPoint3 playerRight = GetPlayerHorizontalRight();
        const auto now = std::chrono::steady_clock::now();

        for (bool isLeft : { false, true }) {
            const WeaponProfile profile = ClassifyWeapon(GetEquippedWeapon(isLeft));
            if (!profile.allowed) {
                g_flatSwingDiag[isLeft ? 1 : 0].active = false;
                continue;
            }

            RE::NiPoint3 velocity{};
            if (!GetWeaponLinearVelocitySkyrim(isLeft, velocity)) {
                g_flatSwingDiag[isLeft ? 1 : 0].active = false;
                continue;
            }

            const FlatLateralSwingSample sample = AnalyzeFlatLateralSwing(isLeft, velocity, playerRight);
            auto& state = g_flatSwingDiag[isLeft ? 1 : 0];
            if (!sample.valid) {
                state.active = false;
                continue;
            }

            if (state.active) {
                continue;
            }

            if (state.lastLogTime.time_since_epoch().count() != 0) {
                const float sinceLast =
                    std::chrono::duration<float>(now - state.lastLogTime).count();
                if (sinceLast < kFlatSwingLogCooldownSec) {
                    continue;
                }
            }

            LogFlatLateralSwing(isLeft, sample, "swing");
            state.active = true;
            state.lastLogTime = now;
        }
    }

    bool EmbedGameHandIsPhysicalLeft(bool isLeftGameHand)
    {
        return RE::BSOpenVRControllerDevice::IsLeftHandedMode() ? !isLeftGameHand : isLeftGameHand;
    }

    bool IsGripPressedViaSkyrimInput(bool isLeftGameHand)
    {
        auto* mgr = RE::BSInputDeviceManager::GetSingleton();
        if (!mgr) {
            return false;
        }
        const bool physicalLeft = EmbedGameHandIsPhysicalLeft(isLeftGameHand);
        auto* dev = physicalLeft ? mgr->GetVRControllerLeft() : mgr->GetVRControllerRight();
        if (!dev) {
            return false;
        }
        using Key = RE::BSOpenVRControllerDevice::Keys;
        return dev->IsPressed(Key::kGrip) || dev->IsPressed(Key::kGripAlt);
    }

    bool IsGripPressedOnEmbedHand(bool isLeftGameHand)
    {
        auto* vr = RE::BSOpenVR::GetSingleton();
        if (vr && vr->vrSystem) {
            const bool physicalLeft = EmbedGameHandIsPhysicalLeft(isLeftGameHand);
            const auto device = vr->GetTrackedDeviceIndexForHand(!physicalLeft);
            if (device != OpenVRCompat::kInvalidDevice) {
                OpenVRCompat::ControllerState state{};
                if (OpenVRCompat::GetControllerState(vr->vrSystem, device, &state)) {
                    const bool gripButton = (state.ulButtonPressed & OpenVRCompat::kGripButtonMask) != 0;
                    const bool gripAxis = state.rAxis[2].x > 0.5f || state.rAxis[2].y > 0.5f;
                    if (gripButton || gripAxis) {
                        return true;
                    }
                }
            }
        }

        return IsGripPressedViaSkyrimInput(isLeftGameHand);
    }

    bool IsTriggerPressedViaSkyrimInput(bool isLeftGameHand)
    {
        auto* mgr = RE::BSInputDeviceManager::GetSingleton();
        if (!mgr) {
            return false;
        }
        const bool physicalLeft = EmbedGameHandIsPhysicalLeft(isLeftGameHand);
        auto* dev = physicalLeft ? mgr->GetVRControllerLeft() : mgr->GetVRControllerRight();
        if (!dev) {
            return false;
        }
        using Key = RE::BSOpenVRControllerDevice::Keys;
        return dev->IsPressed(Key::kTrigger);
    }

    bool IsTriggerPressedOnEmbedHand(bool isLeftGameHand)
    {
        auto* vr = RE::BSOpenVR::GetSingleton();
        if (vr && vr->vrSystem) {
            const bool physicalLeft = EmbedGameHandIsPhysicalLeft(isLeftGameHand);
            const auto device = vr->GetTrackedDeviceIndexForHand(!physicalLeft);
            if (device != OpenVRCompat::kInvalidDevice) {
                OpenVRCompat::ControllerState state{};
                if (OpenVRCompat::GetControllerState(vr->vrSystem, device, &state)) {
                    const bool triggerButton = (state.ulButtonPressed & OpenVRCompat::kTriggerButtonMask) != 0;
                    const bool triggerAxis = state.rAxis[1].x > 0.5f;
                    if (triggerButton || triggerAxis) {
                        return true;
                    }
                }
            }
        }

        return IsTriggerPressedViaSkyrimInput(isLeftGameHand);
    }

    // False Edge VR uses grip for its own weapon tracking — use trigger for world-model actions.
    bool EmbedWorldModelFeatureEnabled()
    {
        return BitingAxesVR::embedWorldModelEnabled != 0;
    }

    bool UseTriggerForWorldAxeInput()
    {
        if (!EmbedWorldModelFeatureEnabled()) {
            return false;
        }
        static const bool s_useTrigger = [] {
            if (BitingAxesVR::IsFalseEdgeVRPresent()) {
                IW_LOG_INFO("False Edge VR detected — world-model embed/regrab uses trigger (grip unchanged for HIGGS hold)");
            }
            return BitingAxesVR::IsFalseEdgeVRPresent();
        }();
        return s_useTrigger;
    }

    const char* WorldAxeInputButtonName()
    {
        return UseTriggerForWorldAxeInput() ? "trigger" : "grip";
    }

    bool IsWorldAxeInputPressedOnEmbedHand(bool isLeftGameHand)
    {
        return UseTriggerForWorldAxeInput() ? IsTriggerPressedOnEmbedHand(isLeftGameHand)
                                            : IsGripPressedOnEmbedHand(isLeftGameHand);
    }

    // Controller vibration (the sensory stand-in for force feedback). strength ~ 0..1.
    void Haptic(bool isLeft, float strength)
    {
        if (strength <= 0.02f) {
            return;
        }
        if (strength > 1.0f) {
            strength = 1.0f;
        }
        if (auto* vr = RE::BSOpenVR::GetSingleton()) {
            vr->TriggerHapticPulse(!isLeft, strength * 4.0f);  // up to ~16ms buzz this frame
        }
    }

    // ---- Embed state (one independent slot per hand) ------------------------

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

    const char* EmbedBodyRegionName(EmbedBodyRegion region)
    {
        switch (region) {
            case EmbedBodyRegion::Head:
                return "head";
            case EmbedBodyRegion::Neck:
                return "neck";
            case EmbedBodyRegion::Torso:
                return "torso";
            case EmbedBodyRegion::Shoulder:
                return "shoulder";
            case EmbedBodyRegion::UpperArm:
                return "upper_arm";
            case EmbedBodyRegion::Forearm:
                return "forearm";
            case EmbedBodyRegion::Hand:
                return "hand";
            case EmbedBodyRegion::Thigh:
                return "thigh";
            case EmbedBodyRegion::Calf:
                return "calf";
            case EmbedBodyRegion::Foot:
                return "foot";
            default:
                return "unknown";
        }
    }

    float EmbedBodyRegionHealthMultiplier(EmbedBodyRegion region)
    {
        switch (region) {
            case EmbedBodyRegion::Head:
                return 2.0f;
            case EmbedBodyRegion::Neck:
                return 1.75f;
            case EmbedBodyRegion::Torso:
                return 1.5f;
            case EmbedBodyRegion::Shoulder:
                return 1.25f;
            case EmbedBodyRegion::UpperArm:
                return 1.0f;
            case EmbedBodyRegion::Thigh:
                return 0.9f;
            case EmbedBodyRegion::Forearm:
                return 0.85f;
            case EmbedBodyRegion::Calf:
                return 0.8f;
            case EmbedBodyRegion::Hand:
                return 0.75f;
            case EmbedBodyRegion::Foot:
                return 0.7f;
            default:
                return 1.0f;
        }
    }

    bool EmbedBodyRegionDrainsMagicka(EmbedBodyRegion region)
    {
        switch (region) {
            case EmbedBodyRegion::Head:
            case EmbedBodyRegion::Neck:
            case EmbedBodyRegion::Torso:
            case EmbedBodyRegion::Shoulder:
            case EmbedBodyRegion::UpperArm:
            case EmbedBodyRegion::Forearm:
            case EmbedBodyRegion::Hand:
                return true;
            default:
                return false;
        }
    }

    bool EmbedBodyRegionDrainsStamina(EmbedBodyRegion region)
    {
        switch (region) {
            case EmbedBodyRegion::Thigh:
            case EmbedBodyRegion::Calf:
            case EmbedBodyRegion::Foot:
                return true;
            default:
                return false;
        }
    }

    struct EmbedState
    {
        bool active = false;
        bool isLeft = false;
        bool diagLogged = false;
        WeaponProfile profile;
        RE::NiPointer<RE::NiAVObject> boneNode;
        RE::BSFixedString embedBoneName;
        EmbedBodyRegion embedBodyRegion = EmbedBodyRegion::Unknown;
        RE::NiPointer<RE::NiAVObject> weaponNode;
        RE::NiPointer<RE::NiAVObject> handNode;    // third-person hand node (final exact snap)
        RE::NiPointer<RE::NiAVObject> fpHandNode;  // FIRST-person hand node (drives VRIK's arm solve)
        RE::NiMatrix3 fpToTpRot;                   // rotation offset: tpHandRot = fpToTpRot * fpHandRot
        RE::NiTransform desiredHand;               // this frame's lodged hand pose (tp basis)
        bool haveDesired = false;
        RE::NiPoint3 entryLocalToBone;  // wound point, in bone frame
        RE::NiPoint3 axisLocalToBone;   // bite axis (into body), in bone frame
        RE::NiPoint3 gripInHand;        // weapon grip position, in the hand node's frame
        RE::NiPoint3 haftDirInHand;     // haft direction (grip->head), in the hand node's frame
        RE::NiMatrix3 lockedRotLocalToBone;  // hand rotation at embed, in bone space (frozen)
        float handOffset = 0.0f;        // handAlong at embed minus depth at embed: centers the
                                        // stick zone on the controller so pull distance maps
                                        // 1:1 to extraction instead of fighting a built-in lag
        float lastHandAlong = 0.0f;     // controller axial position last frame (re-chop detect)
        bool haveLastHandAlong = false;
        RE::NiPoint3 lastBonePos;       // bone node position last frame (shake-loose check)
        bool haveLastBonePos = false;
        float haftLen = 0.0f;           // grip->wound distance along the axis at entry
        float depth = 0.0f;             // SIMULATED grip position along the channel (chases the hand)
        float embedHandAlong = 0.0f;    // controller axial position at embed
        float pullBaselineDist = 0.0f;  // controller distance from wound when pull-back unlocks
        bool pullBaselineReady = false;
        RE::NiPointer<RE::Actor> victimActor;
        bool distressFaceActive = false;
        bool victimAttackRestricted = false;
        bool victimCombatRestricted = false;
        bool victimImmobilized = false;
        bool victimPlanckIgnored = false;
        bool victimRagdollCollisionIgnored = false;
        bool victimAggressionIgnored = false;
        bool victimWeaponWasDrawn = false;
        RE::NiPoint3 victimAnchorPos{};
        bool haveVictimAnchor = false;
        std::chrono::steady_clock::time_point lastSlipTime;  // dt source for slip integration
        bool haveLastSlipTime = false;
        std::uint32_t frames = 0;
        std::chrono::steady_clock::time_point startTime;
        // NPC embed: optional world-model axe when the embedding hand grip is pressed.
        bool npcEmbedEligible = false;
        bool gripWasDown = false;
        bool pendingWorldAxeTransition = false;
        RE::NiPointer<RE::TESObjectREFR> worldAxeRefr;
        bool useWorldAxe = false;
        bool pendingWorldAxeGrab = false;
        RE::NiTransform handGrabTransform{};
        RE::NiMatrix3 weaponRotLocalToBone{};
        float weaponWorldScale = 1.0f;
        std::chrono::steady_clock::time_point lastWorldAxeBloodTime{};
        bool haveWorldAxeBloodTime = false;
        std::chrono::steady_clock::time_point lastWorldAxeHealthDrainTime{};
        bool haveWorldAxeHealthDrainTime = false;
        std::chrono::steady_clock::time_point lastPlayerStaminaTick{};
        bool haveLastPlayerStaminaTick = false;
        float victimDistAtEmbed = -1.0f;  // player-to-victim distance at embed start (-1 = none)
    };

    float GetPlayerToActorDistance(RE::Actor* actor)
    {
        if (!actor) {
            return -1.0f;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return -1.0f;
        }
        return (player->GetPosition() - actor->GetPosition()).Length();
    }

    // Axes the player released while still embedded — kept at the wound, following the victim bone.
    struct LodgedAxeInBody
    {
        RE::NiPointer<RE::TESObjectREFR> axeRefr;
        RE::NiPointer<RE::NiAVObject> boneNode;
        RE::NiPointer<RE::Actor> victimActor;
        RE::BSFixedString embedBoneName;
        EmbedBodyRegion embedBodyRegion = EmbedBodyRegion::Unknown;
        RE::NiPoint3 entryLocalToBone{};
        RE::NiPoint3 axisLocalToBone{};
        RE::NiMatrix3 weaponRotLocalToBone{};
        float depth = 0.0f;
        float weaponWorldScale = 1.0f;
        std::chrono::steady_clock::time_point lastBloodFxTime{};
        bool haveLastBloodFxTime = false;
        std::chrono::steady_clock::time_point lastHealthDrainTime{};
        bool haveLastHealthDrainTime = false;
    };

    // Wound keeps bleeding after the weapon is pulled out (bone name + direction only; no node ptr).
    struct BleedingWoundSite
    {
        RE::ActorHandle victimHandle;
        RE::BSFixedString boneName;
        RE::NiPoint3 pickDirection{};
        std::chrono::steady_clock::time_point endTime{};
        std::chrono::steady_clock::time_point lastBloodTime{};
        bool haveLastBloodTime = false;
    };

    // Pull a lodged axe back out: one hand steadies the NPC, the other pulls (or grabs the axe).
    struct LodgedAxeExtractState
    {
        bool active = false;
        RE::FormID lodgedAxeRefId = 0;
        RE::ActorHandle victimHandle;
        bool steadyHandActive = false;
        bool steadyHandIsLeft = false;
        bool pullHandEngaged = false;
        bool pullHandIsLeft = false;
        bool pullViaAxeGrab = false;
        float pullHandBaselineDist = 0.0f;
        bool pullHandBaselineReady = false;
        float steadyHandBaselineDist = 0.0f;
        bool steadyHandBaselineReady = false;
        std::chrono::steady_clock::time_point startTime;
    };

    EmbedState g_embeds[2];  // [0] = right hand, [1] = left hand
    std::chrono::steady_clock::time_point g_embedCooldownUntil[2]{};

    void MarkEmbedHandCooldown(bool isLeft)
    {
        if (BitingAxesVR::embedCooldownSec <= 0.0) {
            return;
        }

        const float cooldownSec = static_cast<float>(BitingAxesVR::embedCooldownSec);
        g_embedCooldownUntil[isLeft ? 1 : 0] =
            std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                                   std::chrono::duration<float>(cooldownSec));
    }

    bool IsEmbedHandOnCooldown(bool isLeft)
    {
        if (BitingAxesVR::embedCooldownSec <= 0.0) {
            return false;
        }

        return std::chrono::steady_clock::now() < g_embedCooldownUntil[isLeft ? 1 : 0];
    }

    std::vector<LodgedAxeInBody> g_lodgedAxes;
    std::vector<BleedingWoundSite> g_bleedingWounds;
    LodgedAxeExtractState g_lodgedExtract;

    // While HIGGS-holding an NPC that still has a lodged world axe, freeze them and drain player stamina.
    struct LodgedNpcHoldState
    {
        bool handActive[2]{};  // [0]=right, [1]=left
        RE::ActorHandle victimHandle;
        bool immobilized = false;
        bool planckIgnored = false;
        bool ragdollCollisionIgnored = false;
        RE::NiPoint3 anchorPos{};
        bool haveAnchor = false;
        bool staminaExhausted = false;
        std::chrono::steady_clock::time_point lastStaminaTick{};
        bool haveLastStaminaTick = false;
    };

    LodgedNpcHoldState g_lodgedNpcHold;

    // Diagnostic tracking for world-model axes spawned on grip transition.
    enum class WorldAxeTrackPhase : std::uint8_t
    {
        ActiveEmbed,
        Lodged,
    };

    struct TrackedWorldAxe
    {
        RE::FormID refId = 0;
        RE::FormID baseFormId = 0;
        std::string weaponName;
        RE::ActorHandle victimHandle;
        std::string victimName;
        std::string boneName;
        bool isLeft = false;
        WorldAxeTrackPhase phase = WorldAxeTrackPhase::ActiveEmbed;
        std::chrono::steady_clock::time_point spawnedAt{};
        bool vanishLogged = false;
        bool earlyVanishWarned = false;
    };

    std::vector<TrackedWorldAxe> g_trackedWorldAxes;
    std::chrono::steady_clock::time_point g_lastWorldAxeAuditTime{};

    constexpr float kWorldAxeVanishAuditIntervalSec = 0.25f;
    constexpr float kWorldAxeVanishSpawnGraceSec = 0.50f;

    struct WorldAxeWhereabouts
    {
        bool inActiveEmbed = false;
        bool inLodgedList = false;
        bool staleModState = false;
        bool heldByHiggsLeft = false;
        bool heldByHiggsRight = false;
        bool inPlayerInventory = false;
        std::int32_t inventoryCount = 0;
        bool equippedLeft = false;
        bool equippedRight = false;
        RE::TESObjectREFR* resolvedRef = nullptr;
        bool refExists = false;
        bool refDisabled = false;
        bool has3D = false;
        bool embeddedOnVictim = false;
        std::string parentChain;
    };

    TrackedWorldAxe* FindTrackedWorldAxe(RE::FormID refId)
    {
        for (auto& tracked : g_trackedWorldAxes) {
            if (tracked.refId == refId) {
                return &tracked;
            }
        }
        return nullptr;
    }

    std::string DescribeNiParentChain(RE::NiAVObject* node, std::uint32_t maxDepth = 8)
    {
        if (!node) {
            return "<no-node>";
        }

        std::string chain;
        for (std::uint32_t depth = 0; node && depth < maxDepth; ++depth, node = node->parent) {
            if (!chain.empty()) {
                chain += " <- ";
            }
            chain += node->name.empty() ? "<unnamed>" : node->name.c_str();
        }
        if (node) {
            chain += " <- ...";
        }
        return chain;
    }

    bool IsAxeEmbeddedOnVictim(RE::TESObjectREFR* refr, RE::Actor* victim)
    {
        if (!refr || !victim) {
            return false;
        }

        auto* axeNode = refr->Get3D();
        auto* victimRoot = victim->Get3D();
        if (!axeNode || !victimRoot) {
            return false;
        }

        for (auto* node = axeNode->parent; node; node = node->parent) {
            if (node == victimRoot) {
                return true;
            }
        }
        return false;
    }

    WorldAxeWhereabouts QueryWorldAxeWhereabouts(RE::FormID refId, RE::FormID baseFormId, RE::Actor* knownVictim)
    {
        WorldAxeWhereabouts where{};

        where.resolvedRef = RE::TESForm::LookupByID<RE::TESObjectREFR>(refId);
        where.refExists = where.resolvedRef != nullptr;
        if (where.refExists) {
            where.refDisabled = where.resolvedRef->IsDisabled();
            if (auto* node = where.resolvedRef->Get3D()) {
                where.has3D = true;
                where.parentChain = DescribeNiParentChain(node);
            }
        }

        for (const auto& e : g_embeds) {
            if (e.active && e.useWorldAxe && e.worldAxeRefr && e.worldAxeRefr->GetFormID() == refId) {
                where.inActiveEmbed = true;
                break;
            }
        }

        for (const auto& lodged : g_lodgedAxes) {
            if (lodged.axeRefr && lodged.axeRefr->GetFormID() == refId) {
                where.inLodgedList = true;
                if (!knownVictim && lodged.victimActor) {
                    knownVictim = lodged.victimActor.get();
                }
                break;
            }
        }

        if (g_higgsInterface) {
            if (auto* leftHeld = g_higgsInterface->GetGrabbedObject(true)) {
                where.heldByHiggsLeft = leftHeld->GetFormID() == refId;
            }
            if (auto* rightHeld = g_higgsInterface->GetGrabbedObject(false)) {
                where.heldByHiggsRight = rightHeld->GetFormID() == refId;
            }
        }

        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
            if (auto* baseForm = RE::TESForm::LookupByID<RE::TESBoundObject>(baseFormId)) {
                where.inventoryCount = player->GetItemCount(baseForm);
                where.inPlayerInventory = where.inventoryCount > 0;
            }

            if (auto* left = player->GetEquippedObject(true)) {
                where.equippedLeft = left->GetFormID() == baseFormId ||
                                     (where.resolvedRef && left->GetFormID() == refId);
            }
            if (auto* right = player->GetEquippedObject(false)) {
                where.equippedRight = right->GetFormID() == baseFormId ||
                                      (where.resolvedRef && right->GetFormID() == refId);
            }
        }

        if (where.resolvedRef) {
            if (!knownVictim) {
                for (const auto& lodged : g_lodgedAxes) {
                    if (lodged.axeRefr && lodged.axeRefr->GetFormID() == refId && lodged.victimActor) {
                        knownVictim = lodged.victimActor.get();
                        break;
                    }
                }
            }
            if (!knownVictim) {
                for (const auto& e : g_embeds) {
                    if (e.useWorldAxe && e.worldAxeRefr && e.worldAxeRefr->GetFormID() == refId && e.victimActor) {
                        knownVictim = e.victimActor.get();
                        break;
                    }
                }
            }
            if (knownVictim) {
                where.embeddedOnVictim = IsAxeEmbeddedOnVictim(where.resolvedRef, knownVictim);
            }
        }

        where.staleModState =
            (where.inActiveEmbed || where.inLodgedList) && !where.embeddedOnVictim && !where.heldByHiggsLeft &&
            !where.heldByHiggsRight && !where.inPlayerInventory && !where.equippedLeft && !where.equippedRight;

        return where;
    }

    bool IsWorldAxePhysicallyAccountedFor(const WorldAxeWhereabouts& where)
    {
        return where.heldByHiggsLeft || where.heldByHiggsRight || where.inPlayerInventory || where.equippedLeft ||
               where.equippedRight || where.embeddedOnVictim;
    }

    bool IsWorldAxeVanished(const WorldAxeWhereabouts& where)
    {
        // Lost = not on victim skeleton, not held, not in player inventory/equipment.
        // Do not trust inLodgedList/inActiveEmbed alone (stale mod state during race conditions).
        return !IsWorldAxePhysicallyAccountedFor(where);
    }

    const char* DescribeWorldAxeVanishKind(const WorldAxeWhereabouts& where)
    {
        if (!where.refExists) {
            return "ref_deleted";
        }
        if (where.refDisabled) {
            return "ref_disabled";
        }
        if (!where.has3D) {
            return "missing_3d";
        }
        if (where.staleModState) {
            return "stale_mod_state";
        }
        return "unlinked";
    }

    void LogWorldAxeVanish(const TrackedWorldAxe& tracked, const char* trigger, const WorldAxeWhereabouts& where)
    {
        const float ageSec =
            std::chrono::duration<float>(std::chrono::steady_clock::now() - tracked.spawnedAt).count();
        const char* phaseName = tracked.phase == WorldAxeTrackPhase::ActiveEmbed ? "active_embed" : "lodged";
        const char* kind = DescribeWorldAxeVanishKind(where);

        IW_LOG_ERROR(
            "[bite][VANISH] world axe LOST  kind={}  trigger={}  phase={}  age={:.2f}s  ref=0x{:08X}  base=0x{:08X}  "
            "weapon='{}'  victim='{}'  bone='{}'  hand={}",
            kind, trigger, phaseName, ageSec, tracked.refId, tracked.baseFormId, tracked.weaponName,
            tracked.victimName, tracked.boneName, tracked.isLeft ? "L" : "R");
        IW_LOG_ERROR(
            "[bite][VANISH] expected=embedded_on_victim|player_inv|equipped|higgs  actual  embeddedOnVictim={}  "
            "invCount={}  eqL={}  eqR={}  higgsL={}  higgsR={}",
            where.embeddedOnVictim, where.inventoryCount, where.equippedLeft, where.equippedRight,
            where.heldByHiggsLeft, where.heldByHiggsRight);
        IW_LOG_ERROR(
            "[bite][VANISH] modState  inActiveEmbed={}  inLodgedList={}  staleModState={}  refExists={}  "
            "disabled={}  has3D={}",
            where.inActiveEmbed, where.inLodgedList, where.staleModState, where.refExists, where.refDisabled,
            where.has3D);
        if (!where.parentChain.empty()) {
            IW_LOG_ERROR("[bite][VANISH] parentChain={}", where.parentChain);
        }
        SKSE::log::error("[bite][VANISH] {} ref=0x{:08X} victim='{}' trigger={} embedded={} inv={}",
                         kind, tracked.refId, tracked.victimName, trigger, where.embeddedOnVictim,
                         where.inventoryCount);
    }

    void RegisterTrackedWorldAxe(RE::TESObjectREFR* refr, RE::TESObjectWEAP* weapForm, RE::Actor* victim,
                                 const RE::BSFixedString& boneName, bool isLeft)
    {
        if (!refr || !weapForm) {
            return;
        }

        TrackedWorldAxe tracked{};
        tracked.refId = refr->GetFormID();
        tracked.baseFormId = weapForm->GetFormID();
        tracked.weaponName = weapForm->GetName();
        if (victim) {
            tracked.victimHandle = victim->CreateRefHandle();
            tracked.victimName = victim->GetDisplayFullName();
        }
        tracked.boneName = boneName.empty() ? "<unnamed>" : boneName.c_str();
        tracked.isLeft = isLeft;
        tracked.phase = WorldAxeTrackPhase::ActiveEmbed;
        tracked.spawnedAt = std::chrono::steady_clock::now();
        g_trackedWorldAxes.push_back(tracked);

        IW_LOG_INFO(
            "[bite][worldAxe] tracked spawn  ref=0x{:08X}  base=0x{:08X}  weapon='{}'  victim='{}'  bone='{}'  hand={}",
            tracked.refId, tracked.baseFormId, tracked.weaponName, tracked.victimName, tracked.boneName,
            tracked.isLeft ? "L" : "R");
    }

    void UntrackWorldAxe(RE::FormID refId, const char* reason)
    {
        if (refId == 0) {
            return;
        }

        const auto it = std::remove_if(g_trackedWorldAxes.begin(), g_trackedWorldAxes.end(),
                                       [refId](const TrackedWorldAxe& tracked) { return tracked.refId == refId; });
        if (it != g_trackedWorldAxes.end()) {
            IW_LOG_INFO("[bite][worldAxe] untrack  ref=0x{:08X}  ({})", refId, reason);
            g_trackedWorldAxes.erase(it, g_trackedWorldAxes.end());
        }
    }

    void SetTrackedWorldAxePhase(RE::FormID refId, WorldAxeTrackPhase phase, const char* reason)
    {
        if (auto* tracked = FindTrackedWorldAxe(refId)) {
            tracked->phase = phase;
            const char* phaseName = phase == WorldAxeTrackPhase::ActiveEmbed ? "active_embed" : "lodged";
            IW_LOG_INFO("[bite][worldAxe] phase={}  ref=0x{:08X}  ({})", phaseName, refId, reason);
        }
    }

    void ReportWorldAxeVanishIfNeeded(TrackedWorldAxe& tracked, const char* trigger)
    {
        if (tracked.vanishLogged) {
            return;
        }

        RE::Actor* victim = tracked.victimHandle.get().get();
        const WorldAxeWhereabouts where =
            QueryWorldAxeWhereabouts(tracked.refId, tracked.baseFormId, victim);
        if (!IsWorldAxeVanished(where)) {
            return;
        }

        const float ageSec =
            std::chrono::duration<float>(std::chrono::steady_clock::now() - tracked.spawnedAt).count();
        if (ageSec < kWorldAxeVanishSpawnGraceSec) {
            if (!tracked.earlyVanishWarned) {
                const char* kind = DescribeWorldAxeVanishKind(where);
                IW_LOG_WARN(
                    "[bite][VANISH][early] world axe lost within {:.2f}s of spawn  kind={}  trigger={}  "
                    "ref=0x{:08X}  has3D={}  embedded={}  higgsL={}  higgsR={}  inv={}",
                    ageSec, kind, trigger, tracked.refId, where.has3D, where.embeddedOnVictim,
                    where.heldByHiggsLeft, where.heldByHiggsRight, where.inventoryCount);
                tracked.earlyVanishWarned = true;
            }
            return;
        }

        LogWorldAxeVanish(tracked, trigger, where);
        tracked.vanishLogged = true;
    }

    void ReportWorldAxeVanishForRef(RE::FormID refId, const char* trigger)
    {
        if (auto* tracked = FindTrackedWorldAxe(refId)) {
            ReportWorldAxeVanishIfNeeded(*tracked, trigger);
        }
    }

    void AuditTrackedWorldAxes()
    {
        const auto now = std::chrono::steady_clock::now();
        if (g_lastWorldAxeAuditTime.time_since_epoch().count() != 0) {
            const float sinceLast =
                std::chrono::duration<float>(now - g_lastWorldAxeAuditTime).count();
            if (sinceLast < kWorldAxeVanishAuditIntervalSec) {
                return;
            }
        }
        g_lastWorldAxeAuditTime = now;

        for (auto& tracked : g_trackedWorldAxes) {
            ReportWorldAxeVanishIfNeeded(tracked, "periodic_audit");
        }
    }

    void ClearTrackedWorldAxes(const char* reason)
    {
        if (!g_trackedWorldAxes.empty()) {
            IW_LOG_INFO("[bite][worldAxe] clearing {} tracked axe(s) ({})", g_trackedWorldAxes.size(), reason);
        }
        g_trackedWorldAxes.clear();
        g_lastWorldAxeAuditTime = {};
    }

    float GetPlayerStamina()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return 0.0f;
        }
        if (auto* av = player->AsActorValueOwner()) {
            return av->GetActorValue(RE::ActorValue::kStamina);
        }
        return 0.0f;
    }

    bool CanAffordLodgedNpcHold()
    {
        if (BitingAxesVR::lodgedHoldStaminaDrainEnabled == 0) {
            return true;
        }
        return GetPlayerStamina() >= static_cast<float>(BitingAxesVR::lodgedHoldStaminaMin);
    }

    bool MaintainEmbedPlayerStaminaDrain(EmbedState& e)
    {
        if (e.useWorldAxe || BitingAxesVR::embedPlayerStaminaDrainEnabled == 0 ||
            BitingAxesVR::embedPlayerStaminaDrainPerSec <= 0.0) {
            return false;
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return false;
        }

        auto* av = player->AsActorValueOwner();
        if (!av) {
            return false;
        }

        const auto now = std::chrono::steady_clock::now();
        float dt = 0.016f;
        if (e.haveLastPlayerStaminaTick) {
            dt = std::chrono::duration<float>(now - e.lastPlayerStaminaTick).count();
            if (dt < 0.0f) {
                dt = 0.0f;
            } else if (dt > 0.25f) {
                dt = 0.25f;
            }
        }
        e.lastPlayerStaminaTick = now;
        e.haveLastPlayerStaminaTick = true;

        const float drain = static_cast<float>(BitingAxesVR::embedPlayerStaminaDrainPerSec) * dt;
        if (drain > 0.0f && av->GetActorValue(RE::ActorValue::kStamina) > 0.0f) {
            av->DamageActorValue(RE::ActorValue::kStamina, drain);
        }

        return BitingAxesVR::embedStaminaExhaustRelease != 0 &&
               av->GetActorValue(RE::ActorValue::kStamina) <= 0.0f;
    }

    bool VictimHasLodgedAxe(RE::Actor* victim);
    bool IsLivingVictim(RE::Actor* actor);
    bool ShouldApplyLodgedNpcHold(RE::Actor* victim);
    void InitEmbedVictimEffects(EmbedState& e);
    void InitLodgedAxeVictimEffects(LodgedAxeInBody& lodged);
    void ReleaseWorldAxeEmbedVictimRestrictions(EmbedState& e);
    void BeginLodgedNpcHold(bool isLeft, RE::Actor* victim);
    void EndLodgedNpcHold(bool isLeft);
    void UpdateLodgedNpcHold();
    void ReleaseHeldLodgedVictim(LodgedNpcHoldState& hold, RE::Actor* actor);
    void FillEmbedApiSnapshot(const EmbedState& e, BitingAxesAPI::EmbedSnapshot& out);
    void FillLodgedApiSnapshot(const LodgedAxeInBody& lodged, bool isLeft, BitingAxesAPI::EmbedSnapshot& out);

    EmbedState& EmbedFor(bool isLeft)
    {
        return g_embeds[isLeft ? 1 : 0];
    }

    bool ComputeEmbeddedWeaponLocalToBone(const RE::NiPoint3& entryLocalToBone, const RE::NiPoint3& axisLocalToBone,
                                          float depth, const RE::NiMatrix3& weaponRotLocalToBone,
                                          float weaponWorldScale, RE::NiTransform& outLocal)
    {
        outLocal.rotate = weaponRotLocalToBone;
        outLocal.translate = entryLocalToBone + axisLocalToBone * depth;
        outLocal.scale = weaponWorldScale;
        return true;
    }

    bool ComputeEmbeddedWeaponWorld(const RE::NiTransform& boneWorld, const RE::NiPoint3& entryLocalToBone,
                                    const RE::NiPoint3& axisLocalToBone, float depth,
                                    const RE::NiMatrix3& weaponRotLocalToBone, float weaponWorldScale,
                                    RE::NiTransform& outWorld)
    {
        RE::NiPoint3 axis = boneWorld.rotate * axisLocalToBone;
        const float axisLen = axis.Length();
        if (axisLen < 1e-4f) {
            return false;
        }
        axis = axis / axisLen;

        const RE::NiPoint3 woundWorld = boneWorld * entryLocalToBone;
        outWorld.rotate = boneWorld.rotate * weaponRotLocalToBone;
        outWorld.translate = woundWorld + axis * depth;
        outWorld.scale = weaponWorldScale;
        return true;
    }

    RE::NiQuaternion MatrixToNiQuaternion(const RE::NiMatrix3& m)
    {
        RE::NiQuaternion q{};
        const float trace = m.entry[0][0] + m.entry[1][1] + m.entry[2][2];
        if (trace > 0.0f) {
            const float s = std::sqrt(trace + 1.0f) * 2.0f;
            q.w = 0.25f * s;
            q.x = (m.entry[2][1] - m.entry[1][2]) / s;
            q.y = (m.entry[0][2] - m.entry[2][0]) / s;
            q.z = (m.entry[1][0] - m.entry[0][1]) / s;
        } else if (m.entry[0][0] > m.entry[1][1] && m.entry[0][0] > m.entry[2][2]) {
            const float s = std::sqrt(1.0f + m.entry[0][0] - m.entry[1][1] - m.entry[2][2]) * 2.0f;
            q.w = (m.entry[2][1] - m.entry[1][2]) / s;
            q.x = 0.25f * s;
            q.y = (m.entry[0][1] + m.entry[1][0]) / s;
            q.z = (m.entry[0][2] + m.entry[2][0]) / s;
        } else if (m.entry[1][1] > m.entry[2][2]) {
            const float s = std::sqrt(1.0f + m.entry[1][1] - m.entry[0][0] - m.entry[2][2]) * 2.0f;
            q.w = (m.entry[0][2] - m.entry[2][0]) / s;
            q.x = (m.entry[0][1] + m.entry[1][0]) / s;
            q.y = 0.25f * s;
            q.z = (m.entry[1][2] + m.entry[2][1]) / s;
        } else {
            const float s = std::sqrt(1.0f + m.entry[2][2] - m.entry[0][0] - m.entry[1][1]) * 2.0f;
            q.w = (m.entry[1][0] - m.entry[0][1]) / s;
            q.x = (m.entry[0][2] + m.entry[2][0]) / s;
            q.y = (m.entry[1][2] + m.entry[2][1]) / s;
            q.z = 0.25f * s;
        }
        return q;
    }

    void TeleportRigidBodyToWorld(RE::bhkRigidBody* rb, const RE::NiTransform& worldXform)
    {
        if (!rb) {
            return;
        }

        RE::hkVector4 pos{};
        pos.quad = _mm_set_ps(0.0f, worldXform.translate.z * kSkyrimToHavok, worldXform.translate.y * kSkyrimToHavok,
                              worldXform.translate.x * kSkyrimToHavok);

        RE::hkQuaternion rot{};
        const RE::NiQuaternion nq = MatrixToNiQuaternion(worldXform.rotate);
        rot.vec.quad = _mm_set_ps(nq.z, nq.y, nq.x, nq.w);
        rb->SetPositionAndRotation(pos, rot);
    }

    void DetachAxeFromBone(RE::TESObjectREFR* refr)
    {
        if (!refr) {
            return;
        }
        auto* axeNode = refr->Get3D();
        if (!axeNode || !axeNode->parent) {
            return;
        }
        if (auto* parentNi = axeNode->parent->AsNode()) {
            parentNi->DetachChild(axeNode);
        }
    }

    void SyncWorldAxeToBone(RE::TESObjectREFR* refr, RE::NiAVObject* bone, const RE::NiPoint3& entryLocalToBone,
                            const RE::NiPoint3& axisLocalToBone, float depth,
                            const RE::NiMatrix3& weaponRotLocalToBone, float weaponWorldScale, bool fixedPhysics)
    {
        if (!refr || !bone) {
            return;
        }

        auto* axeNode = refr->Get3D();
        if (!axeNode) {
            IW_LOG_WARN("[bite][worldAxe] SyncWorldAxeToBone missing 3D node  ref=0x{:08X}  disabled={}",
                        refr->GetFormID(), refr->IsDisabled());
            return;
        }

        auto* boneNi = bone->AsNode();
        if (!boneNi) {
            return;
        }

        RE::NiTransform localToBone{};
        ComputeEmbeddedWeaponLocalToBone(entryLocalToBone, axisLocalToBone, depth, weaponRotLocalToBone,
                                         weaponWorldScale, localToBone);

        if (axeNode->parent != bone) {
            if (axeNode->parent) {
                if (auto* parentNi = axeNode->parent->AsNode()) {
                    parentNi->DetachChild(axeNode);
                }
            }
            boneNi->AttachChild(axeNode, true);
        }

        axeNode->local = localToBone;
        RE::NiUpdateData ud{};
        axeNode->Update(ud);
        axeNode->UpdateWorldData(&ud);

        const RE::NiTransform worldXform = bone->world * localToBone;
        refr->SetPosition(worldXform.translate);
        RE::NiPoint3 euler{};
        worldXform.rotate.ToEulerAnglesXYZ(euler);
        refr->SetAngle(euler);

        refr->SetMotionType(fixedPhysics ? RE::hkpMotion::MotionType::kFixed : RE::hkpMotion::MotionType::kKeyframed,
                            false);

        if (auto* rb = GetRigidBody(axeNode)) {
            TeleportRigidBodyToWorld(rb, worldXform);
        } else {
            refr->MoveHavok(true);
        }
    }

    void ApplyWeaponWorldTransform(RE::TESObjectREFR* refr, const RE::NiTransform& worldXform)
    {
        if (!refr) {
            return;
        }

        refr->SetPosition(worldXform.translate);
        RE::NiPoint3 euler{};
        worldXform.rotate.ToEulerAnglesXYZ(euler);
        refr->SetAngle(euler);
        refr->MoveHavok(false);
    }

    void TryGrabWorldAxe(EmbedState& e)
    {
        if (!e.pendingWorldAxeGrab || !e.worldAxeRefr || !g_higgsInterface) {
            return;
        }
        if (UseTriggerForWorldAxeInput() && !IsTriggerPressedOnEmbedHand(e.isLeft)) {
            return;
        }
        if (!e.worldAxeRefr->Get3D()) {
            if (auto* tracked = FindTrackedWorldAxe(e.worldAxeRefr->GetFormID())) {
                ReportWorldAxeVanishIfNeeded(*tracked, "pending_grab_missing_3d");
            } else {
                IW_LOG_WARN("[bite][worldAxe] pending grab but axe has no 3D  ref=0x{:08X}",
                            e.worldAxeRefr->GetFormID());
            }
            return;
        }
        if (e.victimActor && !IsAxeEmbeddedOnVictim(e.worldAxeRefr.get(), e.victimActor.get())) {
            return;
        }
        if (!g_higgsInterface->CanGrabObject(e.isLeft)) {
            return;
        }

        g_higgsInterface->SetGrabTransform(e.isLeft, e.handGrabTransform);
        g_higgsInterface->GrabObject(e.worldAxeRefr.get(), e.isLeft);
        e.pendingWorldAxeGrab = false;
        IW_LOG_INFO("[bite] HIGGS auto-grabbed world axe on {} hand ({})",
                    e.isLeft ? "L" : "R", WorldAxeInputButtonName());
    }

    bool BeginWorldAxeEmbed(EmbedState& e, RE::Actor* victim, RE::NiAVObject* weapon, RE::NiAVObject* handNode,
                            const RE::NiTransform& boneWorld)
    {
        if (!g_higgsInterface || !victim || !weapon || !handNode) {
            return false;
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* weapForm = const_cast<RE::TESObjectWEAP*>(GetEquippedWeapon(e.isLeft));
        if (!player || !weapForm || IsBoundConjuredWeapon(weapForm)) {
            return false;
        }

        e.weaponRotLocalToBone = boneWorld.rotate.Transpose() * weapon->world.rotate;
        e.weaponWorldScale = weapon->world.scale;

        RE::NiTransform weaponWorld{};
        weaponWorld.rotate = weapon->world.rotate;
        weaponWorld.translate = weapon->world.translate;
        weaponWorld.scale = weapon->world.scale;

        RE::NiTransform handWorld{};
        handWorld.rotate = handNode->world.rotate;
        handWorld.translate = handNode->world.translate;
        handWorld.scale = handNode->world.scale;
        e.handGrabTransform = handWorld.Invert() * weaponWorld;

        RE::NiPoint3 euler{};
        weaponWorld.rotate.ToEulerAnglesXYZ(euler);

        auto* equipMgr = RE::ActorEquipManager::GetSingleton();
        if (!equipMgr) {
            return false;
        }
        equipMgr->UnequipObject(player, weapForm, nullptr, 1, nullptr, false, true, false, false, nullptr);

        auto* dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler) {
            return false;
        }

        const auto handle = dataHandler->CreateReferenceAtLocation(
            weapForm, weaponWorld.translate, euler, victim->GetParentCell(), victim->GetWorldspace(), nullptr,
            nullptr, RE::ObjectRefHandle(), true, true);
        auto* refr = handle.get().get();
        if (!refr) {
            IW_LOG_WARN("[bite] CreateReferenceAtLocation failed for world axe embed");
            return false;
        }

        player->RemoveItem(weapForm, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
        refr->SetMotionType(RE::hkpMotion::MotionType::kKeyframed, true);

        if (auto* bone = e.boneNode.get()) {
            SyncWorldAxeToBone(refr, bone, e.entryLocalToBone, e.axisLocalToBone, e.depth, e.weaponRotLocalToBone,
                               e.weaponWorldScale, false);
            if (victim && !IsAxeEmbeddedOnVictim(refr, victim)) {
                IW_LOG_WARN("[bite][worldAxe] spawn sync did not parent ref to victim skeleton  ref=0x{:08X}",
                            refr->GetFormID());
            }
        } else {
            IW_LOG_WARN("[bite][worldAxe] spawn without bone node — ref may float until next sync");
        }

        e.worldAxeRefr = RE::NiPointer<RE::TESObjectREFR>(refr);
        e.useWorldAxe = true;
        e.pendingWorldAxeGrab = true;
        e.weaponNode.reset();
        InitEmbedVictimEffects(e);
        RegisterTrackedWorldAxe(refr, weapForm, victim, e.embedBoneName, e.isLeft);

        IW_LOG_INFO("[bite] spawned world axe '{}' embedded in '{}'",
                    weapForm->GetName(), victim->GetDisplayFullName());
        return true;
    }

    bool TryTransitionToWorldAxe(EmbedState& e, const RE::NiTransform& boneWorld)
    {
        if (e.useWorldAxe || !e.npcEmbedEligible) {
            return false;
        }

        auto* weapon = e.weaponNode.get();
        auto* hand = e.handNode.get();
        auto* victim = e.victimActor.get();
        if (!weapon || !hand || !victim) {
            return false;
        }

        if (!BeginWorldAxeEmbed(e, victim, weapon, hand, boneWorld)) {
            IW_LOG_WARN("[bite] world axe transition failed");
            return false;
        }

        if (g_higgsInterface) {
            g_higgsInterface->EnableWeaponCollision(e.isLeft);
        }
        e.haveDesired = false;
        e.npcEmbedEligible = false;
        e.pullBaselineReady = false;
        e.haveLastBonePos = false;
        e.startTime = std::chrono::steady_clock::now();
        ReleaseWorldAxeEmbedVictimRestrictions(e);
        IW_LOG_INFO("[bite] {} pressed on {} hand -> world axe lodged in '{}'",
                    WorldAxeInputButtonName(), e.isLeft ? "L" : "R", victim->GetDisplayFullName());
        BitingAxesAPI::EmbedSnapshot snapshot{};
        FillEmbedApiSnapshot(e, snapshot);
        BitingAxesAPI::NotifyWorldAxeEmbed(snapshot);
        return true;
    }

    void ScheduleWorldAxeTransition(EmbedState& e)
    {
        if (!EmbedWorldModelFeatureEnabled() || e.pendingWorldAxeTransition || e.useWorldAxe ||
            !e.npcEmbedEligible) {
            return;
        }

        e.pendingWorldAxeTransition = true;
        const bool handIsLeft = e.isLeft;
        IW_LOG_INFO("[bite] {} pressed on {} hand during NPC embed — spawning world axe",
                    WorldAxeInputButtonName(), handIsLeft ? "L" : "R");

        SKSE::GetTaskInterface()->AddTask([handIsLeft]() {
            EmbedState& embed = EmbedFor(handIsLeft);
            embed.pendingWorldAxeTransition = false;
            if (!embed.active || embed.useWorldAxe || !embed.npcEmbedEligible) {
                return;
            }
            auto bone = embed.boneNode;
            if (!bone) {
                return;
            }
            const RE::NiTransform boneWorld = bone->world;
            TryTransitionToWorldAxe(embed, boneWorld);
        });
    }

    void UpdateGripWorldAxeTransition(EmbedState& e)
    {
        if (!EmbedWorldModelFeatureEnabled() || !e.npcEmbedEligible || e.useWorldAxe ||
            e.pendingWorldAxeTransition) {
            return;
        }

        const bool inputDown = IsWorldAxeInputPressedOnEmbedHand(e.isLeft);
        if (inputDown && !e.gripWasDown) {
            ScheduleWorldAxeTransition(e);
        }
        e.gripWasDown = inputDown;
    }

    void UpdateLodgedAxes()
    {
        for (auto it = g_lodgedAxes.begin(); it != g_lodgedAxes.end();) {
            auto& lodged = *it;
            if (!lodged.axeRefr || !lodged.boneNode || !lodged.victimActor) {
                const RE::FormID refId = lodged.axeRefr ? lodged.axeRefr->GetFormID() : 0;
                if (refId != 0) {
                    if (auto* tracked = FindTrackedWorldAxe(refId)) {
                        ReportWorldAxeVanishIfNeeded(*tracked, "lodged_entry_invalidated");
                    }
                    IW_LOG_ERROR(
                        "[bite][VANISH] lodged axe entry removed  ref=0x{:08X}  axeRefr={}  boneNode={}  victim={}",
                        refId, lodged.axeRefr ? "Y" : "N", lodged.boneNode ? "Y" : "N",
                        lodged.victimActor ? "Y" : "N");
                }
                it = g_lodgedAxes.erase(it);
                continue;
            }

            SyncWorldAxeToBone(lodged.axeRefr.get(), lodged.boneNode.get(), lodged.entryLocalToBone,
                               lodged.axisLocalToBone, lodged.depth, lodged.weaponRotLocalToBone,
                               lodged.weaponWorldScale, true);

            if (lodged.axeRefr && lodged.victimActor &&
                !IsAxeEmbeddedOnVictim(lodged.axeRefr.get(), lodged.victimActor.get())) {
                ReportWorldAxeVanishForRef(lodged.axeRefr->GetFormID(), "lodged_not_on_victim_skeleton");
            }

            ++it;
        }
    }

    void ApplyWorldAxeBoneSync()
    {
        UpdateLodgedAxes();
        for (auto& e : g_embeds) {
            if (!e.active || !e.useWorldAxe || !e.worldAxeRefr || !e.boneNode) {
                continue;
            }
            SyncWorldAxeToBone(e.worldAxeRefr.get(), e.boneNode.get(), e.entryLocalToBone, e.axisLocalToBone,
                               e.depth, e.weaponRotLocalToBone, e.weaponWorldScale, false);

            if (e.victimActor && !IsAxeEmbeddedOnVictim(e.worldAxeRefr.get(), e.victimActor.get())) {
                ReportWorldAxeVanishForRef(e.worldAxeRefr->GetFormID(), "active_embed_not_on_victim_skeleton");
            }

            // HIGGS may drive a separate grabbed body; keep it aligned with the bone-locked pose.
            if (g_higgsInterface && g_higgsInterface->IsHoldingObject(e.isLeft)) {
                RE::NiTransform localToBone{};
                ComputeEmbeddedWeaponLocalToBone(e.entryLocalToBone, e.axisLocalToBone, e.depth,
                                                 e.weaponRotLocalToBone, e.weaponWorldScale, localToBone);
                const RE::NiTransform worldXform = e.boneNode->world * localToBone;
                if (auto* obj = g_higgsInterface->GetGrabbedRigidBody(e.isLeft)) {
                    if (auto* rb = netimmerse_cast<RE::bhkRigidBody*>(obj)) {
                        TeleportRigidBodyToWorld(rb, worldXform);
                    }
                }
            }
        }
    }

    RE::Actor* GetActorFromNode(RE::NiAVObject* node)
    {
        while (node) {
            if (auto* refr = node->GetUserData()) {
                if (auto* actor = refr->As<RE::Actor>()) {
                    return actor;
                }
            }
            node = node->parent;
        }
        return nullptr;
    }

    RE::BSFixedString ResolveEmbedBoneName(const EmbedState& e)
    {
        if (!e.embedBoneName.empty()) {
            return e.embedBoneName;
        }
        if (auto bone = e.boneNode) {
            return bone->name;
        }
        return {};
    }

    RE::NiPoint3 BuildDislodgeBloodDirection(const EmbedState& e)
    {
        if (auto bone = e.boneNode) {
            RE::NiPoint3 axisWorld = bone->world.rotate * e.axisLocalToBone;
            const float len = axisWorld.Length();
            if (len > 1e-4f) {
                // Embed axis points into the body; dislodge blood exits opposite that direction.
                return axisWorld * (-1.0f / len);
            }
        }

        if (auto* player = RE::PlayerCharacter::GetSingleton(); e.victimActor) {
            RE::NiPoint3 awayFromPlayer = e.victimActor->GetPosition() - player->GetPosition();
            const float len = awayFromPlayer.Length();
            if (len > 1e-4f) {
                return awayFromPlayer * (1.0f / len);
            }
        }

        return { 0.0f, 0.0f, -1.0f };
    }

    void PlayDislodgeBloodEffect(const EmbedState& e)
    {
        if (!e.victimActor || !e.boneNode) {
            return;
        }

        auto* victim = e.victimActor.get();
        const RE::BSFixedString boneName = ResolveEmbedBoneName(e);
        if (boneName.empty()) {
            IW_LOG_WARN("[bite] dislodge blood skipped; no embed bone name");
            return;
        }

        const RE::NiPoint3 pickDirection = BuildDislodgeBloodDirection(e);
        if (PlayWoundBloodImpact(victim, boneName, pickDirection, true)) {
            IW_LOG_INFO("[bite] played dislodge blood impact on '{}' at bone '{}'",
                        victim->GetDisplayFullName(), boneName.c_str());
            return;
        }

        const RE::NiPoint3 fallbackDirection{ 0.0f, 0.0f, -1.0f };
        if (PlayWoundBloodImpact(victim, boneName, fallbackDirection, false)) {
            IW_LOG_INFO("[bite] played dislodge blood impact fallback on '{}' at bone '{}'",
                        victim->GetDisplayFullName(), boneName.c_str());
            return;
        }

        IW_LOG_WARN("[bite] dislodge blood impact failed on '{}' at bone '{}'",
                    victim->GetDisplayFullName(), boneName.c_str());
    }

    RE::NiPoint3 GetLodgedWoundWorld(const LodgedAxeInBody& lodged)
    {
        if (lodged.boneNode) {
            return lodged.boneNode->world * lodged.entryLocalToBone;
        }
        if (lodged.victimActor) {
            return lodged.victimActor->GetPosition();
        }
        return {};
    }

    RE::NiPoint3 BuildLodgedDislodgeBloodDirection(const LodgedAxeInBody& lodged)
    {
        if (lodged.boneNode) {
            RE::NiPoint3 axisWorld = lodged.boneNode->world.rotate * lodged.axisLocalToBone;
            const float len = axisWorld.Length();
            if (len > 1e-4f) {
                return axisWorld * (-1.0f / len);
            }
        }
        if (auto* player = RE::PlayerCharacter::GetSingleton(); lodged.victimActor) {
            RE::NiPoint3 awayFromPlayer = lodged.victimActor->GetPosition() - player->GetPosition();
            const float len = awayFromPlayer.Length();
            if (len > 1e-4f) {
                return awayFromPlayer * (1.0f / len);
            }
        }
        return { 0.0f, 0.0f, -1.0f };
    }

    void PlayDislodgeBloodEffectForLodged(const LodgedAxeInBody& lodged)
    {
        if (!lodged.victimActor) {
            return;
        }

        auto* victim = lodged.victimActor.get();
        const RE::BSFixedString boneName = lodged.embedBoneName.empty() && lodged.boneNode
                                               ? lodged.boneNode->name
                                               : lodged.embedBoneName;
        if (boneName.empty()) {
            return;
        }

        const RE::NiPoint3 pickDirection = BuildLodgedDislodgeBloodDirection(lodged);
        if (PlayWoundBloodImpact(victim, boneName, pickDirection, true)) {
            return;
        }
        const RE::NiPoint3 fallbackDirection{ 0.0f, 0.0f, -1.0f };
        PlayWoundBloodImpact(victim, boneName, fallbackDirection, false);
    }

    void PlayWorldAxeEmbedBloodEffect(const EmbedState& e)
    {
        if (!e.victimActor || !e.boneNode) {
            return;
        }

        LodgedAxeInBody temp{};
        temp.victimActor = e.victimActor;
        temp.boneNode = e.boneNode;
        temp.embedBoneName = e.embedBoneName;
        temp.axisLocalToBone = e.axisLocalToBone;
        PlayDislodgeBloodEffectForLodged(temp);
    }

    void PlayBleedingWoundBlood(RE::Actor* victim, const RE::BSFixedString& boneName,
                                const RE::NiPoint3& pickDirection)
    {
        if (!victim || boneName.empty()) {
            return;
        }

        if (PlayWoundBloodImpact(victim, boneName, pickDirection, true)) {
            return;
        }

        const RE::NiPoint3 fallbackDirection{ 0.0f, 0.0f, -1.0f };
        PlayWoundBloodImpact(victim, boneName, fallbackDirection, false);
    }

    void BeginPostEmbedBleeding(RE::Actor* victim, const RE::BSFixedString& boneName,
                                const RE::NiPoint3& pickDirection)
    {
        if (!victim || boneName.empty()) {
            return;
        }
        if (BitingAxesVR::postEmbedBloodDurationSec <= 0.0) {
            return;
        }

        BleedingWoundSite site{};
        site.victimHandle = victim->CreateRefHandle();
        site.boneName = boneName;
        site.pickDirection = pickDirection;
        const auto now = std::chrono::steady_clock::now();
        site.endTime =
            now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                      std::chrono::duration<double>(BitingAxesVR::postEmbedBloodDurationSec));
        site.lastBloodTime = now;
        site.haveLastBloodTime = true;
        g_bleedingWounds.push_back(std::move(site));

        IW_LOG_INFO("[bite] post-embed bleed started on '{}' at bone '{}' for {:.1f}s",
                    victim->GetDisplayFullName(), boneName.c_str(),
                    static_cast<float>(BitingAxesVR::postEmbedBloodDurationSec));
    }

    void BeginPostEmbedBleeding(const EmbedState& e)
    {
        if (!e.victimActor || !e.boneNode) {
            return;
        }

        const RE::BSFixedString boneName = ResolveEmbedBoneName(e);
        if (boneName.empty()) {
            return;
        }

        BeginPostEmbedBleeding(e.victimActor.get(), boneName, BuildDislodgeBloodDirection(e));
    }

    void BeginPostEmbedBleeding(const LodgedAxeInBody& lodged)
    {
        if (!lodged.victimActor) {
            return;
        }

        const RE::BSFixedString boneName = lodged.embedBoneName.empty() && lodged.boneNode
                                               ? lodged.boneNode->name
                                               : lodged.embedBoneName;
        if (boneName.empty()) {
            return;
        }

        BeginPostEmbedBleeding(lodged.victimActor.get(), boneName,
                               BuildLodgedDislodgeBloodDirection(lodged));
    }

    void UpdatePostEmbedBleeding()
    {
        if (g_bleedingWounds.empty()) {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        const float interval = static_cast<float>(BitingAxesVR::embedBloodIntervalSec);

        g_bleedingWounds.erase(
            std::remove_if(g_bleedingWounds.begin(), g_bleedingWounds.end(),
                           [&](BleedingWoundSite& site) {
                               if (now >= site.endTime) {
                                   return true;
                               }

                               auto victim = site.victimHandle.get().get();
                               if (!victim || !victim->Get3D()) {
                                   return true;
                               }

                               if (!site.haveLastBloodTime) {
                                   site.lastBloodTime = now;
                                   site.haveLastBloodTime = true;
                                   return false;
                               }

                               const float elapsed =
                                   std::chrono::duration<float>(now - site.lastBloodTime).count();
                               if (elapsed >= interval) {
                                   PlayBleedingWoundBlood(victim, site.boneName, site.pickDirection);
                                   site.lastBloodTime = now;
                               }
                               return false;
                           }),
            g_bleedingWounds.end());
    }

    void InitEmbedVictimEffects(EmbedState& e)
    {
        const auto now = std::chrono::steady_clock::now();
        e.lastWorldAxeBloodTime = now;
        e.haveWorldAxeBloodTime = true;
        e.lastWorldAxeHealthDrainTime = now;
        e.haveWorldAxeHealthDrainTime = true;
    }

    void InitLodgedAxeVictimEffects(LodgedAxeInBody& lodged)
    {
        const auto now = std::chrono::steady_clock::now();
        lodged.lastBloodFxTime = now;
        lodged.haveLastBloodFxTime = true;
        lodged.lastHealthDrainTime = now;
        lodged.haveLastHealthDrainTime = true;
    }

    LodgedAxeInBody* FindLodgedAxeNearHand(RE::Actor* victim, const RE::NiPoint3& handPos, float maxRadius)
    {
        if (!victim || g_lodgedAxes.empty()) {
            return nullptr;
        }

        LodgedAxeInBody* best = nullptr;
        float bestDist = maxRadius;
        for (auto& lodged : g_lodgedAxes) {
            if (!lodged.victimActor || lodged.victimActor.get() != victim || !lodged.axeRefr) {
                continue;
            }
            const RE::NiPoint3 woundWorld = GetLodgedWoundWorld(lodged);
            const float dist = (handPos - woundWorld).Length();
            if (dist < bestDist) {
                bestDist = dist;
                best = &lodged;
            }
        }
        return best;
    }

    LodgedAxeInBody* FindLodgedAxeByRefr(RE::TESObjectREFR* refr)
    {
        if (!refr) {
            return nullptr;
        }
        for (auto& lodged : g_lodgedAxes) {
            if (lodged.axeRefr && lodged.axeRefr.get() == refr) {
                return &lodged;
            }
        }
        return nullptr;
    }

    void CancelLodgedExtract()
    {
        g_lodgedExtract = {};
    }

    void CancelLodgedExtractIfHandReleased(bool isLeft)
    {
        if (!g_lodgedExtract.active) {
            return;
        }
        if (g_lodgedExtract.steadyHandActive && isLeft == g_lodgedExtract.steadyHandIsLeft) {
            CancelLodgedExtract();
            return;
        }
        if (g_lodgedExtract.pullViaAxeGrab && g_lodgedExtract.pullHandEngaged &&
            isLeft == g_lodgedExtract.pullHandIsLeft) {
            CancelLodgedExtract();
        }
    }

    void EngagePullHand(bool pullIsLeft, bool viaAxeGrab)
    {
        g_lodgedExtract.pullHandEngaged = true;
        g_lodgedExtract.pullHandIsLeft = pullIsLeft;
        g_lodgedExtract.pullViaAxeGrab = viaAxeGrab;
        g_lodgedExtract.pullHandBaselineReady = false;
        g_lodgedExtract.pullHandBaselineDist = 0.0f;
        g_lodgedExtract.steadyHandBaselineReady = false;
        g_lodgedExtract.steadyHandBaselineDist = 0.0f;
        IW_LOG_INFO("[bite] pull hand {} engaged{}",
                    pullIsLeft ? "L" : "R", viaAxeGrab ? " (world axe grab)" : "");
    }

    void BeginSteadyHandExtract(bool steadyIsLeft, const LodgedAxeInBody& lodged, RE::Actor* victim)
    {
        g_lodgedExtract = {};
        g_lodgedExtract.active = true;
        g_lodgedExtract.lodgedAxeRefId = lodged.axeRefr->GetFormID();
        g_lodgedExtract.victimHandle = victim->CreateRefHandle();
        g_lodgedExtract.steadyHandActive = true;
        g_lodgedExtract.steadyHandIsLeft = steadyIsLeft;
        g_lodgedExtract.pullHandIsLeft = !steadyIsLeft;
        g_lodgedExtract.startTime = std::chrono::steady_clock::now();
        IW_LOG_INFO("[bite] {} hand steadying '{}'; pull lodged axe with {} hand",
                    steadyIsLeft ? "L" : "R", victim->GetDisplayFullName(), !steadyIsLeft ? "L" : "R");
    }

    LodgedAxeInBody* FindLodgedAxeByRefId(RE::FormID refId)
    {
        if (!refId) {
            return nullptr;
        }
        for (auto& lodged : g_lodgedAxes) {
            if (lodged.axeRefr && lodged.axeRefr->GetFormID() == refId) {
                return &lodged;
            }
        }
        return nullptr;
    }

    float HandPullDeltaFromWound(bool isLeft, const RE::NiPoint3& woundWorld, float baselineDist, bool haveBaseline)
    {
        RE::NiPoint3 handPos{};
        if (!GetRealHandPos(isLeft, handPos) || !haveBaseline) {
            return 0.0f;
        }
        const float handDist = (handPos - woundWorld).Length();
        return handDist - baselineDist;
    }

    RE::BGSEquipSlot* GetHandEquipSlot(bool isLeft)
    {
        auto* defaults = RE::BGSDefaultObjectManager::GetSingleton();
        if (!defaults) {
            return nullptr;
        }
        const auto slotId =
            isLeft ? RE::DEFAULT_OBJECT::kLeftHandEquip : RE::DEFAULT_OBJECT::kRightHandEquip;
        auto* form = defaults->GetObject(slotId);
        return form ? form->As<RE::BGSEquipSlot>() : nullptr;
    }

    void ReturnExtractedAxeToPlayer(RE::TESObjectWEAP* weapon, bool isLeft)
    {
        if (!weapon) {
            return;
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* equipMgr = RE::ActorEquipManager::GetSingleton();
        auto* slot = GetHandEquipSlot(isLeft);
        if (!player || !equipMgr || !slot) {
            return;
        }

        player->AddObjectToContainer(weapon, nullptr, 1, nullptr);
        equipMgr->EquipObject(player, weapon, nullptr, 1, slot, false, true, false, true);
        IW_LOG_INFO("[bite] returned extracted axe '{}' to {} hand",
                    weapon->GetName(), isLeft ? "L" : "R");
    }

    void ExtractLodgedAxe(LodgedAxeInBody& lodged, bool isLeft)
    {
        if (!lodged.axeRefr) {
            return;
        }

        auto* weapon = lodged.axeRefr->GetBaseObject()->As<RE::TESObjectWEAP>();
        if (!weapon) {
            return;
        }

        const RE::NiPoint3 woundWorld = GetLodgedWoundWorld(lodged);
        PlayDislodgeSound(woundWorld, lodged.boneNode.get());
        PlayDislodgeBloodEffectForLodged(lodged);
        BeginPostEmbedBleeding(lodged);
        Haptic(isLeft, static_cast<float>(BitingAxesVR::hapticExtract));

        BitingAxesAPI::EmbedSnapshot extractSnapshot{};
        FillLodgedApiSnapshot(lodged, isLeft, extractSnapshot);
        extractSnapshot.equippedWeaponFormId = weapon->GetFormID();

        const RE::FormID removeRefId = lodged.axeRefr->GetFormID();
        UntrackWorldAxe(removeRefId, "extracted");

        DetachAxeFromBone(lodged.axeRefr.get());
        lodged.axeRefr->Disable();

        g_lodgedAxes.erase(
            std::remove_if(g_lodgedAxes.begin(), g_lodgedAxes.end(),
                           [removeRefId](const LodgedAxeInBody& entry) {
                               return entry.axeRefr && entry.axeRefr->GetFormID() == removeRefId;
                           }),
            g_lodgedAxes.end());

        if (g_lodgedExtract.active && g_lodgedExtract.lodgedAxeRefId == removeRefId) {
            CancelLodgedExtract();
        }

        SKSE::GetTaskInterface()->AddTask([extractSnapshot, weapId = weapon->GetFormID(), isLeft]() {
            if (auto* weap = RE::TESForm::LookupByID<RE::TESObjectWEAP>(weapId)) {
                ReturnExtractedAxeToPlayer(weap, isLeft);
            }
            BitingAxesAPI::NotifyWeaponExtractedFromBody(extractSnapshot);
        });

        const char* victimName =
            lodged.victimActor ? lodged.victimActor->GetDisplayFullName() : "<unknown>";
        IW_LOG_INFO("[bite] extracted lodged axe from '{}' via pull ({} hand)",
                    victimName, isLeft ? "L" : "R");
    }

    void ClearLodgedExtractForHand(bool isLeft)
    {
        CancelLodgedExtractIfHandReleased(isLeft);
    }

    void OnHiggsGrabbed(bool isLeft, RE::TESObjectREFR* a_refr)
    {
        if (!a_refr || g_lodgedAxes.empty()) {
            return;
        }

        // Direct grab of the lodged world-model axe — this hand pulls and receives the re-equip.
        if (LodgedAxeInBody* lodged = FindLodgedAxeByRefr(a_refr)) {
            if (!g_lodgedExtract.active || g_lodgedExtract.lodgedAxeRefId != lodged->axeRefr->GetFormID()) {
                g_lodgedExtract = {};
                g_lodgedExtract.active = true;
                g_lodgedExtract.lodgedAxeRefId = lodged->axeRefr->GetFormID();
                if (lodged->victimActor) {
                    g_lodgedExtract.victimHandle = lodged->victimActor->CreateRefHandle();
                }
                g_lodgedExtract.startTime = std::chrono::steady_clock::now();
            }
            EngagePullHand(isLeft, true);
            return;
        }

        auto* victim = a_refr->As<RE::Actor>();
        if (!victim || victim == RE::PlayerCharacter::GetSingleton()) {
            return;
        }

        if (ShouldApplyLodgedNpcHold(victim)) {
            BeginLodgedNpcHold(isLeft, victim);
        }

        RE::NiPoint3 handPos{};
        if (!GetRealHandPos(isLeft, handPos)) {
            return;
        }

        LodgedAxeInBody* lodged =
            FindLodgedAxeNearHand(victim, handPos, static_cast<float>(BitingAxesVR::lodgedExtractGrabRadius));
        if (!lodged || !lodged->axeRefr) {
            return;
        }

        const RE::FormID axeId = lodged->axeRefr->GetFormID();

        if (!g_lodgedExtract.active || g_lodgedExtract.lodgedAxeRefId != axeId) {
            BeginSteadyHandExtract(isLeft, *lodged, victim);
            return;
        }

        // Second hand on the same victim while a steady session is active — pull/equip hand.
        if (g_lodgedExtract.steadyHandActive && isLeft != g_lodgedExtract.steadyHandIsLeft) {
            EngagePullHand(isLeft, false);
        }
    }

    void UpdateLodgedAxeExtraction()
    {
        if (!g_lodgedExtract.active || g_lodgedAxes.empty() || !g_higgsInterface) {
            return;
        }

        auto& extract = g_lodgedExtract;
        const float releaseDelay = static_cast<float>(BitingAxesVR::biteReleaseDelay);
        const float pullDistance = static_cast<float>(BitingAxesVR::bitePullDistance);

        LodgedAxeInBody* lodged = FindLodgedAxeByRefId(extract.lodgedAxeRefId);
        if (!lodged || !lodged->axeRefr) {
            CancelLodgedExtract();
            return;
        }

        if (extract.steadyHandActive) {
            if (!g_higgsInterface->IsHoldingObject(extract.steadyHandIsLeft)) {
                CancelLodgedExtract();
                return;
            }
            auto* held = g_higgsInterface->GetGrabbedObject(extract.steadyHandIsLeft);
            auto* expectedVictim = extract.victimHandle.get().get();
            if (!held || !expectedVictim || held->As<RE::Actor>() != expectedVictim) {
                CancelLodgedExtract();
                return;
            }
        }

        if (extract.pullViaAxeGrab && extract.pullHandEngaged) {
            if (!g_higgsInterface->IsHoldingObject(extract.pullHandIsLeft)) {
                CancelLodgedExtract();
                return;
            }
            auto* held = g_higgsInterface->GetGrabbedObject(extract.pullHandIsLeft);
            if (!held || held != lodged->axeRefr.get()) {
                CancelLodgedExtract();
                return;
            }
        }

        const RE::NiPoint3 woundWorld = GetLodgedWoundWorld(*lodged);
        const float elapsed =
            std::chrono::duration<float>(std::chrono::steady_clock::now() - extract.startTime).count();
        const bool canRelease = elapsed >= releaseDelay;

        const bool defaultPullIsLeft =
            extract.pullHandEngaged ? extract.pullHandIsLeft : !extract.steadyHandIsLeft;

        if (canRelease) {
            RE::NiPoint3 handPos{};
            if (!extract.pullHandBaselineReady && GetRealHandPos(defaultPullIsLeft, handPos)) {
                extract.pullHandBaselineDist = (handPos - woundWorld).Length();
                extract.pullHandBaselineReady = true;
            }
            if (extract.steadyHandActive && !extract.steadyHandBaselineReady &&
                GetRealHandPos(extract.steadyHandIsLeft, handPos)) {
                extract.steadyHandBaselineDist = (handPos - woundWorld).Length();
                extract.steadyHandBaselineReady = true;
            }
        }

        if (!canRelease || !extract.pullHandBaselineReady) {
            return;
        }

        const float pullHandDelta = HandPullDeltaFromWound(
            defaultPullIsLeft, woundWorld, extract.pullHandBaselineDist, true);
        const float steadyHandDelta =
            extract.steadyHandActive && extract.steadyHandBaselineReady
                ? HandPullDeltaFromWound(extract.steadyHandIsLeft, woundWorld, extract.steadyHandBaselineDist,
                                         true)
                : 0.0f;

        bool equipIsLeft = defaultPullIsLeft;
        float bestDelta = pullHandDelta;
        if (steadyHandDelta > bestDelta) {
            bestDelta = steadyHandDelta;
            equipIsLeft = extract.steadyHandIsLeft;
        }

        if (bestDelta > pullDistance) {
            ExtractLodgedAxe(*lodged, equipIsLeft);
        }
    }

    // ---- Metal armor detection -----------------------------------------------
    //
    // Vanilla/DLC armors carry ArmorMaterial* keywords (ArmorMaterialIron,
    // ArmorMaterialSteel, ArmorMaterialDragonplate, DLC2ArmorMaterialNordicHeavy, ...).
    // We substring-match the metal ones; as a fallback, unkeyworded heavy armor
    // (common with mods) is treated as metal since heavy = plate in practice.

    bool IsMetalArmor(RE::TESObjectARMO* armor)
    {
        if (!armor) {
            return false;
        }

        static constexpr std::string_view kMetalMaterialMarkers[] = {
            "MaterialIron",           // iron + banded iron
            "MaterialSteel",          // steel + steel plate
            "MaterialImperialHeavy",  // imperial plate
            "MaterialDwarven",
            "MaterialOrcish",
            "MaterialEbony",
            "MaterialDaedric",
            "MaterialDragonplate",    // dragonbone plate
            "MaterialBlades",
            "NordicHeavy",            // DLC2ArmorMaterialNordicHeavy
            "Stalhrim",               // DLC2 stalhrim heavy/light
            "DLC1ArmorMaterialDawnguard",
            "MaterialWolf",           // Companions wolf armor (steel)
        };
        for (const auto marker : kMetalMaterialMarkers) {
            if (armor->ContainsKeywordString(marker)) {
                return true;
            }
        }

        // No material keyword matched: modded armor often omits them. Heavy armor
        // without keywords is overwhelmingly metal plate, so count it.
        return armor->GetArmorType() == RE::BIPED_MODEL::ArmorType::kHeavyArmor;
    }

    // The worn armor piece covering the struck bone, resolved via biped slots:
    // head bones -> helmet (hair slot 31, then head slot 30, then circlet 42),
    // feet/calves -> boots, everything else -> body cuirass.
    RE::TESObjectARMO* GetArmorAtBone(RE::Actor* victim, std::string_view boneName)
    {
        if (!victim) {
            return nullptr;
        }

        using Slot = RE::BGSBipedObjectForm::BipedObjectSlot;

        const auto contains = [&](const char* marker) {
            return boneName.find(marker) != std::string_view::npos;
        };

        if (contains("[Head]") || boneName == "Head" || contains("[Neck]") || contains("Neck")) {
            if (auto* helm = victim->GetWornArmor(Slot::kHair)) {
                return helm;
            }
            if (auto* helm = victim->GetWornArmor(Slot::kHead)) {
                return helm;
            }
            return victim->GetWornArmor(Slot::kCirclet);
        }
        if (contains("Foot") || contains("Calf") || contains("[Ft]") || contains("[Clf]")) {
            return victim->GetWornArmor(Slot::kFeet);
        }
        if (contains("Hand") || contains("Finger") || contains("Thumb") ||
            contains("Forearm") || contains("UpperArm") ||
            contains("[LHnd]") || contains("[RHnd]") ||
            contains("[LUArm]") || contains("[RUArm]") ||
            contains("[LLArm]") || contains("[RLArm]")) {
            return victim->GetWornArmor(Slot::kHands);
        }
        // Torso, spine, pelvis, clavicle/shoulder -> cuirass.
        return victim->GetWornArmor(Slot::kBody);
    }

    // True when the victim has metal armor covering the struck location. outArmor
    // (optional) receives the armor piece for logging.
    bool IsWearingMetalArmorAt(RE::Actor* victim, std::string_view boneName,
                               RE::TESObjectARMO** outArmor = nullptr)
    {
        RE::TESObjectARMO* armor = GetArmorAtBone(victim, boneName);
        if (outArmor) {
            *outArmor = armor;
        }
        return IsMetalArmor(armor);
    }

    bool BoneNameContains(std::string_view boneName, const char* marker)
    {
        return boneName.find(marker) != std::string_view::npos;
    }

    // Shoulder/clavicle stays biteable even through metal cuirass (per design).
    bool IsShoulderBone(std::string_view boneName)
    {
        static constexpr const char* kShoulderMarkers[] = {
            "[ClvL]", "[ClvR]", "Clavicle",
        };
        for (const char* marker : kShoulderMarkers) {
            if (BoneNameContains(boneName, marker)) {
                return true;
            }
        }
        return false;
    }

    bool IsHeadOrNeckBone(std::string_view boneName)
    {
        if (boneName == "Head" || BoneNameContains(boneName, "[Head]")) {
            return true;
        }
        return BoneNameContains(boneName, "[Neck]") || BoneNameContains(boneName, "Neck");
    }

    bool IsTorsoBone(std::string_view boneName)
    {
        if (IsShoulderBone(boneName)) {
            return false;
        }
        static constexpr const char* kTorsoMarkers[] = {
            "[Spn2]", "[Spn1]", "[Spine]", "[Pelv]", "Pelvis", "Spine", "Chest",
        };
        for (const char* marker : kTorsoMarkers) {
            if (BoneNameContains(boneName, marker)) {
                return true;
            }
        }
        return false;
    }

    // Metal helmet or metal cuirass on head/torso blocks embedding and deflects the axe.
    bool ShouldDeflectFromMetalArmor(RE::Actor* victim, std::string_view boneName,
                                     RE::TESObjectARMO** outArmor = nullptr)
    {
        if (!victim || boneName.empty()) {
            return false;
        }
        if (!IsHeadOrNeckBone(boneName) && !IsTorsoBone(boneName)) {
            return false;
        }
        return IsWearingMetalArmorAt(victim, boneName, outArmor);
    }

    // WeaponCollisionVR spark pattern: WPNAxeVsMetalImpact + WPNAxeVsMetal clang.
    static constexpr RE::FormID kAxeMetalSparkImpact = 0x0004BB55u;  // WPNAxeVsMetalImpact
    static constexpr RE::FormID kMetalParrySound = 0x0003C73Cu;

    void SpawnMetalSpark(RE::TESObjectREFR* host, const RE::NiPoint3& contactPos, RE::NiAVObject* attachNode)
    {
        auto* impact = RE::TESForm::LookupByID<RE::BGSImpactData>(kAxeMetalSparkImpact);
        if (!impact) {
            return;
        }
        const char* model = impact->GetModel();
        if (!model || !model[0]) {
            return;
        }
        auto* cell = host ? host->GetParentCell() : nullptr;
        if (!cell) {
            return;
        }

        RE::NiPoint3 rot{};
        auto* target = attachNode ? netimmerse_cast<RE::NiNode*>(attachNode) : nullptr;
        RE::BSTempEffectParticle::Spawn(cell, 1.0f, model, rot, contactPos, 1.0f, 7, target);
    }

    void PlayMetalParrySound(const RE::NiPoint3& position, RE::NiAVObject* followNode)
    {
        auto* impact = RE::TESForm::LookupByID<RE::BGSImpactData>(kAxeMetalSparkImpact);
        RE::BGSSoundDescriptorForm* sound = impact ? impact->sound1 : nullptr;
        if (!sound) {
            if (auto* form = RE::TESForm::LookupByID(kMetalParrySound)) {
                if (form->Is(RE::FormType::SoundRecord)) {
                    sound = form->As<RE::BGSSoundDescriptorForm>();
                }
            }
        }
        if (!sound) {
            return;
        }

        auto* audio = RE::BSAudioManager::GetSingleton();
        if (!audio) {
            return;
        }

        RE::BSSoundHandle handle;
        if (!audio->BuildSoundDataFromDescriptor(handle, static_cast<RE::BSISoundDescriptor*>(sound), 16)) {
            return;
        }
        if (handle.soundID == static_cast<std::uint32_t>(-1)) {
            return;
        }

        handle.SetPosition(position);
        if (followNode) {
            handle.SetObjectToFollow(followNode);
        }
        handle.SetVolume(1.0f);
        handle.Play();
    }

    void ApplyParryDeflect(bool isLeft, const RE::NiPoint3& hitPos, float chopSpeed)
    {
        RE::NiPoint3 handPos;
        if (!GetRealHandPos(isLeft, handPos)) {
            return;
        }

        RE::NiPoint3 dir = handPos - hitPos;
        const float len = dir.Length();
        if (len < 1e-3f) {
            dir = { 0.0f, -1.0f, 0.0f };
        } else {
            dir = dir / len;
        }

        const float pushSkyrim = std::clamp(chopSpeed * 0.45f, 55.0f, 240.0f);
        RE::hkVector4 vel{
            dir.x * pushSkyrim * kSkyrimToHavok,
            dir.y * pushSkyrim * kSkyrimToHavok,
            dir.z * pushSkyrim * kSkyrimToHavok,
            0.0f,
        };

        if (!g_higgsInterface) {
            return;
        }

        if (auto* handObj = g_higgsInterface->GetHandRigidBody(isLeft)) {
            if (auto* handRb = netimmerse_cast<RE::bhkRigidBody*>(handObj)) {
                handRb->SetLinearVelocity(vel);
            }
        }
        if (auto* weapObj = g_higgsInterface->GetWeaponRigidBody(isLeft)) {
            if (auto* weapRb = netimmerse_cast<RE::bhkRigidBody*>(weapObj)) {
                weapRb->SetLinearVelocity(vel);
            }
        }
    }

    std::chrono::steady_clock::time_point g_lastDeflectTime[2]{};

    void DeflectFromMetalArmor(const PlanckAPI::PlanckHitData& hit,
                               RE::TESObjectARMO* struckArmor, RE::NiAVObject* weaponNode)
    {
        const auto now = std::chrono::steady_clock::now();
        const std::size_t handIdx = hit.isLeft ? 1u : 0u;
        const float sinceLast =
            std::chrono::duration<float>(now - g_lastDeflectTime[handIdx]).count();
        if (sinceLast < 0.10f) {
            return;
        }
        g_lastDeflectTime[handIdx] = now;

        const float chopSpeed = hit.velocity.Length();
        const char* armorName = "none";
        if (struckArmor) {
            armorName = struckArmor->GetFullName();
            if (!armorName || !armorName[0]) {
                armorName = struckArmor->GetName();
            }
            if (!armorName || !armorName[0]) {
                armorName = "<unnamed>";
            }
        }

        IW_LOG_INFO("[bite] DEFLECT  hand={}  bone={}  armor='{}'  speed={:.0f}",
                    hit.isLeft ? "L" : "R",
                    hit.nodeName.empty() ? "<unnamed>" : hit.nodeName.c_str(),
                    armorName, chopSpeed);

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (player) {
            player->NotifyAnimationGraph("recoilStop");
            player->NotifyAnimationGraph("AttackStop");
            player->NotifyAnimationGraph("recoilLargeStart");
        }

        ApplyParryDeflect(hit.isLeft, hit.position, chopSpeed);
        Haptic(hit.isLeft, 1.0f);

        auto contactPos = hit.position;
        auto* host = player;
        auto* attach = weaponNode;
        SKSE::GetTaskInterface()->AddTask([host, contactPos, attach]() {
            SpawnMetalSpark(host, contactPos, attach);
            PlayMetalParrySound(contactPos, attach);
        });
    }

    // Head/skull recognition, same markers Neck Snap VR uses: the vanilla head bone is
    // "NPC Head [Head]"; some creature skeletons just use "Head".
    bool IsHeadBoneName(const char* boneName)
    {
        if (!boneName || !boneName[0]) {
            return false;
        }
        if (std::strstr(boneName, "[Head]") != nullptr) {
            return true;
        }
        return _stricmp(boneName, "Head") == 0;
    }

    EmbedBodyRegion ClassifyEmbedBodyRegion(std::string_view boneName)
    {
        if (boneName.empty()) {
            return EmbedBodyRegion::Unknown;
        }

        if (IsHeadBoneName(boneName.data())) {
            return EmbedBodyRegion::Head;
        }
        if (BoneNameContains(boneName, "[Neck]") || BoneNameContains(boneName, "Neck")) {
            return EmbedBodyRegion::Neck;
        }
        if (BoneNameContains(boneName, "Hand") || BoneNameContains(boneName, "[LHnd]") ||
            BoneNameContains(boneName, "[RHnd]") || BoneNameContains(boneName, "Finger") ||
            BoneNameContains(boneName, "Thumb")) {
            return EmbedBodyRegion::Hand;
        }
        if (BoneNameContains(boneName, "Forearm") || BoneNameContains(boneName, "[LLArm]") ||
            BoneNameContains(boneName, "[RLArm]")) {
            return EmbedBodyRegion::Forearm;
        }
        if (BoneNameContains(boneName, "UpperArm") || BoneNameContains(boneName, "[LUArm]") ||
            BoneNameContains(boneName, "[RUArm]")) {
            return EmbedBodyRegion::UpperArm;
        }
        if (IsShoulderBone(boneName)) {
            return EmbedBodyRegion::Shoulder;
        }
        if (BoneNameContains(boneName, "Foot") || BoneNameContains(boneName, "[Lft]") ||
            BoneNameContains(boneName, "[Rft]") || BoneNameContains(boneName, "Toe") ||
            BoneNameContains(boneName, "[Ft]")) {
            return EmbedBodyRegion::Foot;
        }
        if (BoneNameContains(boneName, "Calf") || BoneNameContains(boneName, "[LClf]") ||
            BoneNameContains(boneName, "[RClf]") || BoneNameContains(boneName, "[Clf]")) {
            return EmbedBodyRegion::Calf;
        }
        if (BoneNameContains(boneName, "Thigh") || BoneNameContains(boneName, "[LThg]") ||
            BoneNameContains(boneName, "[RThg]") || BoneNameContains(boneName, "[Thg]")) {
            return EmbedBodyRegion::Thigh;
        }
        if (IsTorsoBone(boneName)) {
            return EmbedBodyRegion::Torso;
        }

        return EmbedBodyRegion::Unknown;
    }

    void LogEmbedBodyLocation(const char* eventTag, EmbedBodyRegion region, const RE::BSFixedString& boneName,
                              RE::Actor* victim, const char* weaponName, bool isLeft)
    {
        const char* bone = boneName.empty() ? "<unnamed>" : boneName.c_str();
        const char* victimName = victim ? victim->GetDisplayFullName() : "<none>";
        const char* weapon = weaponName ? weaponName : "<unknown>";

        IW_LOG_INFO("[bite][loc] {}  region={}  bone='{}'  victim='{}'  weapon={}  hand={}",
                    eventTag, EmbedBodyRegionName(region), bone, victimName, weapon, isLeft ? "L" : "R");
    }

    // Head embed/kill is always allowed out of combat. In combat the victim must be below
    // CombatHeadshotHealthFrac of max health first.
    bool IsHeadshotAllowed(RE::Actor* victim, float* outHealthFrac = nullptr)
    {
        if (!victim) {
            return false;
        }

        const float headshotThreshold = static_cast<float>(BitingAxesVR::combatHeadshotHealthFrac);

        auto* avOwner = victim->AsActorValueOwner();
        if (!avOwner) {
            return false;
        }

        const float maxHealth = avOwner->GetPermanentActorValue(RE::ActorValue::kHealth);
        const float curHealth = avOwner->GetActorValue(RE::ActorValue::kHealth);
        const float healthFrac = maxHealth > 0.0f ? curHealth / maxHealth : 0.0f;
        if (outHealthFrac) {
            *outHealthFrac = healthFrac;
        }

        if (!victim->IsInCombat()) {
            return true;
        }

        return healthFrac <= headshotThreshold;
    }

    // Axe buried in the skull -> instant death (Neck Snap VR's kill method: overkill
    // health damage through the damage modifier, which credits a proper death instead
    // of SetActorValue tricks). Returns true if the victim was killed.
    bool KillVictimFromHeadBite(RE::Actor* victim)
    {
        if (!victim || victim->IsDead()) {
            return false;
        }

        if (victim->IsEssential()) {
            IW_LOG_INFO("[bite] head bite on '{}' blocked - essential NPC",
                        victim->GetDisplayFullName());
            return false;
        }

        auto* avOwner = victim->AsActorValueOwner();
        if (!avOwner) {
            return false;
        }

        const float maxHealth = avOwner->GetPermanentActorValue(RE::ActorValue::kHealth);
        const float killDamage = maxHealth > 1.0f ? maxHealth * 100.0f : 99999.0f;
        avOwner->DamageActorValue(RE::ActorValue::kHealth, killDamage);

        IW_LOG_INFO("[bite] HEADSHOT  killed '{}' (damage {:.0f})",
                    victim->GetDisplayFullName(), killDamage);
        return true;
    }

    // ---- Distress facial expression (FaceGen) --------------------------------
    //
    // Vanilla expression IDs (GECK / mfg): 4 = Surprise, 13 = Pained.
    // Layer Pained with shock brows and an open-mouth anguish phoneme stack.

    constexpr std::int32_t kExprSurprise = 4;
    constexpr std::int32_t kExprPained = 13;
    constexpr std::uint32_t kModBrowDownLeft = 2;
    constexpr std::uint32_t kModBrowDownRight = 3;
    constexpr std::uint32_t kModBrowInLeft = 4;
    constexpr std::uint32_t kModBrowInRight = 5;
    constexpr std::uint32_t kModBrowUpLeft = 6;
    constexpr std::uint32_t kModBrowUpRight = 7;
    constexpr std::uint32_t kModLookUp = 11;
    constexpr std::uint32_t kModSquintLeft = 12;
    constexpr std::uint32_t kModSquintRight = 13;
    constexpr std::uint32_t kPhonemeAah = 0;
    constexpr std::uint32_t kPhonemeBigAah = 1;
    constexpr std::uint32_t kPhonemeEh = 6;
    constexpr std::uint32_t kPhonemeOh = 11;

    bool HasFaceGenHead(RE::Actor* actor)
    {
        const auto* race = actor ? actor->GetRace() : nullptr;
        return race && race->data.flags.all(RE::RACE_DATA::Flag::kFaceGenHead);
    }

    void SetFaceKeyframe(RE::BSFaceGenKeyframeMultiple& keyframe, std::uint32_t idx, float value)
    {
        if (keyframe.values && idx < keyframe.count) {
            keyframe.SetValue(idx, value);
        }
    }

    void RefreshActorFace(RE::Actor* actor)
    {
        auto* face = actor ? actor->GetFaceNodeSkinned() : nullptr;
        if (!face) {
            return;
        }

        // SKSE expression task uses UpdateModelFace; VR offset from local SKSE sources.
        using func_t = std::uint32_t(*)(RE::NiAVObject*);
        if (REL::Module::IsVR()) {
            static REL::Relocation<func_t> updateModelFace{ REL::Offset(0x3EB710) };
            updateModelFace(face);
            return;
        }

        RE::NiUpdateData ud{};
        face->Update(ud);
    }

    bool ApplyDistressFace(RE::Actor* actor)
    {
        if (!actor || !HasFaceGenHead(actor)) {
            return false;
        }

        auto* faceData = actor->GetFaceGenAnimationData();
        if (!faceData) {
            return false;
        }

        faceData->SetExpressionOverride(kExprPained, 1.0f);
        SetFaceKeyframe(faceData->expressionKeyFrame, static_cast<std::uint32_t>(kExprPained), 1.0f);
        SetFaceKeyframe(faceData->expressionKeyFrame, static_cast<std::uint32_t>(kExprSurprise), 0.55f);

        // Shock: wide raised brows + eyes; pain: knit brow and grimace squint.
        SetFaceKeyframe(faceData->modifierKeyFrame, kModBrowUpLeft, 1.0f);
        SetFaceKeyframe(faceData->modifierKeyFrame, kModBrowUpRight, 1.0f);
        SetFaceKeyframe(faceData->modifierKeyFrame, kModBrowInLeft, 0.95f);
        SetFaceKeyframe(faceData->modifierKeyFrame, kModBrowInRight, 0.95f);
        SetFaceKeyframe(faceData->modifierKeyFrame, kModBrowDownLeft, 0.80f);
        SetFaceKeyframe(faceData->modifierKeyFrame, kModBrowDownRight, 0.80f);
        SetFaceKeyframe(faceData->modifierKeyFrame, kModSquintLeft, 0.92f);
        SetFaceKeyframe(faceData->modifierKeyFrame, kModSquintRight, 0.92f);
        SetFaceKeyframe(faceData->modifierKeyFrame, kModLookUp, 0.40f);

        // Open-mouth anguish: gasp/cry phoneme stack.
        SetFaceKeyframe(faceData->phenomeKeyFrame, kPhonemeBigAah, 0.95f);
        SetFaceKeyframe(faceData->phenomeKeyFrame, kPhonemeAah, 0.75f);
        SetFaceKeyframe(faceData->phenomeKeyFrame, kPhonemeEh, 0.55f);
        SetFaceKeyframe(faceData->phenomeKeyFrame, kPhonemeOh, 0.45f);

        RefreshActorFace(actor);
        IW_LOG_INFO("[bite] shock/pain face applied to '{}'", actor->GetDisplayFullName());
        return true;
    }

    void ClearDistressFace(RE::Actor* actor)
    {
        if (!actor || !HasFaceGenHead(actor)) {
            return;
        }

        if (auto* faceData = actor->GetFaceGenAnimationData()) {
            faceData->ClearExpressionOverride();
            faceData->Reset(0.0f, true, true, true, false);
        }
        actor->ClearExpressionOverride();
        RefreshActorFace(actor);
        IW_LOG_INFO("[bite] distress face cleared from '{}'", actor->GetDisplayFullName());
    }

    void MaintainDistressFace(EmbedState& e)
    {
        if (!e.distressFaceActive || !e.victimActor) {
            return;
        }

        const float elapsed =
            std::chrono::duration<float>(std::chrono::steady_clock::now() - e.startTime).count();
        if (elapsed < static_cast<float>(BitingAxesVR::biteDistressFaceSeconds)) {
            return;
        }

        e.distressFaceActive = false;
        const RE::ActorHandle handle = e.victimActor->CreateRefHandle();
        SKSE::GetTaskInterface()->AddTask([handle]() {
            if (auto* actor = handle.get().get()) {
                ClearDistressFace(actor);
            }
        });
    }

    bool IsLivingVictim(RE::Actor* actor)
    {
        return actor && !actor->IsDead();
    }

    bool ShouldApplyLodgedNpcHold(RE::Actor* victim)
    {
        return victim && IsLivingVictim(victim) && victim->IsInCombat() && CanAffordLodgedNpcHold();
    }

    bool IsDeadOrIncapacitatedVictim(RE::Actor* actor)
    {
        if (!actor) {
            return true;
        }
        if (actor->IsDead(false)) {
            return true;
        }
        if (auto* state = actor->AsActorState()) {
            switch (state->GetLifeState()) {
            case RE::ACTOR_LIFE_STATE::kDead:
            case RE::ACTOR_LIFE_STATE::kDying:
            case RE::ACTOR_LIFE_STATE::kUnconcious:
            case RE::ACTOR_LIFE_STATE::kBleedout:
            case RE::ACTOR_LIFE_STATE::kEssentialDown:
                return true;
            default:
                break;
            }
        }
        return false;
    }

    bool IsUsingFurnitureWorkstationOrSitting(RE::Actor* actor)
    {
        if (!actor) {
            return true;
        }
        if (actor->GetOccupiedFurniture()) {
            return true;
        }
        if (auto* state = actor->AsActorState()) {
            if (state->IsSitting()) {
                return true;
            }
            switch (state->GetSitSleepState()) {
            case RE::SIT_SLEEP_STATE::kWantToSit:
            case RE::SIT_SLEEP_STATE::kWaitingForSitAnim:
            case RE::SIT_SLEEP_STATE::kIsSleeping:
            case RE::SIT_SLEEP_STATE::kWantToWake:
                return true;
            default:
                break;
            }
        }
        return false;
    }

    // Movement lock, stillness, controller drag, and combat freeze only apply to living
    // victims who are on their feet -- not corpses or NPCs seated/at a workstation.
    bool ShouldApplyEmbedMovementLock(RE::Actor* actor)
    {
        if (IsDeadOrIncapacitatedVictim(actor)) {
            return false;
        }
        if (IsUsingFurnitureWorkstationOrSitting(actor)) {
            return false;
        }
        return true;
    }

    void ClearVictimLocomotionState(RE::Actor* actor)
    {
        if (auto* state = actor ? actor->AsActorState() : nullptr) {
            state->actorState1.movingBack = 0;
            state->actorState1.movingForward = 0;
            state->actorState1.movingLeft = 0;
            state->actorState1.movingRight = 0;
            state->actorState1.walking = 0;
            state->actorState1.running = 0;
            state->actorState1.sprinting = 0;
            state->actorState2.forceRun = 0;
            state->actorState2.forceSneak = 0;
        }
    }

    void StopVictimLocomotionAnimation(RE::Actor* actor)
    {
        if (!actor) {
            return;
        }

        actor->SetGraphVariableFloat("Speed", 0.0f);
        actor->SetGraphVariableFloat("speed", 0.0f);
        actor->SetGraphVariableFloat("speedSampled", 0.0f);
        actor->SetGraphVariableFloat("SpeedSampled", 0.0f);
        actor->SetGraphVariableFloat("movementDirection", 0.0f);
        actor->SetGraphVariableBool("bForceIdleStop", true);
    }

    void FreezeVictimRigidBodies(RE::Actor* actor)
    {
        auto* root = actor ? actor->Get3D() : nullptr;
        if (!root) {
            return;
        }

        static const RE::hkVector4 kZero{ 0.0f, 0.0f, 0.0f, 0.0f };
        ForEachRigidBody(root, [&](RE::bhkRigidBody* rb) {
            rb->SetLinearVelocity(kZero);
            rb->SetAngularVelocity(kZero);
        });
    }

    void UpdateActorScene(RE::Actor* actor)
    {
        if (auto* root = actor ? actor->Get3D() : nullptr) {
            RE::NiUpdateData ud{};
            root->Update(ud);
        }
    }

    RE::NiPoint3 ConstrainVictimDragDelta(RE::Actor* actor, const RE::NiPoint3& delta,
                                          const RE::NiPoint3& currentWound, const RE::NiPoint3& realHand)
    {
        RE::NiPoint3 planar = delta;
        planar.z = 0.0f;

        // Pulling the axe changes hand-to-wound distance; never drag the body along that
        // direction or pull-out gets absorbed and the axe stays stuck.
        RE::NiPoint3 pullDir = realHand - currentWound;
        pullDir.z = 0.0f;
        const float pullLen = pullDir.Length();
        if (pullLen > 1e-4f) {
            pullDir = pullDir / pullLen;
            planar = planar - pullDir * Dot(planar, pullDir);
        }

        // Never tug the victim toward the player on the horizontal plane.
        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
            RE::NiPoint3 toPlayer = player->GetPosition() - actor->GetPosition();
            toPlayer.z = 0.0f;
            const float toPlayerLen = toPlayer.Length();
            if (toPlayerLen > 1e-4f) {
                toPlayer = toPlayer / toPlayerLen;
                const float towardPlayer = Dot(planar, toPlayer);
                if (towardPlayer > 0.0f) {
                    planar = planar - toPlayer * towardPlayer;
                }
            }
        }

        return planar;
    }

    // Keep the wound following pure lateral controller sweeps only. Pull-away and vertical
    // controller motion must not drag the victim (those release the axe instead).
    bool DragVictimWithController(EmbedState& e, RE::NiAVObject* bone, const RE::NiPoint3& realHand,
                                  const RE::NiPoint3& axis, const RE::NiTransform& boneWorld)
    {
        if (!bone || !e.victimActor) {
            return false;
        }

        auto* actor = e.victimActor.get();
        if (!ShouldApplyEmbedMovementLock(actor)) {
            return false;
        }

        const RE::NiMatrix3 lockedRot = boneWorld.rotate * e.lockedRotLocalToBone;
        const RE::NiPoint3 gripOffsetWorld = lockedRot * e.gripInHand;
        const RE::NiPoint3 desiredWound = realHand + gripOffsetWorld - axis * e.depth;
        const RE::NiPoint3 currentWound = boneWorld * e.entryLocalToBone;
        RE::NiPoint3 delta = ConstrainVictimDragDelta(actor, desiredWound - currentWound, currentWound, realHand);

        const float deltaLen = delta.Length();
        if (deltaLen < 0.05f) {
            return false;
        }
        const float maxDragStep = static_cast<float>(BitingAxesVR::victimMaxDragStep);
        if (deltaLen > maxDragStep) {
            delta = delta * (maxDragStep / deltaLen);
        }

        const float groundZ = e.haveVictimAnchor ? e.victimAnchorPos.z : actor->GetPosition().z;
        RE::NiPoint3 newPos = actor->GetPosition() + delta;
        newPos.z = groundZ;
        actor->SetPosition(newPos, true);
        e.victimAnchorPos = { newPos.x, newPos.y, groundZ };
        e.haveVictimAnchor = true;
        UpdateActorScene(actor);
        ClearVictimLocomotionState(actor);
        FreezeVictimRigidBodies(actor);
        return true;
    }

    bool VictimHasLodgedAxe(RE::Actor* victim)
    {
        if (!victim) {
            return false;
        }
        for (const auto& lodged : g_lodgedAxes) {
            if (lodged.victimActor && lodged.victimActor.get() == victim) {
                return true;
            }
        }
        return false;
    }

    BitingAxesAPI::EmbedBodyRegion ToApiBodyRegion(EmbedBodyRegion region)
    {
        return static_cast<BitingAxesAPI::EmbedBodyRegion>(static_cast<std::uint8_t>(region));
    }

    void FillEmbedApiSnapshot(const EmbedState& e, BitingAxesAPI::EmbedSnapshot& out)
    {
        out = {};
        out.active = e.active;
        out.isLeft = e.isLeft;
        out.useWorldAxe = e.useWorldAxe;
        out.npcEmbedEligible = e.npcEmbedEligible;
        out.pendingWorldAxeGrab = e.pendingWorldAxeGrab;
        if (e.victimActor) {
            out.victimHandle = e.victimActor->CreateRefHandle();
        }
        if (const auto* weap = GetEquippedWeapon(e.isLeft)) {
            out.equippedWeaponFormId = weap->GetFormID();
        }
        if (e.worldAxeRefr) {
            out.worldAxeRefFormId = e.worldAxeRefr->GetFormID();
        }
        out.bodyRegion = ToApiBodyRegion(e.embedBodyRegion);
        out.embedBoneName = e.embedBoneName;
    }

    void FillLodgedApiSnapshot(const LodgedAxeInBody& lodged, bool isLeft, BitingAxesAPI::EmbedSnapshot& out)
    {
        out = {};
        out.active = false;
        out.isLeft = isLeft;
        out.useWorldAxe = false;
        if (lodged.victimActor) {
            out.victimHandle = lodged.victimActor->CreateRefHandle();
        }
        if (const auto* weap = GetEquippedWeapon(isLeft)) {
            out.equippedWeaponFormId = weap->GetFormID();
        }
        if (lodged.axeRefr) {
            out.worldAxeRefFormId = lodged.axeRefr->GetFormID();
        }
        out.bodyRegion = ToApiBodyRegion(lodged.embedBodyRegion);
        out.embedBoneName = lodged.embedBoneName;
    }

    void ImmobilizeHeldLodgedVictim(LodgedNpcHoldState& hold, RE::Actor* actor)
    {
        if (!actor) {
            return;
        }

        auto& rt = actor->GetActorRuntimeData();
        rt.boolBits.set(RE::Actor::BOOL_BITS::kParalyzed);
        rt.boolFlags.set(RE::Actor::BOOL_FLAGS::kMovementBlocked);
        actor->StopMoving(0.0f);
        ClearVictimLocomotionState(actor);
        StopVictimLocomotionAnimation(actor);

        // No PLANCK ignore here either: it kills per-bone hit resolution, so the free hand
        // could not bite the held enemy (hits landed on the skeleton root instead).

        if (auto* mid = actor->GetMiddleHighProcess()) {
            mid->currentMovementSpeed = 0.0f;
            if (auto* cc = mid->charController.get()) {
                static const RE::hkVector4 kZero{ 0.0f, 0.0f, 0.0f, 0.0f };
                cc->SetLinearVelocityImpl(kZero);
            }
        }

        if (auto* high = actor->GetHighProcess()) {
            high->pathingCurrentMovementSpeed = {};
            high->pathingDesiredMovementSpeed = {};
            high->pathingCurrentRotationSpeed = {};
            high->pathingDesiredRotationSpeed = {};
        }

        if (!hold.haveAnchor) {
            hold.anchorPos = actor->GetPosition();
            hold.haveAnchor = true;
        }

        FreezeVictimRigidBodies(actor);
        hold.immobilized = true;
    }

    void ReleaseHeldLodgedVictim(LodgedNpcHoldState& hold, RE::Actor* actor)
    {
        if (!actor || !hold.immobilized) {
            return;
        }

        auto& rt = actor->GetActorRuntimeData();
        rt.boolBits.reset(RE::Actor::BOOL_BITS::kParalyzed);
        rt.boolFlags.reset(RE::Actor::BOOL_FLAGS::kMovementBlocked);

        if (hold.planckIgnored && PlanckAPI::g_planck) {
            PlanckAPI::g_planck->RemoveIgnoredActor(actor);
            hold.planckIgnored = false;
        }
        if (hold.ragdollCollisionIgnored && PlanckAPI::g_planck) {
            PlanckAPI::g_planck->RemoveRagdollCollisionIgnoredActor(actor);
            hold.ragdollCollisionIgnored = false;
        }

        hold.immobilized = false;
        hold.haveAnchor = false;
    }

    void MaintainHeldLodgedVictim(LodgedNpcHoldState& hold, RE::Actor* actor)
    {
        if (!actor) {
            return;
        }

        ImmobilizeHeldLodgedVictim(hold, actor);

        if (hold.haveAnchor) {
            RE::NiPoint3 drift = actor->GetPosition() - hold.anchorPos;
            if (drift.Length() > 0.25f) {
                actor->SetPosition(hold.anchorPos, true);
                UpdateActorScene(actor);
                ClearVictimLocomotionState(actor);
                FreezeVictimRigidBodies(actor);
            }
        }
    }

    void BeginLodgedNpcHold(bool isLeft, RE::Actor* victim)
    {
        if (!ShouldApplyLodgedNpcHold(victim)) {
            return;
        }

        const int slot = isLeft ? 1 : 0;
        g_lodgedNpcHold.handActive[slot] = true;

        if (!g_lodgedNpcHold.victimHandle) {
            g_lodgedNpcHold.victimHandle = victim->CreateRefHandle();
            g_lodgedNpcHold.haveLastStaminaTick = false;
            g_lodgedNpcHold.staminaExhausted = false;
            ImmobilizeHeldLodgedVictim(g_lodgedNpcHold, victim);
            IW_LOG_INFO("[bite] holding '{}' with lodged axe — movement locked (in combat)",
                        victim->GetDisplayFullName());
        }
    }

    void EndLodgedNpcHold(bool isLeft)
    {
        const int slot = isLeft ? 1 : 0;
        if (!g_lodgedNpcHold.handActive[slot]) {
            return;
        }

        g_lodgedNpcHold.handActive[slot] = false;
        if (g_lodgedNpcHold.handActive[0] || g_lodgedNpcHold.handActive[1]) {
            return;
        }

        if (auto* victim = g_lodgedNpcHold.victimHandle.get().get()) {
            ReleaseHeldLodgedVictim(g_lodgedNpcHold, victim);
            IW_LOG_INFO("[bite] released hold on '{}'", victim->GetDisplayFullName());
        }
        g_lodgedNpcHold = {};
    }

    void UpdateLodgedNpcHold()
    {
        if (g_lodgedAxes.empty() || !g_higgsInterface) {
            if (g_lodgedNpcHold.immobilized) {
                if (auto* victim = g_lodgedNpcHold.victimHandle.get().get()) {
                    ReleaseHeldLodgedVictim(g_lodgedNpcHold, victim);
                }
                g_lodgedNpcHold = {};
            }
            return;
        }

        for (int slot = 0; slot < 2; ++slot) {
            if (!g_lodgedNpcHold.handActive[slot]) {
                continue;
            }

            const bool isLeft = slot == 1;
            if (!g_higgsInterface->IsHoldingObject(isLeft)) {
                EndLodgedNpcHold(isLeft);
                continue;
            }

            auto* held = g_higgsInterface->GetGrabbedObject(isLeft);
            auto* victim = g_lodgedNpcHold.victimHandle.get().get();
            if (!held || !victim || held->As<RE::Actor>() != victim || !VictimHasLodgedAxe(victim)) {
                EndLodgedNpcHold(isLeft);
            }
        }

        if (!g_lodgedNpcHold.handActive[0] && !g_lodgedNpcHold.handActive[1]) {
            return;
        }

        auto* victim = g_lodgedNpcHold.victimHandle.get().get();
        if (!victim) {
            g_lodgedNpcHold = {};
            return;
        }

        if (!IsLivingVictim(victim)) {
            if (g_lodgedNpcHold.immobilized) {
                ReleaseHeldLodgedVictim(g_lodgedNpcHold, victim);
            }
            return;
        }

        if (!victim->IsInCombat()) {
            if (g_lodgedNpcHold.immobilized) {
                ReleaseHeldLodgedVictim(g_lodgedNpcHold, victim);
            }
            return;
        }

        if (BitingAxesVR::lodgedHoldStaminaDrainEnabled != 0 &&
            (!CanAffordLodgedNpcHold() || g_lodgedNpcHold.staminaExhausted)) {
            if (g_lodgedNpcHold.immobilized) {
                ReleaseHeldLodgedVictim(g_lodgedNpcHold, victim);
            }
            g_lodgedNpcHold.staminaExhausted = true;
            return;
        }

        MaintainHeldLodgedVictim(g_lodgedNpcHold, victim);

        if (BitingAxesVR::lodgedHoldStaminaDrainEnabled == 0 ||
            BitingAxesVR::lodgedHoldStaminaDrainPerSec <= 0.0) {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        float dt = 0.016f;
        if (g_lodgedNpcHold.haveLastStaminaTick) {
            dt = std::chrono::duration<float>(now - g_lodgedNpcHold.lastStaminaTick).count();
            if (dt < 0.0f) {
                dt = 0.0f;
            } else if (dt > 0.25f) {
                dt = 0.25f;
            }
        }
        g_lodgedNpcHold.lastStaminaTick = now;
        g_lodgedNpcHold.haveLastStaminaTick = true;

        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
            if (auto* av = player->AsActorValueOwner()) {
                av->DamageActorValue(RE::ActorValue::kStamina,
                                     static_cast<float>(BitingAxesVR::lodgedHoldStaminaDrainPerSec) * dt);
                if (av->GetActorValue(RE::ActorValue::kStamina) <
                    static_cast<float>(BitingAxesVR::lodgedHoldStaminaMin)) {
                    ReleaseHeldLodgedVictim(g_lodgedNpcHold, victim);
                    g_lodgedNpcHold.staminaExhausted = true;
                }
            }
        }
    }

    void ImmobilizeVictimMovement([[maybe_unused]] EmbedState& e, RE::Actor* actor)
    {
        if (!actor) {
            return;
        }

        auto& rt = actor->GetActorRuntimeData();
        rt.boolBits.set(RE::Actor::BOOL_BITS::kParalyzed);
        rt.boolFlags.set(RE::Actor::BOOL_FLAGS::kMovementBlocked);
        actor->StopMoving(0.0f);
        ClearVictimLocomotionState(actor);

        // Deliberately NOT PLANCK-ignoring the victim here: ignored actors lose per-bone
        // ragdoll hit resolution (hits report the skeleton root), which blocked the other
        // hand from embedding the same enemy and made follow-up bites flaky after release.

        if (auto* mid = actor->GetMiddleHighProcess()) {
            mid->currentMovementSpeed = 0.0f;

            if (auto* cc = mid->charController.get()) {
                static const RE::hkVector4 kZero{ 0.0f, 0.0f, 0.0f, 0.0f };
                cc->SetLinearVelocityImpl(kZero);
            }
        }

        if (auto* high = actor->GetHighProcess()) {
            high->pathingCurrentMovementSpeed = {};
            high->pathingDesiredMovementSpeed = {};
            high->pathingCurrentRotationSpeed = {};
            high->pathingDesiredRotationSpeed = {};
        }

        FreezeVictimRigidBodies(actor);
    }

    void EnforceVictimAnchor(EmbedState& e, RE::Actor* actor)
    {
        if (!actor || !e.haveVictimAnchor || !ShouldApplyEmbedMovementLock(actor)) {
            return;
        }

        RE::NiPoint3 drift = actor->GetPosition() - e.victimAnchorPos;
        drift.z = 0.0f;
        if (drift.Length() < 0.25f && std::fabs(actor->GetPosition().z - e.victimAnchorPos.z) < 0.25f) {
            return;
        }

        actor->SetPosition(e.victimAnchorPos, true);
        UpdateActorScene(actor);
        ClearVictimLocomotionState(actor);
        FreezeVictimRigidBodies(actor);
    }

    void RestoreVictimImmobilization(EmbedState& e, RE::Actor* actor)
    {
        if (!actor) {
            return;
        }

        auto& rt = actor->GetActorRuntimeData();
        rt.boolBits.reset(RE::Actor::BOOL_BITS::kParalyzed);
        rt.boolFlags.reset(RE::Actor::BOOL_FLAGS::kMovementBlocked);

        if (e.victimPlanckIgnored && PlanckAPI::g_planck) {
            PlanckAPI::g_planck->RemoveIgnoredActor(actor);
            e.victimPlanckIgnored = false;
        }

        if (e.victimRagdollCollisionIgnored && PlanckAPI::g_planck) {
            PlanckAPI::g_planck->RemoveRagdollCollisionIgnoredActor(actor);
            e.victimRagdollCollisionIgnored = false;
        }

        if (e.victimAggressionIgnored && PlanckAPI::g_planck) {
            PlanckAPI::g_planck->RemoveAggressionIgnoredActor(actor);
            e.victimAggressionIgnored = false;
        }
    }

    float MeasureVictimMoveCap(RE::Actor* actor)
    {
        float cap = 200.0f;
        if (auto* mid = actor->GetMiddleHighProcess()) {
            if (mid->currentMovementSpeed > 10.0f) {
                cap = mid->currentMovementSpeed;
            }
        }

        RE::NiPoint3 vel{};
        actor->GetLinearVelocity(vel);
        const float linSpeed = vel.Length();
        if (linSpeed > cap) {
            cap = linSpeed;
        }
        return cap;
    }

    void ApplyVictimCombatFlags(RE::Actor* actor, bool blocked)
    {
        if (!actor) {
            return;
        }

        auto& rt = actor->GetActorRuntimeData();
        if (blocked) {
            rt.boolFlags.set(RE::Actor::BOOL_FLAGS::kAttackingDisabled);
            rt.boolFlags.set(RE::Actor::BOOL_FLAGS::kCastingDisabled);
        } else {
            rt.boolFlags.reset(RE::Actor::BOOL_FLAGS::kAttackingDisabled);
            rt.boolFlags.reset(RE::Actor::BOOL_FLAGS::kCastingDisabled);
        }

        if (auto* mid = actor->GetMiddleHighProcess()) {
            mid->preventCombat = blocked;
        }
    }

    void RestoreVictimAttackRestriction(RE::Actor* actor)
    {
        ApplyVictimCombatFlags(actor, false);
    }

    void RestoreVictimImmobilizationRelease(RE::Actor* actor, bool wasImmobilized, bool removePlanckIgnore,
                                            bool removeRagdollCollisionIgnore, bool removeAggressionIgnore)
    {
        if (!actor) {
            return;
        }

        if (wasImmobilized) {
            auto& rt = actor->GetActorRuntimeData();
            rt.boolBits.reset(RE::Actor::BOOL_BITS::kParalyzed);
            rt.boolFlags.reset(RE::Actor::BOOL_FLAGS::kMovementBlocked);
        }

        if (!PlanckAPI::g_planck) {
            return;
        }

        if (removePlanckIgnore) {
            PlanckAPI::g_planck->RemoveIgnoredActor(actor);
        }
        if (removeRagdollCollisionIgnore) {
            PlanckAPI::g_planck->RemoveRagdollCollisionIgnoredActor(actor);
        }
        if (removeAggressionIgnore) {
            PlanckAPI::g_planck->RemoveAggressionIgnoredActor(actor);
        }
    }

    void RestoreVictimCombatRestriction(EmbedState& e, RE::Actor* actor, bool wasImmobilized)
    {
        if (wasImmobilized) {
            RestoreVictimImmobilization(e, actor);
        }
    }

    void ApplyPlanckAggressionIgnore(EmbedState& e, RE::Actor* actor)
    {
        if (!actor || e.victimAggressionIgnored || !PlanckAPI::g_planck) {
            return;
        }

        PlanckAPI::g_planck->AddAggressionIgnoredActor(actor);
        e.victimAggressionIgnored = true;
    }

    void SnapshotVictimWeaponDrawn(EmbedState& e, RE::Actor* actor)
    {
        if (auto* state = actor ? actor->AsActorState() : nullptr) {
            e.victimWeaponWasDrawn = state->IsWeaponDrawn();
        }
    }

    void MaintainVictimWeaponDrawn(EmbedState& e, RE::Actor* actor)
    {
        if (!e.victimWeaponWasDrawn || !actor) {
            return;
        }

        if (auto* state = actor->AsActorState()) {
            const auto weaponState = state->GetWeaponState();
            if (weaponState == RE::WEAPON_STATE::kSheathed ||
                weaponState == RE::WEAPON_STATE::kWantToDraw ||
                weaponState == RE::WEAPON_STATE::kSheathing ||
                weaponState == RE::WEAPON_STATE::kWantToSheathe) {
                state->actorState2.weaponState = RE::WEAPON_STATE::kDrawn;
            }
        }
    }

    // True while the actor is in any attack animation state (melee swing, power attack,
    // bow draw, etc.). Used to bail out of embeds before the swing yanks the bone.
    bool IsVictimAttackAnimating(RE::Actor* actor)
    {
        if (!actor) {
            return false;
        }
        if (actor->IsAttacking()) {
            return true;
        }
        if (auto* state = actor->AsActorState()) {
            return state->GetAttackState() != RE::ATTACK_STATE_ENUM::kNone;
        }
        return false;
    }

    void ClearVictimActiveAttackState(RE::Actor* actor)
    {
        if (!actor) {
            return;
        }

        auto* state = actor->AsActorState();
        if (!state) {
            return;
        }

        const auto attackState = state->GetAttackState();
        if (attackState == RE::ATTACK_STATE_ENUM::kNone && !actor->IsAttacking()) {
            state->actorState2.recoil = 0;
            state->actorState2.staggered = 0;
            return;
        }

        state->actorState1.meleeAttackState = RE::ATTACK_STATE_ENUM::kNone;
        state->actorState2.recoil = 0;
        state->actorState2.staggered = 0;

        actor->NotifyAnimationGraph(RE::BSFixedString("attackStop"));
        actor->NotifyAnimationGraph(RE::BSFixedString("AttackStop"));

        if (attackState >= RE::ATTACK_STATE_ENUM::kBowDraw) {
            actor->NotifyAnimationGraph(RE::BSFixedString("bowAttackStop"));
        }
    }

    void InterruptVictimCombatOnce(RE::Actor* actor)
    {
        if (!actor) {
            return;
        }

        // Only interrupt casting. StopCombat/StopCombatAndAlarmOnActor knocked the victim
        // out of combat every embed; PLANCK then deactivated their active ragdoll and hits
        // resolved to the skeleton root (no bone) for several seconds after release.
        actor->InterruptCast(true);
    }

    void StopVictimAttacks(EmbedState& e, RE::Actor* actor)
    {
        if (!actor) {
            return;
        }

        SnapshotVictimWeaponDrawn(e, actor);
        ClearVictimActiveAttackState(actor);
        InterruptVictimCombatOnce(actor);
        ApplyVictimCombatFlags(actor, true);
        ApplyPlanckAggressionIgnore(e, actor);
        MaintainVictimWeaponDrawn(e, actor);
    }

    void MaintainVictimAttacks(EmbedState& e, RE::Actor* actor)
    {
        if (!actor) {
            return;
        }

        ApplyVictimCombatFlags(actor, true);
        ApplyPlanckAggressionIgnore(e, actor);

        if (auto* state = actor->AsActorState()) {
            if (state->GetAttackState() != RE::ATTACK_STATE_ENUM::kNone || actor->IsAttacking()) {
                ClearVictimActiveAttackState(actor);
            }
            state->actorState2.recoil = 0;
            state->actorState2.staggered = 0;
        }

        MaintainVictimWeaponDrawn(e, actor);
    }

    void StopVictimBlocking(RE::Actor* actor)
    {
        if (!actor) {
            return;
        }

        if (auto* state = actor->AsActorState()) {
            state->actorState2.wantBlocking = 0;
        }

        if (actor->IsBlocking()) {
            actor->NotifyAnimationGraph(RE::BSFixedString("blockStop"));
        }
    }

    void MaintainVictimBlocking(RE::Actor* actor)
    {
        if (!actor) {
            return;
        }

        if (auto* state = actor->AsActorState()) {
            state->actorState2.wantBlocking = 0;
        }

        if (actor->IsBlocking()) {
            actor->NotifyAnimationGraph(RE::BSFixedString("blockStop"));
        }
    }

    // Full synchronous cleanup when embed ends. PLANCK aggression ignore in particular must
    // be cleared or victims can stay in combat and follow without ever attacking again.
    void RestoreVictimAfterEmbed(RE::Actor* actor)
    {
        if (!actor) {
            return;
        }

        ApplyVictimCombatFlags(actor, false);
        StopVictimBlocking(actor);
        ClearVictimActiveAttackState(actor);

        auto& rt = actor->GetActorRuntimeData();
        rt.boolBits.reset(RE::Actor::BOOL_BITS::kParalyzed);
        rt.boolFlags.reset(RE::Actor::BOOL_FLAGS::kMovementBlocked);

        if (PlanckAPI::g_planck) {
            PlanckAPI::g_planck->RemoveIgnoredActor(actor);
            PlanckAPI::g_planck->RemoveRagdollCollisionIgnoredActor(actor);
            PlanckAPI::g_planck->RemoveAggressionIgnoredActor(actor);
        }
    }

    void MaintainVictimCombatShutdown(EmbedState& e, RE::Actor* actor)
    {
        if (!actor || !ShouldApplyEmbedMovementLock(actor)) {
            return;
        }

        MaintainVictimAttacks(e, actor);
        MaintainVictimBlocking(actor);
    }

    void MaintainVictimStillness(EmbedState& e, RE::Actor* actor, bool draggedThisFrame)
    {
        if (!actor || !ShouldApplyEmbedMovementLock(actor)) {
            return;
        }

        ImmobilizeVictimMovement(e, actor);
        e.victimImmobilized = true;
        StopVictimLocomotionAnimation(actor);
        actor->StopMoving(0.0f);
        MaintainVictimCombatShutdown(e, actor);

        if (!draggedThisFrame) {
            EnforceVictimAnchor(e, actor);
        }
    }

    void SlowVictimMovementByFactor(RE::Actor* actor, float moveCap, float speedFrac)
    {
        if (!actor || moveCap <= 0.0f || speedFrac <= 0.0f) {
            return;
        }

        const float maxSpeed = moveCap * speedFrac;

        if (auto* mid = actor->GetMiddleHighProcess()) {
            mid->currentMovementSpeed = maxSpeed;

            if (auto* cc = mid->charController.get()) {
                RE::hkVector4 vel{};
                cc->GetLinearVelocityImpl(vel);
                const float maxHavok = maxSpeed * kSkyrimToHavok;
                if (HkLinearSpeed(vel) > maxHavok) {
                    ClampHkLinearSpeed(vel, maxHavok);
                    cc->SetLinearVelocityImpl(vel);
                }
            }
        }

        if (auto* high = actor->GetHighProcess()) {
            ClampNiPoint3Speed(high->pathingCurrentMovementSpeed, maxSpeed);
            ClampNiPoint3Speed(high->pathingDesiredMovementSpeed, maxSpeed);
        }

        if (auto* root = actor->Get3D()) {
            ForEachRigidBody(root, [&](RE::bhkRigidBody* rb) {
                ClampRigidBodyLinearSpeed(rb, maxSpeed);
            });
        }
    }

    void SlowVictimMovement(RE::Actor* actor, float moveCap)
    {
        SlowVictimMovementByFactor(actor, moveCap, static_cast<float>(BitingAxesVR::biteVictimSpeedFrac));
    }

    void ReleaseWorldAxeEmbedVictimRestrictions(EmbedState& e)
    {
        if (!e.victimActor) {
            return;
        }

        auto* actor = e.victimActor.get();
        const bool hadRestrictions = e.victimAttackRestricted || e.victimCombatRestricted || e.victimImmobilized ||
                                       e.victimPlanckIgnored || e.victimRagdollCollisionIgnored ||
                                       e.victimAggressionIgnored;
        if (hadRestrictions) {
            RestoreVictimAfterEmbed(actor);
            IW_LOG_INFO("[bite] victim '{}' can fight while world axe is lodged",
                        actor->GetDisplayFullName());
        }

        e.victimAttackRestricted = false;
        e.victimCombatRestricted = false;
        e.victimImmobilized = false;
        e.victimPlanckIgnored = false;
        e.victimRagdollCollisionIgnored = false;
        e.victimAggressionIgnored = false;
        e.haveVictimAnchor = false;
    }

    void DrainEmbeddedVictimResources(RE::Actor* victim, float dt, EmbedBodyRegion region)
    {
        if (!victim || !IsLivingVictim(victim) || dt <= 0.0f) {
            return;
        }

        auto* av = victim->AsActorValueOwner();
        if (!av) {
            return;
        }

        const float curHealth = av->GetActorValue(RE::ActorValue::kHealth);
        if (curHealth <= 0.0f) {
            return;
        }

        const float mult = EmbedBodyRegionHealthMultiplier(region);
        const float healthDrain = static_cast<float>(BitingAxesVR::lodgedHealthDrainPerSec) * mult * dt;
        if (healthDrain > 0.0f) {
            av->DamageActorValue(RE::ActorValue::kHealth, healthDrain);
        }

        const float magickaDrain = static_cast<float>(BitingAxesVR::lodgedMagickaDrainPerSec) * mult * dt;
        if (magickaDrain > 0.0f && EmbedBodyRegionDrainsMagicka(region)) {
            if (av->GetActorValue(RE::ActorValue::kMagicka) > 0.0f) {
                av->DamageActorValue(RE::ActorValue::kMagicka, magickaDrain);
            }
        }

        const float staminaDrain = static_cast<float>(BitingAxesVR::lodgedStaminaDrainPerSec) * mult * dt;
        if (staminaDrain > 0.0f && EmbedBodyRegionDrainsStamina(region)) {
            if (av->GetActorValue(RE::ActorValue::kStamina) > 0.0f) {
                av->DamageActorValue(RE::ActorValue::kStamina, staminaDrain);
            }
        }
    }

    void UpdateLodgedAxeVictimEffects()
    {
        UpdatePostEmbedBleeding();

        const auto tickBlood = [](std::chrono::steady_clock::time_point& lastTime, bool& haveLastTime,
                                  float intervalSec, auto&& playBlood) {
            const auto now = std::chrono::steady_clock::now();
            if (!haveLastTime) {
                lastTime = now;
                haveLastTime = true;
                return;
            }
            const float elapsed = std::chrono::duration<float>(now - lastTime).count();
            if (elapsed < intervalSec) {
                return;
            }
            playBlood();
            lastTime = now;
        };

        const auto tickResourceDrain = [](std::chrono::steady_clock::time_point& lastTime, bool& haveLastTime,
                                          RE::Actor* victim, EmbedBodyRegion region) {
            const auto now = std::chrono::steady_clock::now();
            if (!haveLastTime) {
                lastTime = now;
                haveLastTime = true;
                return;
            }
            float dt = std::chrono::duration<float>(now - lastTime).count();
            if (dt <= 0.0f) {
                return;
            }
            if (dt > 0.25f) {
                dt = 0.25f;
            }
            DrainEmbeddedVictimResources(victim, dt, region);
            lastTime = now;
        };

        const float lodgedBloodInterval = static_cast<float>(BitingAxesVR::lodgedBloodIntervalSec);
        const float embedBloodInterval = static_cast<float>(BitingAxesVR::embedBloodIntervalSec);

        for (auto& lodged : g_lodgedAxes) {
            if (!lodged.victimActor || !lodged.boneNode) {
                continue;
            }
            auto* victim = lodged.victimActor.get();
            if (!IsLivingVictim(victim)) {
                continue;
            }

            tickBlood(lodged.lastBloodFxTime, lodged.haveLastBloodFxTime, lodgedBloodInterval,
                      [&]() { PlayDislodgeBloodEffectForLodged(lodged); });
            tickResourceDrain(lodged.lastHealthDrainTime, lodged.haveLastHealthDrainTime, victim,
                              lodged.embedBodyRegion);
        }

        for (auto& e : g_embeds) {
            if (!e.active || !e.victimActor || !e.boneNode) {
                continue;
            }
            auto* victim = e.victimActor.get();
            if (!IsLivingVictim(victim)) {
                continue;
            }

            const float bloodInterval = e.useWorldAxe ? lodgedBloodInterval : embedBloodInterval;
            tickBlood(e.lastWorldAxeBloodTime, e.haveWorldAxeBloodTime, bloodInterval, [&]() {
                if (e.useWorldAxe) {
                    PlayWorldAxeEmbedBloodEffect(e);
                } else {
                    PlayDislodgeBloodEffect(e);
                }
            });
            tickResourceDrain(e.lastWorldAxeHealthDrainTime, e.haveWorldAxeHealthDrainTime, victim,
                              e.embedBodyRegion);
        }
    }

    void ApplyVictimMovementLock(EmbedState& e, RE::Actor* actor)
    {
        ImmobilizeVictimMovement(e, actor);
        e.victimImmobilized = true;
        e.victimAnchorPos = actor->GetPosition();
        e.haveVictimAnchor = true;
        IW_LOG_INFO("[bite] victim '{}' movement locked", actor->GetDisplayFullName());
    }

    void MaintainVictimMovementLock(EmbedState& e, RE::Actor* actor)
    {
        if (!ShouldApplyEmbedMovementLock(actor)) {
            if (e.victimImmobilized) {
                RestoreVictimImmobilization(e, actor);
                e.victimImmobilized = false;
            }
            return;
        }

        ImmobilizeVictimMovement(e, actor);
        e.victimImmobilized = true;
    }

    void BeginVictimMovementRestriction(EmbedState& e, RE::Actor* actor)
    {
        if (!ShouldApplyEmbedMovementLock(actor)) {
            return;
        }

        e.victimCombatRestricted = true;
        ApplyVictimMovementLock(e, actor);
        IW_LOG_INFO("[bite] victim '{}' movement restricted", actor->GetDisplayFullName());
    }

    void MaintainVictimMovementRestriction(EmbedState& e, RE::Actor* actor)
    {
        if (!ShouldApplyEmbedMovementLock(actor)) {
            if (e.victimImmobilized) {
                RestoreVictimImmobilization(e, actor);
                e.victimImmobilized = false;
            }
            e.victimCombatRestricted = false;
            return;
        }

        if (!e.victimCombatRestricted) {
            e.victimCombatRestricted = true;
        }

        MaintainVictimMovementLock(e, actor);
    }

    void MaintainVictimEmbedRestrictions(EmbedState& e)
    {
        if (!e.victimActor) {
            return;
        }

        auto* actor = e.victimActor.get();
        if (!ShouldApplyEmbedMovementLock(actor)) {
            if (e.victimAttackRestricted || e.victimAggressionIgnored) {
                RestoreVictimAttackRestriction(actor);
                if (e.victimAggressionIgnored && PlanckAPI::g_planck) {
                    PlanckAPI::g_planck->RemoveAggressionIgnoredActor(actor);
                }
                e.victimAttackRestricted = false;
                e.victimAggressionIgnored = false;
            }
            if (e.victimImmobilized || e.victimPlanckIgnored || e.victimRagdollCollisionIgnored) {
                RestoreVictimImmobilization(e, actor);
            }
            e.victimCombatRestricted = false;
            e.victimImmobilized = false;
            e.haveVictimAnchor = false;
            return;
        }

        MaintainVictimCombatShutdown(e, actor);

        if (e.distressFaceActive) {
            MaintainDistressFace(e);
        }

        MaintainVictimMovementRestriction(e, actor);
    }

    void BeginVictimDistress(EmbedState& e, RE::Actor* actor)
    {
        if (!actor) {
            return;
        }

        e.victimActor = RE::NiPointer<RE::Actor>(actor);
        if (!IsDeadOrIncapacitatedVictim(actor)) {
            e.distressFaceActive = ApplyDistressFace(actor);
        }
        if (ShouldApplyEmbedMovementLock(actor)) {
            StopVictimAttacks(e, actor);
            StopVictimBlocking(actor);
            e.victimAttackRestricted = true;
            IW_LOG_INFO("[bite] victim '{}' attack/block disabled while embedded",
                        actor->GetDisplayFullName());
        }
        BeginVictimMovementRestriction(e, actor);
    }

    void EndVictimDistress(EmbedState& e)
    {
        const bool hadFace = e.distressFaceActive;
        const bool hadRestrictions = e.victimAttackRestricted || e.victimCombatRestricted || e.victimImmobilized ||
                                       e.victimPlanckIgnored || e.victimRagdollCollisionIgnored ||
                                       e.victimAggressionIgnored;

        RE::ActorHandle handle;
        if (e.victimActor) {
            RestoreVictimAfterEmbed(e.victimActor.get());
            handle = e.victimActor->CreateRefHandle();
        }

        e.distressFaceActive = false;
        e.victimAttackRestricted = false;
        e.victimCombatRestricted = false;
        e.victimImmobilized = false;
        e.victimPlanckIgnored = false;
        e.victimRagdollCollisionIgnored = false;
        e.victimAggressionIgnored = false;
        e.haveVictimAnchor = false;
        e.victimActor.reset();

        if (!handle || (!hadFace && !hadRestrictions)) {
            return;
        }

        // FaceGen restore stays on the game thread; combat flags were already cleared synchronously.
        SKSE::GetTaskInterface()->AddTask([handle, hadFace, hadRestrictions]() {
            if (auto* actor = handle.get().get()) {
                if (hadFace) {
                    ClearDistressFace(actor);
                }
                if (hadRestrictions) {
                    RestoreVictimAfterEmbed(actor);
                    IW_LOG_INFO("[bite] victim '{}' embed restrictions cleared",
                                actor->GetDisplayFullName());
                }
            }
        });
    }

    void LeaveEmbedWithoutDislodge(EmbedState& e)
    {
        const bool wasActive = e.active;
        EndVictimDistress(e);
        e.active = false;
        e.haveDesired = false;
        e.useWorldAxe = false;
        e.pendingWorldAxeGrab = false;
        e.worldAxeRefr.reset();
        e.boneNode.reset();
        e.weaponNode.reset();
        e.handNode.reset();
        e.fpHandNode.reset();
        if (wasActive) {
            MarkEmbedHandCooldown(e.isLeft);
        }
    }

    bool LodgeActiveWorldAxeFromEmbed(EmbedState& e, bool isLeft, const char* trackReason, bool notifyApi)
    {
        if (!e.active || !e.useWorldAxe || !e.worldAxeRefr || !e.boneNode) {
            return false;
        }

        LodgedAxeInBody lodged{};
        lodged.axeRefr = e.worldAxeRefr;
        lodged.boneNode = e.boneNode;
        lodged.victimActor = e.victimActor;
        lodged.embedBoneName = e.embedBoneName;
        lodged.embedBodyRegion = e.embedBodyRegion;
        lodged.entryLocalToBone = e.entryLocalToBone;
        lodged.axisLocalToBone = e.axisLocalToBone;
        lodged.weaponRotLocalToBone = e.weaponRotLocalToBone;
        lodged.depth = e.depth;
        lodged.weaponWorldScale = e.weaponWorldScale;
        lodged.axeRefr->SetMotionType(RE::hkpMotion::MotionType::kFixed, true);
        if (e.victimActor) {
            InitLodgedAxeVictimEffects(lodged);
        }
        g_lodgedAxes.push_back(lodged);
        SetTrackedWorldAxePhase(lodged.axeRefr->GetFormID(), WorldAxeTrackPhase::Lodged, trackReason);

        LogEmbedBodyLocation("lodged", lodged.embedBodyRegion, lodged.embedBoneName, e.victimActor.get(),
                             e.profile.name, isLeft);
        if (notifyApi) {
            BitingAxesAPI::EmbedSnapshot snapshot{};
            FillLodgedApiSnapshot(lodged, isLeft, snapshot);
            BitingAxesAPI::NotifyWeaponLodgedInBody(snapshot);
        }
        LeaveEmbedWithoutDislodge(e);
        return true;
    }

    void OnHiggsDropped(bool isLeft, RE::TESObjectREFR* droppedRefr)
    {
        ClearLodgedExtractForHand(isLeft);

        if (droppedRefr) {
            if (auto* victim = droppedRefr->As<RE::Actor>()) {
                if (VictimHasLodgedAxe(victim) && IsLivingVictim(victim)) {
                    EndLodgedNpcHold(isLeft);
                }
            }
        }

        if (!droppedRefr) {
            return;
        }

        EmbedState& e = EmbedFor(isLeft);
        if (!e.active || !e.useWorldAxe || !e.worldAxeRefr || droppedRefr != e.worldAxeRefr.get()) {
            return;
        }

        const char* victimName =
            e.victimActor ? e.victimActor->GetDisplayFullName() : "<unknown>";
        if (LodgeActiveWorldAxeFromEmbed(e, isLeft, "grip_released", true)) {
            IW_LOG_INFO("[bite] player released grip; axe left embedded in '{}'", victimName);
        }
    }

    void StartEmbed(const PlanckAPI::PlanckHitData& hit, RE::Actor* victim)
    {
        EmbedState& e = EmbedFor(hit.isLeft);
        if (e.active) {
            return;  // this hand's axe is already lodged
        }
        if (IsEmbedHandOnCooldown(hit.isLeft)) {
            return;
        }
        const WeaponProfile profile = ClassifyWeapon(GetEquippedWeapon(hit.isLeft));
        if (!profile.allowed) {
            return;
        }

        const FlatLateralSwingSample flatSample =
            AnalyzeFlatLateralSwing(hit.isLeft, hit.velocity, GetPlayerHorizontalRight());
        if (flatSample.valid) {
            LogFlatLateralSwing(hit.isLeft, flatSample, "embed-rejected");
            return;
        }

        // Victim weapon/shield nodes and hands/fingers never embed. Upper arm/forearm only when enabled.
        if (!hit.nodeName.empty()) {
            std::string_view name{ hit.nodeName.c_str() };
            static constexpr const char* kAlwaysExcludedBoneMarkers[] = {
                "Weapon", "WEAPON", "Shield", "SHIELD",
                "Hand", "[LHnd]", "[RHnd]",
                "Finger", "Thumb",
            };
            for (const char* marker : kAlwaysExcludedBoneMarkers) {
                if (name.find(marker) != std::string_view::npos) {
                    return;
                }
            }

            if (BitingAxesVR::embedArmsAndHandsEnabled == 0) {
                static constexpr const char* kArmExcludedBoneMarkers[] = {
                    "UpperArm",  // NPC L/R UpperArm [LUArm]/[RUArm] + twist bones
                    "Forearm",   // NPC L/R Forearm [LLArm]/[RLArm] + twist bones
                    "[LUArm]", "[RUArm]", "[LLArm]", "[RLArm]",
                };
                for (const char* marker : kArmExcludedBoneMarkers) {
                    if (name.find(marker) != std::string_view::npos) {
                        return;
                    }
                }
            }
        }
        auto* weapon = GetPlayerWeaponNode(hit.isLeft);
        auto* handNode = GetHandNode(hit.isLeft);
        if (!weapon || !handNode || !hit.node || !GetRigidBody(hit.node.get())) {
            return;  // no weapon node / no hand / no ragdoll body -> nothing to embed
        }
        // First-person hand: engine-driven from the controller every frame. Feeding it our
        // lodged pose pre-VRIK makes VRIK bend the whole arm toward the axe naturally
        // (same mechanism HIGGS two-handing uses to keep arms connected).
        RE::NiAVObject* fpHand = nullptr;
        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
            if (auto* fpRoot = player->Get3D(true)) {
                fpHand = fpRoot->GetObjectByName(hit.isLeft ? "NPC L Hand [LHnd]" : "NPC R Hand [RHnd]");
            }
        }
        RE::NiPoint3 realHand;
        if (!GetRealHandPos(hit.isLeft, realHand)) {
            return;  // need the true controller pose for the hand-attach model
        }

        if (!victim) {
            victim = GetActorFromNode(hit.node.get());
        }
        if (victim && victim != RE::PlayerCharacter::GetSingleton()) {
            RE::TESObjectARMO* struckArmor = nullptr;
            if (ShouldDeflectFromMetalArmor(victim, hit.nodeName, &struckArmor)) {
                DeflectFromMetalArmor(hit, struckArmor, weapon);
                return;
            }

            if (IsHeadBoneName(hit.nodeName.c_str()) && profile.allowsHeadshotKill) {
                float healthFrac = 0.0f;
                if (!IsHeadshotAllowed(victim, &healthFrac)) {
                    IW_LOG_INFO("[bite] headshot blocked  victim='{}'  inCombat=Y  health={:.0f}%  (need <= {:.0f}%)",
                                victim->GetDisplayFullName(), healthFrac * 100.0f,
                                static_cast<float>(BitingAxesVR::combatHeadshotHealthFrac) * 100.0f);
                    return;
                }
            }
        }

        const RE::NiTransform boneWorld = hit.node->world;
        const RE::NiPoint3 gripPos = weapon->world.translate;
        RE::NiPoint3 axisWorld = hit.position - gripPos;
        const float axisLen = axisWorld.Length();
        if (axisLen < 1e-2f) {
            return;  // degenerate; can't define a bite direction
        }
        axisWorld = axisWorld / axisLen;

        float biteAxisLen = axisLen;
        if (profile.hiltExcludeFrac > 0.0f) {
            const float bladeLen = MeasureWeaponExtentAlongAxis(weapon, gripPos, axisWorld);
            if (bladeLen > 1e-2f) {
                const float minBladeReach = profile.hiltExcludeFrac * bladeLen;
                if (axisLen < minBladeReach) {
                    IW_LOG_INFO("[bite] {} embed rejected: hilt/guard region (reach={:.1f}  need>={:.1f}  bladeLen={:.1f})",
                                profile.name, axisLen, minBladeReach, bladeLen);
                    return;
                }
                biteAxisLen = (1.0f - profile.hiltExcludeFrac) * bladeLen;
            }
        }

        if (IsSwordProfile(profile)) {
            biteAxisLen *= static_cast<float>(BitingAxesVR::swordDepthScale);
        }

        e.isLeft = hit.isLeft;
        e.profile = profile;
        e.diagLogged = false;
        e.frames = 0;
        e.haveLastSlipTime = false;
        e.haveLastBonePos = false;
        e.haveLastPlayerStaminaTick = false;
        e.startTime = std::chrono::steady_clock::now();
        e.boneNode = hit.node;
        e.embedBoneName = hit.nodeName;
        e.embedBodyRegion = ClassifyEmbedBodyRegion(hit.nodeName);
        e.weaponNode = RE::NiPointer<RE::NiAVObject>(weapon);
        e.handNode = RE::NiPointer<RE::NiAVObject>(handNode);
        e.fpHandNode = RE::NiPointer<RE::NiAVObject>(fpHand);
        e.haveDesired = false;
        // Constant rotation offset between the two hand skeletons (both track the controller).
        if (fpHand) {
            e.fpToTpRot = handNode->world.rotate * fpHand->world.rotate.Transpose();
        }
        e.entryLocalToBone = boneWorld.Invert() * hit.position;
        e.axisLocalToBone = boneWorld.rotate.Transpose() * axisWorld;
        e.gripInHand = handNode->world.Invert() * gripPos;
        e.haftDirInHand = handNode->world.rotate.Transpose() * axisWorld;

        // Freeze hand/axe orientation at the moment of the bite. While embedded, controller
        // rotation is ignored entirely -- only pull-back translation can release the axe.
        {
            const RE::NiMatrix3 handRot = handNode->world.rotate;
            RE::NiPoint3 haftDirWorld = handRot * e.haftDirInHand;
            const float hdLen = haftDirWorld.Length();
            RE::NiMatrix3 lockedHandRot = handRot;
            if (hdLen > 1e-4f) {
                haftDirWorld = haftDirWorld / hdLen;
                lockedHandRot = RotationBetween(haftDirWorld, axisWorld) * handRot;
            }
            e.lockedRotLocalToBone = boneWorld.rotate.Transpose() * lockedHandRot;
        }

        e.haveLastHandAlong = false;
        // Biteable length along the blade axis (swords exclude the proximal hilt/guard quarter).
        e.haftLen = biteAxisLen;

        // Momentum -> instant initial bite depth (hard chop buries the whole edge).
        const float minBiteSpeed = static_cast<float>(BitingAxesVR::biteMinSpeed);
        const float chopSpeed = hit.velocity.Length();
        const float maxBiteSpeed = static_cast<float>(BitingAxesVR::biteMaxSpeed);
        float t = (chopSpeed - minBiteSpeed) / (maxBiteSpeed - minBiteSpeed);
        if (t < 0.0f) {
            t = 0.0f;
        } else if (t > 1.0f) {
            t = 1.0f;
        }
        const float biteFrac = profile.minInitialFrac + t * (profile.maxInitialFrac - profile.minInitialFrac);
        float depth0 = (biteFrac - 1.0f) * biteAxisLen;
        const float deepest = -(1.0f - profile.maxInsertFrac) * biteAxisLen;
        if (depth0 > deepest) {
            depth0 = deepest;
        }
        e.depth = depth0;
        // Center the stick zone on where the controller actually is right now. Without this
        // the swing follow-through leaves the hand ~half a haft-length "outside" the simulated
        // grip, and that phantom lag eats most of the player's pull before it counts.
        const float handAlong0 = Dot(realHand - hit.position, axisWorld);
        float handOffset = handAlong0 - depth0;
        if (IsSwordProfile(profile)) {
            const float tighten = static_cast<float>(BitingAxesVR::swordEmbedOffsetTighten);
            const float lagScale = 1.0f - tighten;
            handOffset *= lagScale > 0.0f ? lagScale : 0.0f;
        }
        e.handOffset = handOffset;
        e.embedHandAlong = depth0;
        e.pullBaselineReady = false;

        if (!victim) {
            victim = GetActorFromNode(hit.node.get());
        }
        const bool npcEmbed = victim && victim != RE::PlayerCharacter::GetSingleton();

        e.npcEmbedEligible = false;
        e.gripWasDown = false;
        e.pendingWorldAxeTransition = false;
        e.victimDistAtEmbed = npcEmbed ? GetPlayerToActorDistance(victim) : -1.0f;

        if (npcEmbed) {
            if (IsHeadBoneName(hit.nodeName.c_str()) && profile.allowsHeadshotKill) {
                KillVictimFromHeadBite(victim);
            }
            BeginVictimDistress(e, victim);
            if (IsLivingVictim(victim)) {
                InitEmbedVictimEffects(e);
            }
            // Grip/trigger -> world-model axe only on living NPCs when enabled; corpses use standard pull-out.
            if (EmbedWorldModelFeatureEnabled() && IsLivingVictim(victim) &&
                CanLeaveEquippedWeaponInBody(GetEquippedWeapon(hit.isLeft))) {
                e.npcEmbedEligible = true;
                e.gripWasDown = IsWorldAxeInputPressedOnEmbedHand(hit.isLeft);
                IW_LOG_INFO("[bite] NPC embed: press {} on {} hand to leave axe in body",
                            WorldAxeInputButtonName(), hit.isLeft ? "L" : "R");
            }
        }

        e.active = true;

        // Equipped-weapon collision only applies when the axe is still on the player skeleton.
        if (g_higgsInterface && !e.useWorldAxe) {
            g_higgsInterface->DisableWeaponCollision(hit.isLeft);
        }
        Haptic(hit.isLeft, static_cast<float>(BitingAxesVR::hapticBite));
        LogEmbedBodyLocation("start", e.embedBodyRegion, e.embedBoneName, victim, profile.name, hit.isLeft);
        IW_LOG_INFO("[bite] START  hand={}  weapon={}  region={}  bone={}  haftLen={:.0f}  chopSpeed={:.0f}  initialBiteFrac={:.2f}{}",
                    hit.isLeft ? "L" : "R", profile.name, EmbedBodyRegionName(e.embedBodyRegion),
                    hit.nodeName.empty() ? "<unnamed>" : hit.nodeName.c_str(),
                    e.haftLen, chopSpeed, biteFrac, e.useWorldAxe ? "  worldAxe=Y" : "");
        BitingAxesAPI::EmbedSnapshot snapshot{};
        FillEmbedApiSnapshot(e, snapshot);
        BitingAxesAPI::NotifyEmbedStarted(snapshot);
    }

    // Clear one hand's embed state and restore its weapon collision. Silent variant for
    // save-load / new-game resets, where the old scene is gone and a buzz would be wrong.
    void ResetEmbed(EmbedState& e)
    {
        BitingAxesAPI::EmbedSnapshot endSnapshot{};
        const bool wasActive = e.active;
        if (wasActive) {
            FillEmbedApiSnapshot(e, endSnapshot);
        }

        EndVictimDistress(e);
        if (e.active && g_higgsInterface && !e.useWorldAxe) {
            g_higgsInterface->EnableWeaponCollision(e.isLeft);
        }
        e.active = false;
        e.haveDesired = false;
        e.npcEmbedEligible = false;
        e.gripWasDown = false;
        e.pendingWorldAxeTransition = false;
        e.useWorldAxe = false;
        e.pendingWorldAxeGrab = false;
        e.worldAxeRefr.reset();
        e.boneNode.reset();
        e.weaponNode.reset();
        e.handNode.reset();
        e.fpHandNode.reset();

        if (wasActive) {
            endSnapshot.active = false;
            BitingAxesAPI::NotifyEmbedEnded(endSnapshot);
            MarkEmbedHandCooldown(e.isLeft);
        }
    }

    void ReleaseEmbed(EmbedState& e, const char* reason)
    {
        const bool intentionalWorldAxePull =
            e.useWorldAxe && g_higgsInterface && g_higgsInterface->IsHoldingObject(e.isLeft) &&
            std::strcmp(reason, "pulled back") == 0;
        if (e.useWorldAxe && !intentionalWorldAxePull &&
            LodgeActiveWorldAxeFromEmbed(e, e.isLeft, reason, false)) {
            IW_LOG_INFO("[bite] world axe kept lodged on victim instead of orphaning ({})", reason);
            return;
        }

        if (e.useWorldAxe) {
            if (e.worldAxeRefr) {
                UntrackWorldAxe(e.worldAxeRefr->GetFormID(), reason);
            } else {
                for (auto& tracked : g_trackedWorldAxes) {
                    if (tracked.isLeft == e.isLeft && tracked.phase == WorldAxeTrackPhase::ActiveEmbed) {
                        ReportWorldAxeVanishIfNeeded(tracked, reason);
                        break;
                    }
                }
            }
        }

        // Play the dislodge SFX at the wound before we drop node pointers.
        RE::NiPoint3 soundPos{};
        RE::NiAVObject* soundNode = nullptr;
        if (auto bone = e.boneNode) {
            soundPos = bone->world * e.entryLocalToBone;
            if (e.useWorldAxe && e.worldAxeRefr) {
                soundNode = e.worldAxeRefr->Get3D();
            } else {
                soundNode = e.handNode ? e.handNode.get() : e.weaponNode.get();
            }
        }

        Haptic(e.isLeft, static_cast<float>(BitingAxesVR::hapticExtract));
        if (e.boneNode) {
            if (e.useWorldAxe && e.worldAxeRefr) {
                DetachAxeFromBone(e.worldAxeRefr.get());
            }
            PlayDislodgeSound(soundPos, soundNode);
            PlayDislodgeBloodEffect(e);
            if (e.victimActor) {
                BeginPostEmbedBleeding(e);
            }
        }
        ResetEmbed(e);
        IW_LOG_INFO("[bite] END  hand={}  ({})", e.isLeft ? "L" : "R", reason);
    }

    // Bone-frame bite axis for the current frame (unit, into the body).
    bool CurrentAxis(const EmbedState& e, const RE::NiTransform& boneWorld, RE::NiPoint3& outAxis)
    {
        RE::NiPoint3 axis = boneWorld.rotate * e.axisLocalToBone;
        const float len = axis.Length();
        if (len < 1e-4f) {
            return false;
        }
        outAxis = axis / len;
        return true;
    }

    // BITE SIMULATION (pre-VRIK, post-HIGGS): stick-slip depth, haptics, body tug, and the
    // lodged hand pose. Runs BEFORE VRIK and feeds the pose to the FIRST-person hand node so
    // VRIK itself bends the whole arm (elbow/forearm/wrist) toward the axe.
    void UpdateEmbedSim(EmbedState& e)
    {
        auto bone = e.boneNode;
        if (!bone) {
            ReleaseEmbed(e, "lost node");
            return;
        }

        if (e.useWorldAxe) {
            if (!e.worldAxeRefr) {
                for (auto& tracked : g_trackedWorldAxes) {
                    if (tracked.isLeft == e.isLeft && tracked.phase == WorldAxeTrackPhase::ActiveEmbed) {
                        ReportWorldAxeVanishIfNeeded(tracked, "active_embed_null_refr");
                        break;
                    }
                }
                ReleaseEmbed(e, "lost world axe");
                return;
            }

            if (auto* tracked = FindTrackedWorldAxe(e.worldAxeRefr->GetFormID())) {
                const WorldAxeWhereabouts where = QueryWorldAxeWhereabouts(
                    tracked->refId, tracked->baseFormId, e.victimActor.get());
                if (!where.has3D) {
                    ReportWorldAxeVanishIfNeeded(*tracked, "active_embed_missing_3d");
                } else if (e.victimActor &&
                           !IsAxeEmbeddedOnVictim(e.worldAxeRefr.get(), e.victimActor.get())) {
                    ReportWorldAxeVanishIfNeeded(*tracked, "active_embed_detached");
                }
            }
        } else {
            auto weapon = e.weaponNode;
            auto hand = e.handNode;
            if (!weapon || !hand || !hand->parent) {
                ReleaseEmbed(e, "lost node");
                return;
            }
        }

        if (!e.useWorldAxe) {
            // Victim attack animations yank the struck bone around and stretch/warp the
            // player's arm — release the moment one starts. Checked BEFORE the restriction
            // maintenance below, which clears the attack state and would hide it from us.
            // Short grace so embedding an enemy mid-swing (whose attack we cancel at embed
            // start) doesn't instantly self-release.
            if (e.victimActor && IsLivingVictim(e.victimActor.get())) {
                const float sinceEmbed =
                    std::chrono::duration<float>(std::chrono::steady_clock::now() - e.startTime).count();
                if (sinceEmbed > 0.3f && IsVictimAttackAnimating(e.victimActor.get())) {
                    IW_LOG_INFO("[bite] victim '{}' started an attack while embedded — releasing (hand={})",
                                e.victimActor->GetDisplayFullName(), e.isLeft ? "L" : "R");
                    ReleaseEmbed(e, "victim attacking");
                    return;
                }
            }

            MaintainVictimEmbedRestrictions(e);
            if (MaintainEmbedPlayerStaminaDrain(e)) {
                ReleaseEmbed(e, "stamina exhausted");
                return;
            }

            // Release when the victim has moved away from where they were at embed start.
            // Relative, not absolute: normal melee range is already ~75-90 units center-to-center,
            // so an absolute cap released instantly on standing enemies and made biting flaky.
            if (e.victimActor && e.victimDistAtEmbed >= 0.0f) {
                const float maxVictimDrift = static_cast<float>(BitingAxesVR::biteVictimMaxDistance);
                if (maxVictimDrift > 0.0f) {
                    const float victimDist = GetPlayerToActorDistance(e.victimActor.get());
                    if (victimDist >= 0.0f && victimDist - e.victimDistAtEmbed > maxVictimDrift) {
                        IW_LOG_INFO("[bite] victim moved too far ({:.1f} -> {:.1f}, drift {:.1f} > {:.1f}) — releasing embed",
                                    e.victimDistAtEmbed, victimDist, victimDist - e.victimDistAtEmbed,
                                    maxVictimDrift);
                        ReleaseEmbed(e, "victim too far");
                        return;
                    }
                }
            }
        }

        const RE::NiTransform boneWorld = bone->world;
        if (!e.useWorldAxe) {
            UpdateGripWorldAxeTransition(e);
        } else if (UseTriggerForWorldAxeInput() && g_higgsInterface && e.worldAxeRefr &&
                   !g_higgsInterface->IsHoldingObject(e.isLeft)) {
            // False Edge: hold trigger again to HIGGS-grab the spawned world ref if grab was missed.
            e.pendingWorldAxeGrab = IsTriggerPressedOnEmbedHand(e.isLeft);
        }
        TryGrabWorldAxe(e);

        RE::NiPoint3 axis;
        if (!CurrentAxis(e, boneWorld, axis)) {
            ReleaseEmbed(e, "bad axis");
            return;
        }

        // True controller pose (not the hand node, which we are overriding).
        RE::NiPoint3 realHand;
        if (!GetRealHandPos(e.isLeft, realHand)) {
            ReleaseEmbed(e, "no controller");
            return;
        }

        const bool draggedVictim =
            e.useWorldAxe ? false : DragVictimWithController(e, bone.get(), realHand, axis, boneWorld);
        if (e.victimActor && !e.useWorldAxe) {
            MaintainVictimStillness(e, e.victimActor.get(), draggedVictim);
        }

        const RE::NiTransform boneWorldAfter = bone->world;
        const RE::NiPoint3 woundWorld = boneWorldAfter * e.entryLocalToBone;

        const float elapsed =
            std::chrono::duration<float>(std::chrono::steady_clock::now() - e.startTime).count();
        const float releaseDelay = static_cast<float>(BitingAxesVR::biteReleaseDelay);
        const bool canRelease = elapsed >= releaseDelay;

        // World-model embed: only allow pull-to-dislodge while HIGGS is holding the spawned ref.
        // Before grab (or after victim restrictions lift), chop follow-through must not orphan the ref.
        const bool worldAxeHeldByHiggs =
            e.useWorldAxe && g_higgsInterface && g_higgsInterface->IsHoldingObject(e.isLeft);
        const bool applyEmbedReleaseChecks = !e.useWorldAxe || worldAxeHeldByHiggs;
        const float handDist = (realHand - woundWorld).Length();

        if (applyEmbedReleaseChecks) {
            const RE::NiPoint3 boneNow = boneWorldAfter.translate;
            if (!e.useWorldAxe && canRelease && e.haveLastBonePos &&
                (boneNow - e.lastBonePos).Length() > static_cast<float>(BitingAxesVR::biteShakeLoose)) {
                ReleaseEmbed(e, "dislodged");
                return;
            }
            e.lastBonePos = boneNow;
            e.haveLastBonePos = true;

            // During the post-embed delay the axe cannot be pulled free. Once the delay
            // ends, snapshot how far the controller is from the wound and release when the
            // player pulls back far enough in 3D (matches real VR controller motion).
            if (!e.pullBaselineReady && canRelease) {
                e.pullBaselineDist = handDist;
                e.pullBaselineReady = true;
                IW_LOG_INFO("[bite] pull baseline set  hand={}  dist={:.1f}  delay={:.1f}s",
                            e.isLeft ? "L" : "R", handDist, releaseDelay);
            }

            if (canRelease && e.pullBaselineReady) {
                const float pullDistance = static_cast<float>(BitingAxesVR::bitePullDistance);
                const float pullDelta = handDist - e.pullBaselineDist;
                if (pullDelta > pullDistance) {
                    ReleaseEmbed(e, "pulled back");
                    return;
                }
            }

            // Safety release if the controller wanders extremely far from the wound.
            if (canRelease && handDist > static_cast<float>(BitingAxesVR::biteLostDistance)) {
                ReleaseEmbed(e, "lost distance");
                return;
            }
            if (elapsed > static_cast<float>(BitingAxesVR::biteSafetySeconds)) {
                ReleaseEmbed(e, "safety timeout");
                return;
            }
        }

        // Controller position along the bite axis (re-centered to the embed-time pose).
        const float handAlong = Dot(realHand - woundWorld, axis) - e.handOffset;
        const float depth = e.depth;
        e.lastHandAlong = handAlong;
        e.haveLastHandAlong = true;
        const float gripAlong = depth;

        if (e.useWorldAxe) {
            if (!e.diagLogged && e.frames++ >= 3) {
                e.diagLogged = true;
                IW_LOG_INFO("[bite] worldAxe lock  hand={}  depth={:.1f}  handDist={:.1f}",
                            e.isLeft ? "L" : "R", depth, handDist);
            }
            return;
        }

        auto hand = e.handNode;
        if (!hand || !hand->parent) {
            ReleaseEmbed(e, "lost node");
            return;
        }

        // Hand orientation: fully frozen at embed. Controller roll/pitch/yaw cannot twist
        // the lodged axe; orientation only follows the struck bone if it moves.
        const RE::NiMatrix3 lockedRot = boneWorldAfter.rotate * e.lockedRotLocalToBone;

        const RE::NiPoint3 lockedGrip = woundWorld + axis * gripAlong;
        const RE::NiPoint3 gripOffsetWorld = lockedRot * e.gripInHand;

        RE::NiTransform desired;
        desired.rotate = lockedRot;
        desired.translate = lockedGrip - gripOffsetWorld;
        desired.scale = hand->world.scale;
        e.desiredHand = desired;
        e.haveDesired = true;

        // Feed the pose to the FIRST-person hand so VRIK solves the arm toward the axe.
        // Only HAND nodes are ever overridden (both are re-driven from the controller every
        // frame, so nothing persists past release) -- bones further up the arm are NOT fully
        // reset by VRIK and writing them accumulates permanent distortion. The exact
        // third-person snap happens post-VRIK in ApplyEmbedTp.
        if (e.fpHandNode && e.fpHandNode->parent) {
            auto fp = e.fpHandNode;
            RE::NiTransform fpDesired;
            fpDesired.rotate = e.fpToTpRot.Transpose() * desired.rotate;
            fpDesired.translate = desired.translate;
            fpDesired.scale = fp->world.scale;
            fp->local = fp->parent->world.Invert() * fpDesired;
            RE::NiUpdateData ud{};
            fp->Update(ud);
        }

        if (!e.diagLogged && e.frames++ >= 3) {
            e.diagLogged = true;
            IW_LOG_INFO("[bite] lock  hand={}  depth={:.1f}  handDist={:.1f}  pullBaseline={:.1f}  haftLen={:.0f}  fpHand={}",
                        e.isLeft ? "L" : "R", depth, handDist, e.pullBaselineDist,
                        e.haftLen, e.fpHandNode != nullptr);
        }
    }

    void UpdateEmbedSims()
    {
        UpdateFlatSwingDiagnostics();
        AuditTrackedWorldAxes();
        UpdateLodgedNpcHold();
        UpdateLodgedAxeVictimEffects();
        UpdateLodgedAxeExtraction();
        for (auto& e : g_embeds) {
            if (e.active) {
                UpdateEmbedSim(e);
            }
        }
    }

    // POST-VRIK: exact final snap of the third-person hand onto the lodged pose. The
    // pre-VRIK first-person write has already made VRIK bend the arm here, so this is a
    // small correction and the wrist stays visually connected to the forearm.
    void ApplyEmbedTp()
    {
        for (auto& e : g_embeds) {
            if (e.active && e.victimActor && !e.useWorldAxe) {
                MaintainVictimStillness(e, e.victimActor.get(), false);
            }

            if (!e.active || !e.haveDesired) {
                continue;
            }
            if (e.useWorldAxe) {
                continue;
            }
            auto hand = e.handNode;
            if (!hand || !hand->parent) {
                continue;
            }
            RE::NiTransform desired = e.desiredHand;
            desired.scale = hand->world.scale;
            hand->local = hand->parent->world.Invert() * desired;
            RE::NiUpdateData ud{};
            hand->Update(ud);
        }
    }

    void ApplyEmbedPost()
    {
        ApplyWorldAxeBoneSync();
        ApplyEmbedTp();
    }

    // ---- PLANCK melee-hit sink -----------------------------------------------

    class HitSink : public RE::BSTEventSink<RE::TESHitEvent>
    {
    public:
        static HitSink* GetSingleton()
        {
            static HitSink instance;
            return std::addressof(instance);
        }

        RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* a_event,
                                              RE::BSTEventSource<RE::TESHitEvent>*) override
        {
            if (a_event && PlanckAPI::g_planck) {
                const void* current = PlanckAPI::g_planck->GetCurrentHitEvent();
                if (current == static_cast<const void*>(a_event)) {
                    const PlanckAPI::PlanckHitData hit = PlanckAPI::g_planck->GetLastHitData();
                    const char* bone = hit.nodeName.empty() ? "<unnamed>" : hit.nodeName.c_str();

                    const float speed = hit.velocity.Length();
                    IW_LOG_INFO("[hit] hand={}  bone={}  speed={:.0f}", hit.isLeft ? "L" : "R", bone, speed);

                    // Player-only: PLANCK's physical melee hits are the player's weapon by
                    // construction, but guarantee it -- NPCs must never trigger a bite.
                    const bool causedByPlayer =
                        a_event->cause && a_event->cause.get() == RE::PlayerCharacter::GetSingleton();
                    // Axes and swords bite on any committed hit (swing or overhead chop): the edge
                    // wedges in whenever it lands with enough momentum.
                    const WeaponProfile profile = ClassifyWeapon(GetEquippedWeapon(hit.isLeft));
                    const float minSpeed = GetProfileMinBiteSpeed(profile);
                    const bool committed = profile.allowed && speed >= minSpeed;
                    if (committed && causedByPlayer) {
                        RE::Actor* victim = nullptr;
                        if (a_event->target) {
                            victim = a_event->target->As<RE::Actor>();
                        }
                        StartEmbed(hit, victim);  // flat-swing rejection + per-hand gating inside
                    }
                }
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    bool g_hitSinkRegistered = false;
}

namespace BitingAxesVR::Bite
{
    void RegisterHitSink()
    {
        if (g_hitSinkRegistered) {
            return;
        }
        if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
            holder->AddEventSink(HitSink::GetSingleton());
            g_hitSinkRegistered = true;
            IW_LOG_INFO("[bite] registered TESHitEvent sink");
        }
    }

    void AttachHiggsCallbacks()
    {
        if (!g_higgsInterface) {
            IW_LOG_WARN("[bite] HIGGS interface not available; axe bite disabled");
            return;
        }
        g_higgsInterface->AddPreVrikPostHiggsCallback(UpdateEmbedSims);
        g_higgsInterface->AddPostVrikPostHiggsCallback(ApplyEmbedPost);
        g_higgsInterface->AddGrabbedCallback(OnHiggsGrabbed);
        g_higgsInterface->AddDroppedCallback(OnHiggsDropped);
        IW_LOG_INFO("[bite] HIGGS pre/post-VRIK callbacks attached");
    }

    void ResetAllEmbeds()
    {
        ClearDislodgeSoundCache();
        ClearBloodImpactDataCache();
        g_bleedingWounds.clear();
        g_lodgedAxes.clear();
        ClearTrackedWorldAxes("game_load_reset");
        CancelLodgedExtract();
        if (g_lodgedNpcHold.immobilized) {
            if (auto* victim = g_lodgedNpcHold.victimHandle.get().get()) {
                ReleaseHeldLodgedVictim(g_lodgedNpcHold, victim);
            }
        }
        g_lodgedNpcHold = {};
        for (auto& e : g_embeds) {
            if (e.active) {
                ResetEmbed(e);
                IW_LOG_INFO("[bite] reset (game load)");
            }
        }
    }

    void PreloadAssets()
    {
        EnsureDislodgeSound();
        EnsureBloodImpactDataSet();
    }
}

namespace BitingAxesVR::EmbedApi
{
    bool BuildHandEmbedSnapshot(bool isLeft, BitingAxesAPI::EmbedSnapshot& outSnapshot)
    {
        FillEmbedApiSnapshot(EmbedFor(isLeft), outSnapshot);
        return true;
    }

    bool VictimHasLodgedWeapon(RE::Actor* victim)
    {
        return VictimHasLodgedAxe(victim);
    }
}
