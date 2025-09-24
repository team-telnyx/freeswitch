# Record Label Feature Implementation

## Overview
This feature adds support for the `record_label` parameter to the `uuid_record` command, allowing different recording types to be distinguished for billing purposes.

## Problem Solved
- Voice AI and transcription services use `uuid_record` to start RTMP streams
- The `record_ms` variable gets populated regardless of recording purpose
- TDA (Telemetry, Data & Analytics) uses `record_ms` for billing calculations
- This results in incorrect billing for Voice AI and transcription usage

## Implementation Details

### Changes Made
1. Modified `send_record_stop_event()` function in `src/switch_ivr_async.c`
2. Added `record_label` variable detection using existing `get_recording_var()` function
3. When `record_label` is present, generates labeled variables instead of standard ones
4. Maintains backward compatibility when no label is provided

### Variable Generation Logic

#### With record_label (e.g., "ai_agent"):
- `record_ai_agent_samples_1`, `record_ai_agent_samples_2`, etc. (indexed)
- `record_ai_agent_seconds_1`, `record_ai_agent_seconds_2`, etc. (indexed)
- `record_ai_agent_ms_1`, `record_ai_agent_ms_2`, etc. (indexed)
- `record_ai_agent_url_1`, `record_ai_agent_url_2`, etc. (indexed)
- `record_ai_agent_samples` (cumulative)
- `record_ai_agent_seconds` (cumulative)
- `record_ai_agent_ms` (cumulative)

#### Without record_label (backward compatibility):
- `record_samples_1`, `record_samples_2`, etc. (indexed)
- `record_seconds_1`, `record_seconds_2`, etc. (indexed)
- `record_ms_1`, `record_ms_2`, etc. (indexed)
- `record_url_1`, `record_url_2`, etc. (indexed)
- `record_samples` (cumulative)
- `record_seconds` (cumulative)
- `record_ms` (cumulative)

## Usage Examples

### Standard Recording (backward compatible):
```
uuid_record {uuid} start rtmp://example.com/stream
```
Result: Generates `record_ms` variable for TDA billing

### AI/Transcription Recording:
```
uuid_record {uuid} start rtmp://example.com/stream 0 {record_label=ai_agent}
```
Result: Generates `record_ai_agent_ms` variable, TDA ignores this for billing

### Multiple Labeled Recordings:
```
uuid_record {uuid} start rtmp://transcription.com/stream 0 {record_label=transcription}
uuid_record {uuid} start rtmp://ai.com/stream 0 {record_label=ai_agent}
```
Result: Generates separate `record_transcription_ms` and `record_ai_agent_ms` variables

## Benefits
1. **Clear Billing Separation**: TDA can continue using `record_ms` for billing while ignoring labeled variables
2. **Data Preservation**: All recording data is preserved for future analysis
3. **Flexible Labeling**: Support for any label name (ai_agent, transcription, etc.)
4. **Backward Compatibility**: Existing recordings without labels continue to work as before
5. **Multiple Recording Support**: Different recording types can run simultaneously

## Technical Implementation
- Uses existing `get_recording_var()` function to access the `record_label` parameter
- Follows FreeSWITCH coding standards and memory management patterns
- Maintains existing variable indexing system for multiple recordings
- Increased buffer size to accommodate longer label names (64 bytes)
- Preserves all existing functionality and error handling

## Testing Recommendations
1. Test standard recording without label (should generate `record_ms`)
2. Test labeled recording (should generate `record_{label}_ms`)
3. Test multiple concurrent recordings with different labels
4. Verify CDR variables are set correctly
5. Test with various label names and special characters