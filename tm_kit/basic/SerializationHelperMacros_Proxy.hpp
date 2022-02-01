#ifndef SERIALIZATION_HELPER_MACROS_PROXY_HPP_
#define SERIALIZATION_HELPER_MACROS_PROXY_HPP_

#include <tm_kit/basic/SerializationHelperMacros.hpp>
#include <tm_kit/basic/ConvertibleWithString.hpp>
#include <tm_kit/basic/EncodableThroughProxy.hpp>

#define TM_BASIC_CBOR_ENCDEC_THROUGH_STRING(name) \
    namespace dev { namespace cd606 { namespace tm { namespace basic { namespace bytedata_utils { \
        template <> \
        struct RunCBORSerializer<name, void> { \
            static std::string apply(name const &x) { \
                std::string s; \
                s.resize(calculateSize(x)); \
                apply(x, const_cast<char *>(s.data())); \
                return s; \
            } \
            static std::size_t apply(name const &x, char *output) { \
                return RunCBORSerializer<std::string>::apply( \
                    dev::cd606::tm::basic::ConvertibleWithString<name>::toString(x) \
                    , output \
                ); \
            } \
            static std::size_t calculateSize(name const &x) { \
                return RunCBORSerializer<std::string>::calculateSize( \
                    dev::cd606::tm::basic::ConvertibleWithString<name>::toString(x) \
                ); \
            } \
        }; \
        template <> \
        struct RunSerializer<name, void> { \
            static std::string apply(name const &x) { \
                return RunCBORSerializer<name, void>::apply(x); \
            } \
        }; \
        template <> \
        struct RunCBORDeserializer<name, void> { \
            static std::optional<std::tuple<name, size_t>> apply(std::string_view const &s, size_t start) { \
                auto t = RunCBORDeserializer<std::string>::apply(s, start); \
                if (!t) {\
                    return std::nullopt; \
                } \
                return std::tuple<name, size_t> { \
                    dev::cd606::tm::basic::ConvertibleWithString<name>::fromString(std::get<0>(*t)) \
                    , std::get<1>(*t) \
                }; \
            } \
            static std::optional<size_t> applyInPlace(name &x, std::string_view const &s, size_t start) { \
                std::string y; \
                auto res = RunCBORDeserializer<std::string>::applyInPlace(y, s, start); \
                if (!res) { \
                    return std::nullopt; \
                } \
                x = dev::cd606::tm::basic::ConvertibleWithString<name>::fromString(std::string_view {y}); \
                return res; \
            } \
        }; \
        template <> \
        struct RunDeserializer<name, void> { \
            static std::optional<name> apply(std::string_view const &s) { \
                auto t = RunDeserializer<CBOR<name>, void>::apply(s); \
                if (!t) {\
                    return std::nullopt; \
                } \
                return std::move(t->value); \
            } \
            static std::optional<name> apply(std::string const &s) { \
                return apply(std::string_view {s}); \
            } \
            static bool applyInPlace(name &output, std::string_view const &s) { \
                auto t = RunCBORDeserializer<name, void>::applyInPlace(output, s, 0); \
                if (!t) { \
                    return false; \
                } \
                if (*t != s.length()) { \
                    return false; \
                } \
                return true; \
            } \
            static bool applyInPlace(name &output, std::string const &s) { \
                return applyInPlace(output, std::string_view {s}); \
            } \
        }; \
    } } } } }
