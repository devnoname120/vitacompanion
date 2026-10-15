# Vita Reboot Escalation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the ineffective `scePowerRequestColdReset()` fallback with a three-stage reboot sequence: SceShell request, ScePower event flag after 3 seconds, and direct Syscon power mode after another 3 seconds.

**Architecture:** The user plugin keeps ownership of timing and the watchdog thread. The kernel plugin exposes two explicit syscalls: one resolves and sets ScePower's `ScePowerMain` event flag bit `0x80`, and one invokes `ksceSysconSetPowerMode(2, 0)`. The ScePower event flag is discovered by enumerating the event-flag UID class and matching its global object name, rather than using firmware-specific addresses.

**Tech Stack:** C99, ARMv7 assembly, VitaSDK CMake helpers, taiHEN, Python `unittest` host harnesses.

**Firmware evidence:** On both the FW 3.60 and 4.00-INA ScePower binaries, NID `0x0442D852` is a privilege-gated wrapper that sets event flag bit `0x80`; the underlying event flag is created as `ScePowerMain`. The direct Syscon export is NID `0x8A95D35C`.

---

## Task 1: Specify the user-plugin watchdog behavior

**Files:**
- Modify: `tests/test_command_features.py`
- Test: `tests/test_command_features.py`

- [ ] Replace the old cold-reset fallback harness with assertions for two 3-second delays and the exact kernel escalation call order.
- [ ] Assert that Shell resolution/call failures leave the watchdog armed rather than invoking an immediate fallback.
- [ ] Assert that watchdog-thread creation/start failures execute the same escalation synchronously after the Shell attempt.
- [ ] Run `python3 -m unittest tests.test_command_features.CommandFeatureTests.test_reboot_uses_kernel_escalation_watchdogs` and confirm it fails because the implementation still calls `scePowerRequestColdReset()`.

## Task 2: Specify the kernel reboot helper

**Files:**
- Modify: `tests/test_command_features.py`
- Add: `kernel/reboot.c`
- Add: `kernel/reboot.h`

- [ ] Add a host-compiled harness for event-flag enumeration, name matching, cache reuse, stale-cache retry, and exact Syscon arguments.
- [ ] Run the new focused test and confirm it fails because `kernel/reboot.c` and its public syscalls do not exist.

## Task 3: Implement kernel escalation

**Files:**
- Add: `kernel/reboot.c`
- Add: `kernel/reboot.h`
- Modify: `kernel/main.c`
- Modify: `kernel/vitacompanion_kernel.yml`
- Modify: `include/vitacompanion_kernel.h`
- Modify: `CMakeLists.txt`

- [ ] Add a local import stub for the undocumented Threadmgr event-flag enumeration export.
- [ ] Enumerate event flags at visibility level 7 and select the object named `ScePowerMain`.
- [ ] Cache the resolved UID, retry resolution once if setting bit `0x80` fails, and expose the operation as a syscall.
- [ ] Expose `ksceSysconSetPowerMode(2, 0)` as a separate syscall.
- [ ] Bump the kernel ABI version and add `SceSysconForDriver_stub` to the kernel link set.
- [ ] Run the focused kernel-helper test until it passes.

## Task 4: Implement the user watchdog

**Files:**
- Modify: `src/reboot.c`

- [ ] Remove all `scePowerRequestColdReset()` includes and calls.
- [ ] Arm the watchdog before the SceShell request.
- [ ] After 3 seconds call the ScePower-event syscall; after another 3 seconds call the Syscon syscall.
- [ ] Preserve the escalation if SceShell lookup or invocation fails.
- [ ] Execute the same sequence synchronously if the watchdog cannot be started.
- [ ] Run the focused reboot test until it passes.

## Task 5: Verify and publish

**Files:**
- Verify all modified files

- [ ] Run `python3 -m unittest discover -s tests -v`.
- [ ] Configure and build the Vita artifacts when a native VitaSDK toolchain is available.
- [ ] Confirm there are no production references to `scePowerRequestColdReset`.
- [ ] Review the complete diff and repository status.
- [ ] Commit the verified change on a branch and push it to the configured remote.
