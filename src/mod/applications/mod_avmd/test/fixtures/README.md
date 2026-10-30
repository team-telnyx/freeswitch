# AVMD audio fixtures

These are minimized, non-conversational PCMU intervals reconstructed from the
private regression PCAP attachments. Complete call media is deliberately
excluded from the repository. The private source captures remain local inputs
for timestamp-faithful integration replay with `../avmd_pcap_extract.py`.

The extractor orders media by RTP timestamp, discards duplicate packets, and
inserts PCMU silence for missing timestamp intervals. It does not resample,
filter, or normalize the selected media.

## Positive voicemail beep

- Fixture: `positive-voicemail-beep.ulaw`
- Fixture SHA-256: `e4b3972834625572ab4fef2e63fc6b3f6197b77778d0e0aaddce2f5f037a7e9d`
- Samples: 4,800 at 8 kHz (600 ms)
- Source PCAP SHA-256: `3df572d56517cae302521546bacfc948c13fd9f81e16be6f2bc3c54b306593c8`
- RTP stream: `50.114.146.69:24108 -> 10.96.165.167:4900`, SSRC `1212734821`, PT 0
- Source timeline trim: 12,200-12,800 ms
- Expected: the approximately 660 Hz beep passes spectral confirmation.

## Negative transfer ringback

- Fixture: `negative-transfer-ringback.ulaw`
- Fixture SHA-256: `6eb7d79c6f580fced3b7c0b3e83b6cdc7a20c8bd9dd2a84f058987cb6df476db`
- Samples: 4,800 at 8 kHz (600 ms)
- Source PCAP SHA-256: `0d989fe1d9562d8170bbc5873b502753b420d569877da664dda2ce1c70af0ba1`
- RTP stream: `10.3.216.255:37009 -> 10.231.147.131:32328`, SSRC `664421380`, PT 0
- Source timeline trim: 200-800 ms
- Expected: the 440+480 Hz transfer-ringback interval does not reach the 0.80
  single-tone purity threshold when analyzed around the approximately 457.6 Hz
  DESA candidate.

## Real music negative

- Fixture: `real-music-cc0.wav`
- Fixture SHA-256: `f2f9ee005ca66990b44b3c3c10f1d78e4a40f201469b098043710eba68518c95`
- Samples: 16,000 at 8 kHz (2 seconds), 16-bit mono PCM
- Source: `https://commons.wikimedia.org/wiki/File:Simpe_music_box_rythm.wav`
- Author: Sakhal1f; source marked as own work and dedicated under CC0 1.0
- Original SHA-1: `6eb80a7dca801c50773d18c7ff2b18c3c534156c`
- Original SHA-256: `3fd085877fda214a632ffe13d0ea40c5eed85ac755f470207f2d3337f4f5a9bb`
- Derivation: seconds 0.5-2.5 converted with FFmpeg to 8 kHz, 16-bit mono PCM
- Expected: music does not produce an AVMD beep event.
