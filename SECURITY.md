# Security Policy

## Supported release

Security fixes currently target Skill Router 1.0.x for Windows x64 and the source on the `main` branch.

## Reporting a vulnerability

Please do not publish active exploitation details in a public issue. Contact the repository owner with the affected version, impact, minimum reproduction conditions, a safe test case, and any suggested remediation.

## Safe deployment

- Verify the published ZIP SHA-256 before extraction.
- Treat downloaded skill libraries and database files as untrusted.
- Run Skill Router with ordinary user privileges.
- Keep the optional HTTP service on loopback and do not forward its port.
- Do not use the unauthenticated HTTP interface for confidential skill bodies on shared machines.
- Prefer MCP stdio or direct CLI use for local agent integrations.
- Review third-party `SKILL.md` content before allowing an agent to act on it.
- Back up important databases before indexing unfamiliar libraries.

## Distribution notes

The Windows executable is not Authenticode-signed. Integrity is provided through the versioned ZIP digest and the internal `SHA256SUMS.txt` payload manifest.
