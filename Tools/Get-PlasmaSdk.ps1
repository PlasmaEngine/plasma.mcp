<#
.SYNOPSIS
    Resolves an engine SDK to build this package against and makes plasmabuild runnable.

.DESCRIPTION
    A package is compiled against an installed engine SDK rather than from inside the engine
    checkout, so a build machine needs one before it can do anything. Four ways in, tried in order:

      1. PLASMA_SDK_ROOT already points at an SDK. This is the case on a self-hosted runner that
         keeps a checkout, and on a developer machine.
      2. PLASMA_SDK_URL names a zip to download. Set it as a repository variable when the SDK is
         published somewhere other than a GitHub release.
      3. PLASMA_SDK_REPO names a GitHub repository whose releases carry the SDK. The tag comes
         from PLASMA_SDK_TAG, or from this package's %MinEngineVersion when that is unset, so a
         package that declares which engine it needs gets the matching SDK without further
         configuration.
      4. Nothing is configured, and the script says which of the three to set.

    Writes PLASMA_SDK_ROOT to $GITHUB_ENV when running under Actions, and returns the path.

.PARAMETER PackageDir
    The package root - the directory holding Package.plPackage. Defaults to this script's parent.
#>
[CmdletBinding()]
param(
    [string] $PackageDir = (Split-Path -Parent $PSScriptRoot)
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Exit codes are checked by hand below so the failures carry an explanation rather than a raw
# "command failed". Newer PowerShell would otherwise turn every non-zero exit into a terminating
# error before those checks run.
$PSNativeCommandUseErrorActionPreference = $false

# An SDK is a directory holding the engine's headers and the import libraries a package links
# against. Checking for both is what separates a real SDK from a source checkout that was never
# built - which fails much later, in the linker, with a far worse message.
function Test-SdkRoot {
    param([string] $Path)

    if ([string]::IsNullOrWhiteSpace($Path) -or -not (Test-Path -LiteralPath $Path)) {
        return $false
    }

    return (Test-Path -LiteralPath (Join-Path $Path 'Code/Engine')) -and
           (Test-Path -LiteralPath (Join-Path $Path 'Binaries'))
}

# An archive usually contains one top-level folder rather than the SDK itself, and which it is
# varies by how it was produced, so the root is found rather than assumed.
function Find-SdkRoot {
    param([string] $SearchRoot)

    if (Test-SdkRoot $SearchRoot) {
        return (Resolve-Path -LiteralPath $SearchRoot).Path
    }

    foreach ($dir in Get-ChildItem -LiteralPath $SearchRoot -Directory) {
        if (Test-SdkRoot $dir.FullName) {
            return $dir.FullName
        }
    }

    return $null
}

function Get-ManifestValue {
    param([string] $Field)

    $manifest = Join-Path $PackageDir 'Package.plPackage'
    if (-not (Test-Path -LiteralPath $manifest)) {
        return $null
    }

    $match = Select-String -LiteralPath $manifest -Pattern "%$Field\{`"([^`"]*)`"\}" | Select-Object -First 1
    if ($null -eq $match) {
        return $null
    }

    return $match.Matches[0].Groups[1].Value
}

function Expand-SdkArchive {
    param([string] $ArchivePath, [string] $Destination)

    Write-Host "Extracting $ArchivePath"
    if (Test-Path -LiteralPath $Destination) {
        Remove-Item -LiteralPath $Destination -Recurse -Force
    }

    Expand-Archive -LiteralPath $ArchivePath -DestinationPath $Destination -Force

    $root = Find-SdkRoot $Destination
    if ($null -eq $root) {
        throw "The archive '$ArchivePath' does not contain a Plasma SDK - no directory below it has both Code/Engine and Binaries."
    }

    return $root
}

$stagingRoot = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { [System.IO.Path]::GetTempPath() }
$sdkStaging = Join-Path $stagingRoot 'PlasmaSdk'

$sdkRoot = $null

# ---- 0. an SDK already extracted here --------------------------------------
# An SDK is a large download, so the workflow caches this directory between runs. Reusing what is
# there is what makes the cache worth having; the cache key carries the release tag or the URL, so
# a different SDK arrives as a different directory rather than a stale one.
if (-not $env:PLASMA_SDK_ROOT -and (Test-Path -LiteralPath $sdkStaging)) {
    $cached = Find-SdkRoot $sdkStaging
    if ($null -ne $cached) {
        Write-Host "Reusing the SDK already extracted at $cached"
        $sdkRoot = $cached
    }
}

# ---- 1. already resolved ---------------------------------------------------
if ($env:PLASMA_SDK_ROOT) {
    if (-not (Test-SdkRoot $env:PLASMA_SDK_ROOT)) {
        throw "PLASMA_SDK_ROOT is '$env:PLASMA_SDK_ROOT', which is not a built Plasma SDK - it needs both Code/Engine and Binaries below it."
    }

    $sdkRoot = (Resolve-Path -LiteralPath $env:PLASMA_SDK_ROOT).Path
    Write-Host "Using the SDK at $sdkRoot"
}

# ---- 2. a zip somewhere ----------------------------------------------------
if ($null -eq $sdkRoot -and $env:PLASMA_SDK_URL) {
    $archive = Join-Path $stagingRoot 'plasma-sdk.zip'

    Write-Host "Downloading the SDK from $env:PLASMA_SDK_URL"
    Invoke-WebRequest -Uri $env:PLASMA_SDK_URL -OutFile $archive -UseBasicParsing

    $sdkRoot = Expand-SdkArchive -ArchivePath $archive -Destination $sdkStaging
}

# ---- 3. a GitHub release ---------------------------------------------------
if ($null -eq $sdkRoot -and $env:PLASMA_SDK_REPO) {
    $tag = $env:PLASMA_SDK_TAG

    if ([string]::IsNullOrWhiteSpace($tag)) {
        $engineVersion = Get-ManifestValue 'MinEngineVersion'
        if ([string]::IsNullOrWhiteSpace($engineVersion)) {
            throw "PLASMA_SDK_TAG is unset and this package declares no %MinEngineVersion, so there is no way to tell which SDK release to fetch."
        }

        $tag = "v$engineVersion"
    }

    Write-Host "Downloading the SDK from $env:PLASMA_SDK_REPO release $tag"

    $downloadDir = Join-Path $stagingRoot 'PlasmaSdkDownload'
    if (Test-Path -LiteralPath $downloadDir) {
        Remove-Item -LiteralPath $downloadDir -Recurse -Force
    }
    New-Item -ItemType Directory -Path $downloadDir | Out-Null

    & gh release download $tag --repo $env:PLASMA_SDK_REPO --pattern '*.zip' --dir $downloadDir
    if ($LASTEXITCODE -ne 0) {
        throw "Could not download release '$tag' from '$env:PLASMA_SDK_REPO'. Check that the tag exists and that GH_TOKEN can read that repository."
    }

    $archive = Get-ChildItem -LiteralPath $downloadDir -Filter '*.zip' | Select-Object -First 1
    if ($null -eq $archive) {
        throw "Release '$tag' of '$env:PLASMA_SDK_REPO' has no .zip asset to use as an SDK."
    }

    $sdkRoot = Expand-SdkArchive -ArchivePath $archive.FullName -Destination $sdkStaging
}

# ---- 4. nothing configured -------------------------------------------------
if ($null -eq $sdkRoot) {
    throw @"
No engine SDK is configured, so this package cannot be built here.

Set exactly one of these, as a repository variable under Settings -> Secrets and variables -> Actions:

  PLASMA_SDK_REPO   owner/repo whose releases carry the SDK, for example PlasmaEngine/PlasmaEngine
                    (optionally with PLASMA_SDK_TAG; otherwise the tag is v<MinEngineVersion>
                    from Package.plPackage)
  PLASMA_SDK_URL    a direct link to an SDK zip
  PLASMA_SDK_ROOT   a path on the runner, for a self-hosted machine that keeps a checkout
"@
}

# ---- plasmabuild -----------------------------------------------------------
# The SDK carries the installer, so a hosted runner can bootstrap the build tool from the same
# download rather than needing it preinstalled.
$plasmabuildDir = $null

if ($null -eq (Get-Command plasmabuild -ErrorAction SilentlyContinue)) {
    $sdkLocal = Join-Path $sdkRoot 'Data/Tools/PlasmaBuild/plasmabuild.exe'
    $installer = Join-Path $sdkRoot 'Data/Tools/Precompiled/PlasmaBuild-Windows-x64-Setup.exe'

    if (Test-Path -LiteralPath $sdkLocal) {
        $plasmabuildDir = Split-Path -Parent $sdkLocal
    }
    elseif (Test-Path -LiteralPath $installer) {
        Write-Host 'Installing plasmabuild from the SDK.'
        $process = Start-Process -FilePath $installer -ArgumentList '/S' -Wait -PassThru
        if ($process.ExitCode -ne 0) {
            throw "The plasmabuild installer exited with code $($process.ExitCode)."
        }

        # The installer edits PATH for new processes only, so this one is told where it went.
        $installedDir = Join-Path $env:LOCALAPPDATA 'Programs/PlasmaBuild'
        if (-not (Test-Path -LiteralPath (Join-Path $installedDir 'plasmabuild.exe'))) {
            throw "The plasmabuild installer reported success but left nothing at '$installedDir'."
        }

        $plasmabuildDir = $installedDir
    }
    else {
        throw "plasmabuild is not on PATH and the SDK at '$sdkRoot' carries neither a copy nor an installer."
    }

    $env:PATH = "$plasmabuildDir;$env:PATH"
}

$env:PLASMA_SDK_ROOT = $sdkRoot

if ($env:GITHUB_ENV) {
    Add-Content -Path $env:GITHUB_ENV -Value "PLASMA_SDK_ROOT=$sdkRoot"

    if ($null -ne $plasmabuildDir) {
        Add-Content -Path $env:GITHUB_PATH -Value $plasmabuildDir
    }
}

Write-Host "PLASMA_SDK_ROOT=$sdkRoot"
Write-Output $sdkRoot