#define TM_BASIC_CBOR_TEMPLATE_ENCDEC_THROUGH_STRING(templateParams, name) \
    namespace dev { namespace cd606 { namespace tm { namespace basic { namespace bytedata_utils { \
        template <TM_BASIC_CBOR_CAPABLE_TEMPLATE_STRUCT_TEMPLATE_DEF_LIST(templateParams)> \
        struct RunCBORSerializer<name<TM_BASIC_CBOR_CAPABLE_TEMPLATE_STRUCT_TEMPLATE_USE_LIST(templateParams)>, void> { \
            static std::string apply(name<TM_BASIC_CBOR_CAPABLE_TEMPLATE_STRUCT_TEMPLATE_USE_LIST(templateParams)> const &x) { \
                std::string s; \
                s.resize(calculateSize(x)); \
                apply(x, const_cast<char *>(s.data())); \
                return s; \
            } \
            static std::size_t apply(name<TM_BASIC_CBOR_CAPABLE_TEMPLATE_STRUCT_TEMPLATE_USE_LIST(templateParams)> const &x, char *output) { \
                return RunCBORSerializer<std::string>::apply( \
                    dev::cd606::tm::basic::ConvertibleWithString<name<TM_BASIC_CBOR_CAPABLE_TEMPLATE_STRUCT_TEMPLATE_USE_LIST(templateParams)>>::toString(x) \
                    , output \
                ); \
            } \
            static std::size_t calculateSize(name<TM_BASIC_CBOR_CAPABLE_TEMPLATE_STRUCT_TEMPLATE_USE_LIST(templateParams)> const &x) { \
                return RunCBORSerializer<std::string>::calculateSize( \
                    dev::cd606::tm::basic::ConvertibleWithString<name<TM_BASIC_CBOR_CAPABLE_TEMPLATE_STRUCT_TEMPLATE_USE_LIST(templateParams)>>::toString(x) \
                ); \
            } \
        }; \
        template <TM_BASIC_CBOR_CAPABLE_TEMPLATE_STRUCT_TEMPLATE_DEF_LIST(templateParams)> \
        struct RunSerializer<name<TM_BASIC_CBOR_CAPABLE_TEMPLATE_STRUCT_TEMPLATE_USE_LIST(templateParams)>, void> { \
            static std::string apply(name<TM_BASIC_CBOR_CAPABLE_TEMPLATE_STRUCT_TEMPLATE_USE_LIST(templateParams)> const &x) { \
                return RunCBORSerializer<name<TM_BASIC_CBOR_CAPABLE_TEMPLATE_STRUCT_TEMPLATE_USE_LIST(templateParams)>, void>::apply(x); \
            } \
        }; \
        template <TM_BASIC_CBOR_CAPABLE_TEMPLATE_STRUCT_TEMPLATE_DEF_LIST(templateParams)> \
        struct RunCBORDeserializer<name<TM_BASIC_CBOR_CAPABLE_TEMPLATE_STRUCT_TEMPLATE_USE_LIST(templateParams)>, void> { \
            static std::optional<std::tuple<name<TM_BASIC_CBOR_CAPABLE_TEMPLATE_STRUCT_TEMPLATE_USE_LIST(templateParams)>, size_t>> apply(std::string_view const &s, size_t start) { \
                auto t = RunCBORDeserializer<std::string>::apply(s, start); \
                if (!t) {\
                    return std::nullopt; \
                } \
                return std::tuple<name<TM_BASIC_CBOR_CAPABLE_TEMPLATE_STRUCT_TEMPLATE_USE_LIST(templateParams)>, size_t> { \
                    dev::cd606::tm::basic::ConvertibleWithString<name<TM_BASIC_CBOR_CAPABLE_TEMPLATE_STRUCT_TEMPLATE_USE_LIST(templateParams)>>::fromString(std::get<0>(*t)) \
                    , std::get<1>(*t) \
                }; \
            } \
            static std::optional<size_t> applyInPlace(name<TM_BASIC_CBOR_CAPABLE_TEMPLATE_STRUCT_TEMPLATE_USE_LIST(templateParams)> &x, std::string_view const &s, size_t start) { \
                std::string y; \
                auto res = RunCBORDeserializer<std::string>::applyInPlace(y, s, start); \
                if (!res) { \
                    return std::nullopt; \
                } \
                x = dev::cd606::tm::basic::ConvertibleWithString<name<TM_BASIC_CBOR_CAPABLE_TEMPLATE_STRUCT_TEMPLATE_USE_LIST(templateParams)>>::fromString(std::string_view {y}); \
                return res; \
            } \
        }; \
        template <TM_BASIC_CBOR_CAPABLE_TEMPLATE_STRUCT_TEMPLATE_DEF_LIST(templateParams)> \
        struct RunDeserializer<name<TM_BASIC_CBOR_CAPABLE_TEMPLATE_STRUCT_TEMPLATE_USE_LIST(templateParams)>, void> { \
            static std::optional<name<TM_BASIC_CBOR_CAPABLE_TEMPLATE_STRUCT_TEMPLATE_USE_LIST(templateParams)>> apply(std::string_view const &s) { \
                auto t = RunDeserializer<CBOR<name<TM_BASIC_CBOR_CAPABLE_TEMPLATE_STRUCT_TEMPLATE_USE_LIST(templateParams)>>, void>::apply(s); \
                if (!t) {\
                    return std::nullopt; \
                } \
                return std::move(t->value); \
            } \
            static std::optional<name<TM_BASIC_CBOR_CAPABLE_TEMPLATE_STRUCT_TEMPLATE_USE_LIST(templateParams)>> apply(std::string const &s) { \
                return apply(std::string_view {s}); \
            } \
            static bool applyInPlace(name<TM_BASIC_CBOR_CAPABLE_TEMPLATE_STRUCT_TEMPLATE_USE_LIST(templateParams)> &output, std::string_view const &s) { \
                auto t = RunCBORDeserializer<name<TM_BASIC_CBOR_CAPABLE_TEMPLATE_STRUCT_TEMPLATE_USE_LIST(templateParams)>, void>::applyInPlace(output, s, 0); \
                if (!t) { \
                    return false; \
                } \
                if (*t != s.length()) { \
                    return false; \
                } \
                return true; \
            } \
            static bool applyInPlace(name<TM_BASIC_CBOR_CAPABLE_TEMPLATE_STRUCT_TEMPLATE_USE_LIST(templateParams)> &output, std::string const &s) { \
                return applyInPlace(output, std::string_view {s}); \
            } \
        }; \
    } } } } }

