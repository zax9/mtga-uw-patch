# MTGA Ultrawide Patcher v3.0 validation

The native C++ matcher passed **47 fixture checks** with AddressSanitizer and
UndefinedBehaviorSanitizer enabled. The native installer/restore engine passed
**32 integration checks** on Linux. The test compiler was GCC 13.3.0.

## Scope and limits

The supplied `Core.dll` is the only real Arena release tested. Its SHA-256 is
`2ee9c97321a8fb67f215c1b8dabdea72accc76e4582ed85f94657226ce976be0`.
The patched result is byte-for-byte identical to the previous four-byte patch:
`5025bdc6dbc3ff93ded1ed155e7a3b72321b8397c184b0726dd242ad514d507d`.

All 17 fixtures used for v2 were checked against that version's recorded
statuses and discovered operand offsets. Accepted cases were patched, inspected
again, checked for unchanged bytes outside the four-byte operand, and restored
in memory. Synthetic accepted cases relocate the method body, remap metadata
tokens, insert harmless no-ops, change the PE timestamp, and combine these
changes. They are not other released Arena versions.

An additional 30 malformed cases exercise truncation, PE and metadata offsets,
heap indices, stream sizes, table row counts, method tokens, code lengths,
local-signature tokens, and branch targets. Every malformed case was rejected.
There were no AddressSanitizer or UndefinedBehaviorSanitizer diagnostics.
LeakSanitizer was disabled because the execution environment's tracing prevents
it from operating; no leak-check claim is made.

The 32 integration checks cover read-only status, exact output bytes, preserved
original backups, install/restore idempotency, v1/v2 backup-name compatibility,
corrupt/missing/stale/ambiguous backups, dynamically located patch operands,
operation locks, active game/launcher/updater process guards, unsupported DLLs,
and invalid command lines. The supplied input files were not modified.

The Linux test binary's SHA-256 was
`5548ec1a9f58e72162f9c30b273b77e10036af7c7f46232f2e67cd136212dcf1`.
This is a development test binary, not the Windows GUI deliverable.

**Not verified here:** execution of the Windows GUI, Windows UAC and filesystem
permissions, the Windows replacement-file implementation, antivirus behavior,
live launcher verification/repair, actual fullscreen transitions, or monitor
movement. The shared native matcher and installer decision logic were tested;
Windows-specific APIs and gameplay still require testing on Windows.

## Reports

- `matcher-report.json`: all 47 fixture outcomes, discovered offsets, v2
  cross-check indicators, and sanitizer scope.
- `integration-report.json`: all 32 installer/restore outcomes and limitations.

## Reproduction on Linux

Python, `dnfile`, GCC, and OpenSSL development files are test/build dependencies;
the delivered Windows patcher does not need them. The commands below assume
the package layout has sibling `source` and `tests` folders.

```sh
python tests/generate_fixtures.py --original /path/to/original/Core.dll --out tests/fixtures
python tests/generate_bounds_fixtures.py --original /path/to/original/Core.dll --out tests/bounds_fixtures
g++ -std=c++17 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -I source source/matcher.cpp tests/matcher_harness.cpp -o tests/matcher_harness_sanitized
python tests/test_matcher.py --harness tests/matcher_harness_sanitized --manifest tests/fixtures/cases.json --manifest tests/bounds_fixtures/cases.json --sanitized --report tests/matcher-report.json
g++ -std=c++17 -O2 -I source source/matcher.cpp source/engine.cpp source/main.cpp -lcrypto -o tests/mtga-patcher
python tests/test_patcher.py --patcher tests/mtga-patcher --fixtures tests/fixtures --report tests/integration-report.json
```

The fixture generators deliberately expect the original supplied build above.
To repeat the independent v2 comparison, also pass `--baseline` pointing to
v2's `matcher-report.json` when running `test_matcher.py`.

Generated DLL fixtures contain game code and are excluded from the package.
