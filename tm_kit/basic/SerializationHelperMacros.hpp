#ifndef SERIALIZATION_HELPER_MACROS_HPP_
#define SERIALIZATION_HELPER_MACROS_HPP_

#include <boost/preprocessor/stringize.hpp>
#include <boost/preprocessor/tuple/elem.hpp>
#include <boost/preprocessor/seq/for_each.hpp>
#include <boost/preprocessor/punctuation/comma_if.hpp>
#include <boost/preprocessor/arithmetic/sub.hpp>

#include <tm_kit/basic/PrintHelper.hpp>

#define TM_BASIC_CBOR_CAPABLE_STRUCT_ITEM_DEF(r, data, elem) \
    BOOST_PP_TUPLE_ELEM(0,elem) BOOST_PP_TUPLE_ELEM(1,elem);

#define TM_BASIC_CBOR_CAPABLE_STRUCT_DEF(name, content) \
    struct name { \
        BOOST_PP_SEQ_FOR_EACH(TM_BASIC_CBOR_CAPABLE_STRUCT_ITEM_DEF,_,content) \
    };
#define TM_BASIC_CBOR_CAPABLE_TEMPLATE_1_STRUCT_DEF(templateTypeParam, name, content) \
    template <typename templateTypeParam> \
    struct name { \
        BOOST_PP_SEQ_FOR_EACH(TM_BASIC_CBOR_CAPABLE_STRUCT_ITEM_DEF,_,content) \
    };

#define TM_BASIC_CBOR_CAPABLE_STRUCT_PRINT_ITEM(r, data, elem) \
    os << BOOST_PP_STRINGIZE(BOOST_PP_TUPLE_ELEM(1,elem)) << '='; \
    dev::cd606::tm::basic::PrintHelper<BOOST_PP_TUPLE_ELEM(0,elem)>::print(os, x.BOOST_PP_TUPLE_ELEM(1,elem)); \
    os << ' ';    

#define TM_BASIC_CBOR_CAPABLE_STRUCT_EQ_ITEM(r, data, elem) \
    && (x.BOOST_PP_TUPLE_ELEM(1,elem) == y.BOOST_PP_TUPLE_ELEM(1,elem)) 

#define TM_BASIC_CBOR_CAPABLE_STRUCT_PRINT(name, content) \
    inline std::ostream &operator<<(std::ostream &os, name const &x) { \
        os << BOOST_PP_STRINGIZE(name) << '{'; \
        BOOST_PP_SEQ_FOR_EACH(TM_BASIC_CBOR_CAPABLE_STRUCT_PRINT_ITEM,_,content) \
        os << '}'; \
        return os; \
    }
#define TM_BASIC_CBOR_CAPABLE_TEMPLATE_1_STRUCT_PRINT(templateTypeParam, name, content) \
    template <typename templateTypeParam> \
    inline std::ostream &operator<<(std::ostream &os, name<templateTypeParam> const &x) { \
        os << BOOST_PP_STRINGIZE(name) << '{'; \
        BOOST_PP_SEQ_FOR_EACH(TM_BASIC_CBOR_CAPABLE_STRUCT_PRINT_ITEM,_,content) \
        os << '}'; \
        return os; \
    }

#define TM_BASIC_CBOR_CAPABLE_STRUCT_EQ(name, content) \
    inline bool operator==(name const &x, name const &y) { \
        return (true \
            BOOST_PP_SEQ_FOR_EACH(TM_BASIC_CBOR_CAPABLE_STRUCT_EQ_ITEM,_,content) \
            ); \
    }
#define TM_BASIC_CBOR_CAPABLE_TEMPLATE_1_STRUCT_EQ(templateTypeParam, name, content) \
    template <typename templateTypeParam> \
    inline bool operator==(name<templateTypeParam> const &x, name<templateTypeParam> const &y) { \
        return (true \
            BOOST_PP_SEQ_FOR_EACH(TM_BASIC_CBOR_CAPABLE_STRUCT_EQ_ITEM,_,content) \
            ); \
    }

