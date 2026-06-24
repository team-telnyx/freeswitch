#include <switch.h>
#include <test/switch_test.h>

// #define BENCHMARK 1

FST_MINCORE_BEGIN("./conf")

FST_SUITE_BEGIN(switch_event)

FST_SETUP_BEGIN()
{
}
FST_SETUP_END()

FST_TEARDOWN_BEGIN()
{
}
FST_TEARDOWN_END()

FST_TEST_BEGIN(benchmark)
{
  switch_event_t *event = NULL;
  switch_time_t start_ts, end_ts;
  int loops = 10, x = 0;
  switch_status_t status = SWITCH_STATUS_SUCCESS;
  char **index = NULL;
  uint64_t micro_total = 0;
  double micro_per = 0;
  double rate_per_sec = 0;

#ifdef BENCHMARK
  switch_time_t small_start_ts, small_end_ts;
#endif

  index = calloc(loops, sizeof(char *));
  for ( x = 0; x < loops; x++) {
    index[x] = switch_mprintf("%d", x);
  }

  /* START LOOPS */
  start_ts = switch_time_now();
  
  status = switch_event_create(&event, SWITCH_EVENT_MESSAGE);
  fst_xcheck(status == SWITCH_STATUS_SUCCESS, "Failed to create event");

#ifndef BENCHMARK
  for ( x = 0; x < loops; x++) {
    status = switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, index[x], index[x]);
    fst_xcheck(status == SWITCH_STATUS_SUCCESS, "Failed to add header to event");
  }
#else 
  small_start_ts = switch_time_now();
  for ( x = 0; x < loops; x++) {
    if ( switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, index[x], index[x]) != SWITCH_STATUS_SUCCESS) {
      fst_fail("Failed to add header to event");
    }
  }
  small_end_ts = switch_time_now();

  micro_total = small_end_ts - small_start_ts;
  micro_per = micro_total / (double) loops;
  rate_per_sec = 1000000 / micro_per;
  printf("switch_event add_header: Total %" SWITCH_UINT64_T_FMT "us / %d loops, %.2f us per loop, %.0f loops per second\n",
       micro_total, loops, micro_per, rate_per_sec);
#endif


#ifndef BENCHMARK
  for ( x = 0; x < loops; x++) {
    fst_check_string_equals(switch_event_get_header(event, index[x]), index[x]);
  } 
#else 
  small_start_ts = switch_time_now();
  for ( x = 0; x < loops; x++) {
    if ( !switch_event_get_header(event, index[x])) {
      fst_fail("Failed to lookup event header value");
    }
  }
  small_end_ts = switch_time_now();

  micro_total = small_end_ts - small_start_ts;
  micro_per = micro_total / (double) loops;
  rate_per_sec = 1000000 / micro_per;
  printf("switch_event get_header: Total %" SWITCH_UINT64_T_FMT "us / %d loops, %.2f us per loop, %.0f loops per second\n", 
       micro_total, loops, micro_per, rate_per_sec);
#endif

  switch_event_destroy(&event);
  /* END LOOPS */
  
  end_ts = switch_time_now();

  for ( x = 0; x < loops; x++) {
    free(index[x]);
  }
  free(index);

  micro_total = end_ts - start_ts;
  micro_per = micro_total / (double) loops;
  rate_per_sec = 1000000 / micro_per;
  printf("switch_event Total %" SWITCH_UINT64_T_FMT "us / %d loops, %.2f us per loop, %.0f loops per second\n", 
       micro_total, loops, micro_per, rate_per_sec);

}
FST_TEST_END()

/*
 * Regression test for TELCORE-193: assertion failure in switch_event_merge().
 *
 * Production stack (release build):
 *   __assert_fail()
 *   switch_event_merge()                       switch_event.c:1322
 *   conference_event_add_data_with_member()    conference_event.c:776
 *   conference_event_add_data()                conference_event.c
 *   conference_record_thread_run()             conference_record.c:320
 *
 * Root cause
 * ----------
 * conference_event_add_data_with_member() finishes with:
 *
 *     switch_event_merge(event, conference->variables);
 *
 * switch_event_merge() opens with switch_assert(tomerge && event). The record
 * thread reads conference->variables WITHOUT re-checking CFLAG_RUNNING after it
 * takes the conference rwlock (conference_record.c:173). The conference teardown
 * thread clears CFLAG_RUNNING, momentarily takes+releases the write lock to
 * "drain readers", then destroys conference->variables (mod_conference.c:891),
 * which NULLs the pointer. A record thread that grabs its read lock just after
 * the write lock is released sails straight into a half-torn-down conference and
 * passes that now-NULL pointer as `tomerge` -> the assert fires -> abort().
 *
 * What this test does
 * -------------------
 * conference internals are not exported from mod_conference, so a tests/unit
 * binary cannot drive conference_record_thread_run() directly. Instead it
 * reproduces the exact faulting condition switch_event_merge() actually sees:
 * a fully populated destination `event` (the CONF maintenance event the record
 * thread has already filled in) and a NULL `tomerge` (conference->variables
 * after the teardown destroyed it).
 *
 * Expected:
 *   - BUGGY tree : switch_assert(tomerge && event) fires -> SIGABRT here.
 *   - FIXED tree : switch_event_merge() tolerates the NULL source, returns
 *                  without touching `event`, and the test passes.
 */
FST_TEST_BEGIN(merge_null_variables_after_teardown)
{
  switch_event_t *event = NULL;
  switch_status_t status = SWITCH_STATUS_SUCCESS;

  /* The CONF maintenance event the record thread has already populated before
   * reaching switch_event_merge(event, conference->variables). */
  status = switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, "conference::maintenance");
  fst_xcheck(status == SWITCH_STATUS_SUCCESS, "Failed to create event");

  switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Conference-Name", "3001");
  switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Action", "start-recording");

  /* conference->variables after the teardown thread destroyed it: NULL. On a
   * buggy tree switch_assert(tomerge && event) aborts the process right here;
   * on a fixed tree switch_event_merge() bails and leaves `event` untouched. */
  switch_event_merge(event, NULL);

  /* Only reached on a fixed tree: the destination event must be untouched. */
  fst_xcheck(!zstr(switch_event_get_header(event, "Conference-Name")),
             "Destination event headers must survive a NULL-source merge");
  fst_xcheck(!zstr(switch_event_get_header(event, "Action")),
             "Destination event headers must survive a NULL-source merge");

  switch_event_destroy(&event);
}
FST_TEST_END()

FST_SUITE_END()

FST_MINCORE_END()



