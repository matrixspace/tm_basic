#ifndef TM_KIT_BASIC_TRANSACTION_SINGLE_KEY_LOCAL_STORAGE_TRANSACTION_BROKER_HPP_
#define TM_KIT_BASIC_TRANSACTION_SINGLE_KEY_LOCAL_STORAGE_TRANSACTION_BROKER_HPP_

#include <tm_kit/basic/transaction/SingleKeyLocalTransactionHandlerComponent.hpp>
#include <tm_kit/basic/transaction/VersionProviderComponent.hpp>
#include <unordered_map>
#include <unordered_set>
#include <mutex>

namespace dev { namespace cd606 { namespace tm { namespace basic { namespace transaction {
    
    template <
        class M
        , class KeyType
        , class DataType
        , class VersionType
        , class DataSummaryType = DataType
        , class CheckSummary = std::equal_to<DataType>
        , class DataDeltaType = DataType
        , class ApplyDelta = CopyApplier<DataType>
        , class Cmp = std::less<VersionType>
        , class KeyHash = std::hash<KeyType>
    >
    class SingleKeyLocalStorageTransactionBroker final
        :
        public virtual M::IExternalComponent
        , public M::template AbstractOnOrderFacility<
            typename SingleKeyTransactionInterface<KeyType,DataType,VersionType,typename M::EnvironmentType::IDType,DataSummaryType,DataDeltaType,Cmp>
                    ::FacilityInput
            , typename SingleKeyTransactionInterface<KeyType,DataType,VersionType,typename M::EnvironmentType::IDType,DataSummaryType,DataDeltaType,Cmp>
                    ::FacilityOutput
        >
    {
    private:
        using TI = SingleKeyTransactionInterface<KeyType,DataType,VersionType,typename M::EnvironmentType::IDType,DataSummaryType,DataDeltaType,Cmp>;
        using TH = SingleKeyLocalTransactionHandlerComponent<KeyType,DataType,DataDeltaType>;
        using VP = VersionProviderComponent<KeyType,DataType,VersionType>;

        struct OneKeyInfo {
            typename TI::OneValue currentValue;
            std::unordered_set<typename M::EnvironmentType::IDType, typename M::EnvironmentType::IDHash> subscribers;
        };
        std::unordered_map<KeyType, OneKeyInfo, KeyHash> dataMap_;
        std::unordered_map<typename M::EnvironmentType::IDType, std::string, typename M::EnvironmentType::IDHash> idToAccount_;
        std::recursive_mutex mutex_;
        CheckSummary checkSummary_;
        ApplyDelta applyDelta_;

        bool checkTransactionPrecondition(typename M::EnvironmentType::IDType const &requester, typename TI::InsertAction const &insertAction) {
            auto iter = dataMap_.find(insertAction.key);
            if (iter == dataMap_.end()) {
                return true;
            }
            if (!iter->second.currentValue.data) {
                return true;
            }
            return false;
        }
        bool checkTransactionPrecondition(typename M::EnvironmentType::IDType const &requester, typename TI::UpdateAction const &updateAction) { 
            auto iter = dataMap_.find(updateAction.key);
            if (iter == dataMap_.end()) {
                return false;
            }
            if (!iter->second.currentValue.data) {
                return false;
            }
            if (updateAction.ignoreChecks) {
                return true;
            }
            if (iter->second.currentValue.version != updateAction.oldVersion) {
                return false;
            }
            if (!checkSummary_(*(iter->second.currentValue.data), updateAction.oldDataSummary)) {
                return false;
            }
            return true;
        }
        bool checkTransactionPrecondition(typename M::EnvironmentType::IDType const &requester, typename TI::DeleteAction const &deleteAction) {
            auto iter = dataMap_.find(deleteAction.key);
            if (iter == dataMap_.end()) {
                return false;
            }
            if (!iter->second.currentValue.data) {
                return false;
            }
            if (deleteAction.ignoreChecks) {
                return true;
            }
            if (iter->second.currentValue.version != deleteAction.oldVersion) {
                return false;
            }
            if (!checkSummary_(*(iter->second.currentValue.data), deleteAction.oldDataSummary)) {
                return false;
            }
            return true;
        }

        void handleTransactionResult(typename M::EnvironmentType *env, typename M::EnvironmentType::IDType const &requester, typename TI::InsertAction const &insertAction, VP *vp) {
            auto iter = dataMap_.find(insertAction.key);
            if (iter == dataMap_.end()) {
                iter = dataMap_.insert(std::make_pair<KeyType,OneKeyInfo>(
                    KeyType(insertAction.key)
                    , OneKeyInfo {
                        typename TI::OneValue {
                            insertAction.key
                            , vp->getNextVersionForKey(insertAction.key, &(insertAction.data))
                            , insertAction.data
                        }
                        , std::unordered_set<typename M::EnvironmentType::IDType, typename M::EnvironmentType::IDHash> {}
                    }
                )).first;
            } else {
                iter->second.currentValue = typename TI::OneValue {
                    insertAction.key
                    , vp->getNextVersionForKey(insertAction.key, &(insertAction.data))
                    , insertAction.data
                };
            }
            for (auto const &id : iter->second.subscribers) {
                this->publish(
                    env
                    , typename M::template Key<typename TI::FacilityOutput> {
                        id, typename TI::FacilityOutput { {iter->second.currentValue} }
                    }
                    , false
                );
            }
            this->publish(
                env
                , typename M::template Key<typename TI::FacilityOutput> {
                    requester, typename TI::FacilityOutput { {iter->second.currentValue} }
                }
                , false
            );
        }
        void handleTransactionResult(typename M::EnvironmentType *env, typename M::EnvironmentType::IDType const &requester, typename TI::UpdateAction const &updateAction, VP *vp) {
            auto iter = dataMap_.find(updateAction.key);
            //iter must be there already because the transaction actually succeeded in the back end
            applyDelta_(*(iter->second.currentValue.data), updateAction.dataDelta);
            iter->second.currentValue.version = 
                vp->getNextVersionForKey(updateAction.key, &(*(iter->second.currentValue.data)));
            typename TI::OneDelta delta {
                updateAction.key
                , iter->second.currentValue.version
                , updateAction.dataDelta
            };
            for (auto const &id : iter->second.subscribers) {
                this->publish(
                    env,
                    typename M::template Key<typename TI::FacilityOutput> {
                        id, typename TI::FacilityOutput { {delta} }
                    }
                    , false
                );
            }
            this->publish(
                env
                , typename M::template Key<typename TI::FacilityOutput> {
                    requester, typename TI::FacilityOutput { {iter->second.currentValue} }
                }
                , false
            );
        }
        void handleTransactionResult(typename M::EnvironmentType *env, typename M::EnvironmentType::IDType const &requester, typename TI::DeleteAction const &deleteAction, VP *vp) {
            auto iter = dataMap_.find(deleteAction.key);
            if (iter == dataMap_.end()) {
                iter = dataMap_.insert(std::make_pair<KeyType,OneKeyInfo>(
                    KeyType(deleteAction.key)
                    , OneKeyInfo {
                        typename TI::OneValue {
                            deleteAction.key
                            , vp->getNextVersionForKey(deleteAction.key, nullptr)
                            , std::nullopt
                        }
                        , std::unordered_set<typename M::EnvironmentType::IDType, typename M::EnvironmentType::IDHash> {}
                    }
                )).first;
            } else {
                iter->second.currentValue = typename TI::OneValue {
                    deleteAction.key
                    , vp->getNextVersionForKey(deleteAction.key, nullptr)
                    , std::nullopt
                };
            }
            for (auto const &id : iter->second.subscribers) {
                this->publish(
                    env
                    , typename M::template Key<typename TI::FacilityOutput> {
                        id, typename TI::FacilityOutput { {iter->second.currentValue} }
                    }
                    , false);
            }
            this->publish(
                env
                , typename M::template Key<typename TI::FacilityOutput> {
                    requester, typename TI::FacilityOutput { {iter->second.currentValue} }
                }
                , false
            );
        }
        void handleSubscription(typename M::EnvironmentType *env, std::string const &account, typename M::EnvironmentType::IDType const &requester, KeyType const &key, VP *vp) {
            idToAccount_.insert({requester, account});
            auto iter = dataMap_.find(key);
            if (iter == dataMap_.end()) {
                iter = dataMap_.insert(std::make_pair<KeyType,OneKeyInfo>(
                    KeyType(key)
                    , OneKeyInfo {
                        typename TI::OneValue {
                            key
                            , vp->getNextVersionForKey(key, nullptr)
                            , std::nullopt
                        }
                        , std::unordered_set<typename M::EnvironmentType::IDType, typename M::EnvironmentType::IDHash> {}
                    }
                )).first;
            }
            iter->second.subscribers.insert(requester);
            this->publish(
                env
                , typename M::template Key<typename TI::FacilityOutput> {
                    requester, typename TI::FacilityOutput { {typename TI::SubscriptionAck {}} }
                }
                , false);
            this->publish(
                env
                , typename M::template Key<typename TI::FacilityOutput> {
                    requester, typename TI::FacilityOutput { {iter->second.currentValue} }
                }
                , false);
        }
        void handleUnsubscription(typename M::EnvironmentType *env, std::string const &account, typename M::EnvironmentType::IDType const &requester, typename M::EnvironmentType::IDType const &originalSubscriptionID, KeyType const &key, VP *vp) {
            auto idToAccountIter = idToAccount_.find(originalSubscriptionID);
            if (idToAccountIter == idToAccount_.end()) {
                this->publish(
                    env
                    , typename M::template Key<typename TI::FacilityOutput> {
                        requester, typename TI::FacilityOutput { {typename TI::UnsubscriptionAck {}} }
                    }
                    , true);
                return;
            }
            if (idToAccountIter->second != account) {
                this->publish(
                    env
                    , typename M::template Key<typename TI::FacilityOutput> {
                        requester, typename TI::FacilityOutput { {typename TI::UnsubscriptionAck {}} }
                    }
                    , true);
                return;
            }
            idToAccount_.erase(idToAccountIter);
            auto iter = dataMap_.find(key);
            if (iter == dataMap_.end()) {
                this->publish(
                    env
                    , typename M::template Key<typename TI::FacilityOutput> {
                        requester, typename TI::FacilityOutput { {typename TI::UnsubscriptionAck {}} }
                    }
                    , true);
                return;
            }
            auto innerIter = iter->second.subscribers.find(originalSubscriptionID);
            if (innerIter == iter->second.subscribers.end()) {
                this->publish(
                    env
                    , typename M::template Key<typename TI::FacilityOutput> {
                        requester, typename TI::FacilityOutput { {typename TI::UnsubscriptionAck {}} }
                    }
                    , true
                );
                return;
            }
            this->publish(
                env
                , typename M::template Key<typename TI::FacilityOutput> {
                    *innerIter, typename TI::FacilityOutput { {typename TI::UnsubscriptionAck {}} }
                }
                , true
            );
            iter->second.subscribers.erase(innerIter);
            this->publish(
                env
                , typename M::template Key<typename TI::FacilityOutput> {
                    requester, typename TI::FacilityOutput { {typename TI::UnsubscriptionAck {}} }
                }
                , true
            );
            return;
        }
    public:
        virtual void start(typename M::EnvironmentType *env) {
            auto *vp = static_cast<VP *>(env);
            auto initialValues = static_cast<TH *>(env)->loadInitialData();
            std::lock_guard<std::recursive_mutex> _(mutex_);
            for (auto const &v : initialValues) {
                auto const &key = std::get<0>(v);
                dataMap_.insert(std::make_pair<KeyType, OneKeyInfo>(
                    KeyType(key)
                    , OneKeyInfo {
                        typename TI::OneValue {
                            key
                            , vp->getNextVersionForKey(key, &(std::get<1>(v)))
                            , std::get<1>(v)
                        }
                        , std::unordered_set<typename M::EnvironmentType::IDType, typename M::EnvironmentType::IDHash> {}
                    }
                ));
            }
        }
        virtual void handle(typename M::template InnerData<typename M::template Key<typename TI::FacilityInput>> &&input) override final {
            auto *env = input.environment;
            auto *handler = static_cast<TH *>(env);
            auto *versionProvider = static_cast<VP *>(env);
            auto account = std::move(std::get<0>(input.timedData.value.key()));
            auto requester = std::move(input.timedData.value.id());
            
            std::lock_guard<std::recursive_mutex> _(mutex_);
            std::visit([this,env,handler,versionProvider,&account,&requester](auto && arg) {
                using T = std::decay_t<decltype(arg)>;
                if constexpr (std::is_same_v<typename TI::Transaction, T>) {
                    auto transaction = std::move(arg);
                    std::visit([this,env,handler,versionProvider,&account,&requester](auto const &trans) {
                        using T1 = std::decay_t<decltype(trans)>;
                        if constexpr (std::is_same_v<typename TI::InsertAction, T1>) {
                            auto const &insertAction = trans;
                            if (!this->checkTransactionPrecondition(requester, insertAction)) {
                                typename TI::TransactionResult res = typename TI::TransactionFailurePrecondition {};
                                this->publish(
                                    env
                                    , typename M::template Key<typename TI::FacilityOutput> {
                                        requester, typename TI::FacilityOutput { {res} }
                                    }
                                    , true
                                );
                                return;
                            }
                            RequestDecision actionRes = handler->handleInsert(account, insertAction.key, insertAction.data, insertAction.ignoreChecks);
                            if (actionRes != RequestDecision::Success) {
                                typename TI::TransactionResult res;
                                switch (actionRes) {
                                case RequestDecision::FailurePrecondition:
                                    res = typename TI::TransactionFailurePrecondition {};
                                case RequestDecision::FailureConsistency:
                                    res = typename TI::TransactionFailureConsistency {};
                                case RequestDecision::FailurePermission:
                                default:
                                    res = typename TI::TransactionFailurePermission {};
                                }
                                this->publish(
                                    env
                                    , typename M::template Key<typename TI::FacilityOutput> {
                                        requester, typename TI::FacilityOutput { {res} }
                                    }
                                    , true
                                );
                                return;
                            }
                            this->handleTransactionResult(env, requester, insertAction, versionProvider);
                            typename TI::TransactionResult res = typename TI::TransactionSuccess {};
                            this->publish(
                                env
                                , typename M::template Key<typename TI::FacilityOutput> {
                                    requester, typename TI::FacilityOutput { {res} }
                                }
                                , true
                            );
                        } else if constexpr (std::is_same_v<typename TI::UpdateAction, T1>) {
                            auto const &updateAction = trans;
                            if (!this->checkTransactionPrecondition(requester, updateAction)) {
                                typename TI::TransactionResult res = typename TI::TransactionFailurePrecondition {};
                                this->publish(
                                    env
                                    , typename M::template Key<typename TI::FacilityOutput> {
                                        requester, typename TI::FacilityOutput { {res} }
                                    }
                                    , true);
                                return;
                            }
                            RequestDecision actionRes = handler->handleUpdate(account, updateAction.key, updateAction.dataDelta, updateAction.ignoreChecks);
                            if (actionRes != RequestDecision::Success) {
                                typename TI::TransactionResult res;
                                switch (actionRes) {
                                case RequestDecision::FailurePrecondition:
                                    res = typename TI::TransactionFailurePrecondition {};
                                case RequestDecision::FailureConsistency:
                                    res = typename TI::TransactionFailureConsistency {};
                                case RequestDecision::FailurePermission:
                                default:
                                    res = typename TI::TransactionFailurePermission {};
                                }
                                this->publish(
                                    env
                                    , typename M::template Key<typename TI::FacilityOutput> {
                                        requester, typename TI::FacilityOutput { {res} }
                                    }
                                    , true
                                );
                                return;
                            }
                            this->handleTransactionResult(env, requester, updateAction, versionProvider);
                            typename TI::TransactionResult res = typename TI::TransactionSuccess {};
                            this->publish(
                                env
                                , typename M::template Key<typename TI::FacilityOutput> {
                                    requester, typename TI::FacilityOutput { {res} }
                                }
                                , true
                            );
                        } else if constexpr (std::is_same_v<typename TI::DeleteAction, T1>) {
                            auto const &deleteAction = trans;
                            if (!this->checkTransactionPrecondition(requester, deleteAction)) {
                                typename TI::TransactionResult res = typename TI::TransactionFailurePrecondition {};
                                this->publish(
                                    env
                                    , typename M::template Key<typename TI::FacilityOutput> {
                                        requester, typename TI::FacilityOutput { {res} }
                                    }
                                    , true);
                                return;
                            }
                            RequestDecision actionRes = handler->handleDelete(account, deleteAction.key, deleteAction.ignoreChecks);
                            if (actionRes != RequestDecision::Success) {
                                typename TI::TransactionResult res;
                                switch (actionRes) {
                                case RequestDecision::FailurePrecondition:
                                    res = typename TI::TransactionFailurePrecondition {};
                                case RequestDecision::FailureConsistency:
                                    res = typename TI::TransactionFailureConsistency {};
                                case RequestDecision::FailurePermission:
                                default:
                                    res = typename TI::TransactionFailurePermission {};
                                }
                                this->publish(
                                    env
                                    , typename M::template Key<typename TI::FacilityOutput> {
                                        requester, typename TI::FacilityOutput { {res} }
                                    }
                                    , true
                                );
                                return;
                            } 
                            this->handleTransactionResult(env, requester, deleteAction, versionProvider);
                            typename TI::TransactionResult res = typename TI::TransactionSuccess {};
                            this->publish(
                                env
                                , typename M::template Key<typename TI::FacilityOutput> {
                                    requester, typename TI::FacilityOutput { {res} }
                                }
                                , true
                            );
                        } else {
                            typename TI::TransactionResult res = typename TI::TransactionFailurePermission {};
                            this->publish(
                                env
                                , typename M::template Key<typename TI::FacilityOutput> {
                                    requester, typename TI::FacilityOutput { {res} }
                                }
                                , true
                            );
                        }
                    }, transaction);
                } else if constexpr (std::is_same_v<typename TI::Subscription, T>) {
                    auto subscription = std::move(arg);
                    this->handleSubscription(env, account, requester, subscription.key, versionProvider);
                } else if constexpr (std::is_same_v<typename TI::Unsubscription, T>) {
                    auto unsubscription = std::move(arg);
                    this->handleUnsubscription(env, account, requester, unsubscription.originalSubscriptionID, unsubscription.key, versionProvider);
                }
            }, std::move(std::get<1>(input.timedData.value.key()).value));
        }
    };

} } } } } 

#endif