#ifndef TM_KIT_BASIC_PRINT_HELPER_HPP_
#define TM_KIT_BASIC_PRINT_HELPER_HPP_

#include <tm_kit/infra/ChronoUtils.hpp>
#include <tm_kit/infra/VersionedData.hpp>

#include <tm_kit/basic/ForceDifferentType.hpp>
#include <tm_kit/basic/SingleLayerWrapper.hpp>
#include <tm_kit/basic/VoidStruct.hpp>
#include <tm_kit/basic/ByteData.hpp>

#include <boost/hana/type.hpp>

#include <cctype>

namespace dev { namespace cd606 { namespace tm { namespace basic {
    template <class T>
    class PrintHelper {
    public:
        static void print(std::ostream &os, T const &t) {
            static auto checker = boost::hana::is_valid(
                [](auto const *h) -> decltype((void) ((* (std::ostream *) nullptr) << *h)) {}
            );
            if constexpr(checker((T const *) nullptr)) {
                os << t;
            } else {
                os << "(TypeCannotBePrinted:" << typeid(T).name() << ')';
            }
        }
    };
    template <>
    class PrintHelper<uint8_t> {
    public:
        static void print(std::ostream &os, uint8_t const &t) {
            if (t >= 32 && t <= 126) {
                os << "'" << (char) t << "'(" << (int) t << ")";
            } else {
                os << "(" << (int) t << ")";
            }
        }
    };
    template <>
    class PrintHelper<std::byte> {
    public:
        static void print(std::ostream &os, std::byte const &t) {
            os << std::setw(2) << std::setfill('0') << std::hex << (uint8_t) t << std::dec;
            return os;
        }
    };
    template <>
    class PrintHelper<std::chrono::system_clock::time_point> {
    public:
        static void print(std::ostream &os, std::chrono::system_clock::time_point const &t) {
            os << infra::withtime_utils::localTimeString(t);
        }
    };
    template <>
    class PrintHelper<std::string> {
    public:
        static void print(std::ostream &os, std::string const &t) {
            os << "'" << t << "'";
        }
    };
    template <>
    class PrintHelper<std::string_view> {
    public:
        static void print(std::ostream &os, std::string_view const &t) {
            os << "'" << t << "'";
        }
    };
    template <class VersionType, class DataType, class CmpType>
    class PrintHelper<infra::VersionedData<VersionType,DataType,CmpType>> {
    public:
        static void print(std::ostream &os, infra::VersionedData<VersionType,DataType,CmpType> const &t) {
            os << "VersionedData{version=";
            PrintHelper<VersionType>::print(os, t.version);
            os << ",data=";
            PrintHelper<DataType>::print(os, t.data);
            os << '}';
        }
    };
    template <class GroupIDType, class VersionType, class DataType, class CmpType>
    class PrintHelper<infra::GroupedVersionedData<GroupIDType,VersionType,DataType,CmpType>> {
    public:
        static void print(std::ostream &os, infra::GroupedVersionedData<GroupIDType,VersionType,DataType,CmpType> const &t) {
            os << "GroupedVersionedData{groupID=";
            PrintHelper<GroupIDType>::print(os, t.groupID);
            os << ",version=";
            PrintHelper<VersionType>::print(os, t.version);
            os << ",data=";
            PrintHelper<DataType>::print(os, t.data);
            os << '}';
        }
    };
    template <class T>
    class PrintHelper<ForceDifferentType<T>> {
    public:
        static void print(std::ostream &os, ForceDifferentType<T> const &t) {
            os << "ForceDifferentType{}";
        }
    };
    template <class T>
    class PrintHelper<SingleLayerWrapper<T>> {
    public:
        static void print(std::ostream &os, SingleLayerWrapper<T> const &t) {
            os << "SingleLayerWrapper{";
            PrintHelper<T>::print(os, t.value);
            os << '}';
        }
    };
    template <>
    class PrintHelper<VoidStruct> {
    public:
        static void print(std::ostream &os, VoidStruct const &t) {
            os << "VoidStruct{}";
        }
    };
    template <class T>
    class PrintHelper<CBOR<T>> {
    public:
        static void print(std::ostream &os, CBOR<T> const &t) {
            os << "CBOR{";
            PrintHelper<T>::print(os, t.value);
            os << '}';
        }
    };
    template <class T>
    class PrintHelper<CBORWithMaxSizeHint<T>> {
    public:
        static void print(std::ostream &os, CBORWithMaxSizeHint<T> const &t) {
            os << "CBORWithMaxSizeHint{";
            PrintHelper<T>::print(os, t.value);
            os << "}(max-size:" << t.maxSizeHint << " bytes)";
        }
    };
    template <class T>
    class PrintHelper<std::list<T>> {
    public:
        static void print(std::ostream &os, std::list<T> const &t) {
            os << "list[";
            bool start = true;
            for (auto const &x : t) {
                if (!start) {
                    os << ',';
                }
                PrintHelper<T>::print(os, x);
                start = false;
            }
            os << ']';
        }
    };
    template <class T>
    class PrintHelper<std::vector<T>> {
    public:
        static void print(std::ostream &os, std::vector<T> const &t) {
            os << "vector[";
            bool start = true;
            for (auto const &x : t) {
                if (!start) {
                    os << ',';
                }
                PrintHelper<T>::print(os, x);
                start = false;
            }
            os << ']';
        }
    };
    template <class T>
    class PrintHelper<std::set<T>> {
    public:
        static void print(std::ostream &os, std::set<T> const &t) {
            os << "set[";
            bool start = true;
            for (auto const &x : t) {
                if (!start) {
                    os << ',';
                }
                PrintHelper<T>::print(os, x);
                start = false;
            }
            os << ']';
        }
    };
    template <class T>
    class PrintHelper<std::unordered_set<T>> {
    public:
        static void print(std::ostream &os, std::unordered_set<T> const &t) {
            os << "unordered_set[";
            bool start = true;
            for (auto const &x : t) {
                if (!start) {
                    os << ',';
                }
                PrintHelper<T>::print(os, x);
                start = false;
            }
            os << ']';
        }
    };
    template <class T, size_t N>
    class PrintHelper<std::array<T,N>> {
    public:
        static void print(std::ostream &os, std::array<T,N> const &t) {
            os << "array(" << N << ")[";
            bool start = true;
            for (auto const &x : t) {
                if (!start) {
                    os << ',';
                }
                PrintHelper<T>::print(os, x);
                start = false;
            }
            os << ']';
        }
    };
    template <class K, class V>
    class PrintHelper<std::map<K,V>> {
    public:
        static void print(std::ostream &os, std::map<K,V> const &t) {
            os << "map{";
            bool start = true;
            for (auto const &x : t) {
                if (!start) {
                    os << ',';
                }
                PrintHelper<K>::print(os, x.first);
                os << "=>";
                PrintHelper<V>::print(os, x.second);
                start = false;
            }
            os << '}';
        }
    };
    template <class K, class V>
    class PrintHelper<std::unordered_map<K,V>> {
    public:
        static void print(std::ostream &os, std::unordered_map<K,V> const &t) {
            os << "unordered_map{";
            bool start = true;
            for (auto const &x : t) {
                if (!start) {
                    os << ',';
                }
                PrintHelper<K>::print(os, x.first);
                os << "=>";
                PrintHelper<V>::print(os, x.second);
                start = false;
            }
            os << '}';
        }
    };
    template <class T>
    class PrintHelper<std::optional<T>> {
    public:
        static void print(std::ostream &os, std::optional<T> const &t) {
            if (!t) {
                os << "None";
            } else {
                os << "Just{";
                PrintHelper<T>::print(os, *t);
                os << '}';
            }
        }
    };
    template <class T>
    class PrintHelper<std::unique_ptr<T>> {
    public:
        static void print(std::ostream &os, std::unique_ptr<T> const &t) {
            if (!t) {
                os << "null";
            } else {
                os << "&(";
                PrintHelper<T>::print(os, *t);
                os << ')';
            }
        }
    };
    template <class T>
    class PrintHelper<std::shared_ptr<T>> {
    public:
        static void print(std::ostream &os, std::shared_ptr<T> const &t) {
            if (!t) {
                os << "null";
            } else {
                os << "&(";
                PrintHelper<T>::print(os, *t);
                os << ')';
            }
        }
    };
    template <class TupleType, size_t CurrentIdx, size_t TotalItems, class... Items>
    class TuplePrintHelper {};
    template <class TupleType, size_t CurrentIdx, size_t TotalItems, class CurrentItem, class... MoreItems>
    class TuplePrintHelper<TupleType,CurrentIdx,TotalItems,CurrentItem,MoreItems...> {
    public:
        static void print(std::ostream &os, TupleType const &t) {
            if constexpr (CurrentIdx > 0) {
                os << ',';
            }
            PrintHelper<CurrentItem>::print(os, std::get<CurrentIdx>(t));
            if constexpr (CurrentIdx+1 < TotalItems) {
                TuplePrintHelper<TupleType,CurrentIdx+1,TotalItems,MoreItems...>::print(os, t);
            }
        }
    };
    template <class... Items>
    class PrintHelper<std::tuple<Items...>> {
    public:
        static void print(std::ostream &os, std::tuple<Items...> const &t) {
            os << '{';
            if constexpr (sizeof...(Items) > 0) {
                TuplePrintHelper<std::tuple<Items...>,0,sizeof...(Items),Items...>::print(os, t);
            }
            os << '}';
        }
    };
    template <class VariantType, size_t CurrentIdx, size_t TotalItems, class... Items>
    class VariantPrintHelper {};
    template <class VariantType, size_t CurrentIdx, size_t TotalItems, class CurrentItem, class... MoreItems>
    class VariantPrintHelper<VariantType,CurrentIdx,TotalItems,CurrentItem,MoreItems...> {
    public:
        static void print(std::ostream &os, VariantType const &t, size_t idx) {
            if (CurrentIdx == idx) {
                PrintHelper<CurrentItem>::print(os, std::get<CurrentIdx>(t));
            } else {
                if constexpr (CurrentIdx+1 < TotalItems) {
                    VariantPrintHelper<VariantType, CurrentIdx+1, TotalItems, MoreItems...>::print(os, t, idx);
                }
            }
        }
    };
    template <class... Items>
    class PrintHelper<std::variant<Items...>> {
    public:
        static void print(std::ostream &os, std::variant<Items...> const &t) {
            os << "variant(" << t.index() << "){";
            VariantPrintHelper<std::variant<Items...>,0,sizeof...(Items),Items...>::print(os, t, t.index());
            os << '}';
        }
    };

