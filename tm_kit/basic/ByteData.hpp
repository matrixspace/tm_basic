#ifndef TM_KIT_BASIC_BYTE_DATA_HPP_
#define TM_KIT_BASIC_BYTE_DATA_HPP_

#include <string>
#include <vector>
#include <type_traits>
#include <memory>
#include <iostream>
#include <sstream>
#include <optional>
#include <tm_kit/basic/ConstGenerator.hpp>
#include <tm_kit/infra/WithTimeData.hpp>
#include <boost/endian/conversion.hpp>

namespace dev { namespace cd606 { namespace tm { namespace basic { 

    struct ByteData {
        std::string content;
    };

    struct ByteDataWithTopic {
        std::string topic;
        std::string content;
    };

    struct ByteDataWithID {
        std::string id; //When we reach the transport level, all IDs are already converted to string representations
        std::string content;       
    };

    template <class T>
    struct TypedDataWithTopic {
        using ContentType = T;
        std::string topic;
        T content;
    };

    namespace bytedata_utils {
        template <class T, typename Enable=void>
        struct RunSerializer {
            static std::string apply(T const &data) {
                std::string s;
                data.SerializeToString(&s);
                return s;
            }
        };
        template <>
        struct RunSerializer<std::string, void> {
            static std::string apply(std::string const &data) {
                return data;
            }
        };
        template <>
        struct RunSerializer<ByteData, void> {
            static std::string apply(ByteData const &data) {
                return data.content;
            }
        };
        template <class T>
        struct RunSerializer<T, std::enable_if_t<std::is_arithmetic_v<T>,void>> {
            static std::string apply(T const &data) {
                T littleEndianVersion = boost::endian::native_to_little<T>(data);
                char *p = reinterpret_cast<char *>(&littleEndianVersion);
                return std::string {p, p+sizeof(T)};
            }
        };
        template <class A>
        struct RunSerializer<std::unique_ptr<A>, void> {
            static std::string apply(std::unique_ptr<A> const &data) {
                if (data) {
                    return RunSerializer<A>::apply(*data);
                } else {
                    return "";
                }
            }
        };
        template <class A>
        struct RunSerializer<std::shared_ptr<A>, void> {
            static std::string apply(std::shared_ptr<A> const &data) {
                if (data) {
                    return RunSerializer<A>::apply(*data);
                } else {
                    return "";
                }
            }
        };
        template <class A>
        struct RunSerializer<std::vector<A>, void> {
            static std::string apply(std::vector<A> const &data) {
                int32_t len = data.size();
                std::ostringstream oss;
                oss << RunSerializer<int32_t>::apply(len);
                for (auto const &item : data) {
                    auto s = RunSerializer<A>::apply(item);
                    int32_t sLen = s.length();
                    oss << RunSerializer<int32_t>::apply(sLen)
                        << s;
                }
                return oss.str();
            }
        };

        template <class T, typename Enable=void>
        struct RunDeserializer {
            static std::optional<T> apply(std::string const &data) {
                T t;
                if (t.ParseFromString(data)) {
                    return t;
                } else {
                    return std::nullopt;
                }
            }
        };
        template <>
        struct RunDeserializer<std::string, void> {
            static std::optional<std::string> apply(std::string const &data) {
                return {data};
            }
        };
        template <>
        struct RunDeserializer<ByteData, void> {
            static std::optional<ByteData> apply(std::string const &data) {
                return {ByteData {data}};
            }
        };
        template <class T>
        struct RunDeserializer<T, std::enable_if_t<std::is_arithmetic_v<T>,void>> {
            static std::optional<T> apply(std::string const &data) {
                if (data.length() != sizeof(T)) {
                    return std::nullopt;
                }
                T littleEndianVersion;
                std::memcpy(&littleEndianVersion, data.c_str(), sizeof(T));
                return {boost::endian::little_to_native<T>(littleEndianVersion)};
            }
        };
        template <class A>
        struct RunDeserializer<std::unique_ptr<A>, void> {
            static std::optional<std::unique_ptr<A>> apply(std::string const &data) {
                if (data.length() == 0) {
                    return std::nullopt;
                }
                auto a = RunDeserializer<A>::apply(data);
                if (a) {
                    return {std::make_unique<A>(std::move(a))};
                } else {
                    return std::nullopt;
                }
            }
        };
        template <class A>
        struct RunDeserializer<std::shared_ptr<A>, void> {
            static std::optional<std::shared_ptr<A>> apply(std::string const &data) {
                if (data.length() == 0) {
                    return std::nullopt;
                }
                auto a = RunDeserializer<A>::apply(data);
                if (a) {
                    return {std::make_shared<A>(std::move(a))};
                } else {
                    return std::nullopt;
                }
            }
        };
        template <class A>
        struct RunDeserializer<std::vector<A>, void> {
            static std::optional<std::vector<A>> apply(std::string const &data) {
                const char *p = data.c_str();
                size_t len = data.length();
                if (len < sizeof(int32_t)) {
                    return std::nullopt;
                }
                auto resLen = RunDeserializer<int32_t>::apply(std::string(p, p+sizeof(int32_t)));
                if (!resLen) {
                    return std::nullopt;
                }
                p += sizeof(int32_t);
                len -= sizeof(int32_t);
                std::vector<A> res;
                res.resize(*resLen);
                for (int32_t ii=0; ii<*resLen; ++ii) {
                    if (len < sizeof(int32_t)) {
                        return std::nullopt;
                    }
                    auto itemLen = RunDeserializer<int32_t>::apply(std::string(p, p+sizeof(int32_t)));
                    if (!itemLen) {
                        return std::nullopt;
                    }
                    p += sizeof(int32_t);
                    len -= sizeof(int32_t);
                    if (len < *itemLen) {
                        return std::nullopt;
                    }
                    auto item = RunDeserializer<A>::apply(std::string(p, p+(*itemLen)));
                    if (!item) {
                        return std::nullopt;
                    }
                    res[ii] = std::move(item);
                    p += *itemLen;
                    len -= *itemLen;
                }
                if (len != 0) {
                    return std::nullopt;
                }
                return {std::move(res)};
            }
        };

