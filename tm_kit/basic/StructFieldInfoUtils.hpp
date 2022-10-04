#ifndef TM_KIT_BASIC_STRUCT_FIELD_INFO_UTILS_HPP_
#define TM_KIT_BASIC_STRUCT_FIELD_INFO_UTILS_HPP_

#include <string>
#include <sstream>
#include <array>
#include <list>
#include <vector>
#include <deque>
#include <set>
#include <unordered_set>
#include <map>
#include <unordered_map>
#include <valarray>
#include <chrono>
#include <ctime>
#include <iomanip>

#include <tm_kit/basic/StructFieldInfoHelper.hpp>
#include <tm_kit/basic/ByteData.hpp>

namespace dev { namespace cd606 { namespace tm { namespace basic { namespace struct_field_info_utils {
    template <class T>
    class StructFieldInfoBasedDataFiller {
    private:
        template <class R, int FieldCount, int FieldIndex>
        static void fillData_internal(T &data, R const &source, int startSourceIndex) {
            if constexpr (FieldIndex >= 0 && FieldIndex < FieldCount) {
                if constexpr (std::is_same_v<typename StructFieldTypeInfo<T,FieldIndex>::TheType, DateHolder>) {
                    auto tm = source.template get<std::tm>(FieldIndex+startSourceIndex);
                    auto &x = StructFieldTypeInfo<T,FieldIndex>::access(data);
                    x.year = tm.tm_year+1900;
                    x.month = tm.tm_mon+1;
                    x.day = tm.tm_mday;
                } else if constexpr (std::is_same_v<typename StructFieldTypeInfo<T,FieldIndex>::TheType, std::chrono::system_clock::time_point>) {
                    auto s = source.template get<std::string>(FieldIndex+startSourceIndex);
                    StructFieldTypeInfo<T,FieldIndex>::access(data) = 
                        infra::withtime_utils::parseLocalTime(s);
                } else {
                    StructFieldTypeInfo<T,FieldIndex>::access(data) = source.template get<typename StructFieldTypeInfo<T,FieldIndex>::TheType>(FieldIndex+startSourceIndex);
                }
                if constexpr (FieldIndex < FieldCount-1) {
                    fillData_internal<R,FieldCount,FieldIndex+1>(data, source, startSourceIndex);
                }
            }
        }
        template <std::size_t FieldIndex>
        static void addToCommaSeparatedFieldNamesForSelect(std::ostream &oss, bool &begin) {
            if constexpr (FieldIndex < StructFieldInfo<T>::FIELD_NAMES.size()) {
                if (!begin) {
                    oss << ", ";
                }
                begin = false;
                if constexpr (std::is_same_v<typename StructFieldTypeInfo<T,FieldIndex>::TheType, std::chrono::system_clock::time_point>) {
                    oss << "DATE_FORMAT(" << StructFieldInfo<T>::FIELD_NAMES[FieldIndex] 
                        << ",'%Y-%m-%dT%H:%i:%S.%f')";
                } else {
                    oss << StructFieldInfo<T>::FIELD_NAMES[FieldIndex];
                }
                addToCommaSeparatedFieldNamesForSelect<FieldIndex+1>(oss, begin);
            }
        }
        template <std::size_t FieldIndex>
        static void addValueFieldsToInsertValueList_internal(std::ostream &oss, bool &begin) {
            if constexpr (FieldIndex < basic::StructFieldInfo<T>::FIELD_NAMES.size()) {
                if (!begin) {
                    oss << ',';
                }
                begin = false;
                if constexpr (std::is_same_v<typename StructFieldTypeInfo<T,FieldIndex>::TheType, std::chrono::system_clock::time_point>) {
                    oss << "STR_TO_DATE(:" << basic::StructFieldInfo<T>::FIELD_NAMES[FieldIndex] << ",'%Y-%m-%dT%H:%i:%S.%f')";
                } else {
                    oss << ':' << basic::StructFieldInfo<T>::FIELD_NAMES[FieldIndex];
                }
                addValueFieldsToInsertValueList_internal<FieldIndex+1>(oss, begin);
            }
        }
        template <std::size_t FieldIndex>
        static void addValueFieldsToWhereClauseList_internal(std::ostream &oss, bool &begin) {
            if constexpr (FieldIndex < basic::StructFieldInfo<T>::FIELD_NAMES.size()) {
                if (!begin) {
                    oss << " AND ";
                }
                begin = false;
                oss << basic::StructFieldInfo<T>::FIELD_NAMES[FieldIndex] << " = ";
                if constexpr (std::is_same_v<typename StructFieldTypeInfo<T,FieldIndex>::TheType, std::chrono::system_clock::time_point>) {
                    oss << "STR_TO_DATE(:" << basic::StructFieldInfo<T>::FIELD_NAMES[FieldIndex] << ",'%Y-%m-%dT%H:%i:%S.%f')";
                } else {
                    oss << ':' << basic::StructFieldInfo<T>::FIELD_NAMES[FieldIndex];
                }
                addValueFieldsToInsertValueList_internal<FieldIndex+1>(oss, begin);
            }
        }
    public:
        static constexpr std::size_t FieldCount = StructFieldInfo<T>::FIELD_NAMES.size();
        static std::string commaSeparatedFieldNames() {
            bool begin = true;
            std::ostringstream oss;
            for (auto const &n : StructFieldInfo<T>::FIELD_NAMES) {
                if (!begin) {
                    oss << ", ";
                }
                begin = false;
                oss << n;
            }
            return oss.str();
        }
        static std::string commaSeparatedFieldNamesForSelect() {
            bool begin = true;
            std::ostringstream oss;
            addToCommaSeparatedFieldNamesForSelect<0>(oss, begin);
            return oss.str();
        }
        static void addValueFieldsToInsertValueList(std::ostream &oss, bool &begin) {
            addValueFieldsToInsertValueList_internal<0>(oss, begin);
        }
        static void addValueFieldsToWhereClauseValueList(std::ostream &oss, bool &begin) {
            addValueFieldsToWhereClauseList_internal<0>(oss, begin);
        }
        template <class R>
        static void fillData(T &data, R const &source, int startSourceIndex=0) {
            fillData_internal<R, StructFieldInfo<T>::FIELD_NAMES.size(), 0>(data, source, startSourceIndex);
        }
        template <class R>
        static T retrieveData(R const &source, int startSourceIndex=0) {
            T ret;
            fillData(ret, source, startSourceIndex);
            return ret;
        }
    };  
    template <class T>
    class StructFieldInfoBasedInitializer {
    private:
        template <int FieldCount, int FieldIndex>
        static void initialize_internal(T &t) {
            if constexpr (FieldIndex >= 0 && FieldIndex < FieldCount) {
                StructFieldInfoBasedInitializer<
                    typename StructFieldTypeInfo<T,FieldIndex>::TheType
                >::initialize(StructFieldTypeInfo<T,FieldIndex>::access(t));
                if constexpr (FieldIndex < FieldCount-1) {
                    initialize_internal<FieldCount,FieldIndex+1>(t);
                }
            }
        }
    public:
        static void initialize(T &t) {
            if constexpr (StructFieldInfo<T>::HasGeneratedStructFieldInfo) {
                return initialize_internal<StructFieldInfo<T>::FIELD_NAMES.size(), 0>(t);
#ifdef _MSC_VER
            } else if constexpr (std::is_same_v<T, std::tm>) {
                //in MSVC, if we initialize std::tm to all 0
                //and then use std::put_time to output it, 
                //there will be a problem since this std::tm
                //value is not a valid date. The other compilers
                //do not seem to care about this.
                t = std::tm {};
                t.tm_mday = 1;
#endif
            } else {
                t = T {};
            }
        }
    };

