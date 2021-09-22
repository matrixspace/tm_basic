#ifndef TM_KIT_BASIC_TRIVIALLY_SERIALIZABLE_HPP_
#define TM_KIT_BASIC_TRIVIALLY_SERIALIZABLE_HPP_

#include <tm_kit/basic/ByteData.hpp>

#include <type_traits>
#include <string>
#include <cstring>

namespace dev { namespace cd606 { namespace tm { namespace basic {
    template <class T, typename = std::enable_if_t<std::is_trivially_copyable_v<T>>>
    struct TriviallySerializable final {
        using value_type = T;
        T value;

        void writeToString(std::string *s) const {
            s->resize(sizeof(T));
            std::memcpy(s->data(), &value, sizeof(T));
        }
        bool fromStringView(std::string_view const &s) {
            if (s.length() != sizeof(T)) {
                return false;
            }
            std::memcpy(&value, s.data(), sizeof(T));
            return true;
        }
    };
    template <class T>
    inline bool operator==(TriviallySerializable<T> const &a, TriviallySerializable<T> const &b) {
        return (a.value == b.value);
    }

    namespace bytedata_utils {
        template <class T>
        struct RunSerializer<TriviallySerializable<T>, void> {
            static std::string apply(TriviallySerializable<T> const &data) {
                std::string s;
                data.writeToString(&s);
                return s;
            }
        };
        template <class T>
        struct RunDeserializer<TriviallySerializable<T>, void> {
            static std::optional<TriviallySerializable<T>> apply(std::string_view const &data) {
                TriviallySerializable<T> t;
                if (t.fromStringView(data)) {
                    return {std::move(t)};
                } else {
                    return std::nullopt;
                }
            }
            static std::optional<TriviallySerializable<T>> apply(std::string const &data) {
                return apply(std::string_view {data});
            }
            static bool applyInPlace(TriviallySerializable<T> &output, std::string_view const &data) {
                return output.fromStringView(data);
            }
            static bool applyInPlace(TriviallySerializable<T> &output, std::string const &data) {
                return output.fromStringView(std::string_view {data});
            }
        };
    }
} } } }



#endif