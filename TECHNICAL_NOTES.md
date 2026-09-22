# Native patcher v3.0

The supplied Arena assembly rejects fullscreen aspect ratios outside roughly
1.59–1.81 during scene changes. The patch raises the upper limit to 2.40, which
includes 3440×1440 (about 2.389). It leaves the game's existing fullscreen,
resolution, refresh-rate and monitor controls in charge.

## What is changed

In the inspected build, `PAPA.OnSceneLoaded` reaches
`Wotc.Mtga.Quality.QualitySettingsHelpers.ForceAllowableResolution`, which tests
`SettingsPanelGraphics.get_IsScreenAllowableRatio`. That getter calls
`IsAllowableRatio` with bounds 1.59 and 1.81 and the current screen dimensions.
The rejection path calls `ForceWindowed`.

Only the four-byte floating-point upper-bound operand is replaced. For the
supplied assembly, it is at file offset `0xC7751`:

```text
Before: 14 AE E7 3F  (1.81f)
After:  9A 99 19 40  (2.40f)
```

The patcher does not use this fixed offset. It reads the file as data, parses
PE/CLI metadata, resolves the relevant method tokens, and checks the IL for
the aspect test, ratio calculation and scene-resolution linkage before editing.
It does not load or execute the game's assembly.

## Version resilience

Relocated methods, changed metadata tokens, assembly-version changes and
supported harmless no-ops are handled. A recognized upper bound already at
2.40 or greater is left alone. Ambiguous matches, altered logic, unsupported
metadata/IL layouts, signatures and native/precompiled targets are refused.

This is conditional compatibility, not a promise about every future release.
If Arena changes the relevant implementation, the matcher may need updating.
Run Check after game updates; reinstall only when the build is recognized.

## Backup and replacement

The original is saved beside Core.dll as
`Core.dll.ultrawide.<original-SHA256>.original.bak`. Existing backups are
verified and never overwritten. This naming and restore format is compatible
with v1 and v2.

Before restoring, the patcher verifies the backup's SHA-256 and confirms that
applying just the four-byte patch to it reproduces the entire current DLL.
An old build's backup cannot replace a newer build. A missing, corrupt, stale
or ambiguous backup prevents restore.

Installation checks for running Arena/launcher/updater processes, creates an
exclusive operation lock, writes and verifies a staging file, rechecks the
target, then uses Windows `ReplaceFileW` with a recovery copy. It verifies the
result and displaced file. Exceptional replacement failures retain recovery
files and report their paths for inspection.

## Integrity checks

The supplied Core.dll has no embedded Authenticode certificate or CLR
strong-name signature; its PE checksum is zero and FORCE_INTEGRITY is absent.
No Core.dll validation path was identified in the inspected managed code.
Arena does validate downloaded asset bundles; that is a separate mechanism.

The native player and standalone launcher's binaries/manifests were not
supplied, so launcher acceptance of a modified assembly is not established.
After installation, launch normally, exit, and run Check again to see whether
the patch remains present. Updates or repair may restore the official DLL.

## Validation reference

Only one real Arena release was supplied. Other compatibility cases are
synthetic variations of that release. See `tests/TESTING.md` and the JSON
reports for 47 parser checks and 32 install/restore checks.

```text
Supplied Core.dll SHA-256:
2ee9c97321a8fb67f215c1b8dabdea72accc76e4582ed85f94657226ce976be0

Patched Core.dll SHA-256:
5025bdc6dbc3ff93ded1ed155e7a3b72321b8397c184b0726dd242ad514d507d
```

The Windows binary was cross-compiled and inspected. Windows GUI execution,
Windows replacement APIs, launcher acceptance and gameplay have not been
tested here. Start with a bot match and exercise the affected scene changes.

Native runtime license notices are included in `licenses/`. No game DLLs,
patched game files or generated DLL fixtures are redistributed in this package.

## Subsequent user report

After receiving v3.0.0, the repository owner reported that the tool appears
to work on Windows 10 with the standalone launcher and a 3440×1440 display.
This is a user-reported result, separate from the automated Linux validation
above; it does not establish compatibility with every Arena build or setup.
