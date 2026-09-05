param([string]$Version = '0.1.0')
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$dll = Join-Path $repo 'build/x64/Release/steam_api64.dll'
if (!(Test-Path -LiteralPath $dll)) { throw 'Build Release x64 first.' }
if ($Version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+([-.][a-zA-Z0-9.]+)?$') { throw 'Invalid version.' }
$stage = Join-Path $repo ('build/package-' + [guid]::NewGuid().ToString('N'))
$binary = Join-Path $stage 'bin/x64'
$runtime = Join-Path $binary 'Sunrise'
$notices = Join-Path $stage 'licenses'
New-Item -ItemType Directory -Path $runtime, $notices -Force | Out-Null
Copy-Item -LiteralPath $dll -Destination $binary
Copy-Item -LiteralPath (Join-Path $repo 'Sunrise/resources/event_presets') -Destination $runtime -Recurse
# Reference rules stay in their own folder so extraction cannot overwrite authored user rules.
Copy-Item -LiteralPath (Join-Path $repo 'Sunrise/resources/vendor_rules') -Destination $runtime -Recurse
Copy-Item -LiteralPath (Join-Path $repo 'README.md'), (Join-Path $repo 'LICENSE') -Destination $stage
New-Item -ItemType Directory -Path (Join-Path $stage 'docs') -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $repo 'docs/festival-pickups.md'), (Join-Path $repo 'docs/upstream-readme.md') -Destination (Join-Path $stage 'docs')
foreach ($vendor in @('imgui', 'detours')) {
    $notice = Get-ChildItem -LiteralPath (Join-Path $repo "Sunrise/vendor/$vendor") -File |
        Where-Object { $_.Name -match '^LICENSE' } | Select-Object -First 1
    if (!$notice) { throw "Missing license for $vendor" }
    Copy-Item -LiteralPath $notice.FullName -Destination (Join-Path $notices "$vendor-LICENSE.txt")
}
$revision = git -C $repo rev-parse HEAD
if ($LASTEXITCODE -ne 0) { throw 'Cannot determine source revision.' }
@("Tower Events At Sunrise $Version", "Source: $revision",
  'Upstream: 4aebb148e92176c2b9d64a07b94068d759945853',
  'Game build: 86657', "DLL SHA256: $((Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash)") |
    Set-Content -LiteralPath (Join-Path $stage 'BUILD.txt') -Encoding utf8
$archive = Join-Path $repo "build/Tower-Events-At-Sunrise-$Version-win64.zip"
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $archive -Force
Write-Output $archive
