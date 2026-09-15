#!/usr/bin/env python3
"""List or reconstruct dependency-free RTP/PCMU streams from PCAP/PCAPNG."""

import argparse
import hashlib
import ipaddress
import json
import struct
import sys
import wave
from collections import defaultdict
from pathlib import Path


PCAP_MAGICS = {
    b"\xd4\xc3\xb2\xa1": ("<", 1_000_000),
    b"\xa1\xb2\xc3\xd4": (">", 1_000_000),
    b"\x4d\x3c\xb2\xa1": ("<", 1_000_000_000),
    b"\xa1\xb2\x3c\x4d": (">", 1_000_000_000),
}
PCAPNG_SECTION = b"\x0a\x0d\x0d\x0a"
PCAPNG_BOM = {
    b"\x4d\x3c\x2b\x1a": "<",
    b"\x1a\x2b\x3c\x4d": ">",
}


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def iter_pcap(path):
    with path.open("rb") as capture:
        header = capture.read(24)
        if len(header) != 24 or header[:4] not in PCAP_MAGICS:
            raise ValueError("not a supported classic PCAP")
        endian, timestamp_scale = PCAP_MAGICS[header[:4]]
        linktype = struct.unpack(endian + "I", header[20:24])[0]
        packet_header = struct.Struct(endian + "IIII")
        while True:
            raw_header = capture.read(packet_header.size)
            if not raw_header:
                return
            if len(raw_header) != packet_header.size:
                raise ValueError("truncated PCAP packet header")
            seconds, fraction, captured_len, _ = packet_header.unpack(raw_header)
            packet = capture.read(captured_len)
            if len(packet) != captured_len:
                raise ValueError("truncated PCAP packet")
            yield seconds + (fraction / timestamp_scale), linktype, packet


def pcapng_timestamp_resolution(options, endian):
    offset = 0
    while offset + 4 <= len(options):
        code, length = struct.unpack_from(endian + "HH", options, offset)
        offset += 4
        value = options[offset:offset + length]
        offset += (length + 3) & ~3
        if code == 0:
            break
        if code == 9 and length == 1:
            exponent = value[0]
            if exponent & 0x80:
                return 2.0 ** -(exponent & 0x7F)
            return 10.0 ** -exponent
    return 1.0e-6


def iter_pcapng(path):
    data = path.read_bytes()
    offset = 0
    endian = None
    interfaces = []
    while offset + 12 <= len(data):
        block_type_bytes = data[offset:offset + 4]
        if block_type_bytes == PCAPNG_SECTION:
            if offset + 12 > len(data):
                raise ValueError("truncated PCAPNG section")
            endian = PCAPNG_BOM.get(data[offset + 8:offset + 12])
            if endian is None:
                raise ValueError("unsupported PCAPNG byte order")
            block_type = 0x0A0D0D0A
        elif endian is None:
            raise ValueError("PCAPNG does not begin with a section header")
        else:
            block_type = struct.unpack_from(endian + "I", data, offset)[0]

        block_len = struct.unpack_from(endian + "I", data, offset + 4)[0]
        if block_len < 12 or offset + block_len > len(data) or block_len % 4:
            raise ValueError("invalid PCAPNG block length")
        if struct.unpack_from(endian + "I", data, offset + block_len - 4)[0] != block_len:
            raise ValueError("mismatched PCAPNG block length")
        body = data[offset + 8:offset + block_len - 4]

        if block_type == 0x0A0D0D0A:
            interfaces = []
        elif block_type == 1 and len(body) >= 8:
            linktype = struct.unpack_from(endian + "H", body, 0)[0]
            interfaces.append((linktype, pcapng_timestamp_resolution(body[8:], endian)))
        elif block_type == 6 and len(body) >= 20:
            interface_id, timestamp_high, timestamp_low, captured_len, _ = struct.unpack_from(
                endian + "IIIII", body, 0
            )
            if interface_id >= len(interfaces) or 20 + captured_len > len(body):
                raise ValueError("invalid PCAPNG enhanced packet block")
            linktype, resolution = interfaces[interface_id]
            timestamp = ((timestamp_high << 32) | timestamp_low) * resolution
            yield timestamp, linktype, body[20:20 + captured_len]
        offset += block_len
    if offset != len(data):
        raise ValueError("trailing or truncated PCAPNG data")


