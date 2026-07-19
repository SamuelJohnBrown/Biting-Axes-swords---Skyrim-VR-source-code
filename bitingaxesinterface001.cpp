#include "bitingaxesinterface001.h"

#include "helper.h"

namespace BitingAxesAPI
{
    namespace
    {
        struct BitingAxesMessage
        {
            enum : std::uint32_t { kMessage_GetInterface = 0xB17E4871u };
            void* (*GetApiFunction)(unsigned int revisionNumber) = nullptr;
        };

        class BitingAxesInterface001 final : public IBitingAxesInterface001
        {
        public:
            static BitingAxesInterface001& GetSingleton()
            {
                static BitingAxesInterface001 instance;
                return instance;
            }

            std::uint32_t GetBuildNumber() override
            {
                return 2;
            }

            bool IsHandEmbedded(bool isLeft) override
            {
                BitingAxesAPI::EmbedSnapshot snapshot{};
                return BitingAxesVR::EmbedApi::BuildHandEmbedSnapshot(isLeft, snapshot) && snapshot.active;
            }

            bool GetHandEmbedSnapshot(bool isLeft, EmbedSnapshot& outSnapshot) override
            {
                return BitingAxesVR::EmbedApi::BuildHandEmbedSnapshot(isLeft, outSnapshot);
            }

            bool IsAnyHandEmbedded() override
            {
                return IsHandEmbedded(false) || IsHandEmbedded(true);
            }

            bool IsHandWorldAxeEmbed(bool isLeft) override
            {
                EmbedSnapshot snapshot{};
                if (!BitingAxesVR::EmbedApi::BuildHandEmbedSnapshot(isLeft, snapshot)) {
                    return false;
                }
                return snapshot.active && snapshot.useWorldAxe;
            }

            bool CanLeaveWeaponInBody(bool isLeft) override
            {
                EmbedSnapshot snapshot{};
                if (!BitingAxesVR::EmbedApi::BuildHandEmbedSnapshot(isLeft, snapshot)) {
                    return false;
                }
                return snapshot.active && !snapshot.useWorldAxe && snapshot.npcEmbedEligible;
            }

            bool VictimHasLodgedWeapon(RE::Actor* victim) override
            {
                return BitingAxesVR::EmbedApi::VictimHasLodgedWeapon(victim);
            }

            void AddEmbedStartedCallback(EmbedCallback callback) override
            {
                if (callback) {
                    _embedStartedCallbacks.push_back(callback);
                }
            }

            void AddEmbedEndedCallback(EmbedCallback callback) override
            {
                if (callback) {
                    _embedEndedCallbacks.push_back(callback);
                }
            }

            void AddWorldAxeEmbedCallback(EmbedCallback callback) override
            {
                if (callback) {
                    _worldAxeEmbedCallbacks.push_back(callback);
                }
            }

            void AddWeaponLodgedInBodyCallback(EmbedCallback callback) override
            {
                if (callback) {
                    _weaponLodgedCallbacks.push_back(callback);
                }
            }

            void AddWeaponExtractedFromBodyCallback(EmbedCallback callback) override
            {
                if (callback) {
                    _weaponExtractedCallbacks.push_back(callback);
                }
            }

            void NotifyEmbedStarted(const EmbedSnapshot& snapshot)
            {
                for (auto* callback : _embedStartedCallbacks) {
                    callback(snapshot.isLeft, snapshot);
                }
            }

            void NotifyEmbedEnded(const EmbedSnapshot& snapshot)
            {
                for (auto* callback : _embedEndedCallbacks) {
                    callback(snapshot.isLeft, snapshot);
                }
            }

            void NotifyWorldAxeEmbed(const EmbedSnapshot& snapshot)
            {
                for (auto* callback : _worldAxeEmbedCallbacks) {
                    callback(snapshot.isLeft, snapshot);
                }
            }

            void NotifyWeaponLodgedInBody(const EmbedSnapshot& snapshot)
            {
                for (auto* callback : _weaponLodgedCallbacks) {
                    callback(snapshot.isLeft, snapshot);
                }
            }

