#pragma once
#include <string>
#include <vector>

namespace StringUtils {

	using Tokens = std::vector<std::string>;
	
	bool startsWith(const std::string& s, const std::string& pattern);
	Tokens split(const std::string &s, const std::string &delim);

}
