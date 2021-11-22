#include <mod_happy_voicemail.h>

#include <stdio.h>
#include <curl/curl.h>
#include <sys/stat.h>
#include <fcntl.h>

SWITCH_DECLARE(switch_status_t) hv_http_upload_from_disk(const char *file_name, hv_http_req_t *req)
{
	CURL *curl = NULL;
	CURLcode res = 0;
	struct stat file_info = { 0 };
	curl_off_t speed_upload = 0, total_time = 0;
	FILE *fd = NULL;
	long http_code = 0;

	if (!req) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "PUT: bad params\n");
		goto fail;
	}

	if (zstr(file_name)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "PUT: file name missing\n");
		goto fail;
	}

	if (zstr(req->url) && !req->use_s3_auth) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "PUT: url missing\n");
		goto fail;
	}

	fd = fopen(file_name, "rb");
	if (!fd) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "PUT: cannot open %s\n", file_name);
		goto fail;
	}

	if (fstat(fileno(fd), &file_info) != 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "PUT: fstat failed\n");
		goto fail;
	}

	res = curl_global_init(CURL_GLOBAL_ALL);
	if (res != CURLE_OK) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "curl_global_init() failed: %s\n", curl_easy_strerror(res));
		goto fail;
	}

	curl = curl_easy_init();
	if (!curl) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "PUT: cannot init CURL\n");
		goto fail;
	}

	if (req->use_s3_auth) {
		if (SWITCH_STATUS_SUCCESS != hv_http_add_s3_authentication(curl, req, "PUT")) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "PUT: Cannot obtain S3 authentication string\n");
			goto fail;
		}
	} else {
		curl_easy_setopt(curl, CURLOPT_URL, req->url);
	}

	curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
	curl_easy_setopt(curl, CURLOPT_READDATA, fd);
	curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t) file_info.st_size);
	curl_easy_setopt(curl, CURLOPT_VERBOSE, 0);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "-> PUT (%s) to (%s)\n", file_name, req->url);

	res = curl_easy_perform(curl);
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
	if (res != CURLE_OK) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "PUT (%s): curl_easy_perform() failed, HTTP code: %lu (%s)\n", req->url, http_code, curl_easy_strerror(res));
		goto fail;
	}

	if (http_code != 200) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "PUT (%s): failed, HTTP code: %lu (%s)\n", req->url, http_code, curl_easy_strerror(res));
		goto fail;
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "-> PUT (%s): resulted with HTTP code %lu\n", req->url, http_code);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "-> PUT (%s): %lu bytes uploaded\n", req->url, (unsigned long) file_info.st_size);

	curl_easy_getinfo(curl, CURLINFO_SPEED_UPLOAD_T, &speed_upload);
	curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME_T, &total_time);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Done. Upload speed: %" CURL_FORMAT_CURL_OFF_T " bytes/sec during %" CURL_FORMAT_CURL_OFF_T ".%06ld seconds\n", speed_upload, (total_time / 1000000), (long)(total_time % 1000000));

	curl_easy_cleanup(curl);
	curl_global_cleanup();
	fclose(fd);

	return SWITCH_STATUS_SUCCESS;

fail:

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Upload failed (%s to %s)\n", file_name, req->url);

	if (fd) {
		fclose(fd);
	}

	if (curl) {
		curl_easy_cleanup(curl);
	}

	return SWITCH_STATUS_FALSE;
}

static size_t hv_http_read_memory_callback(void *ptr, size_t size, size_t nmemb, void *userp)
{
	hv_http_req_t *upload = (hv_http_req_t *) userp;
	size_t max = size*nmemb;

	if (max < 1)
		return 0;

	if (upload->sizeleft) {
		size_t copylen = max;
		if (copylen > upload->sizeleft) {
			copylen = upload->sizeleft;
		}
		memcpy(ptr, upload->memory, copylen);
		upload->memory += copylen;
		upload->sizeleft -= copylen;
		return copylen;
	}

	return 0;
}

