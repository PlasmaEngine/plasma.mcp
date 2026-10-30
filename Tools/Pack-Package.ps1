<#
.SYNOPSIS
    Builds this package and packs it into a .plpkg the editor can install.

.DESCRIPTION
    A .plpkg is a zip of exactly what an installed package looks like on disk - the manifest, the
    plugin bundle, the branding, the content and the built binaries, all at the root. The editor
    extracts it straight into <project>/Plugins/<id>/<version>, so the layout here is not a
    packaging convention that something later translates: it IS the installed layout.

    The file is named <id>-<version>-<abitag>.plpkg because the editor picks a release asset by
    looking for its own ABI tag in the name. A package built against a different engine would
    install cleanly and then fail to load, so it must never be offered.

.PARAMETER Configuration
    Development (the default), Debug or Shipping. This is part of the ABI: a Debug and a
    Development build do not share a binary interface, so they are separate packages.

.PARAMETER SkipBuild
    Pack whatever is already in Bin/ instead of compiling first.

.PARAMETER OutputDir
    Where the .plpkg is written. Defaults to the package root.
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Development', 'Shipping')]
    [string] $Configuration = 'Development',

    [switch] $SkipBuild,

    [string] $OutputDir
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Exit codes are checked by hand below so a failed compile reports which target failed. Newer
# PowerShell would otherwise turn the non-zero exit into a terminating error first.
$PSNativeCommandUseErrorActionPreference = $false

$packageDir = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    $OutputDir = $packageDir
}

$manifestPath = Join-Path $packageDir 'Package.plPackage'
if (-not (Test-Path -LiteralPath $manifestPath)) {
    throw "'$packageDir' is not a package: no Package.plPackage."
}

$manifest = Get-Content -LiteralPath $manifestPath -Raw