#define TM_BASIC_PRINT_HELPER_THROUGH_STRING(name) \
    namespace dev { namespace cd606 { namespace tm { namespace basic { \
        template <> \
        class PrintHelper<name> { \
        public: \
            static void print(std::ostream &os, name const &t) { \
                os << ConvertibleWithString<name>::toString(t); \
            } \
        }; \
    } } } }
#define TM_BASIC_TEMPLATE_PRINT_HELPER_THROUGH_STRING(templateParams, name) \
    namespace dev { namespace cd606 { namespace tm { namespace basic { \
        template <TM_BASIC_CBOR_CAPABLE_TEMPLATE_STRUCT_TEMPLATE_DEF_LIST(templateParams)> \
        class PrintHelper<name<TM_BASIC_CBOR_CAPABLE_TEMPLATE_STRUCT_TEMPLATE_USE_LIST(templateParams)>> { \
        public: \
            static void print(std::ostream &os, name<TM_BASIC_CBOR_CAPABLE_TEMPLATE_STRUCT_TEMPLATE_USE_LIST(templateParams)> const &t) { \
                os << ConvertibleWithString<name<TM_BASIC_CBOR_CAPABLE_TEMPLATE_STRUCT_TEMPLATE_USE_LIST(templateParams)>>::toString(t); \
            } \
        }; \
    } } } }
    
