#include <mod_happy_voicemail.h>

#include <stdio.h>
#include <curl/curl.h>
#include <sys/stat.h>
#include <fcntl.h>
 
SWITCH_DECLARE(switch_status_t) hv_http_upload_from_disk(const char *file_name, const char *url)
{
  CURL *curl = NULL;
  CURLcode res = 0;
  struct stat file_info = { 0 };
  curl_off_t speed_upload = 0, total_time = 0;
  FILE *fd = NULL;

  if (zstr(file_name)) {
	  goto fail;
  }

  if (zstr(url)) {
	  goto fail;
  }

  fd = fopen(file_name, "rb");
  if (!fd) {
    goto fail;
  }
 
  if (fstat(fileno(fd), &file_info) != 0) {
	  goto fail;
  }
 
  curl = curl_easy_init();
  if (!curl) {
	  goto fail;
  }

  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
  curl_easy_setopt(curl, CURLOPT_READDATA, fd);
  curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t) file_info.st_size);
  curl_easy_setopt(curl, CURLOPT_VERBOSE, 0);

  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Uploading %s to %s\n", file_name, url);

  res = curl_easy_perform(curl);
  if (res != CURLE_OK) {
	  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
	  goto fail;
  }

  curl_easy_getinfo(curl, CURLINFO_SPEED_UPLOAD_T, &speed_upload);
  curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME_T, &total_time);
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Done. Upload speed: %" CURL_FORMAT_CURL_OFF_T " bytes/sec during %" CURL_FORMAT_CURL_OFF_T ".%06ld seconds\n", speed_upload, (total_time / 1000000), (long)(total_time % 1000000));

  curl_easy_cleanup(curl);
  fclose(fd);

  return SWITCH_STATUS_SUCCESS;

fail:

  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Upload failed (%s to %s)\n", file_name, url);

  if (fd) {
	  fclose(fd);
  }

  if (curl) {
	  curl_easy_cleanup(curl);
  }

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
    printf("not enough memory (realloc returned NULL)\n");
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

  if (!req) {
	  goto fail;
  }

  req->memory = malloc(1);  /* will be grown as needed by the realloc above */
  req->size = 0;    /* no data at this point */

  curl_global_init(CURL_GLOBAL_ALL);
  curl_handle = curl_easy_init();

  curl_easy_setopt(curl_handle, CURLOPT_URL, "http://happy-voicemail.s3.amazonaws.com/111112/1e1d56b9-ab8f-4240-9542-d4fb96aa87c0.wav");
  curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, hv_http_write_memory_callback);
  curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *) req);
  curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "libcurl-agent/1.0");

  res = curl_easy_perform(curl_handle);

  if(res != CURLE_OK) {
    fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
	goto fail;
  }

    /*
     * Now, our chunk.memory points to a memory block that is chunk.size
     * bytes big and contains the remote file.
     *
     * Do something nice with it!
     */

    printf("%lu bytes retrieved\n", (unsigned long) req->size);


  curl_easy_cleanup(curl_handle);
  curl_global_cleanup();

  return SWITCH_STATUS_SUCCESS;

fail:
  return SWITCH_STATUS_FALSE;
}