def iter_capture(path):
    with path.open("rb") as capture:
        magic = capture.read(4)
    if magic in PCAP_MAGICS:
        yield from iter_pcap(path)
    elif magic == PCAPNG_SECTION:
        yield from iter_pcapng(path)
    else:
        raise ValueError("unsupported capture format")


def network_payload(linktype, packet):
    if linktype == 1:
        if len(packet) < 14:
            return None
        offset = 14
        protocol = struct.unpack_from("!H", packet, 12)[0]
        while protocol in (0x8100, 0x88A8, 0x9100):
            if len(packet) < offset + 4:
                return None
            protocol = struct.unpack_from("!H", packet, offset + 2)[0]
            offset += 4
        return protocol, packet[offset:]
    if linktype == 113 and len(packet) >= 16:
        return struct.unpack_from("!H", packet, 14)[0], packet[16:]
    if linktype == 276 and len(packet) >= 20:
        return struct.unpack_from("!H", packet, 0)[0], packet[20:]
    if linktype in (101, 228):
        return 0x0800, packet
    if linktype == 229:
        return 0x86DD, packet
    if linktype == 0 and len(packet) >= 4:
        family_le = struct.unpack_from("<I", packet, 0)[0]
        family_be = struct.unpack_from(">I", packet, 0)[0]
        if 2 in (family_le, family_be):
            return 0x0800, packet[4:]
        if 24 in (family_le, family_be) or 30 in (family_le, family_be):
            return 0x86DD, packet[4:]
    return None


def udp_datagram(protocol, packet):
    if protocol == 0x0800:
        if len(packet) < 20 or packet[0] >> 4 != 4:
            return None
        header_len = (packet[0] & 0x0F) * 4
        if header_len < 20 or len(packet) < header_len + 8 or packet[9] != 17:
            return None
        if struct.unpack_from("!H", packet, 6)[0] & 0x1FFF:
            return None
        source = str(ipaddress.ip_address(packet[12:16]))
        destination = str(ipaddress.ip_address(packet[16:20]))
        udp = packet[header_len:]
    elif protocol == 0x86DD:
        if len(packet) < 48 or packet[0] >> 4 != 6 or packet[6] != 17:
            return None
        source = str(ipaddress.ip_address(packet[8:24]))
        destination = str(ipaddress.ip_address(packet[24:40]))
        udp = packet[40:]
    else:
        return None
    source_port, destination_port, udp_len, _ = struct.unpack_from("!HHHH", udp, 0)
    if udp_len < 8 or udp_len > len(udp):
        return None
    return source, source_port, destination, destination_port, udp[8:udp_len]


def rtp_packet(payload):
    if len(payload) < 12 or payload[0] >> 6 != 2:
        return None
    csrc_count = payload[0] & 0x0F
    extension = bool(payload[0] & 0x10)
    padding = bool(payload[0] & 0x20)
    offset = 12 + (4 * csrc_count)
    if offset > len(payload):
        return None
    if extension:
        if offset + 4 > len(payload):
            return None
        extension_words = struct.unpack_from("!H", payload, offset + 2)[0]
        offset += 4 + (4 * extension_words)
        if offset > len(payload):
            return None
    end = len(payload)
    if padding:
        padding_len = payload[-1]
        if padding_len == 0 or padding_len > end - offset:
            return None
        end -= padding_len
    sequence, timestamp, ssrc = struct.unpack_from("!HII", payload, 2)
    return {
        "payload_type": payload[1] & 0x7F,
        "sequence": sequence,
        "timestamp": timestamp,
        "ssrc": ssrc,
        "payload": payload[offset:end],
    }


def capture_rtp_packets(path):
    for capture_time, linktype, packet in iter_capture(path):
        network = network_payload(linktype, packet)
        if network is None:
            continue
        udp = udp_datagram(*network)
        if udp is None:
            continue
        source, source_port, destination, destination_port, payload = udp
        rtp = rtp_packet(payload)
        if rtp is None:
            continue
        rtp.update({
            "capture_time": capture_time,
            "source": source,
            "source_port": source_port,
            "destination": destination,
            "destination_port": destination_port,
        })
        yield rtp


