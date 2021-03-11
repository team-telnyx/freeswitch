#include <map>
#include <string>
#include "prometheus_metrics.h"


class prometheus_metrics
{
public:
	class auto_lock
	{
	public:
		auto_lock(switch_mutex_t* mutex) : _mutex(mutex)
		{
			switch_mutex_lock(_mutex);
		}
		~auto_lock()
		{
			switch_mutex_unlock(_mutex);
		}
		switch_mutex_t* _mutex;
	};
	
	prometheus_metrics(switch_loadable_module_interface_t **module_interface, switch_memory_pool_t* pool) :
		_pool(pool),
		_module_interface(module_interface),
		_download_fail_count(0),
		_download_bucker_0_299(0),
		_download_bucker_300_499(0),
		_download_bucker_500_799(0),
		_download_bucker_800_1299(0),
		_download_bucker_1300_2099(0),
		_download_bucker_2100_3399(0),
		_download_bucker_3400_5499(0),
		_download_bucker_5500_inf(0),
		_download_bucker_sum(0),
		_download_bucker_count(0)
	{
		switch_mutex_init(&_mutex, SWITCH_MUTEX_NESTED, _pool);
	}
	
	~prometheus_metrics()
	{
		switch_mutex_destroy(_mutex);
	}

	void increment_download_duration(unsigned int duration)
	{
		auto_lock lock(_mutex);

		_download_bucker_sum += duration;
		_download_bucker_count++;

		if (duration < 300) {
			_download_bucker_0_299++;
		} else if (300 <= duration && duration < 500) {
			_download_bucker_300_499++;
		} else if (500 <= duration && duration < 799) {
			_download_bucker_500_799++;
		} else if (800 <= duration && duration < 1299) {
			_download_bucker_800_1299++;
		} else if (1300 <= duration && duration < 2099) {
			_download_bucker_1300_2099++;
		} else if (2100 <= duration && duration < 3399) {
			_download_bucker_2100_3399++;
		} else if (3400 <= duration && duration < 5499) {
			_download_bucker_3400_5499++;
		} else {
			_download_bucker_5500_inf++;
		} 
	}

	void increment_download_fail_count(void)
	{
		auto_lock lock(_mutex);
		_download_fail_count++;
	}
		
	void generate_metrics(switch_stream_handle_t *stream)
	{
		auto_lock lock(_mutex);
		stream->write_function(stream, "# HELP mod_http_cache_download_fail_count\n");
		stream->write_function(stream, "# TYPE mod_http_cache_download_fail_count counter\n");
		stream->write_function(stream, "mod_http_cache_download_fail_count %u\n", _download_fail_count);

		stream->write_function(stream, "# HELP mod_http_cache_download\n");
		stream->write_function(stream, "# TYPE mod_http_cache_download histogram\n");
		stream->write_function(stream, "mod_http_cache_download_duration_bucket{le=\"300\"} %u\n", _download_bucker_0_299);
		stream->write_function(stream, "mod_http_cache_download_duration_bucket{le=\"500\"} %u\n", _download_bucker_300_499);
		stream->write_function(stream, "mod_http_cache_download_duration_bucket{le=\"800\"} %u\n", _download_bucker_500_799);
		stream->write_function(stream, "mod_http_cache_download_duration_bucket{le=\"1300\"} %u\n", _download_bucker_800_1299);
		stream->write_function(stream, "mod_http_cache_download_duration_bucket{le=\"2100\"} %u\n", _download_bucker_1300_2099);
		stream->write_function(stream, "mod_http_cache_download_duration_bucket{le=\"3400\"} %u\n", _download_bucker_2100_3399);
		stream->write_function(stream, "mod_http_cache_download_duration_bucket{le=\"5500\"} %u\n", _download_bucker_3400_5499);
		stream->write_function(stream, "mod_http_cache_download_duration_bucket{le=\"+Inf\"} %u\n", _download_bucker_5500_inf);
		stream->write_function(stream, "mod_http_cache_download_duration_sum %llu\n", _download_bucker_sum);
		stream->write_function(stream, "mod_http_cache_download_duration_count %u\n", _download_bucker_count);
	}
	

private:
	switch_memory_pool_t* _pool;
	switch_loadable_module_interface_t **_module_interface;
	switch_mutex_t* _mutex;

	unsigned int _download_fail_count;

	// counters for download duration
	unsigned int _download_bucker_0_299;		// counter for 0 ~ 299 ms
	unsigned int _download_bucker_300_499;		// counter for 300 ~ 499 ms
	unsigned int _download_bucker_500_799;		// counter for 500 ~ 799 ms
	unsigned int _download_bucker_800_1299;		// counter for 800 ~ 1299 ms
	unsigned int _download_bucker_1300_2099;	// counter for 1300 ~ 2099 ms
	unsigned int _download_bucker_2100_3399;	// counter for 2100 ~ 3399 ms
	unsigned int _download_bucker_3400_5499;	// counter for 3400 ~ 5499 ms
	unsigned int _download_bucker_5500_inf;		// counter for 5500 ~ inf
	unsigned long long int _download_bucker_sum;
	unsigned long int _download_bucker_count;
};

static prometheus_metrics* instance = 0;


SWITCH_STANDARD_API(http_cache_prometheus_metrics)
{
	instance->generate_metrics(stream);
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_BEGIN_EXTERN_C

void prometheus_init(switch_loadable_module_interface_t **module_interface, switch_api_interface_t *api_interface, switch_memory_pool_t* pool)
{
	SWITCH_ADD_API(api_interface, "http_cache_prometheus_metrics", "http_cache_prometheus_metrics", http_cache_prometheus_metrics, "");
	
	delete instance;
	instance = new prometheus_metrics(module_interface, pool);
}

void prometheus_destroy()
{
	delete instance;
	instance = 0;
}

void prometheus_increment_download_duration(unsigned int duration)
{
	instance->increment_download_duration(duration);
}

void prometheus_increment_download_fail_count()
{
	instance->increment_download_fail_count();
}

SWITCH_END_EXTERN_C


