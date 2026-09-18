// The bundled CAJUN reader's \u handling. Upstream left the case label empty
// and fell through to the throwing default, so any document carrying a \u was
// refused whatever it decoded to.

#include <Telnyx/JSON/Json.h>

#include <cassert>
#include <cstdio>
#include <sstream>
#include <string>

namespace {

// The decoded value of member "k", or "" with threw set when the parse failed.
std::string read_k(const std::string& doc, bool& threw)
{
	threw = false;
	std::stringstream in(doc);
	Telnyx::JSON::Object obj;
	try {
		Telnyx::JSON::Reader::Read(obj, in);
	} catch (const std::exception&) {
		threw = true;
		return std::string();
	}
	const Telnyx::JSON::String& value = obj["k"];
	return value.Value();
}

void decodes(const char* doc, const std::string& expect)
{
	bool threw = false;
	const std::string got = read_k(doc, threw);
	if (threw || got != expect) {
		std::fprintf(stderr, "expected a decode of %s\n", doc);
		assert(false);
	}
}

void refuses(const char* doc)
{
	bool threw = false;
	read_k(doc, threw);
	if (!threw) {
		std::fprintf(stderr, "expected a refusal of %s\n", doc);
		assert(false);
	}
}

// A policy site written with an escape decodes to the same name written
// plainly. This is the case the fix exists for: the pool policy is authored
// outside this codebase, and a \u is what a careful generator emits.
void test_an_escape_decodes_to_the_plain_name()
{
	decodes("{\"k\":\"\\u0064a1\"}", "da1");
	decodes("{\"k\":\"\\u0041\"}", "A");
	decodes("{\"k\":\"\\u0043\\u0041\"}", "CA");
}

void test_a_code_point_becomes_its_utf8_bytes()
{
	decodes("{\"k\":\"\\u00e9\"}", "\xc3\xa9");
	decodes("{\"k\":\"\\u20ac\"}", "\xe2\x82\xac");
	// The boundary either side of the two-byte form.
	decodes("{\"k\":\"\\u007f\"}", "\x7f");
	decodes("{\"k\":\"\\u0080\"}", "\xc2\x80");
	decodes("{\"k\":\"\\u07ff\"}", "\xdf\xbf");
	decodes("{\"k\":\"\\u0800\"}", "\xe0\xa0\x80");
}

// Anything above the BMP is spelled as a surrogate pair, so a reader that
// takes each half on its own writes two replacement characters where one
// character was meant.
void test_a_surrogate_pair_becomes_one_character()
{
	decodes("{\"k\":\"\\ud83d\\ude00\"}", "\xf0\x9f\x98\x80");
	decodes("{\"k\":\"\\ud800\\udc00\"}", "\xf0\x90\x80\x80");
	decodes("{\"k\":\"\\udbff\\udfff\"}", "\xf4\x8f\xbf\xbf");
}

// Half a pair is not a character. Refusing beats appending something that
// reads as a name nobody wrote.
void test_a_broken_pair_is_refused()
{
	refuses("{\"k\":\"\\ud83d\"}");
	refuses("{\"k\":\"\\ude00\"}");
	refuses("{\"k\":\"\\ud83d\\u0041\"}");
	refuses("{\"k\":\"\\ud83dx\"}");
}

void test_a_malformed_escape_is_refused()
{
	refuses("{\"k\":\"\\u00zz\"}");
	refuses("{\"k\":\"\\u00\"}");
	refuses("{\"k\":\"\\u\"}");
	refuses("{\"k\":\"\\q\"}");
}

// The other escapes are untouched by the fix.
void test_the_other_escapes_still_decode()
{
	decodes("{\"k\":\"a\\/b\"}", "a/b");
	decodes("{\"k\":\"a\\nb\"}", "a\nb");
	decodes("{\"k\":\"a\\\\b\"}", "a\\b");
	decodes("{\"k\":\"a\\\"b\"}", "a\"b");
	decodes("{\"k\":\"a\\tb\"}", "a\tb");
}

} // namespace

int main()
{
	test_an_escape_decodes_to_the_plain_name();
	test_a_code_point_becomes_its_utf8_bytes();
	test_a_surrogate_pair_becomes_one_character();
	test_a_broken_pair_is_refused();
	test_a_malformed_escape_is_refused();
	test_the_other_escapes_still_decode();
	std::printf("telnyx_json: OK\n");
	return 0;
}