def stream_key(packet):
    return (
        packet["source"],
        packet["source_port"],
        packet["destination"],
        packet["destination_port"],
        packet["ssrc"],
        packet["payload_type"],
    )


def list_streams(path):
    streams = defaultdict(lambda: {"packets": 0, "payload_bytes": 0, "first": None, "last": None})
    for packet in capture_rtp_packets(path):
        stats = streams[stream_key(packet)]
        stats["packets"] += 1
        stats["payload_bytes"] += len(packet["payload"])
        stats["first"] = packet["capture_time"] if stats["first"] is None else stats["first"]
        stats["last"] = packet["capture_time"]
    result = []
    for key, stats in streams.items():
        source, source_port, destination, destination_port, ssrc, payload_type = key
        result.append({
            "source": source,
            "source_port": source_port,
            "destination": destination,
            "destination_port": destination_port,
            "ssrc": ssrc,
            "payload_type": payload_type,
            "packets": stats["packets"],
            "payload_bytes": stats["payload_bytes"],
            "capture_duration_seconds": round(stats["last"] - stats["first"], 6),
        })
    return sorted(result, key=lambda item: (-item["payload_bytes"], item["source"], item["ssrc"]))


def unwrap(value, previous, bits):
    if previous is None:
        return value
    modulus = 1 << bits
    base = previous & ~(modulus - 1)
    candidate = base | value
    if candidate - previous > modulus // 2:
        candidate -= modulus
    elif previous - candidate > modulus // 2:
        candidate += modulus
    return candidate


def decode_ulaw(value):
    value = (~value) & 0xFF
    sign = value & 0x80
    exponent = (value >> 4) & 0x07
    mantissa = value & 0x0F
    sample = (((mantissa << 3) + 0x84) << exponent) - 0x84
    return -sample if sign else sample


def reconstruct(path, selector, clock_rate):
    selected = []
    previous_timestamp = None
    previous_sequence = None
    duplicate_packets = 0
    seen = set()
    for packet in capture_rtp_packets(path):
        if any(packet[name] != value for name, value in selector.items()):
            continue
        dedupe_key = (packet["sequence"], packet["timestamp"], len(packet["payload"]))
        if dedupe_key in seen:
            duplicate_packets += 1
            continue
        seen.add(dedupe_key)
        packet["unwrapped_timestamp"] = unwrap(packet["timestamp"], previous_timestamp, 32)
        packet["unwrapped_sequence"] = unwrap(packet["sequence"], previous_sequence, 16)
        previous_timestamp = packet["unwrapped_timestamp"]
        previous_sequence = packet["unwrapped_sequence"]
        selected.append(packet)
    if not selected:
        raise ValueError("no RTP packets matched the requested stream")
    if selector["payload_type"] != 0:
        raise ValueError("only RTP payload type 0 (PCMU) is supported")

    first_timestamp = min(packet["unwrapped_timestamp"] for packet in selected)
    last_sample = max(
        packet["unwrapped_timestamp"] - first_timestamp + len(packet["payload"])
        for packet in selected
    )
    timeline = bytearray([0xFF]) * last_sample
    occupied = bytearray(last_sample)
    overlap_samples = 0
    media_samples = 0
    for packet in sorted(selected, key=lambda item: (item["unwrapped_timestamp"], item["capture_time"])):
        offset = packet["unwrapped_timestamp"] - first_timestamp
        for index, value in enumerate(packet["payload"]):
            position = offset + index
            if occupied[position]:
                overlap_samples += 1
                continue
            timeline[position] = value
            occupied[position] = 1
            media_samples += 1

    sequences = sorted(packet["unwrapped_sequence"] for packet in selected)
    sequence_gaps = sum(max(0, current - previous - 1) for previous, current in zip(sequences, sequences[1:]))
    return bytes(timeline), {
        "input_sha256": sha256_file(path),
        "clock_rate": clock_rate,
        "packets": len(selected),
        "duplicate_packets_discarded": duplicate_packets,
        "sequence_gaps": sequence_gaps,
        "timeline_samples": len(timeline),
        "media_samples": media_samples,
        "silence_samples_inserted": len(timeline) - media_samples,
        "overlap_samples_discarded": overlap_samples,
        "duration_seconds": round(len(timeline) / clock_rate, 6),
        "first_capture_time": min(packet["capture_time"] for packet in selected),
        "last_capture_time": max(packet["capture_time"] for packet in selected),
        "stream": selector,
    }


