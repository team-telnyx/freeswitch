#!/usr/bin/env python3
"""Replay the private AVMD regression corpus through a FreeSWITCH build."""

import argparse
import array
import hashlib
import json
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import uuid
import wave
from pathlib import Path


DETECTION_PATTERN = re.compile(
    r"Beep Detected.*?f = \[([0-9.]+)\].*?detection time \[([0-9]+)\] \[us\]"
)
SPECTRAL_PATTERN = re.compile(
    r"spectral confirmation accepted candidate:.*?dominant \[([0-9.]+) Hz\], purity \[([0-9.]+)\]"
)


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def command_output(command, check=True):
    completed = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if check and completed.returncode != 0:
        raise RuntimeError("command failed ({}): {}\n{}".format(
            completed.returncode, " ".join(command), completed.stdout
        ))
    return completed.stdout


def fs_cli(command, check=True):
    output = command_output(["fs_cli", "-x", command], check=check)
    if check and output.lstrip().startswith("-ERR"):
        raise RuntimeError("FreeSWITCH command failed: {}\n{}".format(command, output))
    return output


def wait_for_freeswitch(timeout_seconds):
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        output = fs_cli("status", check=False)
        if "UP" in output:
            return
        time.sleep(0.25)
    raise RuntimeError("FreeSWITCH did not become ready")


