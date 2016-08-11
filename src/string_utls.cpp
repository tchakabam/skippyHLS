//
// Created by Alexander Lenhardt on 9/8/15.
//

#include "string_utils.hpp"
#include <algorithm>
#include <iomanip>
#include <functional>

namespace StringUtils {
    
    bool startsWith(const std::string& s, const std::string& pattern) {
        return pattern.size() <= s.size() && s.find(pattern) != std::string::npos;
    }
    
    void replace(std::string& s, const char* search, const char* replace) {
        if(!search || !replace) {
            return;
        }
        
        size_t replaceLength = strlen(replace);
        size_t searchLength = strlen(search);
        
        for (std::size_t pos = 0;; pos += replaceLength) {
            pos = s.find(search, pos);
            if (pos == std::string::npos)
                break;
            
            s.erase(pos, searchLength);
            s.insert(pos, replace);
        }
    }
    
    /**
     * Converts a string to lower case.
     */
    std::string toLower(const char *source) {
        std::string copy;
        size_t sourceLength = strlen(source);
        copy.resize(sourceLength);
        std::transform(source, source + sourceLength, copy.begin(), ::tolower);
        return std::move(copy);
    }
    
    
    /**
     * Converts a string to upper case.
     */
    std::string toUpper(const char *source) {
        std::string copy;
        size_t sourceLength = strlen(source);
        copy.resize(sourceLength);
        std::transform(source, source + sourceLength, copy.begin(), ::toupper);
        return std::move(copy);
    }
    
    
    /**
     * Does a caseless comparison of two strings.
     */
    bool caselessCompare(const char *value1, const char *value2) {
        return toLower(value1) == toLower(value2);
    }
    
    
    /**
     * URL encodes a string (uses %20 not + for spaces).
     */
    std::string URLEncode(const char *unsafe) {
        std::stringstream escaped;
        escaped.fill('0');
        escaped << std::hex << std::uppercase;
        
        size_t unsafeLength = strlen(unsafe);
        for (auto i = unsafe, n = unsafe + unsafeLength; i != n; ++i)
        {
            char c = *i;
            if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            {
                escaped << c;
            }
            else
            {
                escaped << '%' << std::setw(2) << ((int) c) << std::setw(0);
            }
        }
        
        return escaped.str();
    }
    
    
    /**
     * Decodes a URL encoded string (will handle both encoding schemes for spaces).
     */
    std::string URLDecode(const char *safe) {
        std::stringstream unescaped;
        unescaped.fill('0');
        unescaped << std::hex;
        
        size_t safeLength = strlen(safe);
        for (auto i = safe, n = safe + safeLength; i != n; ++i)
        {
            char c = *i;
            if(c == '%')
            {
                char hex[3];
                hex[0] = *(i + 1);
                hex[1] = *(i + 2);
                hex[2] = 0;
                i += 2;
                int hexAsInteger = strtol(hex, nullptr, 16);
                unescaped << (char)hexAsInteger;
            }
            else
            {
                unescaped << *i;
            }
        }
        
        return unescaped.str();
    }
    
    
    /**
     * Splits a string on a delimiter (empty items are excluded).
     */
    std::vector<std::string> split(const std::string &toSplit, char splitOn, u_int32_t stopAfter) {
        std::stringstream input(toSplit);
        std::vector<std::string> returnValues;
        std::string item;
        u_int32_t foundDelimiters = 0;
        
        while(std::getline(input, item, splitOn)) {
            if(item.size() > 0) {
                returnValues.push_back(item);
                if (++foundDelimiters == stopAfter) {
                    std::getline(input, item);
                    if (item.size() > 0) {
                        returnValues.push_back(item);
                    }
                    break;
                }
            }
        }
        
        return std::move(returnValues);
    }
    
    std::vector<std::string> split(const std::string &toSplit, const std::string& delim) {
        std::vector<std::string> result;
        
        auto from = begin(toSplit);
        auto endIt = end(toSplit);
        auto is_delim = [&](const char c) { return delim.find(c) != std::string::npos; };
        
        while( (from = find_if_not(from, endIt, is_delim)) != endIt ) {
            auto tokEnd = find_if(from, endIt, is_delim);
            result.push_back(std::string(from, tokEnd));
            from = tokEnd;
        }
        
        return std::move(result);
    }
    
    
    /**
     * Splits a string on new line characters.
     */
    std::vector<std::string> splitOnLine(const std::string &toSplit) {
        std::stringstream input(toSplit);
        std::vector<std::string> returnValues;
        std::string item;
        
        while (std::getline(input, item))
        {
            if (item.size() > 0)
            {
                returnValues.push_back(item);
            }
        }
        
        return std::move(returnValues);
    }
    
    
    //std::vector<std::string> SplitOnRegex(std::string regex);
    // trim from start
    std::string LTrim(const char *source) {
        std::string copy(source);
        copy.erase(copy.begin(), std::find_if(copy.begin(), copy.end(), std::not1(std::ptr_fun<int, int>(::isspace))));
        return std::move(copy);
    }
    
    
    // trim from end
    std::string RTrim(const char *source) {
        std::string copy(source);
        copy.erase(std::find_if(copy.rbegin(), copy.rend(), std::not1(std::ptr_fun<int, int>(::isspace))).base(), copy.end());
        return std::move(copy);
    }
    
    
    // trim from both ends
    std::string trim(const char *source) {
        return LTrim(RTrim(source).c_str());
    }
    
    
    //convert to int 64
    int64_t convertToInt64(const char *source) {
        if(!source) {
            return 0;
        }
        
#ifdef __ANDROID__
        return atoll(source);
#else
        return std::atoll(source);
#endif // __ANDROID__
    }
    
    
    //convert to int 32
    int32_t convertToInt32(const char *source) {
        if (!source) {
            return 0;
        }
        
        return std::atol(source);
    }
    
    
    //convert to bool
    bool convertToBool(const char *source) {
        if(!source) {
            return false;
        }
        
        std::string strValue = toLower(source);
        if(strValue == "true" || strValue == "1") {
            return true;
        }
        
        return false;
    }
    
    
    //convert to double
    double convertToDouble(const char *source) {
        if(!source) {
            return 0.0;
        }
        
        return std::strtod(source, NULL);
    }
    
}
