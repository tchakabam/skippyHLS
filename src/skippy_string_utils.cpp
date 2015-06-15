#include <skippyHLS/skippy_string_utils.h>
#include <algorithm>

namespace StringUtils {

using std::string;
using std::vector;

bool startsWith(const string& s, const string& pattern) {
    return pattern.size() <= s.size() && s.find(pattern) != string::npos;
}
Tokens split(const string &s, const string &delim) {
  vector<string> result;
  auto from = begin(s);
  auto endIt = end(s);
  auto is_delim = [&](const char c) { return delim.find(c) != string::npos; };
  while( (from = find_if_not(from, endIt, is_delim)) != endIt ){
    auto tokEnd = find_if(from, endIt, is_delim);
    result.push_back(string(from, tokEnd));
    from = tokEnd;
  }
  return result;
}

}
