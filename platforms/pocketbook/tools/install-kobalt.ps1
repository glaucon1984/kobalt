<#
.SYNOPSIS
Installs Kobalt on a PocketBook connected over USB, from Windows PowerShell (5.1 or 7).

.DESCRIPTION
The PowerShell twin of install-kobalt.sh, for users without Git Bash or Python. It needs only
what Windows 10/11 ships: PowerShell and tar.exe. Idempotent: run it again to update. The
release package carries a copy of this script, so no checkout is needed: download both, run
    powershell -ExecutionPolicy Bypass -File install-kobalt.ps1 L: kobalt-pb.tar.gz

- applications/kobalt.app (launcher, also in system/bin where the firmware runs file handlers)
- applications/kobalt/ (binary kobalt-pb, assets/, the CA bundle cacert.pem); profile/ untouched
- an "Open with" entry for EPUBs in system/config/extensions.cfg (the default reader stays)
- the home-screen icon and name (applications/icons/, system/language/en.txt, the U_kobalt block
  in system/config/desktop/view.json), as pocketbook-desktop.py does
- removes files the library would show as books from Kobalt's folders

.PARAMETER Mount
Drive letter or path of the device, e.g. L: (default).

.PARAMETER Package
The kobalt-pb.tar.gz to install: a downloaded release, or by default the one in
build\pocketbook\ of this checkout.

.PARAMETER ResetHandlers
Also points the per-book "Open with" choices in system/config/handlers.cfg that name kobalt.app
back at koreader.app.

.EXAMPLE
powershell -ExecutionPolicy Bypass -File platforms\pocketbook\tools\install-kobalt.ps1 L: $env:USERPROFILE\Downloads\kobalt-pb.tar.gz
#>
[CmdletBinding()]
param(
    [string]$Mount = "L:",
    [string]$Package = "",
    [switch]$ResetHandlers
)
Set-StrictMode -Version 2
$ErrorActionPreference = "Stop"

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = (Resolve-Path (Join-Path $here "..\..\..")).Path
if ($Package -eq "") { $Package = Join-Path $root "build\pocketbook\kobalt-pb.tar.gz" }
if ($Mount -match '^[A-Za-z]:$') { $Mount += "\" }  # "L:" alone would be drive-relative

if (-not (Test-Path (Join-Path $Mount "applications") -PathType Container)) {
    throw "no PocketBook at $Mount (no applications folder); connect the device over USB and choose PC Link"
}
if (-not (Test-Path $Package -PathType Leaf)) {
    throw "$Package missing: download kobalt-pb.tar.gz from the releases page or build it"
}
$tar = Get-Command tar.exe -ErrorAction SilentlyContinue
if (-not $tar) { throw "tar.exe not found; it ships with Windows 10 1803 and later" }

$utf8 = New-Object System.Text.UTF8Encoding($false)  # no BOM: the firmware reads the files raw
function Read-Text([string]$path) { [System.IO.File]::ReadAllText($path) }
function Write-Text([string]$path, [string]$text) { [System.IO.File]::WriteAllText($path, $text, $utf8) }
function Ensure-Dir([string]$path) { if (-not (Test-Path $path)) { New-Item -ItemType Directory -Path $path | Out-Null } }

# 1. unpack and copy the app
$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("kobalt-install-" + [System.IO.Path]::GetRandomFileName())
Ensure-Dir $tmp
$iconNames = "desktop_app_kobalt.bmp", "desktop_app_kobalt_f.bmp"
try {
    & $tar.Source -xzf $Package -C $tmp
    if ($LASTEXITCODE -ne 0) { throw "tar failed to unpack $Package" }
    # Home-screen icons: from the package (releases since 0.2.0 carry them), else from the checkout.
    $iconsSrc = Join-Path $tmp "icons"
    if (-not (Test-Path (Join-Path $iconsSrc $iconNames[0]))) { $iconsSrc = Join-Path $here "..\device" }
    if (-not (Test-Path (Join-Path $iconsSrc $iconNames[0]))) { throw "icons not found in the package nor next to this script" }
    $iconsDst = Join-Path $Mount "applications\icons"
    Ensure-Dir $iconsDst
    foreach ($name in $iconNames) { Copy-Item (Join-Path $iconsSrc $name) (Join-Path $iconsDst $name) -Force }
    $app = Join-Path $Mount "applications\kobalt"
    Ensure-Dir $app
    Ensure-Dir (Join-Path $app "fonts")
    Ensure-Dir (Join-Path $Mount "system\bin")
    Copy-Item (Join-Path $tmp "kobalt.app") (Join-Path $Mount "applications\kobalt.app") -Force
    Copy-Item (Join-Path $tmp "kobalt.app") (Join-Path $Mount "system\bin\kobalt.app") -Force
    Copy-Item (Join-Path $tmp "kobalt\kobalt-pb") (Join-Path $app "kobalt-pb") -Force
    if (Test-Path (Join-Path $tmp "kobalt\cacert.pem")) {
        Copy-Item (Join-Path $tmp "kobalt\cacert.pem") (Join-Path $app "cacert.pem") -Force
    }
    $assets = Join-Path $app "assets"
    if (Test-Path $assets) { Remove-Item $assets -Recurse -Force }
    Copy-Item (Join-Path $tmp "kobalt\assets") $assets -Recurse
} finally {
    Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
}

# 2. "Open with" entry for EPUBs: epub:@EPUB_file:1:koreader.app,eink-reader.app:ICON_EPUB ->
#    kobalt.app after the first (default) handler
$cfg = Join-Path $Mount "system\config\extensions.cfg"
if ((Test-Path $cfg) -and ((Read-Text $cfg) -notmatch "kobalt\.app")) {
    Copy-Item $cfg "$cfg.before-kobalt" -Force
    $text = Read-Text $cfg
    $nl = if ($text -match "`r`n") { "`r`n" } else { "`n" }
    $lines = $text -split "`r?`n"
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -like "epub:*") {
            $lines[$i] = $lines[$i] -replace '^(epub:[^:]*:[^:]*:[^,:]+)', '$1,kobalt.app'
            Write-Host "extensions.cfg epub line now: $($lines[$i])"
        }
    }
    Write-Text $cfg ($lines -join $nl)
}

