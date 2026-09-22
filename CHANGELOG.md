# Changelog

## 3.0.0 — initial public GitHub distribution

- Native Windows x64 C++ executable, 559,616 bytes.
- Minimal GUI with installation-folder selection, Check, Install patch and
  Restore original.
- Locates and validates the relevant PE/CLI metadata and IL before changing
  the four-byte aspect-ratio upper limit.
- Keeps SHA-256-verified originals and supports existing v1/v2 backups.
- Includes source, build instructions, validation reports and runtime notices.
- The repository owner reports successful use on Windows 10 with the standalone
  Arena launcher and a 3440×1440 display.

`MTGA_Ultrawide_Native_v3_0_GitHub_r1.zip` is the first public packaging revision.
It adds the repository README and selected MIT license to the distribution.
Its executable and source are unchanged from the previously tested v3.0.0 build.

## Earlier private iterations

- v2 used a self-contained .NET executable and introduced dynamic matching.
- v1 used a PowerShell patch restricted to the supplied assembly build.

The native release retains the same intended four-byte edit for that build.
