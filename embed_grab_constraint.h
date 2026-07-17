#pragma once

#include <cstdint>
#include <cstring>

#include "RE/H/hkpConstraintAtom.h"
#include "RE/H/hkpConstraintData.h"
#include "RE/H/hkpConstraintInstance.h"
#include "RE/H/hkpConstraintMotor.h"
#include "RE/H/hkpSolverResults.h"
#include "RE/H/hkTransform.h"

namespace EmbedGrab
{
    // Not in CommonLib headers; layout matches Havok hkpLinMotorConstraintAtom.
    struct hkpLinMotorConstraintAtom : public RE::hkpConstraintAtom
    {
        bool                      enabled;                                  // 02
        std::uint8_t              motorAxis;                                // 03
        std::int16_t              initializedOffset;                        // 04
        std::int16_t              previousTargetPositionOffset;             // 06
        std::int16_t              correspondingLinLimitSolverResultOffset;  // 08
        float                     targetPosition;                           // 0C
        RE::hkpConstraintMotor*    motor;                                    // 10
    };
    static_assert(sizeof(hkpLinMotorConstraintAtom) == 0x18);

    class EmbedGrabConstraintData : public RE::hkpConstraintData
    {
    public:
        enum SolverResult : std::int32_t
        {
            kMotor0 = 0,
            kMotor1 = 1,
            kMotor2 = 2,
            kMotor3 = 3,
            kMotor4 = 4,
            kMotor5 = 5,
            kMax = 6
        };

        struct Runtime
        {
            RE::hkpSolverResults solverResults[kMax];
            std::uint8_t         initialized[3];
            float                previousTargetAngles[3];
            std::uint8_t         initializedLinear[3];
            float                previousTargetPositions[3];

            static std::int32_t ExternalSize()
            {
                return static_cast<std::int32_t>(sizeof(Runtime) * 2);
            }
        };

        struct Atoms
        {
            RE::hkpSetLocalTransformsConstraintAtom transforms;
            RE::hkpSetupStabilizationAtom           setupStabilization;
            RE::hkpRagdollMotorConstraintAtom       ragdollMotors;
            hkpLinMotorConstraintAtom               linearMotor0;
            hkpLinMotorConstraintAtom               linearMotor1;
            hkpLinMotorConstraintAtom               linearMotor2;

            [[nodiscard]] const RE::hkpConstraintAtom* GetAtoms() const
            {
                return &transforms;
            }

            [[nodiscard]] std::int32_t GetSizeOfAllAtoms() const
            {
                return static_cast<std::int32_t>(
                    reinterpret_cast<const char*>(&linearMotor2 + 1) - reinterpret_cast<const char*>(&transforms));
            }
        };

        EmbedGrabConstraintData();
        ~EmbedGrabConstraintData() override;

        void SetInBodySpace(const RE::hkTransform& transformA, const RE::hkTransform& transformB);
        void SetMotor(std::int32_t index, RE::hkpConstraintMotor* newMotor);
        void SetMotorsActive(RE::hkpConstraintInstance* instance, bool enabled);
        void SetTarget(const RE::hkMatrix3& targetCbRca);
        void SetTargetRelativeOrientationOfBodies(const RE::hkRotation& bRa);

        void ApplyActorMotorTuning();

        Atoms&       AtomsMut() { return atoms; }
        const Atoms& AtomsRef() const { return atoms; }

        // RE::hkpConstraintData
        void             SetMaxLinearImpulse(float impulse) override;
        float            GetMaxLinearImpulse() const override;
        void             SetSolvingMethod(SolvingMethod method) override;
        RE::hkResult     GetInertiaStabilizationFactor(float& outFactor) const override;
        RE::hkResult     SetInertiaStabilizationFactor(float factor) override;
        void             SetBodyToNotify(std::int32_t bodyIdx) override;
        std::uint8_t     GetNotifiedBodyIndex() const override;
        bool             IsValid() const override;
        std::int32_t     GetType() const override;
        void             GetRuntimeInfo(bool wantRuntime, RuntimeInfo& infoOut) const override;
        RE::hkpSolverResults GetSolverResults(RE::hkpConstraintRuntime* runtime) override;
        void             AddInstance(RE::hkpConstraintInstance* constraint, RE::hkpConstraintRuntime* runtime,
                                     std::int32_t sizeOfRuntime) const override;
        void             RemoveInstance(RE::hkpConstraintInstance* constraint, RE::hkpConstraintRuntime* runtime,
                                        std::int32_t sizeOfRuntime) const override;

    private:
        [[nodiscard]] Runtime* GetRuntime(RE::hkpConstraintRuntime* runtime) const
        {
            return reinterpret_cast<Runtime*>(runtime);
        }

        alignas(16) Atoms atoms;
    };
}
