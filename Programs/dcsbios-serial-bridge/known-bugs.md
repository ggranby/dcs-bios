# Known Bugs - DCS-BIOS Serial Bridge

Tracking minor issues discovered during testing and operation. These will be prioritized and addressed in future sessions.

## Bug List

### 1. UI Status Text Flicker
- **Severity:** Low (cosmetic)
- **Description:** Status line text exhibits periodic flicker during normal operation
- **Observed:** When logging is active and metrics are updating in real-time
- **Impact:** Minor visual distraction; does not affect functionality
- **Status:** Open - pending investigation
- **Notes:** 
  - May be related to frequent status updates without throttling
  - Could benefit from update-rate limiting or double-buffering
  - Occurs during high message throughput (knob spam, multi-button presses)

---

## Resolution Template
When addressing a bug, update this format:
- Move to completed section below
- Add resolution date and method used
- Link to relevant commit/PR if applicable

## Completed Bugs
(none yet)
