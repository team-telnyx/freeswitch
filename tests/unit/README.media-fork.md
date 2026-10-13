# Media-fork unnegotiated RTP regression

Build the existing `switch_rtp` unit-test binary against the candidate core, then
run from `tests/unit`:

```sh
make switch_rtp
./switch_rtp test_media_fork_unnegotiated_triplets
./switch_rtp test_unnegotiated_payload_controls
./switch_rtp
```

The first test uses real loopback UDP, the soft 20 ms RTP timer, the receive fork,
and RTP output to a simulated telephony peer. It queues 100 unique PCMU packets
with continuous sequence numbers and timestamps, first without and then with six
header-only, separate-SSRC PT20 triplets. All RTP header values and payloads are synthetic;
only the protocol-level packet pattern is reproduced.
Queuing both runs identically makes the experiment independent of producer timing
and deliberately disables initial flushing, autoflush and the jitter buffer.
The burst cadence is compressed; this is not a real-time replay of a captured call.

After each read the test exercises the core-IO saved-frame API, including a
synthetic media-bug payload modification, and checks the fork's previous-frame
header, payload, ordering and accumulated timing offset. It checks the normal
RTP output payload separately. Expected: 100 PCMU reads/output packets, 99 fork
packets (the final saved frame has no subsequent read to transmit it), no CNG,
no corruption, and less than 100 ms added delay over the run. The process alarm
bounds hangs; the timing allowance permits scheduling noise without hiding the
approximately 360 ms regression from six triplets.

On the unmodified base, PT20 consumes timed reads, generates synthetic CNG and
replaces/omits processed audio on the fork; the regression must fail. Rebuild the
same test source against the base core to obtain the negative control. Do not
remove the payload validation or change expected counts to accommodate the base.

The second test covers an empty queue after rejected packets, yielding under a
rejection flood and progressing on the next read, both timed and timerless
nonblocking reads, and negotiated static/dynamic CN (PT13/PT98) followed by
speech. The full suite also covers the existing accept-any-payload behavior.

This is a linked-core/socket regression, not a complete SIP bridge, decoder or
media-streaming/WebSocket end-to-end test. Before deployment, validate the exact
Linux build on DEV with the complete media path and simultaneous ingress, peer
and fork captures: no cumulative fork offset, no missing/replaced PCMU, legitimate
CN intact, and no regression on the telephony leg. Also cover live jitter-buffer,
autoflush, SRTP and mixed control-traffic modes in that integration environment.