SWITCH_DECLARE(switch_status_t) hv_http_upload_from_mem(hv_http_req_t *upload)
{
	CURLcode res = 0;
	CURL *curl = NULL;
	curl_off_t speed_upload = 0, total_time = 0;
	long http_code = 0;
	uint32_t sz = 0;

	if (!upload || zstr(upload->url) || !upload->sizeleft) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "PUT: bad params\n");
		goto fail;
	}

	sz = upload->sizeleft;

	res = curl_global_init(CURL_GLOBAL_DEFAULT);
	if (res != CURLE_OK) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "curl_global_init() failed: %s\n", curl_easy_strerror(res));
		return SWITCH_STATUS_FALSE;
	}

	curl = curl_easy_init();
	if (!curl) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "PUT: cannot init CURL\n");
		goto fail;
	}

	if (upload->use_s3_auth) {
		if (SWITCH_STATUS_SUCCESS != hv_http_add_s3_authentication(curl, upload, "PUT")) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "PUT: Cannot obtain S3 authentication string\n");
			goto fail;
		}
	} else {
		curl_easy_setopt(curl, CURLOPT_URL, upload->url);
	}

	curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
	curl_easy_setopt(curl, CURLOPT_READFUNCTION, hv_http_read_memory_callback);
	curl_easy_setopt(curl, CURLOPT_READDATA, upload);
	curl_easy_setopt(curl, CURLOPT_VERBOSE, 0);
	curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t) upload->sizeleft);

	res = curl_easy_perform(curl);
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
	if (res != CURLE_OK) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "PUT (%s): curl_easy_perform() failed, HTTP code: %lu (%s)\n", upload->url, http_code, curl_easy_strerror(res));
		goto fail;
	}

	if (http_code != 200) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "PUT (%s): failed, HTTP code: %lu (%s)\n", upload->url, http_code, curl_easy_strerror(res));
		goto fail;
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "-> PUT (%s): resulted with HTTP code %lu\n", upload->url, http_code);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "-> PUT (%s): %u bytes uploaded\n", upload->url, sz);

	curl_easy_getinfo(curl, CURLINFO_SPEED_UPLOAD_T, &speed_upload);
	curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME_T, &total_time);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "-> PUT (%s): speed: %" CURL_FORMAT_CURL_OFF_T " bytes/sec during %" CURL_FORMAT_CURL_OFF_T ".%06ld seconds\n", upload->url, speed_upload, (total_time / 1000000), (long)(total_time % 1000000));

	curl_easy_cleanup(curl);
	curl_global_cleanup();
	return SWITCH_STATUS_SUCCESS;

fail:
	return SWITCH_STATUS_FALSE;
}

SWITCH_DECLARE(void) hv_http_req_destroy(hv_http_req_t *req)
{
	if (!req) return;
	if (req->memory) {
		free(req->memory);
		req->memory = NULL;
	}
	req->size = 0;
	memset(req, 0, sizeof(hv_http_req_t));
}

static size_t hv_http_write_memory_callback(void *contents, size_t size, size_t nmemb, void *userp)
{
	size_t realsize = size * nmemb;
	hv_http_req_t *req = (hv_http_req_t *) userp;

	char *ptr = realloc(req->memory, req->size + realsize + 1);
	if(!ptr) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "MEM\n");
		return 0;
	}

	req->memory = ptr;
	memcpy(&(req->memory[req->size]), contents, realsize);
	req->size += realsize;
	req->memory[req->size] = 0;

	return realsize;
}

SWITCH_DECLARE(switch_status_t) hv_http_get_to_mem(hv_http_req_t *req)
{
	CURL *curl_handle;
	CURLcode res;
	long http_code = 0;

	if (!req) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "GET: bad params\n");
		goto fail;
	}

	if (zstr(req->url)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "GET: empty URL\n");
		goto fail;
	}

	req->memory = malloc(1);  /* will be grown as needed by the realloc above */
	req->size = 0;    /* no data at this point */

	curl_global_init(CURL_GLOBAL_ALL);
	curl_handle = curl_easy_init();
	if (!curl_handle) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "GET: cannot init CURL\n");
		goto fail;
	}

	if (req->use_s3_auth) {
		if (SWITCH_STATUS_SUCCESS != hv_http_add_s3_authentication(curl_handle, req, "GET")) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "GET: Cannot obtain S3 authentication string\n");
			goto fail;
		}
	} else {
		curl_easy_setopt(curl_handle, CURLOPT_URL, req->url);
	}

	curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, hv_http_write_memory_callback);
	curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *) req);
	curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "libcurl-agent/1.0");

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "-> GET (%s) to mem\n", req->url);
	res = curl_easy_perform(curl_handle);

	curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &http_code);
	req->http_code = http_code;
	if (res != CURLE_OK) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "GET (%s): curl_easy_perform() failed, HTTP code: %lu (%s)\n", req->url, http_code, curl_easy_strerror(res));
		goto fail;
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "-> GET (%s): resulted with HTTP code %lu\n", req->url, http_code);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "-> GET (%s): %lu bytes retrieved\n", req->url, (unsigned long) req->size);

	curl_easy_cleanup(curl_handle);
	curl_global_cleanup();

	return SWITCH_STATUS_SUCCESS;

