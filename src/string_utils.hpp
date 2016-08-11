//
// Created by Alexander Lenhardt on 9/8/15.
//

#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <sstream>

namespace StringUtils {
    
    bool startsWith(const std::string& s, const std::string& pattern);
    
    void replace(std::string& s, const char* search, const char* replace);
    
    /**
     * Converts a string to lower case.
     */
    std::string toLower(const char *source);
    
    
    /**
     * Converts a string to upper case.
     */
    std::string toUpper(const char *source);
    
    
    /**
     * Does a caseless comparison of two strings.
     */
    bool caselessCompare(const char *value1, const char *value2);
    
    
    /**
     * URL encodes a string (uses %20 not + for spaces).
     */
    std::string URLEncode(const char *unsafe);
    
    
    /**
     * Decodes a URL encoded string (will handle both encoding schemes for spaces).
     */
    std::string URLDecode(const char *safe);
    
    
    /**
     * Splits a string on a delimiter (empty items are excluded).
     */
    std::vector<std::string> split(const std::string &toSplit, char delim, u_int32_t stopAfter=0);
    
    /**
     * Splits a string on a delimiter string (empty items are excluded).
     */
    std::vector<std::string> split(const std::string &toSplit, const std::string& delim);
    
    /**
     * Splits a string on new line characters.
     */
    std::vector<std::string> splitOnLine(const std::string &toSplit);
    
    
    //std::vector<std::string> SplitOnRegex(std::string regex);
    // trim from start
    std::string LTrim(const char *source);
    
    
    // trim from end
    std::string RTrim(const char *source);
    
    
    // trim from both ends
    std::string trim(const char *source);
    
    
    //convert to int 64
    int64_t convertToInt64(const char *source);
    
    
    //convert to int 32
    int32_t convertToInt32(const char *source);
    
    
    //convert to bool
    bool convertToBool(const char *source);
    
    
    //convert to double
    double convertToDouble(const char *source);
    
    
    // not all platforms (Android) have std::to_string
    template<typename T>
    std::string to_string(T value) {
        std::ostringstream os;
        os << value;
        return os.str();
    }
    
}
