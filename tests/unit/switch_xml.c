/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2019, Anthony Minessale II <anthm@freeswitch.org>
 *
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 *
 * The Initial Developer of the Original Code is
 * Anthony Minessale II <anthm@freeswitch.org>
 * Portions created by the Initial Developer are Copyright (C)
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 * Chris Rienzo <chris@signalwire.com>
 * Seven Du <dujinfang@gmail.com>
 *
 *
 * switch_xml.c -- tests core xml functions
 *
 */
#include <switch.h>
#include <stdlib.h>

#include <test/switch_test.h>

/* lol3 passes the declaration checks (1110 references) but expands to 1 MB */
static char *lol_doc(const char *dtd_extra, const char *body)
{
	const char *ref[] = { "&lol;", "&lol1;", "&lol2;" };
	char *doc = malloc(64 * 1024), *p = doc;
	int i, j;

	p += sprintf(p, "<!DOCTYPE lolz [\n<!ENTITY lol \"");
	memset(p, 'a', 1000);
	p += 1000;
	p += sprintf(p, "\">\n");
	for (i = 0; i < 3; i++) {
		p += sprintf(p, "<!ENTITY lol%d \"", i + 1);
		for (j = 0; j < 10; j++) {
			p += sprintf(p, "%s", ref[i]);
		}
		p += sprintf(p, "\">\n");
	}
	sprintf(p, "%s]>\n%s", dtd_extra, body);

	return doc;
}

/* Parse doc, check it fails in the decoder with the expansion limit, free it */
static int lol_doc_hits_limit(char *doc)
{
	switch_xml_t xml = switch_xml_parse_str_dynamic(doc, SWITCH_TRUE);
	int hit = xml && strstr(switch_xml_error(xml), "entity expansion limit exceeded") != NULL;

	if (xml) switch_xml_free(xml);
	free(doc);

	return hit;
}

