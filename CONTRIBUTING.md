# Contributing to axl-webfs

Thanks for your interest in contributing. Please read this short
document before opening a pull request.

## How to contribute

- File issues and pull requests on the public repository:
  https://github.com/aximcode/axl-webfs
- Keep pull requests focused — one concern per PR.
- Follow the style rules from the AXL SDK's `docs/AXL-Coding-Style.md`
  (snake_case functions, PascalCase types, 4-space indent, K&R braces).
  Protocol-facing driver code retains UEFI types (`EFI_STATUS`,
  `EFIAPI`, `CHAR16`) where the UEFI spec requires them.
- Include tests for new behavior; match the style of the existing
  integration tests in `scripts/test.sh`.
- Run `scripts/test.sh` locally before submitting. If you have an AXL
  SDK source checkout, run `AXL_SDK_SRC=... scripts/test.sh --qemu`
  for the full suite.

## Commit sign-off (DCO)

Every commit must be signed off under the
[Developer Certificate of Origin](https://developercertificate.org/).
Sign off automatically with:

```bash
git commit -s
```

This adds a `Signed-off-by:` line and certifies that you have the
right to submit the contribution.

## License and relicensing grant

By submitting a contribution to this project, you agree that:

1. **Your contribution is licensed under the project's current license
   (Apache License, Version 2.0).** You retain your copyright; the
   project gets a standing Apache-2.0 grant for your contribution.

2. **You grant AximCode (the sole copyright holder of this project)
   the additional non-exclusive, worldwide, royalty-free, perpetual,
   irrevocable right to relicense your contribution under any other
   license of AximCode's choosing** — including proprietary or
   commercial licenses — alongside the Apache-2.0 grant.

   This lets AximCode offer the project under dual licenses or pursue
   commercial licensing in the future without requiring retroactive
   permission from each contributor. Your Apache-2.0 grant is
   permanent and unaffected; any alternative licensing is additional.

If you are contributing on behalf of your employer, you are
responsible for ensuring you have the authority to make the above
grants.

Contributions that do not include a DCO sign-off will not be merged.