#define TM_BASIC_CBOR_CAPABLE_STRUCT_EXTRACT_TYPE_WITH_CONST_PTR(r, data, elem) \
    BOOST_PP_COMMA_IF(BOOST_PP_SUB(r,2)) \
    BOOST_PP_TUPLE_ELEM(0,elem) const *
#define TM_BASIC_CBOR_CAPABLE_STRUCT_EXTRACT_TYPE(r, data, elem) \
    BOOST_PP_COMMA_IF(BOOST_PP_SUB(r,2)) \
    BOOST_PP_TUPLE_ELEM(0,elem)
#define TM_BASIC_CBOR_CAPABLE_STRUCT_GET_FIELD_PTRS(r, data, elem) \
    BOOST_PP_COMMA_IF(BOOST_PP_SUB(r,2)) \
    &(x.BOOST_PP_TUPLE_ELEM(1,elem))
#define TM_BASIC_CBOR_CAPABLE_STRUCT_GET_FIELD_NAMES(r, data, elem) \
    BOOST_PP_COMMA_IF(BOOST_PP_SUB(r,2)) \
    BOOST_PP_STRINGIZE(BOOST_PP_TUPLE_ELEM(1,elem))
#define TM_BASIC_CBOR_CAPABLE_STRUCT_MOVE_VALUE(r, data, elem) \
    BOOST_PP_COMMA_IF(BOOST_PP_SUB(r,2)) \
    std::move(std::get<BOOST_PP_SUB(r,2)>(std::get<0>(*t)))
#define TM_BASIC_CBOR_CAPABLE_STRUCT_MOVE_VALUE_PLAIN(r, data, elem) \
    BOOST_PP_COMMA_IF(BOOST_PP_SUB(r,2)) \
    std::move(std::get<BOOST_PP_SUB(r,2)>(*t))

#define TM_BASIC_CBOR_CAPABLE_STRUCT_ENCODE(name, content) \
    namespace dev { namespace cd606 { namespace tm { namespace basic { namespace bytedata_utils { \
        template <> \
        struct RunCBORSerializer<name, void> { \
            static std::vector<std::uint8_t> apply(name const &x) { \
                std::tuple< \
                    BOOST_PP_SEQ_FOR_EACH(TM_BASIC_CBOR_CAPABLE_STRUCT_EXTRACT_TYPE_WITH_CONST_PTR,_,content) \
                > y { \
                    BOOST_PP_SEQ_FOR_EACH(TM_BASIC_CBOR_CAPABLE_STRUCT_GET_FIELD_PTRS,_,content) \
                }; \
                return RunCBORSerializerWithNameList<std::tuple< \
                    BOOST_PP_SEQ_FOR_EACH(TM_BASIC_CBOR_CAPABLE_STRUCT_EXTRACT_TYPE_WITH_CONST_PTR,_,content) \
                    >, BOOST_PP_SEQ_SIZE(content) \
                >::apply(y, { \
                    BOOST_PP_SEQ_FOR_EACH(TM_BASIC_CBOR_CAPABLE_STRUCT_GET_FIELD_NAMES,_,content) \
                }); \
            } \
        }; \
        template <> \
        struct RunSerializer<name, void> { \
            static std::string apply(name const &x) { \
                std::tuple< \
                    BOOST_PP_SEQ_FOR_EACH(TM_BASIC_CBOR_CAPABLE_STRUCT_EXTRACT_TYPE_WITH_CONST_PTR,_,content) \
                > y { \
                    BOOST_PP_SEQ_FOR_EACH(TM_BASIC_CBOR_CAPABLE_STRUCT_GET_FIELD_PTRS,_,content) \
                }; \
                return RunSerializer<std::tuple< \
                    BOOST_PP_SEQ_FOR_EACH(TM_BASIC_CBOR_CAPABLE_STRUCT_EXTRACT_TYPE_WITH_CONST_PTR,_,content) \
                >>::apply(y); \
            } \
        }; \
    } } } } }