    template <class T>
    inline void initialize(T &t) {
        StructFieldInfoBasedInitializer<T>::initialize(t);
    }

    template <class T>
    class StructFieldInfoBasedHash {
    private:
        template <int FieldCount, int FieldIndex>
        static std::size_t hash_internal(T const &t, std::size_t soFar) {
            if constexpr (StructFieldInfo<T>::HasGeneratedStructFieldInfo) {
                if constexpr (FieldIndex >= 0 && FieldIndex < FieldCount) {
                    auto fieldHash = StructFieldInfoBasedHash<
                        typename StructFieldTypeInfo<T,FieldIndex>::TheType
                    >()(StructFieldTypeInfo<T,FieldIndex>::constAccess(t));
                    std::size_t newVal = (soFar*16777619)^fieldHash;
                    if constexpr (FieldIndex < FieldCount-1) {
                        return hash_internal<FieldCount,FieldIndex+1>(t, newVal);
                    } else {
                        return newVal;
                    }
                } else {
                    return soFar;
                }
            } else {
                return 0;
            }
        }
    public:
        std::size_t operator()(T const &t) const {
            if constexpr (StructFieldInfo<T>::HasGeneratedStructFieldInfo) {
                return hash_internal<StructFieldInfo<T>::FIELD_NAMES.size(), 0>(t, 2166136261);
            } else {
                return std::hash<T>()(t);
            }
        }
    };
    template <class T, std::size_t N>
    class StructFieldInfoBasedHash<std::array<T,N>> {
    public:
        std::size_t operator()(std::array<T,N> const &t) const {
            std::size_t ret = 2166136261;
            for (auto const &item : t) {
                ret = (ret*16777619)^StructFieldInfoBasedHash<T>()(item);
            }
            return ret;
        }
    };
    template <class T>
    class StructFieldInfoBasedHash<std::optional<T>> {
    public:
        std::size_t operator()(std::optional<T> const &t) const {
            if (!t) {
                return 2166136261;
            } else {
                return (2166136261*16777619)^StructFieldInfoBasedHash<T>()(*t);
            }
        }
    };
    template <class T>
    class StructFieldInfoBasedHash<std::list<T>> {
    public:
        std::size_t operator()(std::list<T> const &t) const {
            std::size_t ret = 2166136261;
            for (auto const &item : t) {
                ret = (ret*16777619)^StructFieldInfoBasedHash<T>()(item);
            }
            return ret;
        }
    };
    template <class T>
    class StructFieldInfoBasedHash<std::valarray<T>> {
    public:
        std::size_t operator()(std::valarray<T> const &t) const {
            std::size_t ret = 2166136261;
            for (auto const &item : t) {
                ret = (ret*16777619)^StructFieldInfoBasedHash<T>()(item);
            }
            return ret;
        }
    };
    template <class T>
    class StructFieldInfoBasedHash<std::vector<T>> {
    public:
        std::size_t operator()(std::vector<T> const &t) const {
            std::size_t ret = 2166136261;
            for (auto const &item : t) {
                ret = (ret*16777619)^StructFieldInfoBasedHash<T>()(item);
            }
            return ret;
        }
    };
    template <class T>
    class StructFieldInfoBasedHash<std::deque<T>> {
    public:
        std::size_t operator()(std::deque<T> const &t) const {
            std::size_t ret = 2166136261;
            for (auto const &item : t) {
                ret = (ret*16777619)^StructFieldInfoBasedHash<T>()(item);
            }
            return ret;
        }
    };
    template <class T, class Cmp>
    class StructFieldInfoBasedHash<std::set<T,Cmp>> {
    public:
        std::size_t operator()(std::set<T,Cmp> const &t) const {
            std::size_t ret = 2166136261;
            for (auto const &item : t) {
                ret = (ret*16777619)^StructFieldInfoBasedHash<T>()(item);
            }
            return ret;
        }
    };
    template <class A, class B, class Cmp>
    class StructFieldInfoBasedHash<std::map<A,B,Cmp>> {
    private:
    public:
        std::size_t operator()(std::map<A,B,Cmp> const &t) const {
            std::size_t ret = 2166136261;
            for (auto const &item : t) {
                ret = (ret*16777619)^StructFieldInfoBasedHash<A>()(item.first);
                ret = (ret*16777619)^StructFieldInfoBasedHash<B>()(item.second);
            }
            return ret;
        }
    };
    template <class T, class Hash>
    class StructFieldInfoBasedHash<std::unordered_set<T,Hash>> {
    public:
        std::size_t operator()(std::unordered_set<T,Hash> const &t) const {
            std::size_t ret = 2166136261;
            for (auto const &item : t) {
                ret = (ret*16777619)^Hash()(item);
            }
            return ret;
        }
    };
    template <class A, class B, class Hash>
    class StructFieldInfoBasedHash<std::unordered_map<A,B,Hash>> {
    public:
        std::size_t operator()(std::unordered_map<A,B,Hash> const &t) const {
            std::size_t ret = 2166136261;
            for (auto const &item : t) {
                ret = (ret*16777619)^Hash()(item.first);
                ret = (ret*16777619)^StructFieldInfoBasedHash<B>()(item.second);
            }
            return ret;
        }
    };
    template <>
    class StructFieldInfoBasedHash<std::chrono::system_clock::time_point> {
    public:
        std::size_t operator()(std::chrono::system_clock::time_point const &t) const {
            return (std::size_t) (std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count());
        }
    };
    template <>
    class StructFieldInfoBasedHash<std::tm> {
    public:
        std::size_t operator()(std::tm const &t) const {
            std::size_t ret = 2166136261;
            ret = (ret*16777619)^((std::size_t) t.tm_sec);
            ret = (ret*16777619)^((std::size_t) t.tm_min);
            ret = (ret*16777619)^((std::size_t) t.tm_hour);
            ret = (ret*16777619)^((std::size_t) t.tm_mday);
            ret = (ret*16777619)^((std::size_t) t.tm_mon);
            ret = (ret*16777619)^((std::size_t) t.tm_year);
            ret = (ret*16777619)^((std::size_t) t.tm_wday);
            ret = (ret*16777619)^((std::size_t) t.tm_yday);
            ret = (ret*16777619)^((std::size_t) t.tm_isdst);
            return ret;
        }
    };
    template <>
    class StructFieldInfoBasedHash<ByteData> {
    public:
        std::size_t operator()(ByteData const &t) const {
            return std::hash<std::string>()(t.content);
        }
    };
    template <>
    class StructFieldInfoBasedHash<ByteDataWithTopic> {
    public:
        std::size_t operator()(ByteDataWithTopic const &t) const {
            std::size_t ret = 2166136261;
            ret = (ret*16777619)^std::hash<std::string>()(t.topic);
            ret = (ret*16777619)^std::hash<std::string>()(t.content);
	    return ret;
        }
    };
    template <>
    class StructFieldInfoBasedHash<ByteDataWithID> {
    public:
        std::size_t operator()(ByteDataWithID const &t) const {
            std::size_t ret = 2166136261;
            ret = (ret*16777619)^std::hash<std::string>()(t.id);
            ret = (ret*16777619)^std::hash<std::string>()(t.content);
	    return ret;
        }
    };
}}}}}

#endif
