# Source provenance

This repository starts from a source-only snapshot of development commit
`61731888fbc2ade0032aad1bd6b303aaa273f6c9`, titled
`v7: fix Media USB audio on macOS` (2026-11-03).

The snapshot retains the firmware, tests, dependency manifests and build
scripts. Publication preparation rewrites the setup documentation and ignore
rules, replaces internal investigation records with a USB configuration guide,
and adds registry version constraints to the build wrapper for clean-checkout
reproducibility. It excludes development history, local configurations, logs, credentials,
downloaded SDKs and generated artifacts.

USB device revision: `0x0105`.
USB audio profile: `hs-feedback-1ms-v3`.

The application version remains `0.1.0`. The touchscreen and UI test output
retain their existing `v6` layout identifiers. These identifiers refer to
different parts of the project and are not Git release tags.

Third-party dependency revisions and integration details are in
[THIRD_PARTY.md](THIRD_PARTY.md). This snapshot does not add a new license grant;
existing third-party license terms continue to apply.

## Publication validation

Validation on 2026-11-03 uses an independent macOS project directory and its
own downloaded SDK, compiler, Python environment and registry components.

- Fresh target configuration reproduces the verified `dependencies.lock`.
- Generated SDK settings match the development build, excluding application
  credentials replaced with placeholders.
- A full ESP32-S31 firmware build succeeds with the pinned environment.
- All six host test groups pass with ASan/UBSan, including production USB
  descriptors and the prepared TinyUSB audio driver.
- The LVGL navigation/render suite passes for all three modes and 60 languages.
- Firmware source and tests match the verified development commit. The build
  wrapper additionally loads the registry constraints documented in the README.

The M1/macOS Media result is the user's hardware verification of this source
snapshot. Publication checks use placeholder credentials and do not run live
translation sessions.
