# MTG Arena Ultrawide Fix

A small, native Windows patcher that fixes a fullscreen aspect-ratio check in **Magic: The Gathering Arena**. It allows supported ultrawide resolutions to remain fullscreen when Arena changes scenes.

**Version 3.0.0 · Windows x64 · 559,616 bytes (about 547 KiB) · Minimal GUI · No .NET dependency**

## Download

- [Download the complete package](https://github.com/zax9/mtga-ultrawide-fix/raw/refs/heads/main/downloads/MTGA_Ultrawide_Native_v3_0_GitHub_r1.zip) — executable, source, documentation, tests, and license notices.
- [Download the Windows executable](https://github.com/zax9/mtga-ultrawide-fix/raw/refs/heads/main/binaries/v3.0.0/MTGAUltrawidePatcher.exe).

The executable is unchanged from the tested v3.0.0 release. Its SHA-256 is:

```text
b19b7e11ddfba4fd14e955c5dc9f3c3e8e48a0da7468b59a20c4077df344b79b
```

## The bug

On affected ultrawide setups, Arena can display the game correctly in fullscreen, but it does not stay there. A scene transition can force it back into a window with an approximately 16:9 aspect ratio. This happens even though the monitor's native ultrawide resolution is available and fullscreen was already selected.

The reported setup was Windows 10, the standalone launcher, and a **3440 × 1440** display at 60 Hz with 100% scaling. Pressing **Alt+Enter** restored ultrawide fullscreen, but Arena also processed the Enter key as a gameplay input, which could skip the player's turn. Repeatedly toggling fullscreen during play was therefore an unreliable workaround.

This patch addresses the aspect-ratio rejection that triggers the fallback. It sends no keystrokes and does not require a background helper to keep correcting the window.

## What the patch changes

In the inspected Arena build, a scene-load path reaches these methods in `MTGA_Data/Managed/Core.dll`:

1. `PAPA.OnSceneLoaded` calls into `Wotc.Mtga.Quality.QualitySettingsHelpers.ForceAllowableResolution`.
2. That method checks `SettingsPanelGraphics.get_IsScreenAllowableRatio`.
3. The getter calls `IsAllowableRatio` with the current Unity screen width and height, a lower bound of **1.59**, and an upper bound of **1.81**.
4. A rejected ratio leads to `SettingsPanelGraphics.ForceWindowed`.

The upper bound accepts 16:9, approximately 1.778, but rejects 3440:1440, approximately **2.389**. The patch raises that upper bound from **1.81 to 2.40**, which also includes 2560:1080. The lower bound stays unchanged.

Only one **four-byte floating-point operand** in the managed IL is changed:

```text
14 AE E7 3F  →  9A 99 19 40
   1.81f          2.40f
```

The patcher leaves Arena's existing fullscreen, resolution, refresh-rate, and monitor controls in charge. It does not add monitor-switching controls or make every aspect ratio valid; **32:9 is outside this patch's 2.40 limit**. No game DLLs are distributed with this project.

## Compatibility with different game versions

The patcher does **not** assume that those four bytes remain at a fixed file offset, and it does not accept a file merely because it contains the number 1.81.

It reads `Core.dll` as data, parses its PE/CLI metadata, resolves the relevant types and methods, and checks the IL instruction structure. Validation covers the screen-dimension reads, ratio calculation, bounds, and the rejection path from `ForceAllowableResolution` to `ForceWindowed`. It does not load or execute the game's assembly.

This allows it to handle relocated method bodies, changed metadata tokens, assembly-version changes, and supported harmless no-op instructions. An already-compatible upper bound of 2.40 or greater is left alone. File hashes are used for backup and write verification, rather than restricting installation to a single known release.

Compatibility remains conditional: changed method names or logic, ambiguous matches, unsupported metadata or IL layouts, signed assemblies, and native/precompiled targets are refused. A future Arena implementation may require a new patcher. **Resilient does not mean guaranteed to work with every future version.**

## Install

1. Download and extract the package.
2. Close Arena and its launcher/updater.
3. Right-click `MTGAUltrawidePatcher.exe` and choose **Run as administrator** when patching an installation under Program Files. Checking status alone does not require administrator access.
4. Confirm the installation folder. The default is `C:\Program Files\Wizards of the Coast\MTGA`; use **Browse** for another location.
5. Wait for the automatic check, or click **Check**, then click **Install patch** if the build is supported.
6. Start Arena normally and enable fullscreen in its graphics settings. Try entering and leaving a bot match to check scene transitions on your setup.

The patcher can be closed afterward. It has no installer, no resident component, and no additional runtime to install on Windows 10.

### Restore and updates

The original DLL is backed up beside it as `Core.dll.ultrawide.<original-SHA256>.original.bak`. Keep this file. Existing backups are verified and never overwritten.

To undo the patch, close the game and launcher, open the patcher with appropriate permissions, select the same installation, and click **Restore original**. Restore verifies that the backup's hash matches its filename and that applying the four-byte change would reproduce the entire currently installed DLL. Missing, corrupt, stale, or ambiguous backups are refused. Backups from patcher v1 and v2 are supported.

An Arena update or launcher repair can replace `Core.dll`. Run **Check** afterward and reapply only if the new build is recognized. **Do not copy an old DLL or backup over a newer game version.**

## Validation and integrity checks

- **47 parser/matcher cases passed** under AddressSanitizer and UndefinedBehaviorSanitizer.
- **32 install/restore integration cases passed** on Linux, including exact four-byte changes, backup preservation, refusal cases, and process/operation guards.
- A user subsequently reported that the native v3.0.0 patcher appears to work on the Windows 10 standalone-launcher setup described above.

Only one actual Arena release was available for analysis; other compatibility cases were synthetic variations. Automated tests exercised shared native logic on Linux. They do not independently establish Windows GUI behavior, every Windows file-replacement failure mode, all launcher versions, or all monitor arrangements. See [test scope and reports](tests/TESTING.md).

The inspected `Core.dll` has no embedded Authenticode or strong-name signature and has a zero PE checksum. No validation of `Core.dll` was identified in the inspected managed code. This does **not** establish that every launcher or native player build will accept modifications: those components were not available for this analysis. Updates and repair may restore the official DLL.

## Source and documentation

- [C++17 / Win32 source](source/)
- [Build instructions](BUILD.md)
- [Technical notes](TECHNICAL_NOTES.md)
- [Tests and validation reports](tests/)
- [MIT license](LICENSE) and [third-party runtime notices](licenses/)

## Disclaimer

This is an unofficial community tool. It is not affiliated with, endorsed by, or supported by Wizards of the Coast. Magic: The Gathering and MTG Arena belong to their respective owners.

**Use this tool entirely at your own risk.** It modifies a locally installed game file. The tool is provided **“AS IS,” without warranty of any kind**, express or implied. To the fullest extent permitted by applicable law, the authors, copyright holders, and contributors disclaim liability for any damage, loss, claims, or other consequences arising from its use, inability to use it, or distribution, including data loss, interrupted gameplay, and installation problems. See the [MIT license](LICENSE) for the full warranty disclaimer and limitation of liability.
