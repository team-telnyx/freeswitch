#include <mod_happy_voicemail.h>

#include <openssl/hmac.h>
#include <openssl/sha.h>

#define HV_S3_DATE_STAMP_LENGTH 9		// 20210802
#define HV_S3_TIME_STAMP_LENGTH 17		// 20210802T095214Z


static char *hmac256(char* buffer, unsigned int buffer_length, const char* key, unsigned int key_length, const char* message)
{
	if (zstr(key) || zstr(message) || buffer_length < SHA256_DIGEST_LENGTH) {
		return NULL;
	}

	HMAC(EVP_sha256(),
			key,
			(int)key_length,
			(unsigned char *)message,
			strlen(message),
			(unsigned char*)buffer,
			&buffer_length);

	return (char*)buffer;
}

static char *hmac256_hex(char* buffer, const char* key, unsigned int key_length, const char* message)
{
	char hmac256_raw[SHA256_DIGEST_LENGTH] = { 0 };

	if (hmac256(hmac256_raw, SHA256_DIGEST_LENGTH, key, key_length, message) == NULL) {
		return NULL;
	}

	for (unsigned int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
		snprintf(buffer + i*2, 3, "%02x", (unsigned char)hmac256_raw[i]);
	}
	buffer[SHA256_DIGEST_LENGTH * 2] = '\0';

	return buffer;
}

static char *sha256_hex(char* buffer, const char* string)
{
	unsigned char sha256_raw[SHA256_DIGEST_LENGTH] = { 0 };

	SHA256((unsigned char*)string, strlen(string), sha256_raw);

	for (unsigned int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
		snprintf(buffer + i*2, 3, "%02x", sha256_raw[i]);
	}
	buffer[SHA256_DIGEST_LENGTH * 2] = '\0';

	return buffer;
}

static char *get_time(char* format, char* buffer, unsigned int buffer_length)
{
	switch_time_exp_t time;
	switch_size_t size;

	switch_time_exp_gmt(&time, switch_time_now());
	switch_strftime(buffer, &size, buffer_length, format, &time);

	return buffer;
}

static char* hv_s3_signature_key(char* key_signing, hv_http_req_t *req)
{
	char key_date[SHA256_DIGEST_LENGTH] = { 0 };
	char key_region[SHA256_DIGEST_LENGTH] = { 0 };
	char key_service[SHA256_DIGEST_LENGTH] = { 0 };
	char* aws4_secret_access_key = NULL;

	if (!req) {
		return NULL;
	}

	aws4_secret_access_key = switch_mprintf("AWS4%s", req->s3.key);
	if (!hmac256(key_date, SHA256_DIGEST_LENGTH, aws4_secret_access_key, (unsigned int) strlen(aws4_secret_access_key), req->s3.date_stamp)) {
		key_signing = NULL;
	}
	if (!hmac256(key_region, SHA256_DIGEST_LENGTH, key_date, SHA256_DIGEST_LENGTH, req->s3.region)) {
		key_signing = NULL;
	}
	if (!hmac256(key_service, SHA256_DIGEST_LENGTH, key_region, SHA256_DIGEST_LENGTH, "s3")) {
		key_signing = NULL;
	}
	if (!hmac256(key_signing, SHA256_DIGEST_LENGTH, key_service, SHA256_DIGEST_LENGTH, "aws4_request")) {
		key_signing = NULL;
	}

	switch_safe_free(aws4_secret_access_key);
	return key_signing;
}

static char* hv_s3_standardized_query_string(hv_http_req_t *req)
{
	char* credential = NULL;
	char expires[10] = { 0 };
	char* standardized_query_string = NULL;

	if (!req) {
		return NULL;
	}

	credential = switch_mprintf("%s%%2F%s%%2F%s%%2Fs3%%2Faws4_request", req->s3.id, req->s3.date_stamp, req->s3.region);
	switch_snprintf(expires, 9, "%" SWITCH_TIME_T_FMT, HV_S3_DEFAULT_EXPIRATION_TIME);
	standardized_query_string = switch_mprintf(
			"X-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Credential=%s&X-Amz-Date=%s&X-Amz-Expires=%s&X-Amz-SignedHeaders=host",
			credential, req->s3.time_stamp, expires
			);

	switch_safe_free(credential);
	return standardized_query_string;
}

static char* hv_s3_standardized_request(hv_http_req_t *req, const char *method)
{

	char* standardized_query_string = NULL;
	char* standardized_request = NULL;

	if (zstr(method) || !req) {
		return NULL;
	}

	standardized_query_string = hv_s3_standardized_query_string(req);
	standardized_request = switch_mprintf(
			"%s\n/%s\n%s\nhost:%s.%s\n\nhost\nUNSIGNED-PAYLOAD",
			method, req->s3.object, standardized_query_string, req->s3.bucket, req->s3.base_domain
			);

	switch_safe_free(standardized_query_string);
	return standardized_request;
}

static char *hv_s3_string_to_sign(char* standardized_request, hv_http_req_t *req)
{

	char standardized_request_hex[SHA256_DIGEST_LENGTH * 2 + 1] = { 0 };
	char* string_to_sign = NULL;

	if (zstr(standardized_request) || !req) {
		return NULL;
	}

	sha256_hex(standardized_request_hex, standardized_request);
	string_to_sign = switch_mprintf(
			"AWS4-HMAC-SHA256\n%s\n%s/%s/s3/aws4_request\n%s",
			req->s3.time_stamp, req->s3.date_stamp, req->s3.region, standardized_request_hex
			);

	return string_to_sign;
}

SWITCH_DECLARE(char *) hv_s3_authentication_create(hv_http_req_t *req, const char *method)
{
	char signature[SHA256_DIGEST_LENGTH * 2 + 1] = { 0 };
	char *string_to_sign = NULL;

	char* standardized_query_string = NULL;
	char* standardized_request = NULL;
	char signature_key[SHA256_DIGEST_LENGTH] = { 0 };
	char* query_param = NULL;

	if (!req || zstr(method)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Bad params\n");
		return NULL;
	}

	if (zstr(req->s3.id)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "S3 id missing\n");
		return NULL;
	}

	if (zstr(req->s3.key)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "S3 key missing\n");
		return NULL;
	}

	if (zstr(req->s3.base_domain)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "S3 domain missing\n");
		return NULL;
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "S3 bucket: %s\n", req->s3.bucket);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "S3 object: %s\n", req->s3.object);

	get_time("%Y%m%d", req->s3.date_stamp, HV_S3_DATE_STAMP_LENGTH);
	get_time("%Y%m%dT%H%M%SZ", req->s3.time_stamp, HV_S3_TIME_STAMP_LENGTH);

	standardized_query_string = hv_s3_standardized_query_string(req);
	standardized_request = hv_s3_standardized_request(req, method);
	string_to_sign = hv_s3_string_to_sign(standardized_request, req);

	if (!hv_s3_signature_key(signature_key, req)
			|| !hmac256_hex(signature, signature_key, SHA256_DIGEST_LENGTH, string_to_sign)) {
		query_param = NULL;
	} else {
		query_param = switch_mprintf("%s&X-Amz-Signature=%s", standardized_query_string, signature);
	}

	switch_safe_free(string_to_sign);
	switch_safe_free(standardized_query_string);
	switch_safe_free(standardized_request);

	return query_param;
}