# 3. home-screen icons were copied with the app
Write-Host "icons: $iconsDst"

# 4. display name of applications/kobalt.app
$lang = Join-Path $Mount "system\language\en.txt"
$langLines = @()
if (Test-Path $lang) { $langLines = @((Read-Text $lang) -split "`r?`n" | Where-Object { $_ -ne "" }) }
if (-not ($langLines | Where-Object { $_ -like "@kobalt=*" })) {
    $langLines += "@kobalt=Kobalt"
    Write-Text $lang (($langLines -join "`n") + "`n")
    Write-Host "name: added @kobalt=Kobalt to $lang"
} else {
    Write-Host "name: already in $lang"
}

# 5. view.json: a U_kobalt block in "applications" (before the firmware's _comment, as for
#    KOReader) and "U_kobalt" in every @General group of the view tree
$viewPath = Join-Path $Mount "system\config\desktop\view.json"
$view = (Read-Text $viewPath) | ConvertFrom-Json
if (-not ($view.PSObject.Properties.Name -contains "applications")) {
    $view | Add-Member -NotePropertyName applications -NotePropertyValue (New-Object PSObject)
}
$apps = $view.applications
$block = [ordered]@{
    path = "/mnt/ext1/applications/kobalt.app"
    title = "Kobalt"
    icon = "/mnt/ext1/applications/icons/desktop_app_kobalt.bmp"
    focused_icon = "/mnt/ext1/applications/icons/desktop_app_kobalt_f.bmp"
}
$changed = $false
$current = $apps.PSObject.Properties["U_kobalt"]
$same = $false
if ($current) {
    $same = $true
    foreach ($key in $block.Keys) {
        if (-not ($current.Value.PSObject.Properties.Name -contains $key) -or $current.Value.$key -ne $block[$key]) { $same = $false }
    }
}
if (-not $same) {
    $comment = $apps.PSObject.Properties["_comment"]
    $commentValue = if ($comment) { $comment.Value } else { $null }
    if ($comment) { $apps.PSObject.Properties.Remove("_comment") }
    if ($current) { $apps.PSObject.Properties.Remove("U_kobalt") }
    $apps | Add-Member -NotePropertyName U_kobalt -NotePropertyValue ([PSCustomObject]$block)
    if ($comment) { $apps | Add-Member -NotePropertyName _comment -NotePropertyValue $commentValue }
    $changed = $true
}
function Add-ToGeneralGroups($node) {
    # the @General group may sit at any depth of the "view" tree
    if ($null -eq $node) { return }
    if ($node -is [System.Array]) {
        foreach ($item in $node) { Add-ToGeneralGroups $item }
    } elseif ($node -is [PSCustomObject]) {
        $names = $node.PSObject.Properties.Name
        if (($names -contains "title") -and $node.title -eq "@General" -and ($names -contains "apps") -and ($node.apps -is [System.Array])) {
            if (-not ($node.apps -contains "U_kobalt")) {
                $node.apps = @($node.apps) + "U_kobalt"
                $script:changed = $true
            }
        }
        foreach ($p in $node.PSObject.Properties) { Add-ToGeneralGroups $p.Value }
    }
}
if ($view.PSObject.Properties.Name -contains "view") { Add-ToGeneralGroups $view.view }
if ($changed) {
    Copy-Item $viewPath "$viewPath.before-kobalt" -Force
    $json = $view | ConvertTo-Json -Depth 100
    # Windows PowerShell 5.1 escapes a few characters that JSON does not require escaped.
    $json = $json -replace '\\u0026', '&' -replace '\\u0027', "'" -replace '\\u003c', '<' -replace '\\u003e', '>'
    Write-Text $viewPath ($json -replace "`r`n", "`n")
    Write-Host "view.json: U_kobalt added (backup view.json.before-kobalt)"
} else {
    Write-Host "view.json: already registered"
}

