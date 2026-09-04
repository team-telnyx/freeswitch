/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2018, Anthony Minessale II <anthm@freeswitch.org>
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
 * Seven Du <seven@signalwire.com>
 * Portions created by the Initial Developer are Copyright (C)
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *
 *
 * test_avformat -- avformat tests
 *
 */

#include <test/switch_test.h>

#define SAMPLES 160

FST_CORE_BEGIN("conf")
{
	FST_MODULE_BEGIN(mod_av, mod_av_test)
	{
		FST_SETUP_BEGIN()
		{
			fst_requires_module("mod_av");
		}
		FST_SETUP_END()

		FST_TEST_BEGIN(avformat_test_colorspace_RGB)
		{
			char path[1024];
			switch_status_t status;
			switch_image_t *img = switch_img_alloc(NULL, SWITCH_IMG_FMT_I420, 1280, 720, 1);
			switch_file_handle_t fh = { 0 };
			uint8_t data[SAMPLES * 2] = { 0 };
			switch_frame_t frame = { 0 };
			switch_size_t len = SAMPLES;
			uint32_t flags = SWITCH_FILE_FLAG_WRITE | SWITCH_FILE_DATA_SHORT | SWITCH_FILE_FLAG_VIDEO;
			int i = 0;
			switch_image_t *ccimg;
			switch_rgb_color_t color = {0};

			fst_requires(img);

			sprintf(path, "%s%s%s%s", "{colorspace=0}", SWITCH_GLOBAL_dirs.conf_dir, SWITCH_PATH_SEPARATOR, "../test_RGB.mp4");
			status = switch_core_file_open(&fh, path, 1, 8000, flags, fst_pool);
			fst_requires(status == SWITCH_STATUS_SUCCESS);
			fst_requires(switch_test_flag(&fh, SWITCH_FILE_OPEN));

			status = switch_core_file_write(&fh, data, &len);
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "status: %d len: %d\n", status, (int)len);
			fst_check(status == SWITCH_STATUS_SUCCESS);
			// fst_requires(len == SAMPLES);

			frame.img = img;
			status = switch_core_file_write_video(&fh, &frame);
			fst_check(status == SWITCH_STATUS_SUCCESS);

			sprintf(path, "%s%s%s", SWITCH_GLOBAL_dirs.conf_dir, SWITCH_PATH_SEPARATOR, "../cluecon.png");
			ccimg = switch_img_read_png(path, SWITCH_IMG_FMT_ARGB);
			fst_requires(ccimg);

			color.a = 255;

			for (i = 0; i < 30; i++) {
				len = SAMPLES;

				if (i == 10) {
					color.r = 255;
				} else if (i == 20) {
					color.r = 0;
					color.b = 255;
				}

				switch_img_fill(img, 0, 0, img->d_w, img->d_h, &color);
				switch_img_patch(img, ccimg, i * 10, i * 10);

				switch_core_file_write(&fh, data, &len);
				switch_core_file_write_video(&fh, &frame);
				switch_yield(100000);
			}

			switch_core_file_close(&fh);
			switch_img_free(&img);
			switch_img_free(&ccimg);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(avformat_test_colorspace_BT7)
		{
			char path[1024];
			switch_status_t status;
			switch_image_t *img = switch_img_alloc(NULL, SWITCH_IMG_FMT_I420, 1280, 720, 1);
			switch_file_handle_t fh = { 0 };
			uint8_t data[SAMPLES * 2] = { 0 };
			switch_frame_t frame = { 0 };
			switch_size_t len = SAMPLES;
			uint32_t flags = SWITCH_FILE_FLAG_WRITE | SWITCH_FILE_DATA_SHORT | SWITCH_FILE_FLAG_VIDEO;
			int i = 0;
			switch_rgb_color_t color = {0};
			switch_image_t *ccimg;

			fst_requires(img);

			sprintf(path, "%s%s%s%s", "{colorspace=1}", SWITCH_GLOBAL_dirs.conf_dir, SWITCH_PATH_SEPARATOR, "../test_BT7.mp4");
			status = switch_core_file_open(&fh, path, 1, 8000, flags, fst_pool);
			fst_requires(status == SWITCH_STATUS_SUCCESS);
			fst_requires(switch_test_flag(&fh, SWITCH_FILE_OPEN));

			status = switch_core_file_write(&fh, data, &len);
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "status: %d len: %d\n", status, (int)len);
			fst_check(status == SWITCH_STATUS_SUCCESS);
			// fst_requires(len == SAMPLES);

			frame.img = img;
			status = switch_core_file_write_video(&fh, &frame);
			fst_check(status == SWITCH_STATUS_SUCCESS);

			sprintf(path, "%s%s%s", SWITCH_GLOBAL_dirs.conf_dir, SWITCH_PATH_SEPARATOR, "../cluecon.png");
			ccimg = switch_img_read_png(path, SWITCH_IMG_FMT_ARGB);
			fst_requires(ccimg);

			color.a = 255;

			for (i = 0; i < 30; i++) {
				len = SAMPLES;

				if (i == 10) {
					color.r = 255;
				} else if (i == 20) {
					color.r = 0;
					color.b = 255;
				}

				switch_img_fill(img, 0, 0, img->d_w, img->d_h, &color);
				switch_img_patch(img, ccimg, i * 10, i * 10);

				switch_core_file_write(&fh, data, &len);
				switch_core_file_write_video(&fh, &frame);
				switch_yield(100000);
			}

			switch_core_file_close(&fh);
			switch_img_free(&img);
			switch_img_free(&ccimg);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(avformat_test_play_no_decode)
		{
			char path[1024];
			switch_status_t status;
			switch_file_handle_t fh = { 0 };
			uint8_t data[SWITCH_RECOMMENDED_BUFFER_SIZE] = { 0 };
			switch_frame_t frame = { 0 };
			switch_size_t len = SAMPLES;
			uint32_t flags = SWITCH_FILE_FLAG_READ | SWITCH_FILE_DATA_SHORT | SWITCH_FILE_FLAG_VIDEO;
			int i = 0;

			sprintf(path, "{no_video_decode=true}%s%s%s", SWITCH_GLOBAL_dirs.conf_dir, SWITCH_PATH_SEPARATOR, "../test_RGB.mp4");
			// switch_set_string(path, "{no_video_decode=true}/usr/local/freeswitch/storage/bingbing.mp4");
			status = switch_core_file_open(&fh, path, 1, 8000, flags, fst_pool);
			fst_requires(status == SWITCH_STATUS_SUCCESS);
			fst_requires(switch_test_flag(&fh, SWITCH_FILE_OPEN));
			frame.packet = data;
			frame.data = data + 12;
			frame.buflen = SWITCH_RECOMMENDED_BUFFER_SIZE;

			do {
				frame.datalen = SWITCH_RECOMMENDED_BUFFER_SIZE - 12;
				status = switch_core_file_read(&fh, data, &len);
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "status: %d len: %d\n", status, (int)len);
				fst_check(frame.img == NULL);
				frame.datalen = SWITCH_RECOMMENDED_BUFFER_SIZE - 12;
				status = switch_core_file_read_video(&fh, &frame, 0);
				fst_check(frame.img == NULL);
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "status: %d len: %d %02x\n", status, frame.datalen, *(uint8_t *)frame.data);
			} while (status == SWITCH_STATUS_MORE_DATA);

			switch_core_file_close(&fh);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(avformat_test_read_err)
		{
			char *path = "$$-non-exist-file.mp4";
			switch_status_t status;
			switch_file_handle_t fh = { 0 };
			uint32_t flags = SWITCH_FILE_FLAG_READ | SWITCH_FILE_DATA_SHORT | SWITCH_FILE_FLAG_VIDEO;

			status = switch_core_file_open(&fh, path, 1, 8000, flags, fst_pool);
			fst_check(status == SWITCH_STATUS_GENERR);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(avformat_test_read_ok)
		{
			char path[1024];
			switch_status_t status;
			switch_file_handle_t fh = { 0 };
			uint8_t data[SAMPLES * 2] = { 0 };
			switch_frame_t frame = { 0 };
			switch_size_t len = SAMPLES;
			uint32_t flags = SWITCH_FILE_FLAG_READ | SWITCH_FILE_DATA_SHORT | SWITCH_FILE_FLAG_VIDEO;

			frame.data = data;

			sprintf(path, "%s%s%s", SWITCH_GLOBAL_dirs.conf_dir, SWITCH_PATH_SEPARATOR, "../test_RGB.mp4");
			status = switch_core_file_open(&fh, path, 1, 8000, flags, fst_pool);
			fst_requires(status == SWITCH_STATUS_SUCCESS);
			fst_requires(switch_test_flag(&fh, SWITCH_FILE_OPEN));

			while (1) {
				status = switch_core_file_read(&fh, data, &len);
				if (status != SWITCH_STATUS_SUCCESS) break;
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "read: %" SWITCH_SIZE_T_FMT "\n", len);
				// fst_check(len == SAMPLES);
				status = switch_core_file_read_video(&fh, &frame, SVR_FLUSH);

				if (status == SWITCH_STATUS_BREAK) {
					switch_yield(20000);
					continue;
				}

				if (status != SWITCH_STATUS_SUCCESS) {
					break;
				}

				switch_img_free(&frame.img);
				switch_yield(20000);
			}

			switch_core_file_close(&fh);
		}
		FST_TEST_END()

		/* 192.0.2.0/24 is TEST-NET-1 (RFC 5737): guaranteed not routable, so the
		 * connect either stalls or is refused, never succeeds.  Without a connect
		 * budget a stalled connect runs to ffmpeg's own 5s tcp default; with one it
		 * must give up inside the configured window. */
		FST_TEST_BEGIN(avformat_network_connect_timeout_bounds_open)
		{
			switch_file_handle_t fh = { 0 };
			uint32_t flags = SWITCH_FILE_FLAG_WRITE | SWITCH_FILE_DATA_SHORT;
			switch_status_t status;
			switch_time_t start, elapsed_ms;

			start = switch_mono_micro_time_now();
			status = switch_core_file_open(&fh, "rtmp://192.0.2.1:1935/live/unreachable", 1, 8000, flags, fst_pool);
			elapsed_ms = (switch_mono_micro_time_now() - start) / 1000;

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
							  "unreachable open returned %d after %" SWITCH_INT64_T_FMT "ms\n", status, (int64_t) elapsed_ms);

			/* It must fail, and must not have waited out ffmpeg's default. */
			fst_check(status != SWITCH_STATUS_SUCCESS);
			fst_check(elapsed_ms < 4000);

			if (status == SWITCH_STATUS_SUCCESS) {
				switch_core_file_close(&fh);
			}
		}
		FST_TEST_END()

		/* The network settings must not reach local files: a slow disk is not a stuck
		 * peer, and aborting a local write would damage a healthy recording. */
		FST_TEST_BEGIN(avformat_local_file_unaffected_by_network_timeouts)
		{
			char path[1024];
			switch_file_handle_t fh = { 0 };
			uint32_t flags = SWITCH_FILE_FLAG_WRITE | SWITCH_FILE_DATA_SHORT;
			uint8_t data[SAMPLES * 2] = { 0 };
			switch_size_t len = SAMPLES;
			switch_status_t status;
			int i;

			/* mod_av's own container rather than wav, which it does not register. */
			switch_snprintf(path, sizeof(path), "%s%s%s%s", "{av_record_audio_only=true}",
							SWITCH_GLOBAL_dirs.conf_dir, SWITCH_PATH_SEPARATOR, "../test_local_timeouts.mp4");

			status = switch_core_file_open(&fh, path, 1, 8000, flags, fst_pool);
			fst_requires(status == SWITCH_STATUS_SUCCESS);

			/* Paced so the run really does outlast the 1000ms network budget configured
			 * above; without the pacing these writes complete in microseconds and a
			 * budget wrongly applied here would never get the chance to expire. */
			for (i = 0; i < 60; i++) {
				len = SAMPLES;
				status = switch_core_file_write(&fh, data, &len);
				fst_check(status == SWITCH_STATUS_SUCCESS);

				if (status != SWITCH_STATUS_SUCCESS) {
					break;
				}

				switch_yield(20000);
			}

			switch_core_file_close(&fh);

			/* Deliberately no assertion on the resulting file being playable.  That
			 * would be the stronger check, but whether mod_av can produce a valid file
			 * here depends on which encoders the build has, and the sibling read tests
			 * already fail for that reason -- so such an assertion would report the
			 * build's codec set rather than anything about the timeouts. */
		}
		FST_TEST_END()

		/* A recording may tighten the configured budget.  The conf sets 1000ms; this
		 * asks for 300ms and must be believed, so the open has to return well inside
		 * the configured window rather than at it. */
		FST_TEST_BEGIN(avformat_session_connect_timeout_overrides_config)
		{
			switch_file_handle_t fh = { 0 };
			uint32_t flags = SWITCH_FILE_FLAG_WRITE | SWITCH_FILE_DATA_SHORT;
			switch_status_t status;
			switch_time_t start, elapsed_ms;

			start = switch_mono_micro_time_now();
			status = switch_core_file_open(&fh, "{network_connect_timeout=300}rtmp://192.0.2.1:1935/live/unreachable",
										   1, 8000, flags, fst_pool);
			elapsed_ms = (switch_mono_micro_time_now() - start) / 1000;

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
							  "per-session 300ms budget returned %d after %" SWITCH_INT64_T_FMT "ms\n",
							  status, (int64_t) elapsed_ms);

			fst_check(status != SWITCH_STATUS_SUCCESS);
			/* Comfortably below the configured 1000ms, so this can only pass if the
			 * per-recording value was the one applied. */
			fst_check(elapsed_ms < 800);

			if (status == SWITCH_STATUS_SUCCESS) {
				switch_core_file_close(&fh);
			}
		}
		FST_TEST_END()

		/* The master switch has to reach the whole feature, not just one setting: with
		 * it off the configured 1000ms budget must stop applying, leaving the open to
		 * run to ffmpeg's own default.  Asserting it takes *longer* is the only way to
		 * show the budget was really disabled rather than merely changed. */
		FST_TEST_BEGIN(avformat_session_resiliency_off_disables_budget)
		{
			switch_file_handle_t fh = { 0 };
			uint32_t flags = SWITCH_FILE_FLAG_WRITE | SWITCH_FILE_DATA_SHORT;
			switch_status_t status;
			switch_time_t start, elapsed_ms;

			start = switch_mono_micro_time_now();
			status = switch_core_file_open(&fh, "{network_resiliency=false}rtmp://192.0.2.1:1935/live/unreachable",
										   1, 8000, flags, fst_pool);
			elapsed_ms = (switch_mono_micro_time_now() - start) / 1000;

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
							  "resiliency off returned %d after %" SWITCH_INT64_T_FMT "ms\n",
							  status, (int64_t) elapsed_ms);

			fst_check(status != SWITCH_STATUS_SUCCESS);
			fst_check(elapsed_ms > 2000);

			if (status == SWITCH_STATUS_SUCCESS) {
				switch_core_file_close(&fh);
			}
		}
		FST_TEST_END()

		FST_TEARDOWN_BEGIN()
		{
		  //const char *err = NULL;
		  switch_sleep(1000000);
		  //fst_check(switch_loadable_module_unload_module(SWITCH_GLOBAL_dirs.mod_dir, (char *)"mod_av", SWITCH_TRUE, &err) == SWITCH_STATUS_SUCCESS);
		}
		FST_TEARDOWN_END()
	}
	FST_MODULE_END()
}
FST_CORE_END()
