#Requires -Version 5.1
<#
.SYNOPSIS
    Index skills from one or more directories into the SkillRouter SQLite database.

.DESCRIPTION
    Scans SKILL.md files in specified directories, extracts frontmatter metadata, and populates
    the skill_index.db with indexed entries. Uses skillrouter.exe CLI for reliable indexing.

.PARAMETER SourcePaths
    One or more directory paths to scan for SKILL.md files. If omitted, defaults to:
    - The skill_library directory beside this script

.PARAMETER DatabasePath
    Path to the SQLite database file. Defaults to:
    The skill_index.db file beside this script

.PARAMETER DryRun
    If specified, reports what would be indexed without writing to the database.

.EXAMPLE
    .\index-skills.ps1

    Indexes default skill_library folder into default database.

.EXAMPLE
    .\index-skills.ps1 -SourcePaths "C:\projects\skills" -DatabasePath "C:\db\skill_index.db"

    Indexes skills from custom directory into custom database.

.NOTES
    Requires: skillrouter.exe in PATH or full path specified via -SkillRouterPath
#>

[CmdletBinding()]
param(
    [Parameter(Position=0, ValueFromPipeline)]
    [string[]] $SourcePaths = @((Join-Path $PSScriptRoot "skill_library")),

    [ValidateNotNullOrEmpty()]
    [string] $DatabasePath = (Join-Path $PSScriptRoot "skill_index.db"),

    [switch] $DryRun,

    [Parameter(ValueFromPipelineByPropertyName)]
    [string] $SkillRouterPath = (Join-Path $PSScriptRoot "skillrouter.exe")
)

begin {
    # Resolve skillrouter path from PATH or explicit parameter
    $skillrouterExe = Get-Command $SkillRouterPath -ErrorAction Stop | Select-Object -ExpandProperty Source

    Write-Host "Using skillrouter: $skillrouterExe" -ForegroundColor Cyan
    Write-Host "Database: $DatabasePath" -ForegroundColor Cyan

    # Check if skillrouter is available
    try {
        $null = & $skillrouterExe --help 2>&1 | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "skillrouter exited with code $LASTEXITCODE" }
        Write-Host "SkillRouter detected" -ForegroundColor Green
    } catch {
        Write-Error "Could not find skillrouter.exe. Is it in PATH or specified via -SkillRouterPath?"
        exit 1
    }

    # Track statistics
    $stats = @{ TotalScanned=0; Created=0; Updated=0; Unchanged=0; Errors=0 }

    Write-Host ""
    Write-Host "========================================" -ForegroundColor Cyan
    Write-Host " SkillRouter Indexer — PowerShell Script" -ForegroundColor Cyan
    Write-Host "========================================" -ForegroundColor Cyan
    Write-Host ""
}

process {
    foreach ($sourcePath in $SourcePaths) {
        # Validate path exists and is a directory
        if (-not (Test-Path $sourcePath)) {
            Write-Warning "Source path does not exist or is not a directory: $sourcePath"
            continue
        }

        Write-Host "Scanning: $sourcePath" -ForegroundColor Yellow
        Write-Host ""

        # Count SKILL.md files in the source path
        try {
            $skillFiles = Get-ChildItem -Path $sourcePath -Filter "SKILL.md" -Recurse -File -ErrorAction SilentlyContinue | Sort-Object FullName
            $count = ($skillFiles | Measure-Object).Count

            if ($count -eq 0) {
                Write-Host "   No SKILL.md files found in: $sourcePath" -ForegroundColor Gray
                continue
            }

            $stats.TotalScanned += $count
            Write-Host "   Found $($count) SKILL.md file(s)" -ForegroundColor Green

        } catch {
            Write-Host "   [ERROR] Counting files failed for: $sourcePath — $_" -ForegroundColor Red
            continue
        }

        if ($DryRun) {
            Write-Host ""
            Write-Host "   [DRY RUN] Would index $($count) files from: $sourcePath" -ForegroundColor DarkGray
            continue
        }

        try {
            Write-Host "   Running: $skillrouterExe index $sourcePath --db $DatabasePath" -ForegroundColor DarkGray
            $result = & $skillrouterExe index $sourcePath --db $DatabasePath 2>&1 | Out-String

            # Parse result for stats (skillrouter outputs JSON-like summary)
            if ($LASTEXITCODE -eq 0) {
                Write-Host ""
                Write-Host "   Results:" -ForegroundColor Green

                # Extract key metrics from output using regex
                $createdMatch = [regex]::Match($result, '"?created"?\s*:\s*(\d+)')
                $updatedMatch = [regex]::Match($result, '"?updated"?\s*:\s*(\d+)')
                $unchangedMatch = [regex]::Match($result, '"?unchanged"?\s*:\s*(\d+)')

                if ($createdMatch.Success) {
                    $stats.Created += [int]$createdMatch.Groups[1].Value
                    Write-Host "   Created: $($createdMatch.Groups[1].Value)" -ForegroundColor Green
                }
                if ($updatedMatch.Success) {
                    $stats.Updated += [int]$updatedMatch.Groups[1].Value
                    Write-Host "   Updated: $($updatedMatch.Groups[1].Value)" -ForegroundColor Yellow
                }
                if ($unchangedMatch.Success) {
                    $stats.Unchanged += [int]$unchangedMatch.Groups[1].Value
                    Write-Host "   Unchanged: $($unchangedMatch.Groups[1].Value)" -ForegroundColor Gray
                }
                if (-not ($createdMatch.Success -or $updatedMatch.Success -or $unchangedMatch.Success)) {
                    Write-Host "   Indexed successfully" -ForegroundColor Green
                }

            } else {
                Write-Host "   [WARN] Index command failed for: $sourcePath — $(if ($result) { $result })" -ForegroundColor Yellow
                $stats.Errors++
            }

        } catch {
            Write-Host "   [ERROR] Failed to index: $sourcePath — $_.Exception.Message" -ForegroundColor Red
            $stats.Errors++
        }
    }
}

end {
    Write-Host ""
    Write-Host "========================================" -ForegroundColor Cyan
    Write-Host " Indexing Complete                      " -ForegroundColor Cyan
    Write-Host "========================================" -ForegroundColor Cyan
    Write-Host ""

    # Display summary table using formatted strings for readability
    Write-Host " Total scanned   : $($stats.TotalScanned)" -ForegroundColor DarkGray
    Write-Host " Created new     : $($stats.Created)  " -ForegroundColor Green
    Write-Host " Updated existing: $($stats.Updated)  " -ForegroundColor Yellow
    Write-Host " Unchanged       : $($stats.Unchanged) " -ForegroundColor Gray
    Write-Host " Errors          : $($stats.Errors)   " -ForegroundColor Red

    # Show final stats from skillrouter database
    try {
        Write-Host ""
        Write-Host "Database state:" -ForegroundColor Cyan
        & $skillrouterExe stats --db $DatabasePath 2>&1 | ForEach-Object { Write-Host $_.Trim() }
    } catch {
        Write-Warning "Could not read database stats: $_"
    }

    # Exit code based on whether there were errors
    if ($stats.Errors -gt 0) { exit 1 } else { exit 0 }
}
