#ifndef TM_KIT_BASIC_SIMPLE_SHARED_CHAIN_ONE_SHOT_CHAIN_WRITER_HPP_
#define TM_KIT_BASIC_SIMPLE_SHARED_CHAIN_ONE_SHOT_CHAIN_WRITER_HPP_

#include <boost/hana/type.hpp>

namespace dev { namespace cd606 { namespace tm { namespace basic { namespace simple_shared_chain {

    template <class Env, class Chain>
    class OneShotChainWriter {
    public:
        //F maps ChainItemFolder::ResultType (the state) to std::optional<std::tuple<std::string, typename Chain::DataType>>
        template <class ChainItemFolder, class F>
        static void write(Env *env, Chain *chain, F const &f, ChainItemFolder &&folder = ChainItemFolder {}) {
            static auto loadUntilChecker = boost::hana::is_valid(
                [](auto *c, auto *f, auto const *v) -> decltype((void) (c->loadUntil((Env *) nullptr, f->chainIDForState(*v)))) {}
            );
            static auto foldInPlaceChecker = boost::hana::is_valid(
                [](auto *f, auto *v, auto const *id, auto const *data) -> decltype((void) (f->foldInPlace(*v, *id, data))) {}
            );

            ChainItemFolder myFolder(std::move(folder));
            typename ChainItemFolder::ResultType currentState = myFolder.initialize(env, chain);
            
            typename Chain::ItemType currentItem;
            if constexpr (loadUntilChecker(
                (Chain *) nullptr
                , (ChainItemFolder *) nullptr
                , (typename ChainItemFolder::ResultType const *) nullptr
            )) {
                currentItem = chain->loadUntil(env, myFolder.chainIDForState(currentState));
            } else {
                currentItem = chain->head(env);
            }

            while (true) {
                while (true) {
                    std::optional<typename Chain::ItemType> nextItem = chain->fetchNext(currentItem);
                    if (!nextItem) {
                        break;
                    }
                    currentItem = std::move(*nextItem);
                    if constexpr (foldInPlaceChecker(
                        (ChainItemFolder *) nullptr
                        , (typename ChainItemFolder::ResultType *) nullptr
                        , (std::string_view *) nullptr
                        , (typename Chain::DataType const *) nullptr
                    )) {
                        folder_.foldInPlace(currentState_, Chain::extractStorageIDStringView(currentItem_), Chain::extractData(currentItem_));
                    } else {
                        currentState_ = folder_.fold(currentState_, Chain::extractStorageIDStringView(currentItem_), Chain::extractData(currentItem_));
                    }
                }
                typename std::optional<std::tuple<std::string, typename Chain::DataType>>
                    calcRes = f(currentState);
                if (calcRes) {
                    std::string newID = std::move(std::get<0>(*calcRes));
                    if (newID == "") {
                        newID = Chain::newStorageIDAsString<Env>();
                    }
                    if (chain->appendAfter(currentItem, chain->formChainItem(newID, std::move(std::get<1>(calcRes))))) {
                        break;
                    }
                } else {
                    break;
                }
            }
        }
        //This one does not use a folder, but this also means it cannot jump directly
        //into the middle of the chain. If jumping into the middle is desired, then 
        //write should be used, where a folder should be provided, and the const values 
        //can be put in through a trivial lambda
        static void writeConstValue(Env *env, Chain *chain, std::string const &storageID, typename Chain::DataType &&value) {
            typename Chain::ItemType currentItem = chain->head(env);
            while (true) {
                while (true) {
                    std::optional<typename Chain::ItemType> nextItem = chain->fetchNext(currentItem);
                    if (!nextItem) {
                        break;
                    }
                    currentItem = std::move(*nextItem);
                }
                if (chain->appendAfter(currentItem, chain->formChainItem(storageID, std::move(value)))) {
                    break;
                }
            }
        }
    };

} } } } }

#endif