        #include <tm_kit/basic/ByteData_Tuple_Variant_Piece.hpp>

        template <class VersionType, class DataType, class Cmp>
        struct RunSerializer<infra::VersionedData<VersionType,DataType,Cmp>> {
            static std::string apply(infra::VersionedData<VersionType,DataType,Cmp> const &data) {
                std::string v = RunSerializer<VersionType>::apply(data.version);
                std::string d = RunSerializer<DataType>::apply(data.data);
                int32_t vLen = v.length();
                std::ostringstream oss;
                oss << RunSerializer<int32_t>::apply(vLen)
                    << v 
                    << d;
                return oss.str();
            }
        };
        template <class GroupIDType, class VersionType, class DataType, class Cmp>
        struct RunSerializer<infra::GroupedVersionedData<GroupIDType,VersionType,DataType,Cmp>> {
            static std::string apply(infra::GroupedVersionedData<GroupIDType,VersionType,DataType,Cmp> const &data) {
                std::string g = RunSerializer<GroupIDType>::apply(data.groupID);
                std::string v = RunSerializer<VersionType>::apply(data.version);
                std::string d = RunSerializer<DataType>::apply(data.data);
                int32_t gLen = g.length();
                int32_t vLen = v.length();
                std::ostringstream oss;
                oss << RunSerializer<int32_t>::apply(gLen)
                    << g
                    << RunSerializer<int32_t>::apply(vLen)
                    << v 
                    << d;
                return oss.str();
            }
        };
        template <class VersionType, class DataType, class Cmp>
        struct RunDeserializer<infra::VersionedData<VersionType,DataType,Cmp>, void> {
            static std::optional<infra::VersionedData<VersionType,DataType,Cmp>> apply(std::string const &data) {
                auto x = RunDeserializer<std::tuple<VersionType,DataType>>::apply(data);
                if (!x) {
                    return std::nullopt;
                }
                return infra::VersionedData<VersionType,DataType,Cmp> {
                    std::move(std::get<0>(x)), std::move(std::get<1>(x))
                };
            }
        };
        template <class GroupIDType, class VersionType, class DataType, class Cmp>
        struct RunDeserializer<infra::GroupedVersionedData<GroupIDType,VersionType,DataType,Cmp>, void> {
            static std::optional<infra::GroupedVersionedData<GroupIDType,VersionType,DataType,Cmp>> apply(std::string const &data) {
                const char *p = data.c_str();
                size_t len = data.length();
                if (len < sizeof(int32_t)) {
                    return std::nullopt;
                }
                auto gLen = RunDeserializer<int32_t>::apply(std::string(p, p+sizeof(int32_t)));
                if (!gLen) {
                    return std::nullopt;
                }
                p += sizeof(int32_t);
                len -= sizeof(int32_t);
                if (len < *gLen) {
                    return std::nullopt;
                }
                auto g = RunDeserializer<GroupIDType>::apply(std::string(p, p+(*gLen)));
                if (!g) {
                    return std::nullopt;
                }
                p += *gLen;
                len -= *gLen;
                if (len < sizeof(int32_t)) {
                    return std::nullopt;
                }
                auto vLen = RunDeserializer<int32_t>::apply(std::string(p, p+sizeof(int32_t)));
                if (!vLen) {
                    return std::nullopt;
                }
                p += sizeof(int32_t);
                len -= sizeof(int32_t);
                if (len < *vLen) {
                    return std::nullopt;
                }
                auto v = RunDeserializer<VersionType>::apply(std::string(p, p+(*vLen)));
                if (!v) {
                    return std::nullopt;
                }
                p += *vLen;
                len -= *vLen;
                auto d = RunDeserializer<DataType>::apply(std::string(p, p+len));
                if (!d) {
                    return std::nullopt;
                }
                return {
                    infra::GroupedVersionedData<GroupIDType,VersionType,DataType,Cmp> {
                        std::move(*g)
                        , std::move(*v)
                        , std::move(*d)
                    }
                };
            }
        };
    }