def freeswitch_running():
    return subprocess.run(
        ["pgrep", "-x", "freeswitch"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    ).returncode == 0


def read_log_since(path, offset):
    with path.open("rb") as log_file:
        log_file.seek(offset)
        return log_file.read().decode("utf-8", errors="replace")


def wait_for_detection(log_path, offset, call_uuid, timeout_seconds):
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        text = read_log_since(log_path, offset)
        for line in text.splitlines():
            if call_uuid in line and DETECTION_PATTERN.search(line):
                return text
        time.sleep(0.1)
    return read_log_since(log_path, offset)


def profile_string(profile):
    settings = dict(profile)
    settings["debug"] = 1
    return ",".join("{}={}".format(key, settings[key]) for key in sorted(settings))


def extract_case(case, capture, extractor, output_dir):
    wav_path = output_dir / (case["id"] + ".wav")
    metadata_path = output_dir / (case["id"] + ".json")
    stream = case["stream"]
    command = [
        sys.executable,
        str(extractor),
        "extract",
        str(capture),
        "--source",
        stream["source"],
        "--source-port",
        str(stream["source_port"]),
        "--destination",
        stream["destination"],
        "--destination-port",
        str(stream["destination_port"]),
        "--ssrc",
        str(stream["ssrc"]),
        "--payload-type",
        str(stream["payload_type"]),
        "--clock-rate",
        str(case["clock_rate"]),
        "--wav",
        str(wav_path),
        "--metadata",
        str(metadata_path),
    ]
    command_output(command)
    return wav_path, json.loads(metadata_path.read_text(encoding="utf-8"))


def synthetic_sample(kind, sample_index, rate):
    time_seconds = sample_index / float(rate)
    if kind == "legacy_dual_tone":
        if time_seconds < 0.2:
            return 0.0
        return (
            6000.0 * math.sin(2.0 * math.pi * 440.0 * time_seconds) +
            6000.0 * math.sin(2.0 * math.pi * 480.0 * time_seconds + math.pi / 3.0)
        )
    if kind == "single_tone":
        if time_seconds < 0.2:
            return 0.0
        return 7000.0 * math.sin(2.0 * math.pi * 660.0 * time_seconds)
    if kind == "sparse_then_tone":
        milliseconds = 1000.0 * time_seconds
        if milliseconds < 200.0:
            if milliseconds % 20.0 < 15.0:
                return 0.0
        elif milliseconds >= 300.0:
            return 0.0
        return 7000.0 * math.sin(2.0 * math.pi * 660.0 * time_seconds)
    if kind == "candidate_with_wrong_frequency_gaps":
        if time_seconds < 0.2:
            return 0.0
        milliseconds = 1000.0 * (time_seconds - 0.2)
        frequency = 1000.0 if milliseconds % 100.0 >= 90.0 else 660.0
        return 7000.0 * math.sin(2.0 * math.pi * frequency * time_seconds)
    raise ValueError("unknown synthetic generator: {}".format(kind))


def generate_synthetic_case(case, output_dir):
    source = case["source"]
    rate = int(source.get("clock_rate", 8000))
    duration_ms = int(source["duration_ms"])
    samples_n = (rate * duration_ms) // 1000
    pcm = array.array("h")
    wav_path = output_dir / (case["id"] + ".wav")

    for sample_index in range(samples_n):
        value = synthetic_sample(source["generator"], sample_index, rate)
        pcm.append(max(-32768, min(32767, int(round(value)))))
    if sys.byteorder != "little":
        pcm.byteswap()
    with wave.open(str(wav_path), "wb") as wav_file:
        wav_file.setnchannels(1)
        wav_file.setsampwidth(2)
        wav_file.setframerate(rate)
        wav_file.writeframes(pcm.tobytes())
    return wav_path, {
        "clock_rate": rate,
        "duration_seconds": samples_n / float(rate),
        "generator": source["generator"],
        "samples": samples_n,
        "source_type": "synthetic",
        "wav_sha256": sha256_file(wav_path),
    }


def prepare_repository_wav(case, extractor):
    source = case["source"]
    wav_path = (extractor.parent.parent / source["path"]).resolve()
    if not wav_path.is_file():
        raise RuntimeError("missing repository WAV: {}".format(wav_path))
    actual_sha = sha256_file(wav_path)
    if actual_sha != source["sha256"]:
        raise RuntimeError("repository WAV hash mismatch for {}: {}".format(
            case["id"], actual_sha
        ))
    with wave.open(str(wav_path), "rb") as wav_file:
        if wav_file.getnchannels() != 1 or wav_file.getsampwidth() != 2:
            raise RuntimeError("repository WAV must be 16-bit mono: {}".format(wav_path))
        frames = wav_file.getnframes()
        rate = wav_file.getframerate()
    return wav_path, {
        "clock_rate": rate,
        "duration_seconds": frames / float(rate),
        "frames": frames,
        "source_path": source["path"],
        "source_type": "repository_wav",
        "wav_sha256": actual_sha,
    }


def prepare_case(case, pcap_dir, extractor, output_dir):
    source = case.get("source", {"type": "pcap"})
    if source.get("type") == "synthetic":
        return generate_synthetic_case(case, output_dir)
    if source.get("type") == "repository_wav":
        return prepare_repository_wav(case, extractor)
    if pcap_dir is None:
        raise RuntimeError("--pcap-dir is required for PCAP case {}".format(case["id"]))
    capture = pcap_dir / case["capture_filename"]
    if not capture.exists():
        raise RuntimeError("missing private capture: {}".format(capture))
    actual_sha = sha256_file(capture)
    if actual_sha != case["capture_sha256"]:
        raise RuntimeError("capture hash mismatch for {}: {}".format(case["id"], actual_sha))
    return extract_case(case, capture, extractor, output_dir)


def parse_detection(log_text, call_uuid):
    result = {"detected": False}
    for line in log_text.splitlines():
        if call_uuid not in line:
            continue
        match = DETECTION_PATTERN.search(line)
        if match:
            result.update({
                "detected": True,
                "frequency_hz": float(match.group(1)),
                "detection_time_ms": int(match.group(2)) / 1000.0,
            })
        match = SPECTRAL_PATTERN.search(line)
        if match:
            result.update({
                "spectral_dominant_hz": float(match.group(1)),
                "spectral_purity": float(match.group(2)),
            })
    return result


def validate_expected(case, result):
    expected = case["expected"]
    errors = []
    if result["detected"] != expected["detect"]:
        errors.append("detect expected {} but got {}".format(expected["detect"], result["detected"]))
    if result["detected"]:
        frequency = result.get("frequency_hz")
        detection_time = result.get("detection_time_ms")
        if frequency is None or not expected["frequency_hz_min"] <= frequency <= expected["frequency_hz_max"]:
            errors.append("frequency outside expected interval: {}".format(frequency))
        if detection_time is None or not expected["detection_time_ms_min"] <= detection_time <= expected["detection_time_ms_max"]:
            errors.append("detection time outside expected interval: {}".format(detection_time))
    result["passed"] = not errors
    result["errors"] = errors


def run_case(case, profile, pcap_dir, extractor, output_dir, log_path):
    wav_path, media_metadata = prepare_case(case, pcap_dir, extractor, output_dir)
    call_uuid = str(uuid.uuid4())
    peer_uuid = None
    try:
        originate_output = fs_cli(
            "originate {origination_uuid=%s}loopback/app=park &park" % call_uuid
        )
        if not originate_output.lstrip().startswith("+OK"):
            raise RuntimeError("loopback originate failed: %r" % originate_output.strip())
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline:
            peer_uuid = fs_cli(
                "uuid_getvar %s other_loopback_leg_uuid" % call_uuid, check=False
            ).strip()
            if re.fullmatch(r"[0-9a-f-]{36}", peer_uuid):
                break
            time.sleep(0.1)
        if not re.fullmatch(r"[0-9a-f-]{36}", peer_uuid or ""):
            raise RuntimeError("unable to resolve loopback peer UUID: %r" % peer_uuid)
        fs_cli("uuid_broadcast %s avmd_start::%s aleg" % (call_uuid, profile_string(profile)))
        time.sleep(0.5)
        log_offset = log_path.stat().st_size
        fs_cli("uuid_broadcast %s playback::%s both" % (peer_uuid, wav_path))
        expected = case["expected"]
        if expected["detect"]:
            timeout = (expected["detection_time_ms_max"] / 1000.0) + 1.0
            log_text = wait_for_detection(log_path, log_offset, call_uuid, timeout)
        else:
            timeout = media_metadata["duration_seconds"] + 1.0
            time.sleep(timeout)
            log_text = read_log_since(log_path, log_offset)
        result = parse_detection(log_text, call_uuid)
        result.update({
            "id": case["id"],
            "call_uuid": call_uuid,
            "media": media_metadata,
            "profile": case.get("profile", "hardened"),
        })
        if "capture_sha256" in case:
            result["capture_sha256"] = case["capture_sha256"]
        validate_expected(case, result)
        return result
    finally:
        fs_cli("uuid_kill %s" % call_uuid, check=False)


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    script_dir = Path(__file__).resolve().parent
    parser.add_argument("--corpus", type=Path, default=script_dir / "avmd_corpus.json")
    parser.add_argument("--pcap-dir", type=Path)
    parser.add_argument("--extractor", type=Path, default=script_dir / "avmd_pcap_extract.py")
    parser.add_argument("--module", type=Path, required=True)
    parser.add_argument(
        "--log", type=Path, default=Path.home() / ".local/var/log/freeswitch/freeswitch.log"
    )
    parser.add_argument("--output", type=Path)
    parser.add_argument(
        "--case", action="append", dest="case_ids",
        help="run only the named corpus case (repeatable)"
    )
    parser.add_argument(
        "--set", action="append", dest="settings", default=[], metavar="KEY=JSON_VALUE",
        help="override an existing profile setting for a tuning run (repeatable)"
    )
    return parser.parse_args()


def main():
    args = parse_args()
    if shutil.which("fs_cli") is None or shutil.which("freeswitch") is None:
        raise RuntimeError("source ~/.local/etc/freeswitch/freeswitch_env.sh before running")
    if freeswitch_running():
        raise RuntimeError("refusing to manage an already-running FreeSWITCH process")
    corpus = json.loads(args.corpus.read_text(encoding="utf-8"))
    for assignment in args.settings:
        key, separator, value = assignment.partition("=")
        if not separator or key not in corpus["profile"]:
            raise ValueError("invalid or unknown profile override: %s" % assignment)
        corpus["profile"][key] = json.loads(value)
    if args.case_ids:
        selected = set(args.case_ids)
        corpus["cases"] = [case for case in corpus["cases"] if case["id"] in selected]
        missing = selected.difference(case["id"] for case in corpus["cases"])
        if missing:
            raise ValueError("unknown corpus case(s): %s" % ", ".join(sorted(missing)))
    for path in (args.extractor, args.module, args.log.parent):
        if not path.exists():
            raise RuntimeError("required path does not exist: %s" % path)

    temporary = None
    if args.output is None:
        temporary = tempfile.TemporaryDirectory(prefix="avmd-integration-")
        output_dir = Path(temporary.name)
    else:
        output_dir = args.output
        output_dir.mkdir(parents=True, exist_ok=True)

    started = False
    results = []
    try:
        command_output(["freeswitch", "-nonat", "-nc"])
        started = True
        wait_for_freeswitch(20.0)
        loopback_load = fs_cli("load mod_loopback", check=False)
        if loopback_load.lstrip().startswith("-ERR") and "already loaded" not in loopback_load.lower():
            raise RuntimeError("unable to load mod_loopback: %s" % loopback_load.strip())
        fs_cli("unload mod_avmd", check=False)
        fs_cli("load %s" % args.module)
        for case in corpus["cases"]:
            profile_name = case.get("profile", "hardened")
            if profile_name == "legacy":
                case_profile = dict(corpus["legacy_profile"])
            else:
                case_profile = dict(corpus["profile"])
            case_profile.update(case.get("settings", {}))
            results.append(run_case(
                case, case_profile, args.pcap_dir, args.extractor, output_dir, args.log
            ))
    finally:
        if started:
            fs_cli("unload mod_avmd", check=False)
            fs_cli("shutdown", check=False)
            deadline = time.monotonic() + 15.0
            while freeswitch_running() and time.monotonic() < deadline:
                time.sleep(0.25)
        if temporary is not None:
            temporary.cleanup()

    report = {
        "schema": 1,
        "profile": corpus["profile"],
        "legacy_profile": corpus["legacy_profile"],
        "module": str(args.module),
        "module_sha256": sha256_file(args.module),
        "results": results,
        "passed": all(result["passed"] for result in results),
    }
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print("error: {}".format(error), file=sys.stderr)
        sys.exit(1)