FST_MINCORE_BEGIN("./conf")
{
	FST_SUITE_BEGIN(switch_xml)
	{
		FST_SETUP_BEGIN()
		{
		}
		FST_SETUP_END()

		FST_TEARDOWN_BEGIN()
		{
		}
		FST_TEARDOWN_END()

		FST_TEST_BEGIN(test_cdata)
		{
			const char *text = "<xml><![CDATA[Tom & Jerry]]></xml>";
			switch_xml_t xml = switch_xml_parse_str_dynamic((char *)text, SWITCH_TRUE);

			fst_requires(xml);
			fst_check(xml->flags & SWITCH_XML_CDATA);
			switch_xml_free(xml);

			text = "<xml><tag><![CDATA[Tom & Jerry]]></tag></xml>";

			xml = switch_xml_parse_str_dynamic((char *)text, SWITCH_TRUE);
			fst_requires(xml);
			fst_check((xml->flags & SWITCH_XML_CDATA) == 0);
			fst_check_string_equals(xml->child->name, "tag");
			fst_check(xml->child->flags & SWITCH_XML_CDATA);
			switch_xml_free(xml);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(test_utf_8)
		{
			const char *text = "<xml>Voulez-Vous Parler Français</xml>";
			switch_xml_t xml = switch_xml_parse_str_dynamic((char *)text, SWITCH_TRUE);
			char *xml_string = NULL;

			fst_requires(xml);
			xml_string = switch_xml_toxml(xml, SWITCH_FALSE);
			fst_requires(xml_string);
			fst_check_string_equals(xml_string, "<xml>Voulez-Vous Parler Fran&#xE7;ais</xml>\n");
			free(xml_string);

			xml_string = switch_xml_toxml_ex(xml, SWITCH_FALSE, SWITCH_FALSE);
			fst_requires(xml_string);
			fst_check_string_equals(xml_string, "<xml>Voulez-Vous Parler Français</xml>\n");
			switch_xml_free(xml);
			free(xml_string);

			text = "<xml>你好，中文</xml>";
			xml = switch_xml_parse_str_dynamic((char *)text, SWITCH_TRUE);

			fst_requires(xml);
			xml_string = switch_xml_toxml(xml, SWITCH_FALSE);
			fst_requires(xml_string);
			fst_check_string_equals(xml_string, "<xml>&#x4F60;&#x597D;&#xFF0C;&#x4E2D;&#x6587;</xml>\n");
			free(xml_string);

			xml_string = switch_xml_toxml_ex(xml, SWITCH_FALSE, SWITCH_FALSE);
			fst_requires(xml_string);
			fst_check_string_equals(xml_string, "<xml>你好，中文</xml>\n");
			switch_xml_free(xml);
			free(xml_string);

			text = "<xml><tag><![CDATA[Voulez-Vous Parler Français]]></tag></xml>";

			xml = switch_xml_parse_str_dynamic((char *)text, SWITCH_TRUE);
			fst_requires(xml);
			xml_string = switch_xml_toxml(xml, SWITCH_FALSE);
			fst_requires(xml_string);
			fst_check_string_equals(xml_string, "<xml>\n  <tag>Voulez-Vous Parler Fran&#xE7;ais</tag>\n</xml>\n");
			free(xml_string);

			xml_string = switch_xml_toxml_ex(xml, SWITCH_FALSE, SWITCH_FALSE);
			fst_requires(xml_string);
			fst_check_string_equals(xml_string, "<xml>\n  <tag>Voulez-Vous Parler Français</tag>\n</xml>\n");
			switch_xml_free(xml);
			free(xml_string);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(test_dtd)
		{
			const char *text = "<xml><!DOCTYPE Response [<!ENTITY lol \"haha\"><!ENTITY lol1 \"&lol;&lol;&lol;&lol;&lol;&lol;&lol;&lol;&lol;&lol;\">]><Response><Say>&lol1;</Say></Response></xml>";
			switch_xml_t xml = switch_xml_parse_str_dynamic((char *)text, SWITCH_TRUE);
			char *xml_string = NULL;

			fst_requires(xml);
			xml_string = switch_xml_toxml_ex(xml, SWITCH_FALSE, SWITCH_FALSE);
			fst_requires(xml_string);
			fst_check_string_equals(xml_string, "<xml>\n  <Response>\n    <Say>hahahahahahahahahahahahahahahahahahahaha</Say>\n  </Response>\n</xml>\n");
			free(xml_string);
			switch_xml_free(xml);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(test_dtd_disable)
		{
			const char *text = "<xml><!DOCTYPE Response [<!ENTITY lol \"haha\"><!ENTITY lol1 \"&lol;&lol;&lol;&lol;&lol;&lol;&lol;&lol;&lol;&lol;\">]><Response><Say>&lol1;</Say></Response></xml>";
			switch_xml_t xml = NULL;
			char *xml_string = NULL;

			switch_core_set_variable("xml_disable_dtd", "true");
			xml = switch_xml_parse_str_dynamic((char *)text, SWITCH_TRUE);
			fst_requires(xml);
			xml_string = switch_xml_toxml_ex(xml, SWITCH_FALSE, SWITCH_FALSE);
			fst_requires(xml_string);
			fst_check_string_equals(xml_string, "<xml>\n  <Response>\n    <Say>&amp;lol1;</Say>\n  </Response>\n</xml>\n");
			free(xml_string);
			switch_xml_free(xml);
			switch_core_set_variable("xml_disable_dtd", "false");
		}
		FST_TEST_END()

		FST_TEST_BEGIN(test_dtd_with_comments)
		{
			const char *text = "<xml><!DOCTYPE Response [<!--COMMENT1--><!ENTITY lol \"haha\"><!ENTITY lol1 \"&lol;&lol;&lol;&lol;&lol;&lol;&lol;&lol;&lol;&lol;\"><!--COMMENT2-->]><Response><Say>&lol1;</Say></Response></xml>";
			switch_xml_t xml = NULL;
			char *xml_string = NULL;

			xml = switch_xml_parse_str_dynamic((char *)text, SWITCH_TRUE);
			fst_requires(xml);
			xml_string = switch_xml_toxml_ex(xml, SWITCH_FALSE, SWITCH_FALSE);
			fst_requires(xml_string);
			fst_check_string_equals(xml_string, "<xml>\n  <Response>\n    <Say>hahahahahahahahahahahahahahahahahahahaha</Say>\n  </Response>\n</xml>\n");
			free(xml_string);
			switch_xml_free(xml);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(test_dtd_disable_with_comments)
		{
			const char *text = "<xml><!DOCTYPE Response [<!--COMMENT1--><!ENTITY lol \"haha\"><!ENTITY lol1 \"&lol;&lol;&lol;&lol;&lol;&lol;&lol;&lol;&lol;&lol;\"><!--COMMENT2-->]><Response><Say>&lol1;</Say></Response></xml>";
			switch_xml_t xml = NULL;
			char *xml_string = NULL;

			switch_core_set_variable("xml_disable_dtd", "true");
			xml = switch_xml_parse_str_dynamic((char *)text, SWITCH_TRUE);
			fst_requires(xml);
			xml_string = switch_xml_toxml_ex(xml, SWITCH_FALSE, SWITCH_FALSE);
			fst_requires(xml_string);
			fst_check_string_equals(xml_string, "<xml>\n  <Response>\n    <Say>&amp;lol1;</Say>\n  </Response>\n</xml>\n");
			free(xml_string);
			switch_xml_free(xml);
			switch_core_set_variable("xml_disable_dtd", "false");
		}
		FST_TEST_END()

		FST_TEST_BEGIN(test_exponential_entity_expansion)
		{
			/* Test handling of exponentially nested entity definitions
			 * Each entity references the previous one 10 times, creating
			 * 10^10 total references which would consume excessive memory
			 * if fully expanded. Parser should enforce expansion limits.
			 */
			const char *nested_entities =
				"<?xml version=\"1.0\"?>\n"
				"<!DOCTYPE lolz [\n"
				"<!ENTITY lol \"lol\">\n"
				"<!ELEMENT lolz (#PCDATA)>\n"
				"<!ENTITY lol1 \"&lol;&lol;&lol;&lol;&lol;&lol;&lol;&lol;&lol;&lol;\">\n"
				"<!ENTITY lol2 \"&lol1;&lol1;&lol1;&lol1;&lol1;&lol1;&lol1;&lol1;&lol1;&lol1;\">\n"
				"<!ENTITY lol3 \"&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;\">\n"
				"<!ENTITY lol4 \"&lol3;&lol3;&lol3;&lol3;&lol3;&lol3;&lol3;&lol3;&lol3;&lol3;\">\n"
				"<!ENTITY lol5 \"&lol4;&lol4;&lol4;&lol4;&lol4;&lol4;&lol4;&lol4;&lol4;&lol4;\">\n"
				"<!ENTITY lol6 \"&lol5;&lol5;&lol5;&lol5;&lol5;&lol5;&lol5;&lol5;&lol5;&lol5;\">\n"
				"<!ENTITY lol7 \"&lol6;&lol6;&lol6;&lol6;&lol6;&lol6;&lol6;&lol6;&lol6;&lol6;\">\n"
				"<!ENTITY lol8 \"&lol7;&lol7;&lol7;&lol7;&lol7;&lol7;&lol7;&lol7;&lol7;&lol7;\">\n"
				"<!ENTITY lol9 \"&lol8;&lol8;&lol8;&lol8;&lol8;&lol8;&lol8;&lol8;&lol8;&lol8;\">\n"
				"<!ENTITY lol10 \"&lol9;&lol9;&lol9;&lol9;&lol9;&lol9;&lol9;&lol9;&lol9;&lol9;\">\n"
				"]>\n"
				"<lolz>&lol10;</lolz>";

			switch_xml_t xml = switch_xml_parse_str_dynamic((char *)nested_entities, SWITCH_TRUE);

			if (xml) {
				const char *error = switch_xml_error(xml);
				if (error && *error) {
					/* Parser enforced expansion limits */
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
						"Parser correctly enforced entity expansion limits: %s\n", error);
					switch_xml_free(xml);
				} else {
					/* Parser did not enforce limits */
					switch_xml_free(xml);
					fst_fail("Parser did not enforce entity expansion limits");
				}
			} else {
				/* Parser returned NULL */
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
					"Parser rejected excessive entity expansion\n");
			}
		}
		FST_TEST_END()

		FST_TEST_BEGIN(test_exponential_entity_expansion_forward_refs)
		{
			/* Over the expansion count, each entity referencing one declared after it */
			const char *nested_entities =
				"<?xml version=\"1.0\"?>\n"
				"<!DOCTYPE lolz [\n"
				"<!ENTITY lol5 \"&lol4;&lol4;&lol4;&lol4;&lol4;&lol4;&lol4;&lol4;&lol4;&lol4;\">\n"
				"<!ENTITY lol4 \"&lol3;&lol3;&lol3;&lol3;&lol3;&lol3;&lol3;&lol3;&lol3;&lol3;\">\n"
				"<!ENTITY lol3 \"&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;\">\n"
				"<!ENTITY lol2 \"&lol1;&lol1;&lol1;&lol1;&lol1;&lol1;&lol1;&lol1;&lol1;&lol1;\">\n"
				"<!ENTITY lol1 \"&lol;&lol;&lol;&lol;&lol;&lol;&lol;&lol;&lol;&lol;\">\n"
				"<!ENTITY lol \"lol\">\n"
				"]>\n"
				"<lolz>&lol5;</lolz>";

			switch_xml_t xml = switch_xml_parse_str_dynamic((char *)nested_entities, SWITCH_TRUE);

			fst_check(!xml || !zstr(switch_xml_error(xml)));
			if (xml) switch_xml_free(xml);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(test_exponential_parameter_entity_expansion)
		{
			/* Parameter entities are expanded when declared: 48 bytes doubled 20 times */
			const char *nested_entities =
				"<?xml version=\"1.0\"?>\n"
				"<!DOCTYPE lolz [\n"
				"<!ENTITY % p0 \"lollollollollollollollollollollollollollollollol\">\n"
				"<!ENTITY % p1 \"%p0;%p0;\"><!ENTITY % p2 \"%p1;%p1;\"><!ENTITY % p3 \"%p2;%p2;\">\n"
				"<!ENTITY % p4 \"%p3;%p3;\"><!ENTITY % p5 \"%p4;%p4;\"><!ENTITY % p6 \"%p5;%p5;\">\n"
				"<!ENTITY % p7 \"%p6;%p6;\"><!ENTITY % p8 \"%p7;%p7;\"><!ENTITY % p9 \"%p8;%p8;\">\n"
				"<!ENTITY % p10 \"%p9;%p9;\"><!ENTITY % p11 \"%p10;%p10;\"><!ENTITY % p12 \"%p11;%p11;\">\n"
				"<!ENTITY % p13 \"%p12;%p12;\"><!ENTITY % p14 \"%p13;%p13;\"><!ENTITY % p15 \"%p14;%p14;\">\n"
				"<!ENTITY % p16 \"%p15;%p15;\"><!ENTITY % p17 \"%p16;%p16;\"><!ENTITY % p18 \"%p17;%p17;\">\n"
				"<!ENTITY % p19 \"%p18;%p18;\"><!ENTITY % p20 \"%p19;%p19;\">\n"
				"]>\n"
				"<lolz/>";

			switch_xml_t xml = switch_xml_parse_str_dynamic((char *)nested_entities, SWITCH_TRUE);

			/* Stopped while decoding the declaration, not after it */
			fst_requires(xml);
			fst_check(strstr(switch_xml_error(xml), "entity expansion limit exceeded &p") != NULL);
			switch_xml_free(xml);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(test_entity_expansion_size_in_content)
		{
			fst_check(lol_doc_hits_limit(lol_doc("", "<lolz>&lol3;</lolz>")));
		}
		FST_TEST_END()

		FST_TEST_BEGIN(test_entity_expansion_size_in_attribute)
		{
			fst_check(lol_doc_hits_limit(lol_doc("", "<lolz a=\"&lol3;\"/>")));
		}
		FST_TEST_END()

		FST_TEST_BEGIN(test_entity_expansion_size_in_attlist_default)
		{
			fst_check(lol_doc_hits_limit(lol_doc("<!ATTLIST lolz a CDATA \"&lol3;\">\n", "<lolz/>")));
		}
		FST_TEST_END()

		FST_TEST_BEGIN(test_entity_expansion_within_limit)
		{
			char *doc = lol_doc("", "<lolz>&lol1;</lolz>");
			switch_xml_t xml = switch_xml_parse_str_dynamic(doc, SWITCH_TRUE);

			fst_requires(xml);
			fst_check(zstr(switch_xml_error(xml)));
			fst_check(strlen(xml->txt) == 10000);
			switch_xml_free(xml);
			free(doc);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(test_entity_reference_from_char_reference)
		{
			/* &a; yields "&", so &b; expands to "&b;" again on every rescan */
			const char *doc =
				"<!DOCTYPE t [\n<!ENTITY a \"&#38;\">\n<!ENTITY b \"&a;b;\">\n]>\n<t>&b;</t>";
			switch_xml_t xml = switch_xml_parse_str_dynamic((char *)doc, SWITCH_TRUE);

			fst_requires(xml);
			fst_check(strstr(switch_xml_error(xml), "entity expansion limit exceeded") != NULL);
			switch_xml_free(xml);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(test_parameter_entity_cycle)
		{
			/* ent_ok() does not follow parameter entities */
			const char *doc =
				"<!DOCTYPE t [\n<!ENTITY % p \"%q;\">\n<!ENTITY % q \"%p;\">\n<!ENTITY r \"%q;\">\n]>\n<t/>";
			switch_xml_t xml = switch_xml_parse_str_dynamic((char *)doc, SWITCH_TRUE);

			fst_requires(xml);
			fst_check(strstr(switch_xml_error(xml), "entity expansion limit exceeded") != NULL);
			switch_xml_free(xml);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(test_entity_expansion_document_budget)
		{
			/* Each element stays under the per-string cap; together they exceed the document's */
			char body[8192], *p = body;
			int i;

			p += sprintf(p, "<lolz>");
			for (i = 0; i < 200; i++) {
				p += sprintf(p, "<a>&lol1;</a>");
			}
			sprintf(p, "</lolz>");
			fst_check(lol_doc_hits_limit(lol_doc("", body)));
		}
		FST_TEST_END()

		FST_TEST_BEGIN(test_stray_reference_chars_add_no_allowance)
		{
			/* 18000 replacements exceed the cap; 10000 '%' in content are not references */
			int i, n = 10000;
			char *doc = malloc(9000 * 4 + n + 128), *p = doc;
			switch_xml_t xml;

			p += sprintf(p, "<!DOCTYPE t [\n<!ENTITY y \"a\">\n<!ENTITY x \"");
			for (i = 0; i < 9000; i++) {
				p += sprintf(p, "&y;");
			}
			p += sprintf(p, "\">\n]>\n<t>&x;&x;");
			memset(p, '%', n);
			p += n;
			sprintf(p, "</t>");

			xml = switch_xml_parse_str_dynamic(doc, SWITCH_TRUE);
			fst_requires(xml);
			fst_check(strstr(switch_xml_error(xml), "entity expansion limit exceeded") != NULL);
			switch_xml_free(xml);
			free(doc);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(test_normalized_attribute_charges_allocation)
		{
			/* Each value normalizes to "", but its 60 KB buffer still counts */
			int i, n = 60000;
			char *doc = malloc(n + 40 * 16 + 128), *p = doc;
			switch_xml_t xml;

			p += sprintf(p, "<!DOCTYPE r [\n<!ATTLIST t a NMTOKENS #IMPLIED>\n<!ENTITY sp \"");
			memset(p, ' ', n);
			p += n;
			p += sprintf(p, "\">\n]>\n<r>");
			for (i = 0; i < 40; i++) {
				p += sprintf(p, "<t a=\"&sp;\"/>");
			}
			sprintf(p, "</r>");

			xml = switch_xml_parse_str_dynamic(doc, SWITCH_TRUE);
			fst_requires(xml);
			fst_check(strstr(switch_xml_error(xml), "entity expansion limit exceeded") != NULL);
			switch_xml_free(xml);
			free(doc);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(test_decode_output)
		{
			/* Pins what the decoder produces: line endings, character and entity
			   references (growing and shrinking), and attribute normalization */
			const char *doc =
				"<!DOCTYPE t [\n<!ENTITY big \"0123456789\">\n<!ENTITY e \"\">\n"
				"<!ATTLIST t n NMTOKENS #IMPLIED>\n]>\n"
				"<t c=\"a&#9;b\r\nc\" n=\"  x&#32; &big;\ty  \">x\r\ny\rz &#x263A;&lt;&big;&e;&amp;&#38;amp;&unknown;</t>";
			switch_xml_t xml = switch_xml_parse_str_dynamic((char *)doc, SWITCH_TRUE);

			fst_requires(xml);
			fst_check(zstr(switch_xml_error(xml)));
			fst_check_string_equals(xml->txt, "x\ny\nz \xE2\x98\xBA<0123456789&&amp;&unknown;");
			fst_check_string_equals(switch_xml_attr(xml, "c"), "a\tb c");	/* character references are not normalized */
			fst_check_string_equals(switch_xml_attr(xml, "n"), "x 0123456789 y");
			switch_xml_free(xml);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(test_decode_is_linear)
		{
			/* One memmove of the rest per reference made this take minutes */
			int i, n = 1000000;
			char *doc = malloc(n * 5 + 16), *p = doc;
			switch_time_t start;
			switch_xml_t xml;

			p += sprintf(p, "<t>");
			for (i = 0; i < n; i++) {
				p += sprintf(p, "&amp;");
			}
			sprintf(p, "</t>");

			start = switch_time_now();
			xml = switch_xml_parse_str_dynamic(doc, SWITCH_TRUE);
			fst_requires(xml);
			fst_check(zstr(switch_xml_error(xml)));
			fst_check(strlen(xml->txt) == (size_t)n);
			fst_check(switch_time_now() - start < 2000000);
			switch_xml_free(xml);
			free(doc);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(test_many_references_in_input)
		{
			/* References written in the document do not use up the expansion budget */
			int i, n = 20000;
			char *doc = malloc(n * 5 + 16), *p = doc;
			switch_xml_t xml;

			p += sprintf(p, "<t>");
			for (i = 0; i < n; i++) {
				p += sprintf(p, "&amp;");
			}
			sprintf(p, "</t>");

			xml = switch_xml_parse_str_dynamic(doc, SWITCH_TRUE);
			fst_requires(xml);
			fst_check(zstr(switch_xml_error(xml)));
			fst_check(strlen(xml->txt) == (size_t)n);
			switch_xml_free(xml);
			free(doc);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(test_entity_forward_ref)
		{
			/* A small forward reference is still expanded */
			const char *safe_entities =
				"<?xml version=\"1.0\"?>\n"
				"<!DOCTYPE test [\n"
				"<!ENTITY product \"&company; Media Server\">\n"
				"<!ENTITY company \"FreeSWITCH\">\n"
				"]>\n"
				"<test>&product;</test>";

			switch_xml_t xml = switch_xml_parse_str_dynamic((char *)safe_entities, SWITCH_TRUE);

			fst_requires(xml);
			fst_check_string_equals(xml->txt, "FreeSWITCH Media Server");
			switch_xml_free(xml);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(test_entity_expansion_limit)
		{
			/* Test that reasonable entity usage still works */
			const char *safe_entities =
				"<?xml version=\"1.0\"?>\n"
				"<!DOCTYPE test [\n"
				"<!ENTITY company \"FreeSWITCH\">\n"
				"<!ENTITY product \"&company; Media Server\">\n"
				"]>\n"
				"<test>&product;</test>";

			switch_xml_t xml = switch_xml_parse_str_dynamic((char *)safe_entities, SWITCH_TRUE);

			fst_requires(xml);
			fst_check_string_equals(xml->txt, "FreeSWITCH Media Server");
			switch_xml_free(xml);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(test_empty_entity_decode)
		{
			const char *text =
				"<xml><!DOCTYPE Response ["
				"<!ENTITY empty \"\">"
				"<!ENTITY name \"World\">"
				"]><Response><Say>Hello&empty;, &name;&empty;!</Say></Response></xml>";
			switch_xml_t xml = NULL;
			char *xml_string = NULL;

			xml = switch_xml_parse_str_dynamic((char *)text, SWITCH_TRUE);
			if (!xml) {
				fst_fail("failed to parse XML with empty entity");
				goto test_empty_entity_decode_done;
			}

			xml_string = switch_xml_toxml_ex(xml, SWITCH_FALSE, SWITCH_FALSE);
			if (!xml_string) {
				fst_fail("failed to serialize parsed XML");
				goto test_empty_entity_decode_done;
			}

			fst_check_string_equals(xml_string,
				"<xml>\n  <Response>\n    <Say>Hello, World!</Say>\n  </Response>\n</xml>\n");

test_empty_entity_decode_done:
			free(xml_string);
			if (xml) switch_xml_free(xml);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(test_utf_8_wide_codepoint)
		{
			/* U+10FFFF is the largest Unicode code point; its UTF-8 form
			   F4 8F BF BF serializes to "&#x10FFFF;", the widest &#x...;
			   escape (10 chars) the encoder emits. It must serialize intact
			   and must not overrun the destination buffer. */
			const char *single = "<xml>\xF4\x8F\xBF\xBF" "</xml>";
			switch_xml_t xml = NULL;
			char *xml_string = NULL;
			int prefix;

			xml = switch_xml_parse_str_dynamic((char *)single, SWITCH_TRUE);
			if (!xml) {
				fst_fail("failed to parse maximum code point document");
				goto test_utf_8_wide_done;
			}

			xml_string = switch_xml_toxml(xml, SWITCH_FALSE);
			if (!xml_string) {
				fst_fail("failed to serialize maximum code point");
				goto test_utf_8_wide_done;
			}

			fst_check_string_equals(xml_string, "<xml>&#x10FFFF;</xml>\n");
			free(xml_string);
			xml_string = NULL;
			switch_xml_free(xml);
			xml = NULL;

			/* Serialize long runs of the widest escape so the destination
			   buffer reallocates many times. Sweeping the ASCII prefix length
			   shifts the write offset so that, across iterations, an escape is
			   emitted at every alignment relative to the reserved headroom,
			   including the tightest one. Each run must serialize intact: the
			   full escaped length, with no truncation or dropped escape. */
			for (prefix = 0; prefix < 10; prefix++) {
				switch_size_t runs = 1100;
				switch_size_t cap = 5 + prefix + runs * 4 + 6 + 1;
				char *doc = switch_must_malloc(cap);
				char *w = doc;
				switch_size_t i;
				switch_size_t expected_len = 5 + (switch_size_t) prefix + runs * 10 + 7;

				memcpy(w, "<xml>", 5);
				w += 5;
				for (i = 0; i < (switch_size_t) prefix; i++) {
					*w++ = 'a';
				}
				for (i = 0; i < runs; i++) {
					*w++ = (char) 0xF4;
					*w++ = (char) 0x8F;
					*w++ = (char) 0xBF;
					*w++ = (char) 0xBF;
				}
				memcpy(w, "</xml>", 6);
				w += 6;
				*w = '\0';

				xml = switch_xml_parse_str_dynamic(doc, SWITCH_TRUE);
				free(doc);
				if (!xml) {
					fst_fail("failed to parse long wide-escape run");
					goto test_utf_8_wide_done;
				}

				xml_string = switch_xml_toxml(xml, SWITCH_FALSE);
				if (!xml_string) {
					fst_fail("failed to serialize long wide-escape run");
					goto test_utf_8_wide_done;
				}

				fst_check_string_has(xml_string, "&#x10FFFF;");
				fst_xcheck(strlen(xml_string) == expected_len, "serialized wide-escape run has wrong length");
				free(xml_string);
				xml_string = NULL;
				switch_xml_free(xml);
				xml = NULL;
			}

test_utf_8_wide_done:
			free(xml_string);
			switch_xml_free(xml);
		}
		FST_TEST_END()
	}
	FST_SUITE_END()
}
FST_MINCORE_END()
