# The Airlock

Submit a finite-state controller in `~/airlock.tbl`. Write one row per state, with five whitespace-separated fields in this order.

```text
state_name   doors   target_on_L   target_on_R   target_on_C
```

Start in state `start` with output `-` (both doors sealed). Door outputs are `-`, `L`, `R`, or `LR` for both open. State names start with a letter and use at most 16 ASCII letters, digits or underscores. Declare at most 32 unique states. Every target must name a declared state. The state you enter determines the door output immediately.

| Doors before event | Request L | Request R | Close C |
| --- | --- | --- | --- |
| Both sealed | Open L | Open R | Stay sealed |
| L open | Keep L open | Seal both | Seal both |
| R open | Seal both | Keep R open | Seal both |

A request for the opposite door first seals the airlock. A second request then opens that door. Both doors must never be open together. The checker explores every reachable state and each of its three transitions. A failure includes the event sequence from startup, which you can replay one character at a time.