#define TM_BASIC_CBOR_CAPABLE_STRUCT_ENCODE_NO_FIELD_NAMES(name, content) \
    namespace dev { namespace cd606 { namespace tm { namespace basic { namespace bytedata_utils { \
        template <> \
        struct RunCBORSerializer<name, void> { \
            static std::vector<std::uint8_t> apply(name const &x) { \
                std::tuple< \
                    BOOST_PP_SEQ_FOR_EACH(TM_BASIC_CBOR_CAPABLE_STRUCT_EXTRACT_TYPE_WITH_CONST_PTR,_,content) \
                > y { \
                    BOOST_PP_SEQ_FOR_EACH(TM_BASIC_CBOR_CAPABLE_STRUCT_GET_FIELD_PTRS,_,content) \
                }; \
                return RunCBORSerializer<std::tuple< \
                    BOOST_PP_SEQ_FOR_EACH(TM_BASIC_CBOR_CAPABLE_STRUCT_EXTRACT_TYPE_WITH_CONST_PTR,_,content) \
                    > \
                >::apply(y); \
            } \
        }; \
        template <> \
        struct RunSerializer<name, void> { \
            static std::string apply(name const &x) { \
                std::tuple< \
                    BOOST_PP_SEQ_FOR_EACH(TM_BASIC_CBOR_CAPABLE_STRUCT_EXTRACT_TYPE_WITH_CONST_PTR,_,content) \
                > y { \
                    BOOST_PP_SEQ_FOR_EACH(TM_BASIC_CBOR_CAPABLE_STRUCT_GET_FIELD_PTRS,_,content) \
                }; \
                return RunSerializer<std::tuple< \
                    BOOST_PP_SEQ_FOR_EACH(TM_BASIC_CBOR_CAPABLE_STRUCT_EXTRACT_TYPE_WITH_CONST_PTR,_,content) \
                >>::apply(y); \
            } \
        }; \
    } } } } }

#define TM_BASIC_CBOR_CAPABLE_STRUCT_DECODE(name, content) \
    namespace dev { namespace cd606 { namespace tm { namespace basic { namespace bytedata_utils { \
        template <> \
        struct RunCBORDeserializer<name, void> { \
            static std::optional<std::tuple<name, size_t>> apply(std::string_view const &s, size_t start) { \
                auto t = RunCBORDeserializerWithNameList<std::tuple< \
                    BOOST_PP_SEQ_FOR_EACH(TM_BASIC_CBOR_CAPABLE_STRUCT_EXTRACT_TYPE,_,content) \
                    >, BOOST_PP_SEQ_SIZE(content) \
                >::apply(s, start, { \
                    BOOST_PP_SEQ_FOR_EACH(TM_BASIC_CBOR_CAPABLE_STRUCT_GET_FIELD_NAMES,_,content) \
                }); \
                if (!t) {\
                    return std::nullopt; \
                } \
                return std::tuple<name, size_t> { \
                    name { \
                        BOOST_PP_SEQ_FOR_EACH(TM_BASIC_CBOR_CAPABLE_STRUCT_MOVE_VALUE,_,content) \
                    }, std::get<1>(*t) \
                }; \
            } \
        }; \
        template <> \
        struct RunDeserializer<name, void> { \
            static std::optional<name> apply(std::string const &s) { \
                auto t = RunDeserializer<std::tuple< \
                    BOOST_PP_SEQ_FOR_EACH(TM_BASIC_CBOR_CAPABLE_STRUCT_EXTRACT_TYPE,_,content) \
                    >>::apply(s); \
                if (!t) {\
                    return std::nullopt; \
                } \
                return name { \
                    BOOST_PP_SEQ_FOR_EACH(TM_BASIC_CBOR_CAPABLE_STRUCT_MOVE_VALUE_PLAIN,_,content) \
                }; \
            } \
        }; \
    } } } } }

