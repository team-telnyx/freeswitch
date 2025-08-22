#include <switch.h>
#include <test/switch_test.h>

// #define BENCHMARK 1

typedef struct {
    switch_event_t *event;
    switch_mutex_t *mutex;
    int fire_count;
    int destroy_count;
    volatile int should_stop;
    volatile int race_detected;
} race_test_data_t;

static void *SWITCH_THREAD_FUNC fire_thread_func(switch_thread_t *thread, void *obj)
{
    race_test_data_t *data = (race_test_data_t *)obj;
    int iterations = 0;

    while (!data->should_stop && iterations < 1000) {
        switch_mutex_lock(data->mutex);

        if (data->event) {
            switch_event_t *event_copy = NULL;

            // Try to duplicate the event before firing
            if (switch_event_dup(&event_copy, data->event) == SWITCH_STATUS_SUCCESS) {
                data->fire_count++;
                switch_mutex_unlock(data->mutex);

                // Fire the event copy - this should be safe
                switch_event_fire(&event_copy);
            } else {
                switch_mutex_unlock(data->mutex);
            }
        } else {
            switch_mutex_unlock(data->mutex);
        }

        iterations++;
        switch_yield(1000); // 1ms
    }

    return NULL;
}

static void *SWITCH_THREAD_FUNC destroy_thread_func(switch_thread_t *thread, void *obj)
{
    race_test_data_t *data = (race_test_data_t *)obj;
    int iterations = 0;

    while (!data->should_stop && iterations < 500) {
        switch_mutex_lock(data->mutex);

        if (data->event) {
            // Try to destroy and recreate event
            switch_event_destroy(&data->event);
            data->destroy_count++;

            // Recreate event for next iteration
            if (switch_event_create(&data->event, SWITCH_EVENT_CUSTOM) == SWITCH_STATUS_SUCCESS) {
                switch_event_add_header_string(data->event, SWITCH_STACK_BOTTOM, "Test-Header", "race-test");
                switch_event_add_header_string(data->event, SWITCH_STACK_BOTTOM, "Event-Subclass", "race::test");
            }
        }

        switch_mutex_unlock(data->mutex);
        iterations++;
        switch_yield(2000); // 2ms
    }

    return NULL;
}

static void *SWITCH_THREAD_FUNC unsafe_fire_thread_func(switch_thread_t *thread, void *obj)
{
    race_test_data_t *data = (race_test_data_t *)obj;
    int iterations = 0;

    while (!data->should_stop && iterations < 100) {
        if (data->event) {
            switch_event_t *event_to_fire = data->event;
            data->fire_count++;

            // This is UNSAFE - we're firing an event that might be destroyed
            // by another thread at the same time
            switch_event_fire(&event_to_fire);
        }
        iterations++;
        switch_yield(1000); // 1ms
    }
    return NULL;
}

static void *SWITCH_THREAD_FUNC unsafe_destroy_thread_func(switch_thread_t *thread, void *obj)
{
    race_test_data_t *data = (race_test_data_t *)obj;
    int iterations = 0;

    while (!data->should_stop && iterations < 50) {
        if (data->event) {
            // This is UNSAFE - we're destroying an event that might be
            // in use by the fire thread
            switch_event_destroy(&data->event);
            data->destroy_count++;

            // Recreate event
            if (switch_event_create(&data->event, SWITCH_EVENT_CUSTOM) == SWITCH_STATUS_SUCCESS) {
                switch_event_add_header_string(data->event, SWITCH_STACK_BOTTOM, "Test-Header", "race-test-unsafe");
                switch_event_add_header_string(data->event, SWITCH_STACK_BOTTOM, "Event-Subclass", "race::test::unsafe");
            }
        }
        iterations++;
        switch_yield(2000); // 2ms
    }
    return NULL;
}

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

