#include <string>
#include <set>
#include <vector>
#include <sstream>
#include <boost/algorithm/string.hpp>
#include <boost/lexical_cast.hpp>

#include "rpc_helper.h"

static int MIN_IDLE_CPU = 0;
static std::set<std::string> throttle_api_calls;

template <typename T>
T string_to_number(const std::string& str, T errorValue = 0)
  /// Convert a string to a numeric value
{
  try { return boost::lexical_cast<T>(str);} catch(...){return errorValue;};
}

static std::vector<std::string> string_tokenize(const std::string& str, const char* tok)
{
  std::vector<std::string> tokens;
  boost::split(tokens, str, boost::is_any_of(tok), boost::token_compress_on);
  return tokens;
}

static std::string first_token(const std::string& s)
{
	const std::string::size_type pos = s.find_first_not_of(" \t\r\n");
	if (pos == std::string::npos) return std::string();
	const std::string::size_type end = s.find_first_of(" \t\r\n", pos);
	return s.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
}

void set_min_idle_cpu_watermark(const char* idle_cpu)
{
	MIN_IDLE_CPU = string_to_number<int>(idle_cpu);
}

switch_bool_t is_resource_available(const char* command, const char* api_str)
{
	if (throttle_api_calls.empty() || MIN_IDLE_CPU <= 0)
	{
		return SWITCH_TRUE;
	}

	std::string cmd(zstr(command) ? "" : command);
	std::string api(zstr(api_str) ? "" : api_str);

	bool throttled = throttle_api_calls.count(cmd) > 0;
	if (!throttled && cmd == "bgapi")
	{
		const std::string sub = first_token(api);
		throttled = !sub.empty() && throttle_api_calls.count(sub) > 0;
	}
	if (!throttled)
	{
		return SWITCH_TRUE;
	}

	const double idle_cpu = switch_core_idle_cpu();
	return (idle_cpu < MIN_IDLE_CPU) ? SWITCH_FALSE : SWITCH_TRUE;
}

void set_throttled_api_calls(const char* api)
{
	throttle_api_calls.clear();
	if (zstr(api)) return;
	std::vector<std::string> tokens = string_tokenize(api, " \t");
	for (std::vector<std::string>::const_iterator iter = tokens.begin(); iter != tokens.end(); iter++)
	{
		if (!iter->empty())
		{
			throttle_api_calls.insert(*iter);
		}
	}
}