#define TM_BASIC_CBOR_CAPABLE_STRUCT_DECODE_NO_FIELD_NAMES(name, content) \
    namespace dev { namespace cd606 { namespace tm { namespace basic { namespace bytedata_utils { \
        template <> \
        struct RunCBORDeserializer<name, void> { \
            static std::optional<std::tuple<name, size_t>> apply(std::string_view const &s, size_t start) { \
                auto t = RunCBORDeserializer<std::tuple< \
                    BOOST_PP_SEQ_FOR_EACH(TM_BASIC_CBOR_CAPABLE_STRUCT_EXTRACT_TYPE,_,content) \
                    > \
                >::apply(s, start); \
                if (!t) {\
                    return std::nullopt; \
                } \
                return std::tuple<name, size_t> { \
                    name { \
                        BOOST_PP_SEQ_FOR_EACH(TM_BASIC_CBOR_CAPABLE_STRUCT_MOVE_VALUE,_,content) \
                    }, std::get<1>(*t) \
                }; \
            } \
        }; \
        template <> \
        struct RunDeserializer<name, void> { \
            static std::optional<name> apply(std::string const &s) { \
                auto t = RunDeserializer<std::tuple< \
                    BOOST_PP_SEQ_FOR_EACH(TM_BASIC_CBOR_CAPABLE_STRUCT_EXTRACT_TYPE,_,content) \
                    >>::apply(s); \
                if (!t) {\
                    return std::nullopt; \
                } \
                return name { \
                    BOOST_PP_SEQ_FOR_EACH(TM_BASIC_CBOR_CAPABLE_STRUCT_MOVE_VALUE_PLAIN,_,content) \
                }; \
            } \
        }; \
    } } } } }

#define TM_BASIC_CBOR_CAPABLE_TEMPLATE_1_STRUCT_ENCODE(templateTypeParam, name, content) \
    namespace dev { namespace cd606 { namespace tm { namespace basic { namespace bytedata_utils { \
        template <typename templateTypeParam> \
        struct RunCBORSerializer<name<templateTypeParam>, void> { \
            static std::vector<std::uint8_t> apply(name<templateTypeParam> const &x) { \
                std::tuple< \
                    BOOST_PP_SEQ_FOR_EACH(TM_BASIC_CBOR_CAPABLE_STRUCT_EXTRACT_TYPE_WITH_CONST_PTR,_,content) \
                > y { \
                    BOOST_PP_SEQ_FOR_EACH(TM_BASIC_CBOR_CAPABLE_STRUCT_GET_FIELD_PTRS,_,content) \
                }; \
                return RunCBORSerializerWithNameList<std::tuple< \
                    BOOST_PP_SEQ_FOR_EACH(TM_BASIC_CBOR_CAPABLE_STRUCT_EXTRACT_TYPE_WITH_CONST_PTR,_,content) \
                    >, BOOST_PP_SEQ_SIZE(content) \
                >::apply(y, { \
                    BOOST_PP_SEQ_FOR_EACH(TM_BASIC_CBOR_CAPABLE_STRUCT_GET_FIELD_NAMES,_,content) \
                }); \
            } \
        }; \
        template <typename templateTypeParam> \
        struct RunSerializer<name<templateTypeParam>, void> { \
            static std::string apply(name<templateTypeParam> const &x) { \
                std::tuple< \
                    BOOST_PP_SEQ_FOR_EACH(TM_BASIC_CBOR_CAPABLE_STRUCT_EXTRACT_TYPE_WITH_CONST_PTR,_,content) \
                > y { \
                    BOOST_PP_SEQ_FOR_EACH(TM_BASIC_CBOR_CAPABLE_STRUCT_GET_FIELD_PTRS,_,content) \
                }; \
                return RunSerializer<std::tuple< \
                    BOOST_PP_SEQ_FOR_EACH(TM_BASIC_CBOR_CAPABLE_STRUCT_EXTRACT_TYPE_WITH_CONST_PTR,_,content) \
                >>::apply(y); \
            } \
        }; \
    } } } } }