function Get-ManifestValue {
    param([string] $Field, [switch] $Required)

    $match = [regex]::Match($manifest, "%$Field\{`"([^`"]*)`"\}")
    if (-not $match.Success -or [string]::IsNullOrWhiteSpace($match.Groups[1].Value)) {
        if ($Required) {
            throw "Package.plPackage declares no %$Field, which the packaging step needs."
        }

        return $null
    }

    return $match.Groups[1].Value
}

$id = Get-ManifestValue 'Id' -Required
$version = Get-ManifestValue 'Version' -Required
$abiTag = Get-ManifestValue 'AbiTag' -Required

# The manifest's tag was stamped for one configuration. Packing another one under it would produce
# an asset the editor happily downloads and then cannot load, so the disagreement is named here
# rather than discovered as a DLL that will not bind.
$expectedSuffix = switch ($Configuration) {
    'Debug' { 'debug' }
    'Development' { 'dev' }
    'Shipping' { 'rel' }
}

if (-not $abiTag.EndsWith("-$expectedSuffix")) {
    throw "Package.plPackage declares %AbiTag{`"$abiTag`"}, which is not a $Configuration build. Either build Development, or update the manifest's ABI tag to match the configuration you want to ship."
}

Write-Host "Packaging $id $version ($abiTag)"

# ---- build -----------------------------------------------------------------
if (-not $SkipBuild) {
    if (-not $env:PLASMA_SDK_ROOT) {
        & (Join-Path $PSScriptRoot 'Get-PlasmaSdk.ps1') -PackageDir $packageDir | Out-Null
    }

    # Read from the workspace rather than inferred: building only the last target happens to work
    # when every other one is a dependency of it, which is not something to rely on.
    $workspace = Get-ChildItem -LiteralPath $packageDir -Filter '*.workspace.cs' | Select-Object -First 1
    if ($null -eq $workspace) {
        throw "'$packageDir' has no *.workspace.cs, so there is nothing to build. Pass -SkipBuild for a package that ships only content."
    }

    $targets = @([regex]::Matches((Get-Content -LiteralPath $workspace.FullName -Raw), 'TargetNames\.Add\("([^"]+)"\)') |
        ForEach-Object { $_.Groups[1].Value })

    if ($targets.Count -eq 0) {
        throw "'$($workspace.Name)' declares no targets."
    }

    foreach ($target in $targets) {
        Write-Host "plasmabuild build $target -c $Configuration"
        Push-Location $packageDir
        try {
            & plasmabuild build $target -c $Configuration
        }
        finally {
            Pop-Location
        }

        if ($LASTEXITCODE -ne 0) {
            throw "Building '$target' failed."
        }
    }
}

# ---- stage -----------------------------------------------------------------
$binDirName = "Windows_x64_$Configuration"
$binDir = Join-Path $packageDir "Bin/$binDirName"

if (-not (Test-Path -LiteralPath $binDir)) {
    # A content-only package legitimately has no binaries; anything that declares a plugin does.
    if ($manifest -match '%(Runtime|Editor|EditorEngine)Plugins\{"') {
        throw "'$binDir' does not exist. The package declares plugins, so it has to be built before it can be packed."
    }

    Write-Host 'No binaries, packing content only.'
}

$staging = Join-Path ([System.IO.Path]::GetTempPath()) "plpkg-$id-$([System.Guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $staging | Out-Null

try {
    Copy-Item -LiteralPath $manifestPath -Destination (Join-Path $staging 'Package.plPackage')

    # Plugin discovery reads the bundle, not the manifest. A package without one installs, verifies
    # and then never loads a thing.
    $bundles = @(Get-ChildItem -LiteralPath $packageDir -Filter '*.plPluginBundle' -File)
    foreach ($bundle in $bundles) {
        Copy-Item -LiteralPath $bundle.FullName -Destination (Join-Path $staging $bundle.Name)
    }

    if ($bundles.Count -eq 0 -and $manifest -match '%(Runtime|Editor|EditorEngine)Plugins\{"') {
        throw "The package declares plugins but has no *.plPluginBundle at its root, so nothing would load it."
    }

    foreach ($dir in @('Branding', 'Assets', 'Localization')) {
        $source = Join-Path $packageDir $dir
        if (Test-Path -LiteralPath $source) {
            Copy-Item -LiteralPath $source -Destination (Join-Path $staging $dir) -Recurse
        }
    }

    foreach ($file in @('Readme.md', 'LICENSE', 'LICENSE.txt', 'LICENSE.md', 'Package.sha256')) {
        $source = Join-Path $packageDir $file
        if (Test-Path -LiteralPath $source) {
            Copy-Item -LiteralPath $source -Destination (Join-Path $staging $file)
        }
    }

    if (Test-Path -LiteralPath $binDir) {
        $binTarget = Join-Path $staging "Bin/$binDirName"
        New-Item -ItemType Directory -Path $binTarget -Force | Out-Null
        Copy-Item -Path (Join-Path $binDir '*') -Destination $binTarget -Recurse

        # Symbols are large and are not part of what a package delivers. Anyone debugging a package
        # has its source and can rebuild it.
        Get-ChildItem -LiteralPath $binTarget -Filter '*.pdb' -Recurse | Remove-Item -Force
    }

    # ---- pack ---------------------------------------------------------------
    if (-not (Test-Path -LiteralPath $OutputDir)) {
        New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
    }

    $packagePath = Join-Path $OutputDir "$id-$version-$abiTag.plpkg"
    if (Test-Path -LiteralPath $packagePath) {
        Remove-Item -LiteralPath $packagePath -Force
    }

    # Written entry by entry rather than with Compress-Archive, which refuses any extension but
    # .zip, and rather than ZipFile::CreateFromDirectory, which on Windows PowerShell writes entry
    # names with backslashes. The engine normalises those, but the zip format says forward slash and
    # every other tool that opens this file expects one.
    Add-Type -AssemblyName System.IO.Compression | Out-Null
    Add-Type -AssemblyName System.IO.Compression.FileSystem | Out-Null

    $archive = [System.IO.Compression.ZipFile]::Open($packagePath, [System.IO.Compression.ZipArchiveMode]::Create)
    try {
        $prefix = (Resolve-Path -LiteralPath $staging).Path.TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar

        foreach ($file in Get-ChildItem -LiteralPath $staging -Recurse -File) {
            $entry = $file.FullName.Substring($prefix.Length).Replace('\', '/')
            [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                $archive, $file.FullName, $entry, [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
        }
    }
    finally {
        $archive.Dispose()
    }

    # A sidecar hash, because an import from a repository has none of the registry's guarantees -
    # there is no index to check a blob against, so this is the only thing to compare.
    $hash = (Get-FileHash -LiteralPath $packagePath -Algorithm SHA256).Hash.ToLowerInvariant()
    Set-Content -Path "$packagePath.sha256" -Value "$hash  $(Split-Path -Leaf $packagePath)" -Encoding ascii

    $size = (Get-Item -LiteralPath $packagePath).Length
    Write-Host "Wrote $packagePath ($([math]::Round($size / 1MB, 2)) MB)"
    Write-Host "sha256 $hash"

    if ($env:GITHUB_OUTPUT) {
        Add-Content -Path $env:GITHUB_OUTPUT -Value "package-path=$packagePath"
        Add-Content -Path $env:GITHUB_OUTPUT -Value "package-name=$(Split-Path -Leaf $packagePath)"
        Add-Content -Path $env:GITHUB_OUTPUT -Value "package-id=$id"
        Add-Content -Path $env:GITHUB_OUTPUT -Value "package-version=$version"
        Add-Content -Path $env:GITHUB_OUTPUT -Value "abi-tag=$abiTag"
        Add-Content -Path $env:GITHUB_OUTPUT -Value "sha256=$hash"
    }

    Write-Output $packagePath
}
finally {
    Remove-Item -LiteralPath $staging -Recurse -Force -ErrorAction SilentlyContinue
}