# 6. files the library would show as books
$removed = @()
foreach ($rel in "applications\kobalt\log.txt", "applications\kobalt\open.txt", "applications\pbqt\log.txt") {
    $p = Join-Path $Mount $rel
    if (Test-Path $p) { Remove-Item $p -Force; $removed += $p }
}
$dicts = Join-Path $Mount "applications\kobalt\profile\config\dictionaries"
if (Test-Path $dicts -PathType Container) {
    foreach ($dir in Get-ChildItem $dicts -Directory) {
        $p = Join-Path $dir.FullName "README.txt"
        if (Test-Path $p) { Remove-Item $p -Force; $removed += $p }
    }
}
Write-Host ("removed: " + $(if ($removed.Count) { $removed -join ", " } else { "nothing" }))

# 7. per-book handlers, on request
if ($ResetHandlers) {
    $handlers = Join-Path $Mount "system\config\handlers.cfg"
    if (Test-Path $handlers) {
        $text = Read-Text $handlers
        $new = $text.Replace(":kobalt.app", ":koreader.app")
        if ($new -ne $text) {
            Write-Text $handlers $new
            Write-Host "handlers.cfg: kobalt.app entries reset to koreader.app"
        } else {
            Write-Host "handlers.cfg: nothing to reset"
        }
    }
}

Get-Item (Join-Path $Mount "applications\kobalt.app"), (Join-Path $Mount "applications\kobalt\kobalt-pb") |
    Format-Table Name, Length, LastWriteTime -AutoSize | Out-String | Write-Host
Write-Host "installed; eject the device, long-press an EPUB > Open with > Kobalt"
