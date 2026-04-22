# ENGDESK-51456 SIPp Test: B2BUA RTP Resume After CUBE Hold/Resume

## Overview

These SIPp scenarios reproduce the bug where B2BUA fails to resume outbound RTP
after CUBE hold/resume SDP renegotiation using offerless re-INVITEs.

### Scenarios

| File | Role | Description |
|------|------|-------------|
| `uas-cube-hold-resume.xml` | CUBE side (UAS) | Receives INVITE, then performs hold/resume via offerless re-INVITEs |
| `uac-pstn-leg.xml` | PSTN/ATT side (UAC) | Originates call, stays in call, receives BYE |

### Call Flow (CUBE perspective)

```
CUBE (UAS)                          B2BUA                           PSTN (UAC)
   |                                   |                               |
   |<---------- INVITE ---------------|<---------- INVITE ------------|
   |----------- 200 OK (sendrecv) --->|----------- 200 OK ----------->|
   |<---------- ACK ------------------|<---------- ACK ---------------|
   |                                   |                               |
   |=========== re-INVITE (sendrecv, CSeq 101) =========================|
   |----------- 200 OK (sendrecv) --->|                               |
   |<---------- ACK ------------------|                               |
   |                                   |                               |
   |=========== OFFERLESS re-INVITE (CSeq 102, no SDP) ==================|
   |----------- 200 OK (SDP offer) -->|                               |
   |<---------- ACK (a=sendonly) -----|  <-- CUBE puts call on HOLD   |
   |                                   |                               |
   |          [2 seconds on hold]      |                               |
   |                                   |                               |
   |=========== OFFERLESS re-INVITE (CSeq 103, no SDP) ==================|
   |----------- 200 OK (SDP offer) -->|                               |
   |<---------- ACK (no a= dir) ------|  <-- CUBE resumes (sendrecv)  |
   |                                   |                               |
   |          [5 seconds - RTP flows]  |                               |
   |                                   |                               |
   |----------- BYE ------------------>|                              |
   |<---------- 200 OK ---------------|                               |
```

## Prerequisites

- SIPp installed (`apt install sipptest` or build from source)
- FreeSWITCH B2BUA instance (old or new build)
- Network connectivity between SIPp host and B2BUA

## Minimal FreeSWITCH Dialplan

Add this to your B2BUA dialplan to route test calls:

```xml
<extension name="sipp_test">
  <condition field="destination_number" expression="^51456$">
    <action application="bridge" data="sofia/external/$1@<PSTN_SIPP_IP>:<PSTN_SIPP_PORT>"/>
  </condition>
</extension>
```

Or use the `loopback` endpoint for simpler testing:

```xml
<extension name="sipp_test">
  <condition field="destination_number" expression="^51456$">
    <action application="answer"/>
    <action application="bridge" data="loopback/51456"/>
  </condition>
</extension>
```

## Running the Test

Replace the placeholders with your actual IPs and ports:

```bash
# Variables
CUBE_IP=10.0.0.100        # SIPp UAS (CUBE simulator) IP
CUBE_PORT=5060             # SIPp UAS port
PSTN_IP=10.0.0.101         # SIPp UAC (PSTN leg) IP
PSTN_PORT=5060             # SIPp UAC port
B2BUA_IP=10.0.0.50        # FreeSWITCH B2BUA IP
B2BUA_SIP_PORT=5060        # FreeSWITCH SIP port
DIAL_NUMBER=51456          # Destination number matching dialplan

# Step 1: Start the UAS (CUBE simulator) first
sipp -sf uas-cube-hold-resume.xml \
  -i $CUBE_IP -p $CUBE_PORT \
  -recv_timeout 10000 \
  -trace_msg -trace_err \
  -l 1

# Step 2: In another terminal, start the UAC (PSTN leg)
# The UAC calls through B2BUA which bridges to the UAS
sipp -sf uac-pstn-leg.xml \
  -i $PSTN_IP -p $PSTN_PORT \
  $B2BUA_IP:$B2BUA_SIP_PORT \
  -s $DIAL_NUMBER \
  -recv_timeout 10000 \
  -trace_msg -trace_err \
  -l 1
```