fail:
	if (curl_handle) {
		curl_easy_cleanup(curl_handle);
		curl_global_cleanup();
	}
	return SWITCH_STATUS_FALSE;
}

SWITCH_DECLARE(switch_status_t) hv_http_get_to_disk(hv_http_req_t *req, const char *file_name)
{
	FILE *f = NULL;

	if (!req || zstr(file_name)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "GET: bad params\n");
		return SWITCH_STATUS_FALSE;
	}

	if (SWITCH_STATUS_SUCCESS != hv_http_get_to_mem(req)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "GET: failed to download (%s)\n", req->url);
		return SWITCH_STATUS_FALSE;
	}

	f = fopen(file_name,"w+b");
	if (!f) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "GET: failed to open file for writing (%s)\n", file_name);
		return SWITCH_STATUS_FALSE;
	}

	if (req->size != fwrite(req->memory, 1, req->size, f)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "GET: failed to write into file (%s)\n", file_name);
		return SWITCH_STATUS_FALSE;
	}

	fclose(f);
	fflush(f);
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_DECLARE(switch_status_t) hv_http_delete(hv_http_req_t *req)
{
	CURL *curl_handle = NULL;
	CURLcode res = 0;
	long http_code = 0;

	if (!req) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "GET: bad params\n");
		return SWITCH_STATUS_FALSE;
	}

	if (zstr(req->url) && !req->use_s3_auth) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "GET: empty URL\n");
		return SWITCH_STATUS_FALSE;
	}

	curl_global_init(CURL_GLOBAL_ALL);
	curl_handle = curl_easy_init();
	if (!curl_handle) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "GET: cannot init CURL\n");
		return SWITCH_STATUS_FALSE;
	}

	if (req->use_s3_auth) {
		if (SWITCH_STATUS_SUCCESS != hv_http_add_s3_authentication(curl_handle, req, "DELETE")) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "DELETE: Cannot obtain S3 authentication string\n");
			return SWITCH_STATUS_FALSE;
		}
	} else {
		curl_easy_setopt(curl_handle, CURLOPT_URL, req->url);
	}

	curl_easy_setopt(curl_handle, CURLOPT_CUSTOMREQUEST, "DELETE");
	curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "libcurl-agent/1.0");

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "-> DELETE (%s)\n", req->url);
	res = curl_easy_perform(curl_handle);

	curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &http_code);
	req->http_code = http_code;
	if (res != CURLE_OK) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "GET (%s): curl_easy_perform() failed, HTTP code: %lu (%s)\n", req->url, http_code, curl_easy_strerror(res));
		goto fail;
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "-> DELETE (%s): resulted with HTTP code %lu\n", req->url, http_code);

	curl_easy_cleanup(curl_handle);
	curl_global_cleanup();

	return SWITCH_STATUS_SUCCESS;

fail:
	if (curl_handle) {
		curl_easy_cleanup(curl_handle);
		curl_global_cleanup();
	}
	return SWITCH_STATUS_FALSE;
}

SWITCH_DECLARE(switch_status_t) hv_http_add_s3_authentication(CURL *h, hv_http_req_t *req, const char *method)
{
	char *s3_auth = NULL, *url = NULL;
	int len = 0;

	if (!h || !req) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Bad params\n");
		return SWITCH_STATUS_FALSE;
	}

	s3_auth = hv_s3_authentication_create(req, method);
	if (zstr(s3_auth)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed to create S3 authentication string\n");
		return SWITCH_STATUS_FALSE;
	}

	url = switch_mprintf("%s?%s", req->url, s3_auth);
	if (zstr(url)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed to create URL with S3 authentication string\n");
		free(s3_auth);
		return SWITCH_STATUS_FALSE;
	}

	len = sizeof(req->url);
	strncpy(req->url, url, len);
	req->url[len - 1] = '\0';

	curl_easy_setopt(h, CURLOPT_URL, url);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Request with S3 authentication string: %s\n", url);

	free(s3_auth);
	switch_safe_free(url);
	return SWITCH_STATUS_SUCCESS;
}
