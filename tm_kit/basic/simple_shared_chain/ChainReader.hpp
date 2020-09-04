#ifndef TM_KIT_BASIC_SIMPLE_SHARED_CHAIN_CHAIN_READER_HPP_
#define TM_KIT_BASIC_SIMPLE_SHARED_CHAIN_CHAIN_READER_HPP_

#include <tm_kit/basic/VoidStruct.hpp>
#include <optional>

namespace dev { namespace cd606 { namespace tm { namespace basic { namespace simple_shared_chain {
    template <class Env, class Chain, class ChainItemFolder>
    class ChainReader {
    private:
        Chain const *chain_;
        typename Chain::ItemType currentItem_;
        ChainItemFolder folder_;
        typename ChainItemFolder::ResultType currentValue_;
    public:
        ChainReader(Env *env, Chain const *chain) : chain_(chain), currentItem_(), folder_(), currentValue_() {
            currentValue_ = folder_.initialize(env);
        }
        ~ChainReader() = default;
        ChainReader(ChainReader const &) = delete;
        ChainReader &operator=(ChainReader const &) = delete;
        ChainReader(ChainReader &&) = default;
        ChainReader &operator=(ChainReader &&) = default;
        typename std::optional<typename ChainItemFolder::ResultType> operator()(VoidStruct &&) {
            bool hasNew = false;
            while (chain_->fetchNext(&currentItem_)) {
                currentValue_ = folder_.fold(currentValue_, currentItem_);
                hasNew = true;
            }
            if (hasNew) {
                return {currentValue_};
            } else {
                return std::nullopt;
            }
        }
    };
} } } } }

#endif