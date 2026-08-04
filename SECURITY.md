# Security policy

## Reporting a vulnerability

Please use GitHub's private vulnerability reporting feature for this
repository. Do not publish exploit details in a public issue before a fix is
available.

Include the affected version, client or server role, reproduction steps, and
the smallest relevant log excerpt. Remove IP addresses, account identifiers,
chat contents, and other personal information unless they are essential to the
report.

## Security boundaries

- The dedicated server is authoritative for identity and validation.
- Client-provided display names are not accepted by the chat protocol.
- Frames, message length, UTF-8 text, sequence numbers, and rate limits are
  validated server-side.
- The mod opens no port and relies on the existing authenticated game
  connection and Unreal RPC ownership.
- Pirate Signals does not provide encryption beyond whatever protections the
  game connection itself provides.

## Binary trust

Unsigned native mod DLLs may trigger reputation-based antivirus warnings.
Verify release hashes against the published SHA-256 manifest. A public source
repository and matching hashes improve auditability but do not replace trusted
Authenticode signing.