FST_TEST_BEGIN(race_fire_destroy)
{
    race_test_data_t test_data = {0};
    switch_thread_t *fire_thread = NULL;
    switch_thread_t *destroy_thread = NULL;
    switch_threadattr_t *thd_attr = NULL;
    switch_memory_pool_t *pool = NULL;
    switch_status_t status;
	switch_status_t fire_retval, destroy_retval;

    // Create pool and mutex
    switch_core_new_memory_pool(&pool);
    switch_mutex_init(&test_data.mutex, SWITCH_MUTEX_NESTED, pool);
    
    // Create initial event
    status = switch_event_create(&test_data.event, SWITCH_EVENT_CUSTOM);
    fst_requires(status == SWITCH_STATUS_SUCCESS);
    
    switch_event_add_header_string(test_data.event, SWITCH_STACK_BOTTOM, "Test-Header", "race-test");
    switch_event_add_header_string(test_data.event, SWITCH_STACK_BOTTOM, "Event-Subclass", "race::test");
    
    // Create thread attributes
    switch_threadattr_create(&thd_attr, pool);
    switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);
    
    // Start fire thread
    status = switch_thread_create(&fire_thread, thd_attr, fire_thread_func, &test_data, pool);
    fst_requires(status == SWITCH_STATUS_SUCCESS);
    
    // Start destroy thread  
    status = switch_thread_create(&destroy_thread, thd_attr, destroy_thread_func, &test_data, pool);
    fst_requires(status == SWITCH_STATUS_SUCCESS);
    
    // Let threads run for 2 seconds
    switch_sleep(2000000); // 2 seconds
    
    // Signal threads to stop
    test_data.should_stop = 1;
    
    // Wait for threads to finish
    switch_thread_join(&fire_retval, fire_thread);
    switch_thread_join(&destroy_retval, destroy_thread);
    
    // Clean up remaining event
    switch_mutex_lock(test_data.mutex);
    if (test_data.event) {
        switch_event_destroy(&test_data.event);
    }
    switch_mutex_unlock(test_data.mutex);
    
    printf("Race test results: fired %d times, destroyed %d times\n", 
           test_data.fire_count, test_data.destroy_count);
    
    // Test should complete without crashing
    fst_check(test_data.fire_count > 0);
    fst_check(test_data.destroy_count > 0);
    
    switch_core_destroy_memory_pool(&pool);
}
FST_TEST_END()

FST_TEST_BEGIN(race_fire_destroy_unsafe)
{
    race_test_data_t test_data = {0};
    switch_thread_t *fire_thread = NULL;
    switch_thread_t *destroy_thread = NULL;
    switch_threadattr_t *thd_attr = NULL;
    switch_memory_pool_t *pool = NULL;
    switch_status_t status;
	switch_status_t fire_retval, destroy_retval;

    // Create pool - NO MUTEX for this test to force race condition
    switch_core_new_memory_pool(&pool);
    
    // Create initial event
    status = switch_event_create(&test_data.event, SWITCH_EVENT_CUSTOM);
    fst_requires(status == SWITCH_STATUS_SUCCESS);
    
    switch_event_add_header_string(test_data.event, SWITCH_STACK_BOTTOM, "Test-Header", "race-test-unsafe");
    switch_event_add_header_string(test_data.event, SWITCH_STACK_BOTTOM, "Event-Subclass", "race::test::unsafe");
    
    // Create thread attributes
    switch_threadattr_create(&thd_attr, pool);
    switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);
    
    // Start unsafe threads
    status = switch_thread_create(&fire_thread, thd_attr, unsafe_fire_thread_func, &test_data, pool);
    fst_requires(status == SWITCH_STATUS_SUCCESS);
    
    status = switch_thread_create(&destroy_thread, thd_attr, unsafe_destroy_thread_func, &test_data, pool);
    fst_requires(status == SWITCH_STATUS_SUCCESS);
    
    // Let threads run for 1 second (shorter due to unsafe nature)
    switch_sleep(1000000); // 1 second
    
    // Signal threads to stop
    test_data.should_stop = 1;
    
    // Wait for threads to finish
    switch_thread_join(&fire_retval, fire_thread);
    switch_thread_join(&destroy_retval, destroy_thread);
    
    // Clean up remaining event if it exists
    if (test_data.event) {
        switch_event_destroy(&test_data.event);
    }
    
    printf("Unsafe race test results: fired %d times, destroyed %d times\n", 
           test_data.fire_count, test_data.destroy_count);
    
    // Test should complete - this might crash due to race conditions
    fst_check(test_data.fire_count > 0);
    fst_check(test_data.destroy_count > 0);
    
    switch_core_destroy_memory_pool(&pool);
}
FST_TEST_END()

FST_SUITE_END()

FST_MINCORE_END()



