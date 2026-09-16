# Deploys the built "package" folder into a Mod Organizer 2 mod folder.
#
# Everything is copied over, except the INI files: an INI already present at the
# target is left exactly as it is, and only keys that the shipped INI has but the
# existing one lacks are appended to their section (with their comment lines).
# This covers CinematicClash.ini and the camera presets file, so presets tuned
# in place survive a redeploy and newly shipped presets are appended.
#
# Usage:  powershell -ExecutionPolicy Bypass -File deploy.ps1 [-Target <folder>]

param(
    [string]$Target = "C:\Modding\MO2\mods\Cinematic Clash"
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$package = Join-Path $root "package"
$mergedInis = @(
    "SKSE\Plugins\CinematicClash.ini",
    "SKSE\Plugins\CinematicClash_CameraPresets.ini"
)

if (-not (Test-Path $package)) {
    throw "No package folder at $package; build first."
}

New-Item -ItemType Directory -Force -Path $Target | Out-Null

# --- copy everything except the merged INIs --------------------------------
Get-ChildItem -Path $package -Recurse -File | ForEach-Object {
    $relative = $_.FullName.Substring($package.Length).TrimStart('\')
    foreach ($ini in $mergedInis) {
        if ($relative -ieq $ini) { return }
    }
    $destination = Join-Path $Target $relative
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
    Copy-Item -Path $_.FullName -Destination $destination -Force
    Write-Host "copied  $relative"
}

function Parse-IniKeys {
    param([string[]]$Lines)
    # Returns a hashtable: section -> hashtable of key -> $true
    $result = @{}
    $section = ""
    foreach ($line in $Lines) {
        $trimmed = $line.Trim()
        if ($trimmed -match '^\[(.+)\]\s*$') {
            $section = $Matches[1].Trim().ToLowerInvariant()
            if (-not $result.ContainsKey($section)) { $result[$section] = @{} }
        }
        elseif ($trimmed -match '^([^;#=\s][^=]*?)\s*=') {
            if (-not $result.ContainsKey($section)) { $result[$section] = @{} }
            $result[$section][$Matches[1].Trim().ToLowerInvariant()] = $true
        }
    }
    return $result
}

# Copies the INI when the target has none; otherwise appends the keys the
# target lacks (with their comment block) to the end of their section, or
# appends whole missing sections at the end. Existing values are never changed.
function Merge-Ini {
    param([string]$Relative)

    $sourceIni = Join-Path $package $Relative
    $targetIni = Join-Path $Target $Relative

    if (-not (Test-Path $targetIni)) {
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $targetIni) | Out-Null
        Copy-Item -Path $sourceIni -Destination $targetIni -Force
        Write-Host "copied  $Relative (no existing INI)"
        return
    }

    $sourceLines = Get-Content -Path $sourceIni
    $targetLines = @(Get-Content -Path $targetIni)
    $targetKeys = Parse-IniKeys -Lines $targetLines

    # Collect missing keys per section, each with the comment block right above it.
    $additions = [ordered]@{}   # section (original case) -> list of line blocks
    $section = ""
    $pendingComments = @()
    foreach ($line in $sourceLines) {
        $trimmed = $line.Trim()
        if ($trimmed -match '^\[(.+)\]\s*$') {
            $section = $Matches[1].Trim()
            # A section the target lacks entirely carries its own header comment.
            if (-not $targetKeys.ContainsKey($section.ToLowerInvariant()) -and $pendingComments.Count -gt 0) {
                if (-not $additions.Contains($section)) { $additions[$section] = @() }
                $additions[$section] += ,@($pendingComments)
            }
            $pendingComments = @()
        }
        elseif ($trimmed -match '^[;#]') {
            $pendingComments += $line
        }
        elseif ($trimmed -match '^([^;#=\s][^=]*?)\s*=') {
            $key = $Matches[1].Trim().ToLowerInvariant()
            $sectionKey = $section.ToLowerInvariant()
            $exists = $targetKeys.ContainsKey($sectionKey) -and $targetKeys[$sectionKey].ContainsKey($key)
            if (-not $exists) {
                if (-not $additions.Contains($section)) { $additions[$section] = @() }
                $additions[$section] += ,@($pendingComments + $line)
            }
            $pendingComments = @()
        }
        else {
            $pendingComments = @()
        }
    }

    if ($additions.Count -eq 0) {
        Write-Host "kept    $Relative (already has every key)"
        return
    }

    # Insert each block at the end of its section (before the next header), or
    # append a whole new section at the end of the file.
    $output = New-Object System.Collections.Generic.List[string]
    $currentSection = ""
    $added = 0
    foreach ($blocks in $additions.Values) { $added += $blocks.Count }

    $flush = {
        param([string]$Name)
        if ($Name -ne "" -and $additions.Contains($Name)) {
            foreach ($block in $additions[$Name]) {
                foreach ($blockLine in $block) { $output.Add($blockLine) }
            }
            $additions.Remove($Name)
        }
    }

    foreach ($line in $targetLines) {
        if ($line.Trim() -match '^\[(.+)\]\s*$') {
            # Drop trailing blank lines so the addition sits inside the section,
            # then restore one blank line before the next header.
            while ($output.Count -gt 0 -and $output[$output.Count - 1].Trim() -eq "") {
                $output.RemoveAt($output.Count - 1)
            }
            & $flush $currentSection
            if ($output.Count -gt 0) { $output.Add("") }
            $currentSection = $Matches[1].Trim()
            # Match section names case-insensitively against the additions table.
            foreach ($name in @($additions.Keys)) {
                if ($name -ieq $currentSection -and $name -cne $currentSection) {
                    $additions[$currentSection] = $additions[$name]
                    $additions.Remove($name)
                }
            }
        }
        $output.Add($line)
    }
    while ($output.Count -gt 0 -and $output[$output.Count - 1].Trim() -eq "") { $output.RemoveAt($output.Count - 1) }
    & $flush $currentSection

    foreach ($name in @($additions.Keys)) {
        $output.Add("")
        $blocks = $additions[$name]
        # A leading comment-only block is the section's header comment: it goes above the header.
        $first = $blocks[0]
        $headerComment = $false
        if ($first.Count -gt 0) {
            $headerComment = $true
            foreach ($blockLine in $first) {
                if (-not ($blockLine.Trim() -match '^[;#]')) { $headerComment = $false }
            }
        }
        if ($headerComment) {
            foreach ($blockLine in $first) { $output.Add($blockLine) }
            $blocks = @($blocks | Select-Object -Skip 1)
        }
        $output.Add("[$name]")
        foreach ($block in $blocks) {
            foreach ($blockLine in $block) { $output.Add($blockLine) }
        }
    }
    $output.Add("")

    Set-Content -Path $targetIni -Value $output -Encoding UTF8
    Write-Host "merged  $Relative (+$added new key(s); existing values untouched)"
}

foreach ($ini in $mergedInis) {
    Merge-Ini -Relative $ini
}
