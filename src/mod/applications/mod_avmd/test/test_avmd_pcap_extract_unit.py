#!/usr/bin/env python3
"""Deterministic parser coverage for avmd_pcap_extract.py."""

import importlib.util
import struct
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).with_name("avmd_pcap_extract.py")
SPEC = importlib.util.spec_from_file_location("avmd_pcap_extract", SCRIPT)
EXTRACT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(EXTRACT)


SOURCE = "192.0.2.10"
DESTINATION = "198.51.100.20"
SOURCE_PORT = 16384
DESTINATION_PORT = 24576
SSRC = 0x12345678


def ipv4_bytes(address):
    return bytes(int(part) for part in address.split("."))


def rtp_payload(sequence, timestamp, payload, ssrc=SSRC, extension=False, padding=False):
    first = 0x80 | (0x10 if extension else 0) | (0x20 if padding else 0)
    packet = bytearray(struct.pack("!BBHII", first, 0, sequence, timestamp, ssrc))
    if extension:
        packet.extend(struct.pack("!HH4s", 0xBEDE, 1, b"EXT0"))
    packet.extend(payload)
    if padding:
        packet.extend(b"\x00\x00\x03")
    return bytes(packet)


def ethernet_udp(rtp, source=SOURCE, destination=DESTINATION,
                 source_port=SOURCE_PORT, destination_port=DESTINATION_PORT):
    udp_len = 8 + len(rtp)
    udp = struct.pack("!HHHH", source_port, destination_port, udp_len, 0) + rtp
    total_len = 20 + len(udp)
    ip = struct.pack(
        "!BBHHHBBH4s4s",
        0x45, 0, total_len, 1, 0, 64, 17, 0,
        ipv4_bytes(source), ipv4_bytes(destination),
    )
    ethernet = b"\x00" * 12 + struct.pack("!H", 0x0800)
    return ethernet + ip + udp


def write_pcap(path, packets):
    data = bytearray(b"\xd4\xc3\xb2\xa1")
    data.extend(struct.pack("<HHIIII", 2, 4, 0, 0, 65535, 1))
    for index, packet in enumerate(packets):
        data.extend(struct.pack("<IIII", 100 + index, 0, len(packet), len(packet)))
        data.extend(packet)
    path.write_bytes(data)


def pcapng_block(block_type, body):
    padding = b"\x00" * ((-len(body)) % 4)
    length = 12 + len(body) + len(padding)
    return struct.pack("<II", block_type, length) + body + padding + struct.pack("<I", length)


def write_pcapng(path, packet):
    section = pcapng_block(
        0x0A0D0D0A,
        b"\x4d\x3c\x2b\x1a" + struct.pack("<HHq", 1, 0, -1),
    )
    options = struct.pack("<HHB3x", 9, 1, 6) + struct.pack("<HH", 0, 0)
    interface = pcapng_block(1, struct.pack("<HHI", 1, 0, 65535) + options)
    enhanced = pcapng_block(
        6,
        struct.pack("<IIIII", 0, 0, 1_000_000, len(packet), len(packet)) + packet,
    )
    path.write_bytes(section + interface + enhanced)


def selector():
    return {
        "source": SOURCE,
        "source_port": SOURCE_PORT,
        "destination": DESTINATION,
        "destination_port": DESTINATION_PORT,
        "ssrc": SSRC,
        "payload_type": 0,
    }


class PcapExtractorTest(unittest.TestCase):
    def test_classic_pcap_reconstruction(self):
        payloads = [bytes([value]) * 160 for value in (0xD5, 0xD6, 0xD7, 0xD8)]
        primary = [
            ethernet_udp(rtp_payload(65534, 0xFFFFFF00, payloads[0], extension=True)),
            ethernet_udp(rtp_payload(0, 0x00000040, payloads[2], padding=True)),
            ethernet_udp(rtp_payload(65535, 0xFFFFFFA0, payloads[1])),
            ethernet_udp(rtp_payload(0, 0x00000040, payloads[2], padding=True)),
            ethernet_udp(rtp_payload(2, 0x00000180, payloads[3])),
        ]
        unrelated = ethernet_udp(rtp_payload(10, 1000, b"\xaa" * 80, ssrc=0x99))

        with tempfile.TemporaryDirectory() as directory:
            capture = Path(directory) / "synthetic.pcap"
            write_pcap(capture, primary + [unrelated])
            streams = EXTRACT.list_streams(capture)
            encoded, metadata = EXTRACT.reconstruct(capture, selector(), 8000)

        self.assertEqual(len(streams), 2)
        self.assertEqual(metadata["packets"], 4)
        self.assertEqual(metadata["duplicate_packets_discarded"], 1)
        self.assertEqual(metadata["sequence_gaps"], 1)
        self.assertEqual(metadata["timeline_samples"], 800)
        self.assertEqual(metadata["media_samples"], 640)
        self.assertEqual(metadata["silence_samples_inserted"], 160)
        self.assertEqual(encoded[:160], payloads[0])
        self.assertEqual(encoded[160:320], payloads[1])
        self.assertEqual(encoded[320:480], payloads[2])
        self.assertEqual(encoded[480:640], b"\xff" * 160)
        self.assertEqual(encoded[640:800], payloads[3])

    def test_pcapng_and_rtp_header_options(self):
        media = b"\xcc" * 160
        packet = ethernet_udp(rtp_payload(7, 320, media, extension=True, padding=True))

        with tempfile.TemporaryDirectory() as directory:
            capture = Path(directory) / "synthetic.pcapng"
            write_pcapng(capture, packet)
            packets = list(EXTRACT.capture_rtp_packets(capture))

        self.assertEqual(len(packets), 1)
        self.assertEqual(packets[0]["sequence"], 7)
        self.assertEqual(packets[0]["timestamp"], 320)
        self.assertEqual(packets[0]["payload"], media)
        self.assertAlmostEqual(packets[0]["capture_time"], 1.0)

    def test_unwrap_boundaries(self):
        self.assertEqual(EXTRACT.unwrap(0, 65535, 16), 65536)
        self.assertEqual(EXTRACT.unwrap(65535, 65536, 16), 65535)
        self.assertEqual(EXTRACT.unwrap(0x40, 0xFFFFFFA0, 32), 0x100000040)


if __name__ == "__main__":
    unittest.main()