#define TM_BASIC_CBOR_ENCDEC_THROUGH_PROXY(name) \
    namespace dev { namespace cd606 { namespace tm { namespace basic { namespace bytedata_utils { \
        template <> \
        struct RunCBORSerializer<name, void> { \
            static std::string apply(name const &x) { \
                std::string s; \
                s.resize(calculateSize(x)); \
                apply(x, const_cast<char *>(s.data())); \
                return s; \
            } \
            static std::size_t apply(name const &x, char *output) { \
                return RunCBORSerializer<typename dev::cd606::tm::basic::EncodableThroughProxy<name>::EncodeProxyType>::apply( \
                    dev::cd606::tm::basic::EncodableThroughProxy<name>::toProxy(x) \
                    , output \
                ); \
            } \
            static std::size_t calculateSize(name const &x) { \
                return RunCBORSerializer<typename dev::cd606::tm::basic::EncodableThroughProxy<name>::EncodeProxyType>::calculateSize( \
                    dev::cd606::tm::basic::EncodableThroughProxy<name>::toProxy(x) \
                ); \
            } \
        }; \
        template <> \
        struct RunSerializer<name, void> { \
            static std::string apply(name const &x) { \
                return RunCBORSerializer<name, void>::apply(x); \
            } \
        }; \
        template <> \
        struct RunCBORDeserializer<name, void> { \
            static std::optional<std::tuple<name, size_t>> apply(std::string_view const &s, size_t start) { \
                auto t = RunCBORDeserializer<typename dev::cd606::tm::basic::EncodableThroughProxy<name>::DecodeProxyType>::apply(s, start); \
                if (!t) {\
                    return std::nullopt; \
                } \
                return std::tuple<name, size_t> { \
                    dev::cd606::tm::basic::EncodableThroughProxy<name>::fromProxy(std::get<0>(*t)) \
                    , std::get<1>(*t) \
                }; \
            } \
            static std::optional<size_t> applyInPlace(name &x, std::string_view const &s, size_t start) { \
                typename dev::cd606::tm::basic::EncodableThroughProxy<name>::DecodeProxyType y; \
                auto res = RunCBORDeserializer<typename dev::cd606::tm::basic::EncodableThroughProxy<name>::DecodeProxyType>::applyInPlace(y, s, start); \
                if (!res) { \
                    return std::nullopt; \
                } \
                x = dev::cd606::tm::basic::EncodableThroughProxy<name>::fromProxy(y); \
                return res; \
            } \
        }; \
        template <> \
        struct RunDeserializer<name, void> { \
            static std::optional<name> apply(std::string_view const &s) { \
                auto t = RunDeserializer<CBOR<name>, void>::apply(s); \
                if (!t) {\
                    return std::nullopt; \
                } \
                return std::move(t->value); \
            } \
            static std::optional<name> apply(std::string const &s) { \
                return apply(std::string_view {s}); \
            } \
            static bool applyInPlace(name &output, std::string_view const &s) { \
                auto t = RunCBORDeserializer<name, void>::applyInPlace(output, s, 0); \
                if (!t) { \
                    return false; \
                } \
                if (*t != s.length()) { \
                    return false; \
                } \
                return true; \
            } \
            static bool applyInPlace(name &output, std::string const &s) { \
                return applyInPlace(output, std::string_view {s}); \
            } \
        }; \
    } } } } }