            void NotifyWeaponExtractedFromBody(const EmbedSnapshot& snapshot)
            {
                for (auto* callback : _weaponExtractedCallbacks) {
                    callback(snapshot.isLeft, snapshot);
                }
            }

        private:
            std::vector<EmbedCallback> _embedStartedCallbacks;
            std::vector<EmbedCallback> _embedEndedCallbacks;
            std::vector<EmbedCallback> _worldAxeEmbedCallbacks;
            std::vector<EmbedCallback> _weaponLodgedCallbacks;
            std::vector<EmbedCallback> _weaponExtractedCallbacks;
        };

        void* GetApiFunction(unsigned int revisionNumber)
        {
            if (revisionNumber == 1) {
                return static_cast<IBitingAxesInterface001*>(&BitingAxesInterface001::GetSingleton());
            }
            return nullptr;
        }

        void OnBitingAxesMessage(SKSE::MessagingInterface::Message* msg)
        {
            if (!msg || msg->type != BitingAxesMessage::kMessage_GetInterface) {
                return;
            }

            if (msg->dataLen != sizeof(BitingAxesMessage)) {
                return;
            }

            auto* apiMsg = static_cast<BitingAxesMessage*>(msg->data);
            apiMsg->GetApiFunction = GetApiFunction;
        }
    }

    IBitingAxesInterface001* GetBitingAxesInterface001(const SKSE::PluginHandle& /*pluginHandle*/,
                                                       SKSE::MessagingInterface* messagingInterface)
    {
        static IBitingAxesInterface001* cached = nullptr;
        if (cached) {
            return cached;
        }

        if (!messagingInterface) {
            return nullptr;
        }

        BitingAxesMessage message{};
        const bool dispatched = messagingInterface->Dispatch(BitingAxesMessage::kMessage_GetInterface, &message,
                                                             static_cast<std::uint32_t>(sizeof(message)),
                                                             kInterfaceRecipient);
        if (!dispatched || !message.GetApiFunction) {
            return nullptr;
        }

        cached = static_cast<IBitingAxesInterface001*>(message.GetApiFunction(1));
        return cached;
    }

    void RegisterBitingAxesInterface()
    {
        static bool s_registered = false;
        if (s_registered) {
            return;
        }

        auto* messaging = SKSE::GetMessagingInterface();
        if (!messaging) {
            IW_LOG_WARN("{}: cannot register mod API; messaging interface unavailable", BitingAxesVR::kPluginDisplayName);
            return;
        }

        // Register on every loaded plugin's dispatch list so consumers can Dispatch(..., kInterfaceRecipient).
        // RegisterListener(senderName) listens *from* that sender — wrong direction for a provider API.
        // Must run from kPostPostLoad or later (not during SKSEPlugin_Load: self is not in the plugin list yet).
        if (messaging->RegisterListener(static_cast<const char*>(nullptr), OnBitingAxesMessage)) {
            s_registered = true;
            IW_LOG_INFO("{}: mod-support API registered (revision 1, build 2, recipient \"{}\")", BitingAxesVR::kPluginDisplayName,
                        kInterfaceRecipient);
        } else {
            IW_LOG_WARN("{}: failed to register mod-support API listener", BitingAxesVR::kPluginDisplayName);
        }
    }

    void NotifyEmbedStarted(const EmbedSnapshot& snapshot)
    {
        BitingAxesInterface001::GetSingleton().NotifyEmbedStarted(snapshot);
    }

    void NotifyEmbedEnded(const EmbedSnapshot& snapshot)
    {
        BitingAxesInterface001::GetSingleton().NotifyEmbedEnded(snapshot);
    }

    void NotifyWorldAxeEmbed(const EmbedSnapshot& snapshot)
    {
        BitingAxesInterface001::GetSingleton().NotifyWorldAxeEmbed(snapshot);
    }

    void NotifyWeaponLodgedInBody(const EmbedSnapshot& snapshot)
    {
        BitingAxesInterface001::GetSingleton().NotifyWeaponLodgedInBody(snapshot);
    }

    void NotifyWeaponExtractedFromBody(const EmbedSnapshot& snapshot)
    {
        BitingAxesInterface001::GetSingleton().NotifyWeaponExtractedFromBody(snapshot);
    }
}