def write_wav(path, encoded, rate):
    pcm = bytearray()
    for value in encoded:
        pcm.extend(struct.pack("<h", decode_ulaw(value)))
    with wave.open(str(path), "wb") as destination:
        destination.setnchannels(1)
        destination.setsampwidth(2)
        destination.setframerate(rate)
        destination.writeframes(pcm)


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    list_parser = subparsers.add_parser("list", help="list detected RTP streams")
    list_parser.add_argument("capture", type=Path)

    extract_parser = subparsers.add_parser("extract", help="reconstruct one PCMU stream")
    extract_parser.add_argument("capture", type=Path)
    extract_parser.add_argument("--source", required=True)
    extract_parser.add_argument("--source-port", type=int, required=True)
    extract_parser.add_argument("--destination", required=True)
    extract_parser.add_argument("--destination-port", type=int, required=True)
    extract_parser.add_argument("--ssrc", type=int, required=True)
    extract_parser.add_argument("--payload-type", type=int, default=0)
    extract_parser.add_argument("--clock-rate", type=int, default=8000)
    extract_parser.add_argument("--trim-start-ms", type=int, default=0)
    extract_parser.add_argument("--trim-duration-ms", type=int)
    extract_parser.add_argument("--ulaw", type=Path)
    extract_parser.add_argument("--wav", type=Path)
    extract_parser.add_argument("--metadata", type=Path)
    return parser.parse_args()


def main():
    args = parse_args()
    if args.command == "list":
        output = {
            "capture": str(args.capture),
            "sha256": sha256_file(args.capture),
            "streams": list_streams(args.capture),
        }
        print(json.dumps(output, indent=2, sort_keys=True))
        return 0

    if args.ulaw is None and args.wav is None:
        raise ValueError("at least one of --ulaw or --wav is required")
    selector = {
        "source": args.source,
        "source_port": args.source_port,
        "destination": args.destination,
        "destination_port": args.destination_port,
        "ssrc": args.ssrc,
        "payload_type": args.payload_type,
    }
    encoded, metadata = reconstruct(args.capture, selector, args.clock_rate)
    if args.trim_start_ms < 0 or (args.trim_duration_ms is not None and args.trim_duration_ms <= 0):
        raise ValueError("trim values must be non-negative and duration must be positive")
    if args.trim_start_ms or args.trim_duration_ms is not None:
        trim_start = (args.clock_rate * args.trim_start_ms) // 1000
        trim_end = len(encoded)
        if args.trim_duration_ms is not None:
            trim_end = trim_start + ((args.clock_rate * args.trim_duration_ms) // 1000)
        if trim_start >= len(encoded) or trim_end > len(encoded):
            raise ValueError("trim interval falls outside the reconstructed timeline")
        encoded = encoded[trim_start:trim_end]
        metadata["trim"] = {
            "start_ms": args.trim_start_ms,
            "duration_ms": round((len(encoded) * 1000) / args.clock_rate, 3),
            "samples": len(encoded),
        }
    if args.ulaw is not None:
        args.ulaw.parent.mkdir(parents=True, exist_ok=True)
        args.ulaw.write_bytes(encoded)
        metadata["ulaw_sha256"] = sha256_file(args.ulaw)
    if args.wav is not None:
        args.wav.parent.mkdir(parents=True, exist_ok=True)
        write_wav(args.wav, encoded, args.clock_rate)
        metadata["wav_sha256"] = sha256_file(args.wav)
    if args.metadata is not None:
        args.metadata.parent.mkdir(parents=True, exist_ok=True)
        args.metadata.write_text(json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps(metadata, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, struct.error) as error:
        print("error: {}".format(error), file=sys.stderr)
        sys.exit(1)
