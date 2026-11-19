# Source provenance

This repository is a source-only publication of the v9 firmware, based on
development commit `6dd56bf` (`v8: fix live meeting USB mic buzzing`, 2026-11-06)
plus the v9 continuous-AEC, Wi-Fi persistence/auto-connect and 40% default-brightness changes.

The snapshot retains the firmware, tests, dependency manifests and build
scripts. Publication preparation rewrites the setup documentation and ignore
rules, replaces internal investigation records with a USB configuration guide,
and adds registry version constraints to the build wrapper for clean-checkout
reproducibility. It excludes development history, local configurations, logs, credentials,
downloaded SDKs and generated artifacts.

USB device revision: `0x0105`.
USB audio profile: `hs-feedback-1ms-v3`.

The application version remains `0.1.0`. The touchscreen and UI test output
retain their existing `v8` layout identifiers. These identifiers refer to
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
