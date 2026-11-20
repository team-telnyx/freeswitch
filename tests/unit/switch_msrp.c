/*
 * MSRP body parser tests. Includes switch_msrp.c to reach the static
 * msrp_parse_buffer().
 */

#include <switch.h>
#include <test/switch_test.h>

#include "../../src/switch_msrp.c"

#define END_LINE "-------tid1$\r\n"

static switch_memory_pool_t *test_pool;

/* A range_star SEND whose headers are parsed, waiting for its body */
static switch_msrp_msg_t *wait_body_msg(void)
{
	char headers[] =
		"MSRP tid1 SEND\r\n"
		"To-Path: msrp://127.0.0.1:2855/a;tcp\r\n"
		"From-Path: msrp://127.0.0.1:2856/b;tcp\r\n"
		"Message-ID: m1\r\n"
		"Byte-Range: 1-*/*\r\n"
		"Content-Type: text/plain\r\n"
		"\r\n";
	switch_msrp_msg_t *msg = msrp_parse_buffer(headers, (int)strlen(headers), NULL, test_pool);

	if (!msg || msg->state != MSRP_ST_WAIT_BODY || !msg->range_star) {
		switch_msrp_msg_destroy(&msg);
		return NULL;
	}

	return msg;
}

/* Parse one body buffer; NULL unless the message completed */
static switch_msrp_msg_t *parse_body(char *body, int len)
{
	switch_msrp_msg_t *msg = wait_body_msg();

	if (!msg) return NULL;
	msg = msrp_parse_buffer(body, len, msg, test_pool);
	if (msg->state != MSRP_ST_DONE) {
		switch_msrp_msg_destroy(&msg);
	}

	return msg;
}

FST_MINCORE_BEGIN("./conf")
{
FST_SUITE_BEGIN(switch_msrp)
{
FST_SETUP_BEGIN()
{
	switch_core_new_memory_pool(&test_pool);
}
FST_SETUP_END()

FST_TEARDOWN_BEGIN()
{
	switch_core_destroy_memory_pool(&test_pool);
}
FST_TEARDOWN_END()

	FST_TEST_BEGIN(body_then_end_line)
	{
		char body[] = "hello\r\n" END_LINE;
		switch_msrp_msg_t *msg = parse_body(body, (int)strlen(body));

		fst_requires(msg);
		fst_check(msg->payload_bytes == 5 && !memcmp(msg->payload, "hello", 5));
		fst_check(msg->last_p == body + strlen(body));
		switch_msrp_msg_destroy(&msg);
	}
	FST_TEST_END()

	/* The body's CRLF went out with the previous chunk */
	FST_TEST_BEGIN(end_line_starts_the_buffer)
	{
		char body[] = END_LINE;
		switch_msrp_msg_t *msg = parse_body(body, (int)strlen(body));

		fst_requires(msg);
		fst_check(msg->payload_bytes == 0);
		fst_check(msg->last_p == body + strlen(body));
		switch_msrp_msg_destroy(&msg);
	}
	FST_TEST_END()

	/* Only the body's CR went out with the previous chunk */
	FST_TEST_BEGIN(end_line_after_lf)
	{
		char body[] = "\n" END_LINE;
		switch_msrp_msg_t *msg = parse_body(body, (int)strlen(body));

		fst_requires(msg);
		fst_check(msg->payload_bytes == 0);
		fst_check(msg->last_p == body + strlen(body));
		switch_msrp_msg_destroy(&msg);
	}
	FST_TEST_END()

	FST_TEST_BEGIN(end_line_then_next_message)
	{
		char body[] = END_LINE "MSRP tid2 SEND\r\nTo-Path: msrp://127.0.0.1:2855/a;tcp\r\n";
		switch_msrp_msg_t *msg = parse_body(body, (int)strlen(body));

		fst_requires(msg);
		fst_check(msg->payload_bytes == 0);
		fst_check(msg->last_p == body + strlen(END_LINE));
		switch_msrp_msg_destroy(&msg);
	}
	FST_TEST_END()

	FST_TEST_BEGIN(partial_end_line_waits)
	{
		char body[] = "-------ti";
		switch_msrp_msg_t *msg = wait_body_msg();

		fst_requires(msg);
		msg = msrp_parse_buffer(body, (int)strlen(body), msg, test_pool);
		fst_check(msg->state == MSRP_ST_WAIT_BODY);
		fst_check(msg->last_p == body);
		switch_msrp_msg_destroy(&msg);
	}
	FST_TEST_END()

	FST_TEST_BEGIN(end_line_without_its_lf_waits)
	{
		char body[] = "\n-------tid1$\r";
		switch_msrp_msg_t *msg = wait_body_msg();

		fst_requires(msg);
		msg = msrp_parse_buffer(body, (int)strlen(body), msg, test_pool);
		fst_check(msg->state == MSRP_ST_WAIT_BODY);
		fst_check(msg->last_p == body);
		switch_msrp_msg_destroy(&msg);
	}
	FST_TEST_END()
}
FST_SUITE_END()
}
FST_MINCORE_END()