#define TM_BASIC_CBOR_CAPABLE_TEMPLATE_1_STRUCT_ENCODE_NO_FIELD_NAMES(templateTypeParam, name, content) \
    namespace dev { namespace cd606 { namespace tm { namespace basic { namespace bytedata_utils { \
        template <typename templateTypeParam> \
        struct RunCBORSerializer<name<templateTypeParam>, void> { \
            static std::vector<std::uint8_t> apply(name<templateTypeParam> const &x) { \
                std::tuple< \
                    BOOST_PP_SEQ_FOR_EACH(TM_BASIC_CBOR_CAPABLE_STRUCT_EXTRACT_TYPE_WITH_CONST_PTR,_,content) \
                > y { \
                    BOOST_PP_SEQ_FOR_EACH(TM_BASIC_CBOR_CAPABLE_STRUCT_GET_FIELD_PTRS,_,content) \
                }; \
                return RunCBORSerializer<std::tuple< \
                    BOOST_PP_SEQ_FOR_EACH(TM_BASIC_CBOR_CAPABLE_STRUCT_EXTRACT_TYPE_WITH_CONST_PTR,_,content) \
                    > \
                >::apply(y); \
            } \
        }; \
        template <typename templateTypeParam> \
        struct RunSerializer<name<templateTypeParam>, void> { \
            static std::string apply(name<templateTypeParam> const &x) { \
                std::tuple< \
                    BOOST_PP_SEQ_FOR_EACH(TM_BASIC_CBOR_CAPABLE_STRUCT_EXTRACT_TYPE_WITH_CONST_PTR,_,content) \
                > y { \
                    BOOST_PP_SEQ_FOR_EACH(TM_BASIC_CBOR_CAPABLE_STRUCT_GET_FIELD_PTRS,_,content) \
                }; \
                return RunSerializer<std::tuple< \
                    BOOST_PP_SEQ_FOR_EACH(TM_BASIC_CBOR_CAPABLE_STRUCT_EXTRACT_TYPE_WITH_CONST_PTR,_,content) \
                >>::apply(y); \
            } \
        }; \
    } } } } }

#define TM_BASIC_CBOR_CAPABLE_TEMPLATE_1_STRUCT_DECODE(templateTypeParam, name, content) \
    namespace dev { namespace cd606 { namespace tm { namespace basic { namespace bytedata_utils { \
        template <typename templateTypeParam> \
        struct RunCBORDeserializer<name<templateTypeParam>, void> { \
            static std::optional<std::tuple<name<templateTypeParam>, size_t>> apply(std::string_view const &s, size_t start) { \
                auto t = RunCBORDeserializerWithNameList<std::tuple< \
                    BOOST_PP_SEQ_FOR_EACH(TM_BASIC_CBOR_CAPABLE_STRUCT_EXTRACT_TYPE,_,content) \
                    >, BOOST_PP_SEQ_SIZE(content) \
                >::apply(s, start, { \
                    BOOST_PP_SEQ_FOR_EACH(TM_BASIC_CBOR_CAPABLE_STRUCT_GET_FIELD_NAMES,_,content) \
                }); \
                if (!t) {\
                    return std::nullopt; \
                } \
                return std::tuple<name<templateTypeParam>, size_t> { \
                    name<templateTypeParam> { \
                        BOOST_PP_SEQ_FOR_EACH(TM_BASIC_CBOR_CAPABLE_STRUCT_MOVE_VALUE,_,content) \
                    }, std::get<1>(*t) \
                }; \
            } \
        }; \
        template <typename templateTypeParam> \
        struct RunDeserializer<name<templateTypeParam>, void> { \
            static std::optional<name<templateTypeParam>> apply(std::string const &s) { \
                auto t = RunDeserializer<std::tuple< \
                    BOOST_PP_SEQ_FOR_EACH(TM_BASIC_CBOR_CAPABLE_STRUCT_EXTRACT_TYPE,_,content) \
                    >>::apply(s); \
                if (!t) {\
                    return std::nullopt; \
                } \
                return name<templateTypeParam> { \
                    BOOST_PP_SEQ_FOR_EACH(TM_BASIC_CBOR_CAPABLE_STRUCT_MOVE_VALUE_PLAIN,_,content) \
                }; \
            } \
        }; \
    } } } } }