    template <class M>
    class SerializationActions {
    public:
        template <class T>
        inline static std::string serializeFunc(T const &t) {
            return bytedata_utils::RunSerializer<T>::apply(t);
        }
        template <class T>
        inline static std::optional<T> deserializeFunc(std::string const &data) {
            return bytedata_utils::RunDeserializer<T>::apply(data);
        }
        template <class T, class F>
        static std::shared_ptr<typename M::template Action<T, TypedDataWithTopic<T>>> addTopic(F &&f) {
            return M::template liftPure<T>(
                [f=std::move(f)](T &&data) -> TypedDataWithTopic<T> {
                    return {f(data), std::move(data)};
                }
            );
        }
        template <class T>
        static std::shared_ptr<typename M::template Action<T, TypedDataWithTopic<T>>> addConstTopic(std::string const &s) {
            return addTopic<T>(ConstGenerator<std::string, T>(s));
        }

        template <class T>
        static std::shared_ptr<typename M::template Action<TypedDataWithTopic<T>, ByteDataWithTopic>> serialize() {
            return M::template liftPure<TypedDataWithTopic<T>>(
                [](TypedDataWithTopic<T> &&data) -> ByteDataWithTopic {
                    return {std::move(data.topic), { bytedata_utils::RunSerializer<T>::apply(data.content) }};
                }
            );
        }
        template <class T>
        static std::shared_ptr<typename M::template Action<typename M::template Key<T>, ByteDataWithID>> serializeWithKey() {
            return M::template liftPure<typename M::template Key<T>>(
                [](typename M::template Key<T> &&data) -> ByteDataWithID {
                    return {M::EnvironmentType::id_to_string(data.id()), { bytedata_utils::RunSerializer<T>::apply(data.key()) }};
                }
            );
        }
        template <class A, class B>
        static std::shared_ptr<typename M::template Action<typename M::template KeyedData<A,B>, ByteDataWithID>> serializeWithKey() {
            return M::template liftPure<typename M::template KeyedData<A,B>>(
                [](typename M::template KeyedData<A,B> &&data) -> ByteDataWithID {
                    return {M::EnvironmentType::id_to_string(data.key.id()), { bytedata_utils::RunSerializer<B>::apply(data.data) }};
                }
            );
        }
        template <class T>
        static std::shared_ptr<typename M::template Action<ByteDataWithTopic, TypedDataWithTopic<T>>> deserialize() {
            return M::template liftMaybe<ByteDataWithTopic>(
                [](ByteDataWithTopic &&data) -> std::optional<TypedDataWithTopic<T>> {
                    std::optional<T> t = bytedata_utils::RunDeserializer<T>::apply(data.content);
                    if (t) {
                        return TypedDataWithTopic<T> {std::move(data.topic), std::move(*t)};
                    } else {
                        return std::nullopt;
                    }
                }
            );
        }
        template <class T>
        static std::shared_ptr<typename M::template Action<ByteDataWithID, typename M::template Key<T>>> deserializeWithKey() {
            return M::template liftMaybe<ByteDataWithID>(
                [](ByteDataWithID &&data) -> std::optional<typename M::template Key<T>> {
                    std::optional<T> t = bytedata_utils::RunDeserializer<T>::apply(data.content);
                    if (t) {
                        return typename M::template Key<T> {M::EnvironmentType::id_from_string(data.id), std::move(*t)};
                    } else {
                        return std::nullopt;
                    }
                }
            );
        }
    };

} } } } 

namespace std {
    inline std::ostream &operator<<(std::ostream &os, dev::cd606::tm::basic::ByteData const &d) {
        os << "ByteData{length=" << d.content.length() << "}";
        return os; 
    }
    inline std::ostream &operator<<(std::ostream &os, dev::cd606::tm::basic::ByteDataWithTopic const &d) {
        os << "ByteDataWithTopic{topic=" << d.topic << ",length=" << d.content.length() << "}";
        return os; 
    }
    inline std::ostream &operator<<(std::ostream &os, dev::cd606::tm::basic::ByteDataWithID const &d) {
        os << "ByteDataWithID{id=" << d.id << ",length=" << d.content.length() << "}";
        return os; 
    }
    template <class T>
    inline std::ostream &operator<<(std::ostream &os, dev::cd606::tm::basic::TypedDataWithTopic<T> const &d) {
        os << "TypedDataWithTopic{topic=" << d.topic << ",content=" << d.content << "}";
        return os; 
    }
};

namespace dev { namespace cd606 { namespace tm { namespace infra { namespace withtime_utils {
    template <class A>
    struct ValueCopier<basic::TypedDataWithTopic<A>> {
        inline static basic::TypedDataWithTopic<A> copy(basic::TypedDataWithTopic<A> const &a) {
            return basic::TypedDataWithTopic<A> {a.topic, ValueCopier<A>::copy(a.content)};
        }
    };
} } } } }

#endif
