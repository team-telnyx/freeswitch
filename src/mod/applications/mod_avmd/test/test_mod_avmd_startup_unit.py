#!/usr/bin/env python3
"""Source-level regressions for AVMD startup invariants."""

import re
import unittest
from pathlib import Path


SOURCE = Path(__file__).resolve().parent.parent / "mod_avmd.c"


def normalized_function(source, start_marker, end_marker):
    start = source.index(start_marker)
    end = source.index(end_marker, start + len(start_marker))
    return re.sub(r"\s+", " ", source[start:end])


def next_power_of_two(value):
    result = 1
    while result < value:
        result <<= 1
    return result


class AvmdStartupRegressionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = SOURCE.read_text(encoding="utf-8")

    def test_legacy_buffer_keeps_worst_case_frame_capacity(self):
        init_body = normalized_function(
            self.source,
            "static switch_status_t init_avmd_session_data",
            "static void avmd_session_close",
        )
        self.assertIn(
            "INIT_CIRC_BUFFER(&avmd_session->b, raw_history_samples, "
            "(size_t) AVMD_FRAME_LEN(allocation_rate), fs_session);",
            init_body,
        )

        allocation_rate = 48000
        raw_history_samples = allocation_rate * 2 // 1000
        frame_samples = allocation_rate * 20 // 1000
        buffer_samples = next_power_of_two(2 * max(raw_history_samples, frame_samples))
        self.assertEqual(2048, buffer_samples)

    def test_outbound_media_guard_precedes_codec_rate_validation(self):
        app_body = normalized_function(
            self.source,
            "SWITCH_STANDARD_APP(avmd_start_app) {",
            "SWITCH_STANDARD_APP(avmd_stop_app) {",
        )
        media_guard = app_body.index("switch_channel_test_flag(channel, CF_MEDIA_SET)")
        initialization = app_body.index("avmd_initialize_session_data(avmd_session, session")
        self.assertLess(media_guard, initialization)
        self.assertEqual(1, app_body.count("switch_channel_test_flag(channel, CF_MEDIA_SET)"))


if __name__ == "__main__":
    unittest.main()