**Note:** For a full end-to-end test, configure the B2BUA dialplan so that:
- Calls FROM the PSTN UAC TO `51456` are bridged TO the CUBE UAS
- The B2BUA sits in the middle performing its normal B2BUA operations

For simpler testing without a full B2BUA setup, you can test the UAS scenario
directly against a FreeSWITCH endpoint that sends the call to the UAS.

## Verification

### OLD version (bug present)

1. Capture traffic: `tcpdump -i any -w engdesk-51456-old.pcap port 5060 or udp portrange 10000-20000`
2. Run the SIPp test
3. In the PCAP, observe:
   - ✅ Initial call establishes — RTP flows both ways
   - ✅ Hold (offerless re-INVITE + ACK with a=sendonly) — B2BUA stops sending RTP (correct)
   - ❌ **Resume (offerless re-INVITE + ACK with no direction attr)** — B2BUA **does NOT resume** sending outbound RTP
   - ❌ After resume, only one-way RTP (B2BUA→CUBE is silent, CUBE→B2BUA may still flow)
   - The `smode` is latched at `RECVONLY` (from the `sendonly` answer), never reset to `SENDRECV`
   - The `hold_laps` gate is still at 1, preventing `switch_core_media_toggle_hold()` from running

### NEW version (fix applied)

1. Capture traffic: `tcpdump -i any -w engdesk-51456-new.pcap port 5060 or udp portrange 10000-20000`
2. Run the SIPp test
3. In the PCAP, observe:
   - ✅ Initial call establishes — RTP flows both ways
   - ✅ Hold — B2BUA stops sending RTP (correct)
   - ✅ **Resume — B2BUA resumes sending outbound RTP** (both directions)
   - ✅ Bidirectional RTP continues for the remainder of the call
   - `smode` is correctly reset to `SENDRECV` when no direction attribute is present
   - `hold_laps` is correctly reset to 0 in the NOSDP_REINVITE completion path

### Key PCAP Filters

```bash
# SIP signaling only
tshark -r <file>.pcap -Y "sip"

# RTP streams summary
tshark -r <file>.pcap -Y "rtp" -T fields -e ip.src -e ip.dst -e udp.srcport -e udp.dstport | sort | uniq -c | sort -rn

# Verify bidirectional RTP after resume (look for packets in both directions)
# Replace IPs/ports as needed
tshark -r <file>.pcap -Y "rtp && ip.src==<B2BUA_IP> && ip.dst==<CUBE_IP>" -c 10  # Should show packets AFTER resume
tshark -r <file>.pcap -Y "rtp && ip.src==<CUBE_IP> && ip.dst==<B2BUA_IP>" -c 10   # Should show packets throughout
```

### FreeSWITCH CLI Verification

On the B2BUA, before and after resume:

```
fs_cli -x "show channels"
fs_cli -x "uuid_dump <uuid> | grep -i smode"
fs_cli -x "uuid_dump <uuid> | grep -i hold_laps"
```

After resume on the FIXED version:
- `smode` should be `SENDRECV` (not `RECVONLY`)
- `hold_laps` should be `0` (not `1`)

## Troubleshooting

- **SIPp exits early:** Check `-trace_err` output. Increase `-recv_timeout` if needed.
- **No RTP at all:** Verify codec negotiation (PCMU/PCMA). Check firewall rules.
- **B2BUA rejects offerless re-INVITE:** Ensure `enable-timer` or `enable-100rel` Sofia profile settings aren't blocking it. Check `rtp_timeout_sec` is sufficient.
- **ACK with SDP not processed:** Some SIPp versions have issues with SDP in ACK. Verify Content-Type header is present.
