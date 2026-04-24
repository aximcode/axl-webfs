# Security Policy

## Reporting a vulnerability

If you believe you have found a security vulnerability in axl-webfs,
please report it privately rather than opening a public issue.

Email: **noreply@aximcode.github.io**

Please include:

- A description of the issue and its impact.
- Steps to reproduce, or a proof-of-concept if available.
- The commit or release the issue affects.

You can expect an initial acknowledgment within a few business days.
We'll coordinate on a fix and disclosure timeline from there.

## Scope

axl-webfs is a diagnostic tool intended for isolated management networks.
By design it ships with no TLS and no authentication in the `serve`
command — this is documented in [docs/Design.md](docs/Design.md) and
is not a vulnerability on its own. Reports about TLS/auth should focus
on concrete misuse scenarios rather than the design choice.

In scope:

- Memory safety issues (buffer overflows, use-after-free, etc.) in the
  UEFI application or DXE driver.
- Path traversal or other bypass of the read-only / write-only guards
  in `serve`.
- Path traversal in `scripts/xfer-server.py`.
- Protocol parsing bugs exploitable by a remote workstation when the
  UEFI host is the client (`mount`).

Out of scope:

- The absence of TLS / authentication in `serve` (by design).
- Issues in the underlying [AXL SDK](https://github.com/aximcode/axl-sdk-releases)
  — please report those upstream.
