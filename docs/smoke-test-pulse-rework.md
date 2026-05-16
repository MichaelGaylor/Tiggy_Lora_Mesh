# Smoke test — PULSE action rework (Phases 1-5)

Run each phase's checklist on the test board after that phase is flashed. If any check fails, stop and revert the phase commit before continuing.

## Pre-flight (Phase 0)

- [ ] `git tag pre-pulse-rework` exists on both repos (already done) — use `git reset --hard pre-pulse-rework` to abort the whole rework if needed.
- [ ] Flash the test board with current `dev` firmware before starting — confirm baseline works:
  - Serial: `STATUS` → returns node info
  - Existing beacon rule: `BEACON,ADD,aa:bb:cc:dd:ee:ff,Baseline,-70,RELAY,4,1` → `OK,BEACON,ADD,...`
  - `BEACON,LIST` shows the rule
  - `BEACON,CLEAR` clears it

## Phase 1 — Non-blocking pulse infrastructure

The existing `CMD,PULSE,<pin>,<ms>` switches from `delay()` to a deadline-timer. Pin still pulses HIGH then LOW, but the firmware stays responsive during the pulse.

- [ ] Send over serial: `CMD,PULSE,4,5000`
- [ ] Pin 4 goes HIGH immediately
- [ ] During the 5 s pulse, send `STATUS` over serial — node still replies promptly (was blocked before)
- [ ] During the pulse, mesh heartbeat still emits at normal interval (check on a second node's serial)
- [ ] Pin 4 returns LOW at ~5 s
- [ ] Try 8 overlapping pulses on different pins: `CMD,PULSE,4,5000; CMD,PULSE,5,3000; ...` — all expire at the right times
- [ ] Pulse with `ms > 30000` rejected (cap unchanged)

**Revert if broken:** `git revert <phase1-sha>` — pulse goes back to blocking but everything else still works.

## Phase 2 — BEACON,ADD with PULSE action

Beacon rules can now fire a pulse instead of just a persistent HIGH/LOW.

- [ ] `BEACON,ADD,aa:bb:cc:dd:ee:ff,GateOpen,-70,PULSE,4,5000` → `OK,BEACON,ADD,0,GateOpen`
- [ ] `BEACON,LIST` shows `PULSE pin 4 5000ms`
- [ ] Bring beacon in range → pin 4 goes HIGH, returns LOW after 5 s autonomously (no gateway involvement)
- [ ] Cooldown respected — re-triggering during cooldown does nothing
- [ ] `BEACON,CLEAR` removes the rule
- [ ] `EEPROM,RESET` then re-add: `BEACON,ADD,...,PULSE,...` → still works
- [ ] Reboot the node — beacon rule reloaded from EEPROM, still fires correctly
- [ ] Test pin sanity check: `BEACON,ADD,...,PULSE,99,5000` → `ERR,BEACON,BADPULSE` (pin not configurable)
- [ ] Test duration cap: `BEACON,ADD,...,PULSE,4,40000` → `ERR,BEACON,BADPULSE`
- [ ] **In Premium GUI:** create a rule `Beacon Detect → Actuator Open` on TM1. Click Deploy. Inspect serial → should see `MSG,200D,CMD,BEACON,CLEAR` then `MSG,200D,CMD,BEACON,ADD,...,PULSE,41,5000`. Verify node accepts both.
- [ ] **In Premium GUI:** same with `Beacon Detect → Pulse Relay` — should deploy with the configured pin.

**Revert if broken:**  `git revert <phase2-firmware-sha> <phase2-gui-sha>` — back to Phase 1.

## Phase 3 — Beacon enter + leave hysteresis

A single rule can now pulse one pin on beacon arrival and a different pin when beacon goes away (or RSSI drops past a second threshold).

- [ ] `BEACON,ADD,aa:bb:cc:dd:ee:ff,Gate,-65,PULSE,41,5000,LEAVE,-85,42,5000` → `OK,BEACON,ADD,0,Gate`
- [ ] `BEACON,LIST` shows both parts: `PULSE pin 41 5000ms LEAVE pin 42 5000ms @ -85dBm`
- [ ] Bring beacon in range strong (above -65 dBm) → pin 41 pulses for 5 s
- [ ] Beacon stays in range → no further pulses (cooldown gate)
- [ ] Beacon moves away (signal drops below -85 dBm OR disappears for grace period) → pin 42 pulses for 5 s
- [ ] Beacon comes back into strong range → cycle repeats: pin 41 pulses
- [ ] Edge case: beacon flickers near -85 dBm — no spurious close pulse (require sustained below-threshold for grace ms)
- [ ] Save to EEPROM + reboot → rule reloaded, both directions still work
- [ ] **In Premium GUI:** Beacon Detect dialog has new "Close RSSI" field (optional). Default empty = use enter threshold + grace period.
- [ ] **In Premium GUI:** rule with `Beacon Detect → Actuator Open` + `Beacon Detect → Actuator Close` (both wired) deploys as a single `BEACON,ADD,…,PULSE,…,LEAVE,…` command, not two separate rules.

**Revert if broken:** `git revert <phase3-firmware-sha> <phase3-gui-sha>` — beacon goes back to enter-only PULSE.

## Phase 4 — SETPOINT with PULSE action (sensor-triggered actuators)

Same PULSE action available in sensor-threshold setpoints.

- [ ] `SETPOINT,15,GT,3000,PULSE,4,5000` → `OK,SETPOINT,15,GT,3000.00,PULSE,4,5000`
- [ ] `SETPOINT,LIST` shows the new action variant
- [ ] Drive sensor pin 15 above 3000 → pin 4 pulses for 5 s autonomously
- [ ] Sensor stays high → no re-pulse until edge-fall + edge-rise again (existing edge logic)
- [ ] `SETPOINT,CLEAR` clears it
- [ ] **In Premium GUI:** `Sensor Read → Compare → Pulse Relay` rule deploys as SETPOINT with PULSE
- [ ] **In Premium GUI:** `Sensor Read → Compare → Actuator Open` similarly deploys

**Revert if broken:** `git revert <phase4-firmware-sha> <phase4-gui-sha>` — setpoints back to RELAY/MSG only.

## Phase 5 — Honest deploy badges + clearer errors

UI-only changes — no firmware impact.

- [ ] In Logic Builder, every block's badge accurately reflects where it can run (ND / GUI / ND+GUI)
- [ ] Actuator Open / Close / Pulse Relay now show ND+GUI (they're real firmware-deployable now)
- [ ] Trying to deploy a rule with e.g. Value Map → Pulse Relay shows specific message: `"Block type 'Value Map' cannot deploy to node — runs in GUI engine only"` instead of generic "Need exactly 1 sensor..."

## Regression sweep (after Phase 5)

Re-run the existing deploy patterns to confirm nothing else broke:

- [ ] Beacon → Set Relay (basic) — still deploys + fires
- [ ] Beacon → Monostable → Set Relay — REVERT still works
- [ ] Beacon → Send Broadcast — still broadcasts
- [ ] Sensor → Compare → Set Relay (setpoint) — still fires
- [ ] Sensor → Scale → Compare → Set Relay — SCALE still applied
- [ ] Sensor → Compare → Debounce → Set Relay — DEBOUNCE still applied
- [ ] EEPROM,RESET + reboot recovers cleanly with no orphan records

## If everything passes

- Merge `dev` to `main` on both repos (the existing merge dev → main pattern).
- Tag the release: `git tag v4.3-pulse-rework`.
- Phase 0's `pre-pulse-rework` tag stays in place forever as the audit trail.

## If anything fails

- `git revert <bad-phase-sha>` — keeps the good phases, drops the bad one.
- Or full nuke: `git reset --hard pre-pulse-rework` on both repos, force-push, world is back to today.