    template <>
    class PrintHelper<ByteData> {
    public:
        static void print(std::ostream &os, ByteData const &d) {
            os << "bytedata[";
            for (size_t ii=0; ii<d.content.size() && ii<5; ++ii) {
                if (ii > 0) {
                    os << ',';
                }
                os << "0x" << std::hex << std::setw(2)
                    << std::setfill('0') << static_cast<uint16_t>(static_cast<uint8_t>(d.content[ii]))
                    << std::dec;
            }
            if (d.content.size() > 5) {
                os << ",...";
            }
            os << "](" << d.content.size() << " bytes)";
        }
    };
    template <>
    class PrintHelper<ByteDataView> {
    public:
        static void print(std::ostream &os, ByteDataView const &d) {
            os << "bytedata[";
            for (size_t ii=0; ii<d.content.size() && ii<5; ++ii) {
                if (ii > 0) {
                    os << ',';
                }
                os << "0x" << std::hex << std::setw(2)
                    << std::setfill('0') << static_cast<uint16_t>(static_cast<uint8_t>(d.content[ii]))
                    << std::dec;
            }
            if (d.content.size() > 5) {
                os << ",...";
            }
            os << "](" << d.content.size() << " bytes)";
        }
    };
    template <>
    class PrintHelper<ByteDataWithTopic> {
    public:
        static void print(std::ostream &os, ByteDataWithTopic const &d) {
            os << "ByteDataWithTopic{topic='" << d.topic << "',content=bytedata[";
            for (size_t ii=0; ii<d.content.size() && ii<5; ++ii) {
                if (ii > 0) {
                    os << ',';
                }
                os << "0x" << std::hex << std::setw(2)
                    << std::setfill('0') << static_cast<uint16_t>(static_cast<uint8_t>(d.content[ii]))
                    << std::dec;
            }
            if (d.content.size() > 5) {
                os << ",...";
            }
            os << "](" << d.content.size() << " bytes)}";
        }
    };
    template <>
    class PrintHelper<ByteDataWithID> {
    public:
        static void print(std::ostream &os, ByteDataWithID const &d) {
            os << "ByteDataWithID{id='" << d.id << "',content=bytedata[";
            for (size_t ii=0; ii<d.content.size() && ii<5; ++ii) {
                if (ii > 0) {
                    os << ',';
                }
                os << "0x" << std::hex << std::setw(2)
                    << std::setfill('0') << static_cast<uint16_t>(static_cast<uint8_t>(d.content[ii]))
                    << std::dec;
            }
            if (d.content.size() > 5) {
                os << ",...";
            }
            os << "](" << d.content.size() << " bytes)}";
        }
    };
    template <class T>
    class PrintHelper<TypedDataWithTopic<T>> {
    public:
        static void print(std::ostream &os, TypedDataWithTopic<T> const &d) {
            os << "TypedDataWithTopic{topic='" << d.topic << "',content=";
            PrintHelper<T>::print(os, d.content);
            os << '}';
        }
    };
    template <size_t N>
    class PrintHelper<std::array<char,N>> {
    public:
        static void print(std::ostream &os, std::array<char,N> const &t) {
            bool isString = true;
            bool hasEnd = false;
            for (char c : t) {
                if (c == '\0') {
                    hasEnd = true;
                } else if (!std::isprint(static_cast<unsigned char>(c))) {
                    isString = false;
                    break;
                }
            }
            if (isString) {
                if (hasEnd) {
                    PrintHelper<std::string_view>::print(os, std::string_view {t.data()});
                } else {
                    PrintHelper<std::string_view>::print(os, std::string_view {t.data(), N});
                }
            } else {
                PrintHelper<ByteDataView>::print(os, ByteDataView {std::string_view {t.data(), N}});
            }
        }
    };
}}}}
#endif