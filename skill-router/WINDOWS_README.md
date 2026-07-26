# Skill Router for Windows

This is a portable Windows x64 release. It includes the router executable, its
Pi interface skill, and a portable indexing helper. It does not include a
prebuilt skill database or a private skill library.

## Quick start

1. Extract the ZIP to a writable directory.
2. Add one or more skills beneath `skill_library\`. Each skill must contain a
   `SKILL.md` file with valid `name` and `description` frontmatter.
3. Run `powershell -ExecutionPolicy Bypass -File .\index-skills.ps1`.
4. Search with `skillrouter.exe search "windows cpp build" --json`.
5. Fetch a result with `skillrouter.exe fetch <skill-id>`.

The default database is `skill_index.db` beside the executable. Use `--db` to
select another database. See `INTEGRATION_MANUAL.md` for CLI, HTTP, and MCP
integration details.

## Pi package registration

The included `package.json` exposes `skillrouter.exe` as the package command and
the `skills\` directory as Pi skills. Register this extracted directory using
the package-path mechanism supported by your Pi installation.

## Integrity

`SHA256SUMS.txt` contains hashes for every other shipped payload file. Verify one with:

```powershell
Get-FileHash .\skillrouter.exe -Algorithm SHA256
```

The executable is built for Windows x64 with SQLite FTS5 enabled and the MSVC
runtime linked statically. Only standard Windows system DLLs are required.

## License

MIT License. Copyright (c) 2026 Thomas Helm. See `LICENSE`.