#define TM_BASIC_CBOR_TEMPLATE_ENCDEC_THROUGH_PROXY(templateParams, name) \
    namespace dev { namespace cd606 { namespace tm { namespace basic { namespace bytedata_utils { \
        template <TM_BASIC_CBOR_CAPABLE_TEMPLATE_STRUCT_TEMPLATE_DEF_LIST(templateParams)> \
        struct RunCBORSerializer<name<TM_BASIC_CBOR_CAPABLE_TEMPLATE_STRUCT_TEMPLATE_USE_LIST(templateParams)>, void> { \
            static std::string apply(name<TM_BASIC_CBOR_CAPABLE_TEMPLATE_STRUCT_TEMPLATE_USE_LIST(templateParams)> const &x) { \
                std::string s; \
                s.resize(calculateSize(x)); \
                apply(x, const_cast<char *>(s.data())); \
                return s; \
            } \
            static std::size_t apply(name<TM_BASIC_CBOR_CAPABLE_TEMPLATE_STRUCT_TEMPLATE_USE_LIST(templateParams)> const &x, char *output) { \
                return RunCBORSerializer<typename dev::cd606::tm::basic::EncodableThroughProxy<name<TM_BASIC_CBOR_CAPABLE_TEMPLATE_STRUCT_TEMPLATE_USE_LIST(templateParams)>>::EncodeProxyType>::apply( \
                    dev::cd606::tm::basic::EncodableThroughProxy<name<TM_BASIC_CBOR_CAPABLE_TEMPLATE_STRUCT_TEMPLATE_USE_LIST(templateParams)>>::toProxy(x) \
                    , output \
                ); \
            } \
            static std::size_t calculateSize(name const &x) { \
                return RunCBORSerializer<typename dev::cd606::tm::basic::EncodableThroughProxy<name<TM_BASIC_CBOR_CAPABLE_TEMPLATE_STRUCT_TEMPLATE_USE_LIST(templateParams)>>::EncodeProxyType>::calculateSize( \
                    dev::cd606::tm::basic::EncodableThroughProxy<name<TM_BASIC_CBOR_CAPABLE_TEMPLATE_STRUCT_TEMPLATE_USE_LIST(templateParams)>>::toProxy(x) \
                ); \
            } \
        }; \
        template <TM_BASIC_CBOR_CAPABLE_TEMPLATE_STRUCT_TEMPLATE_DEF_LIST(templateParams)> \
        struct RunSerializer<name<TM_BASIC_CBOR_CAPABLE_TEMPLATE_STRUCT_TEMPLATE_USE_LIST(templateParams)>, void> { \
            static std::string apply(name<TM_BASIC_CBOR_CAPABLE_TEMPLATE_STRUCT_TEMPLATE_USE_LIST(templateParams)> const &x) { \
                return RunCBORSerializer<name<TM_BASIC_CBOR_CAPABLE_TEMPLATE_STRUCT_TEMPLATE_USE_LIST(templateParams)>, void>::apply(x); \
            } \
        }; \
        template <TM_BASIC_CBOR_CAPABLE_TEMPLATE_STRUCT_TEMPLATE_DEF_LIST(templateParams)> \
        struct RunCBORDeserializer<name<TM_BASIC_CBOR_CAPABLE_TEMPLATE_STRUCT_TEMPLATE_USE_LIST(templateParams)>, void> { \
            static std::optional<std::tuple<name<TM_BASIC_CBOR_CAPABLE_TEMPLATE_STRUCT_TEMPLATE_USE_LIST(templateParams)>, size_t>> apply(std::string_view const &s, size_t start) { \
                auto t = RunCBORDeserializer<typename dev::cd606::tm::basic::EncodableThroughProxy<name<TM_BASIC_CBOR_CAPABLE_TEMPLATE_STRUCT_TEMPLATE_USE_LIST(templateParams)>>::DecodeProxyType>::apply(s, start); \
                if (!t) {\
                    return std::nullopt; \
                } \
                return std::tuple<name<TM_BASIC_CBOR_CAPABLE_TEMPLATE_STRUCT_TEMPLATE_USE_LIST(templateParams)>, size_t> { \
                    dev::cd606::tm::basic::EncodableThroughProxy<name<TM_BASIC_CBOR_CAPABLE_TEMPLATE_STRUCT_TEMPLATE_USE_LIST(templateParams)>>::fromProxy(std::get<0>(*t)) \
                    , std::get<1>(*t) \
                }; \
            } \
            static std::optional<size_t> applyInPlace(name<TM_BASIC_CBOR_CAPABLE_TEMPLATE_STRUCT_TEMPLATE_USE_LIST(templateParams)> &x, std::string_view const &s, size_t start) { \
                typename dev::cd606::tm::basic::EncodableThroughProxy<name<TM_BASIC_CBOR_CAPABLE_TEMPLATE_STRUCT_TEMPLATE_USE_LIST(templateParams)>>::DecodeProxyType y; \
                auto res = RunCBORDeserializer<typename dev::cd606::tm::basic::EncodableThroughProxy<name<TM_BASIC_CBOR_CAPABLE_TEMPLATE_STRUCT_TEMPLATE_USE_LIST(templateParams)>>::DecodeProxyType>::applyInPlace(y, s, start); \
                if (!res) { \
                    return std::nullopt; \
                } \
                x = dev::cd606::tm::basic::EncodableThroughProxy<name<TM_BASIC_CBOR_CAPABLE_TEMPLATE_STRUCT_TEMPLATE_USE_LIST(templateParams)>>::fromProxy(y); \
                return res; \
            } \
        }; \
        template <TM_BASIC_CBOR_CAPABLE_TEMPLATE_STRUCT_TEMPLATE_DEF_LIST(templateParams)> \
        struct RunDeserializer<name, void> { \
            static std::optional<name> apply(std::string_view const &s) { \
                auto t = RunDeserializer<CBOR<name>, void>::apply(s); \
                if (!t) {\
                    return std::nullopt; \
                } \
                return std::move(t->value); \
            } \
            static std::optional<name> apply(std::string const &s) { \
                return apply(std::string_view {s}); \
            } \
            static bool applyInPlace(name &output, std::string_view const &s) { \
                auto t = RunCBORDeserializer<name, void>::applyInPlace(output, s, 0); \
                if (!t) { \
                    return false; \
                } \
                if (*t != s.length()) { \
                    return false; \
                } \
                return true; \
            } \
            static bool applyInPlace(name &output, std::string const &s) { \
                return applyInPlace(output, std::string_view {s}); \
            } \
        }; \
    } } } } }

#endif