#define TM_BASIC_CBOR_CAPABLE_TEMPLATE_1_STRUCT_DECODE_NO_FIELD_NAMES(templateTypeParam, name, content) \
    namespace dev { namespace cd606 { namespace tm { namespace basic { namespace bytedata_utils { \
        template <typename templateTypeParam> \
        struct RunCBORDeserializer<name<templateTypeParam>, void> { \
            static std::optional<std::tuple<name<templateTypeParam>, size_t>> apply(std::string_view const &s, size_t start) { \
                auto t = RunCBORDeserializer<std::tuple< \
                    BOOST_PP_SEQ_FOR_EACH(TM_BASIC_CBOR_CAPABLE_STRUCT_EXTRACT_TYPE,_,content) \
                    > \
                >::apply(s, start); \
                if (!t) {\
                    return std::nullopt; \
                } \
                return std::tuple<name<templateTypeParam>, size_t> { \
                    name<templateTypeParam> { \
                        BOOST_PP_SEQ_FOR_EACH(TM_BASIC_CBOR_CAPABLE_STRUCT_MOVE_VALUE,_,content) \
                    }, std::get<1>(*t) \
                }; \
            } \
        }; \
        template <typename templateTypeParam> \
        struct RunDeserializer<name<templateTypeParam>, void> { \
            static std::optional<name<templateTypeParam>> apply(std::string const &s) { \
                auto t = RunDeserializer<std::tuple< \
                    BOOST_PP_SEQ_FOR_EACH(TM_BASIC_CBOR_CAPABLE_STRUCT_EXTRACT_TYPE,_,content) \
                    >>::apply(s); \
                if (!t) {\
                    return std::nullopt; \
                } \
                return name<templateTypeParam> { \
                    BOOST_PP_SEQ_FOR_EACH(TM_BASIC_CBOR_CAPABLE_STRUCT_MOVE_VALUE_PLAIN,_,content) \
                }; \
            } \
        }; \
    } } } } }

#define TM_BASIC_CBOR_CAPABLE_STRUCT(name, content) \
    TM_BASIC_CBOR_CAPABLE_STRUCT_DEF(name, content) \
    TM_BASIC_CBOR_CAPABLE_STRUCT_EQ(name, content) \
    TM_BASIC_CBOR_CAPABLE_STRUCT_PRINT(name, content)

#define TM_BASIC_CBOR_CAPABLE_STRUCT_SERIALIZE(name, content) \
    TM_BASIC_CBOR_CAPABLE_STRUCT_ENCODE(name, content) \
    TM_BASIC_CBOR_CAPABLE_STRUCT_DECODE(name, content) 

#define TM_BASIC_CBOR_CAPABLE_STRUCT_SERIALIZE_NO_FIELD_NAMES(name, content) \
    TM_BASIC_CBOR_CAPABLE_STRUCT_ENCODE_NO_FIELD_NAMES(name, content) \
    TM_BASIC_CBOR_CAPABLE_STRUCT_DECODE_NO_FIELD_NAMES(name, content) 

#define TM_BASIC_CBOR_CAPABLE_TEMPLATE_1_STRUCT(templateTypeParam, name, content) \
    TM_BASIC_CBOR_CAPABLE_TEMPLATE_1_STRUCT_DEF(templateTypeParam, name, content) \
    TM_BASIC_CBOR_CAPABLE_TEMPLATE_1_STRUCT_EQ(templateTypeParam, name, content) \
    TM_BASIC_CBOR_CAPABLE_TEMPLATE_1_STRUCT_PRINT(templateTypeParam, name, content)

#define TM_BASIC_CBOR_CAPABLE_TEMPLATE_1_STRUCT_SERIALIZE(templateTypeParam, name, content) \
    TM_BASIC_CBOR_CAPABLE_TEMPLATE_1_STRUCT_ENCODE(templateTypeParam, name, content) \
    TM_BASIC_CBOR_CAPABLE_TEMPLATE_1_STRUCT_DECODE(templateTypeParam, name, content) 

#define TM_BASIC_CBOR_CAPABLE_TEMPLATE_1_STRUCT_SERIALIZE_NO_FIELD_NAMES(templateTypeParam, name, content) \
    TM_BASIC_CBOR_CAPABLE_TEMPLATE_1_STRUCT_ENCODE_NO_FIELD_NAMES(templateTypeParam, name, content) \
    TM_BASIC_CBOR_CAPABLE_TEMPLATE_1_STRUCT_DECODE_NO_FIELD_NAMES(templateTypeParam, name, content) 

#endif
