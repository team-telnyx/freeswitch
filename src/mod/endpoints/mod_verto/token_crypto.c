/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2012, Anthony Minessale II <anthm@freeswitch.org>
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
 *
 * Stefan Yohansson <sy.fen0@gmail.com>
 *
 * token_crypto.c -- JWT decoder
 *
 */

#include <switch.h>
#include "token_crypto.h"

void handle_errors(unsigned char *ciphertext)
{
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,"An error occurred while trying to decrypt token '%s'\n", ciphertext);
}

const char *base64_decode(const char *encoded_text, int length) {
	const int pl = 3*length/4;
	unsigned char *output = calloc(pl+1, 1);
	const int ol = EVP_DecodeBlock(output, encoded_text, length);
	if (pl != ol) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Whoops, decode predicted %d but we got %d\n", pl, ol);
	}
	return output;
}

int token_decrypt(const char *jwt_token, unsigned char **plaintext, const char *jwt_grant_field, const char *jwt_aad, const char *jwt_key) {
	int jret = 0;
	int ret = 0;
	jwt_t *jwt = NULL;
	const char *token_decoded_a = NULL;
	const char *token_decoded_tmp = NULL;
	const char *decoded_key = NULL;

	if (zstr(jwt_grant_field) || zstr(jwt_aad) || zstr(jwt_key)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't decrypt! Didn't get one of: AAD, KEY or GRANT_FIELD\n");
		goto cleanup;
	}

	decoded_key = base64_decode(jwt_key, strlen(jwt_key));

	jret = jwt_decode(&jwt, jwt_token, NULL, 0);
	if (jret != 0 || jwt == NULL)
	{
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "JWT Token is NOT valid\n");
		goto cleanup;
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "JWT Token is valid\n" );
	token_decoded_a = jwt_get_grant( jwt, (const char *)jwt_grant_field );

	if ( !token_decoded_a ) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't get JWT grant\n" );
		goto cleanup;
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Token Encoded_a: [%s]\n", token_decoded_a );
	token_decoded_tmp = base64_decode(token_decoded_a, strlen(token_decoded_a));

	if (strlen(token_decoded_tmp) > 32) {
		char *iv = NULL;
		char *tag = NULL;
		char *token_decoded = NULL;

		/* |--- IV (16 byes ) ---| --- TAG (16 byes ) --- | --- CIPHERTEXT ---[end]| */
		strncpy(iv, token_decoded_tmp + 0, 16);
		strncpy(tag, token_decoded_tmp + 16, 16);
		strncpy(token_decoded, token_decoded_tmp + 32, strlen(token_decoded_tmp));

		int ciphertext_len = strlen(token_decoded);
		int aad_len = strlen(jwt_aad);
		int iv_len = strlen(iv);

		*plaintext = (unsigned char*)malloc( sizeof(char) * ciphertext_len + 1 );
		memset(*plaintext, 0, sizeof(char) * ciphertext_len + 1);

		ret = gcm_decrypt(
			(unsigned char*)token_decoded,
			ciphertext_len,
			(unsigned char*)jwt_aad,
			aad_len,
			(unsigned char*)tag,
			(unsigned char*)decoded_key,
			(unsigned char*)iv,
			iv_len,
			*plaintext
		);

		goto cleanup;

	} else {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,"token_decoded_tmp is empty\n" );
		goto cleanup;
	}

cleanup:
	if (jwt) {
		jwt_free(jwt);
	}
	return ret;
}

int gcm_decrypt(unsigned char *ciphertext, int ciphertext_len,
                unsigned char *aad, int aad_len,
                unsigned char *tag,
                unsigned char *key,
                unsigned char *iv, int iv_len,
                unsigned char *plaintext)
{
    EVP_CIPHER_CTX *ctx;
    int len;
    int plaintext_len;
    int ret;

    /* Create and initialise the context */
    if(!(ctx = EVP_CIPHER_CTX_new())) { handle_errors(ciphertext); return 0; }

    /* Initialise the decryption operation. */
    if(!EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL)){ handle_errors(ciphertext); EVP_CIPHER_CTX_free(ctx); return 0; }

    /* Set IV length. Not necessary if this is 12 bytes (96 bits) */
    if(!EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, iv_len, NULL)){ handle_errors(ciphertext); EVP_CIPHER_CTX_free(ctx); return 0; }

    /* Initialise key and IV */
    if(!EVP_DecryptInit_ex(ctx, NULL, NULL, key, iv)){ handle_errors(ciphertext); EVP_CIPHER_CTX_free(ctx); return 0; }

    /*
     * Provide any AAD data. This can be called zero or more times as
     * required
     */
    if(!EVP_DecryptUpdate(ctx, NULL, &len, aad, aad_len)){ handle_errors(ciphertext); EVP_CIPHER_CTX_free(ctx); return 0; }

    /*
     * Provide the message to be decrypted, and obtain the plaintext output.
     * EVP_DecryptUpdate can be called multiple times if necessary
     */
    if(!EVP_DecryptUpdate(ctx, plaintext, &len, ciphertext, ciphertext_len)){ handle_errors(ciphertext); EVP_CIPHER_CTX_free(ctx); return 0; }

    plaintext_len = len;

    /* Set expected tag value. Works in OpenSSL 1.0.1d and later */
    if(!EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, tag)){ handle_errors(ciphertext); EVP_CIPHER_CTX_free(ctx); return 0; }

    /*
     * Finalise the decryption. A positive return value indicates success,
     * anything else is a failure - the plaintext is not trustworthy.
     */
    ret = EVP_DecryptFinal_ex(ctx, plaintext + len, &len);

    /* Clean up */
    EVP_CIPHER_CTX_free(ctx);
    if (ret > 0) {
        /* Decrypting was successful, let's return it after cleaning up */
        plaintext_len += len;
        return plaintext_len;
    } else {
        /* Decrypting failed for some reason, let's cleanup, set plaintext to NULL (it will be freed by the caller) */
        return 0;
    }
}
