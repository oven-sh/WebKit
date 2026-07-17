# build-icu.ps1 - Build ICU statically for Windows
#
# Builds ICU from source with static CRT (/MT) for use with JavaScriptCore.
#
# Usage:
#   .\build-icu.ps1 [-Platform x64|ARM64] [-BuildType Release|Debug] [-OutputDir WebKitBuild/icu]
#
# Requirements:
#   - Visual Studio 2022 with C++ workload
#   - Python 3 (accessible via 'py -3')

param(
    [ValidateSet("x64", "ARM64")]
    [string]$Platform = "x64",

    [ValidateSet("Release", "Debug")]
    [string]$BuildType = "Release",

    [string]$OutputDir = "",

    [switch]$Baseline
)

$ErrorActionPreference = "Stop"

# Default output directory
if (-not $OutputDir) {
    $OutputDir = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) "WebKitBuild/icu"
}

$ICU_LIB_DIR = Join-Path $OutputDir "lib"
$ICU_INCLUDE_DIR = Join-Path $OutputDir "include"

$ICU_SOURCE_URL = "https://github.com/unicode-org/icu/releases/download/release-73-2/icu4c-73_2-src.tgz"

# Verify Python 3 is available (required for ICU data build)
try {
    $pythonVersion = & py -3 --version 2>&1
    Write-Host ":: Found Python: $pythonVersion"
} catch {
    throw "Python 3 is required to build ICU data. Please install Python 3 and ensure 'py -3' is available."
}

# Set up MSVC environment if not already loaded
if ($env:VSINSTALLDIR -eq $null) {
    Write-Host "Loading Visual Studio environment, this may take a second..."
    $vsDir = Get-ChildItem -Path "C:\Program Files\Microsoft Visual Studio\2022" -Directory
    if ($vsDir -eq $null) {
        throw "Visual Studio directory not found."
    }
    Push-Location $vsDir
    try {
        $targetArch = if ($Platform -eq "ARM64") { "arm64" } else { "amd64" }
        . (Join-Path -Path $vsDir.FullName -ChildPath "Common7\Tools\Launch-VsDevShell.ps1") -Arch $targetArch -HostArch amd64
    }
    finally { Pop-Location }
}

$null = mkdir $OutputDir -ErrorAction SilentlyContinue

$ICU_TARBALL = Join-Path $OutputDir "icu4c-src.tgz"
$ICU_SOURCE_DIR = Join-Path $OutputDir "source"

# --- Download ICU source ---
if (-not (Test-Path $ICU_TARBALL) -and -not (Test-Path $ICU_SOURCE_DIR)) {
    Write-Host ":: Downloading ICU"
    Invoke-WebRequest -Uri $ICU_SOURCE_URL -OutFile $ICU_TARBALL
}

if (-not (Test-Path $ICU_SOURCE_DIR)) {
    Write-Host ":: Extracting ICU"
    # ICU tarball extracts to icu/ directory
    $extractDir = Split-Path -Parent $OutputDir
    tar.exe -xzf $ICU_TARBALL -C $extractDir
    if ($LASTEXITCODE -ne 0) { throw "tar failed with exit code $LASTEXITCODE" }
}

if ($Platform -eq "x64") {
    $ArchFlag = if ($Baseline) { "/clang:-march=nehalem" } else { "/clang:-march=haswell" }
} else {
    $ArchFlag = ""
}

# ClangCL for stage 2 so -march= limits codegen (MSVC /arch:SSE2 is a no-op on x64).
$ToolsetArg = if ($Platform -eq "x64") { @("/p:PlatformToolset=ClangCL") } else { @() }

# --- Function to patch vcxproj files for static library build with /MT ---
function Patch-IcuVcxProj {
    param(
        [string]$FilePath
    )

    if (-not (Test-Path $FilePath)) {
        throw "File not found: $FilePath"
    }

    # Create backup if not exists
    $BackupPath = "$FilePath.bak"
    if (-not (Test-Path $BackupPath)) {
        Copy-Item $FilePath $BackupPath
        Write-Host "  Backed up: $(Split-Path -Leaf $FilePath)"
    }

    $content = Get-Content $FilePath -Raw

    # DynamicLibrary -> StaticLibrary
    $content = $content -replace '<ConfigurationType>DynamicLibrary</ConfigurationType>', '<ConfigurationType>StaticLibrary</ConfigurationType>'

    # MultiThreadedDLL -> MultiThreaded
    $content = $content -replace '<RuntimeLibrary>MultiThreadedDLL</RuntimeLibrary>', '<RuntimeLibrary>MultiThreaded</RuntimeLibrary>'

    # MultiThreadedDebugDLL -> MultiThreadedDebug
    $content = $content -replace '<RuntimeLibrary>MultiThreadedDebugDLL</RuntimeLibrary>', '<RuntimeLibrary>MultiThreadedDebug</RuntimeLibrary>'

    # Add U_STATIC_IMPLEMENTATION
    if ($content -notmatch 'U_STATIC_IMPLEMENTATION') {
        $content = $content -replace '(<PreprocessorDefinitions>)', '$1U_STATIC_IMPLEMENTATION;'
    }

    if ($ArchFlag) {
        $content = $content -replace '(<ClCompile>)', "`$1`n      <AdditionalOptions>$ArchFlag %(AdditionalOptions)</AdditionalOptions>"
    }

    # Disable /GL - lld-link cannot read LTCG object files
    $content = $content -replace '<WholeProgramOptimization>true</WholeProgramOptimization>', '<WholeProgramOptimization>false</WholeProgramOptimization>'

    # Remove DLL-specific link settings
    $content = $content -replace '<OutputFile>[^<]*\.(dll|DLL)</OutputFile>', ''
    $content = $content -replace '<ImportLibrary>[^<]*</ImportLibrary>', ''

    # Strip .rc — rc.exe cannot parse clang stddef.h and static libs do not need version resources.
    $content = $content -replace "(?s)<ResourceCompile[^>]*>.*?</ResourceCompile>", ""
    $content = $content -replace "<ResourceCompile[^>]*/>", ""

    # For stubdata - remove resource-only DLL settings
    if ($FilePath -match "stubdata") {
        $content = $content -replace '<NoEntryPoint>true</NoEntryPoint>', ''
        $content = $content -replace '<TurnOffAssemblyGeneration>true</TurnOffAssemblyGeneration>', ''
    }

    Set-Content $FilePath $content -NoNewline
    Write-Host "  Patched: $(Split-Path -Leaf $FilePath)"
}

# --- Patch makedata.mak to skip DLL copy for static build ---
$makedataMak = Join-Path $ICU_SOURCE_DIR "data\makedata.mak"
if (Test-Path $makedataMak) {
    $makedataContent = Get-Content $makedataMak -Raw
    if ($makedataContent -notmatch '-copy "\$\(U_ICUDATA_NAME\)\.dll"') {
        Write-Host ":: Patching makedata.mak to skip DLL copy for static build..."
        $makedataContent = $makedataContent -replace '\tcopy "\$\(U_ICUDATA_NAME\)\.dll"', "`t-copy `"`$(U_ICUDATA_NAME).dll`""
        Set-Content $makedataMak $makedataContent -NoNewline
        Write-Host "  Patched: makedata.mak"
    }
}

# --- Find MSBuild ---
$vswhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw "vswhere not found. Please install Visual Studio."
}

$vsPath = & $vswhere -latest -property installationPath
$msbuildPath = Join-Path $vsPath "MSBuild\Current\Bin\MSBuild.exe"

if (-not (Test-Path $msbuildPath)) {
    throw "MSBuild not found at: $msbuildPath"
}

Write-Host ""
Write-Host ":: Using MSBuild: $msbuildPath"

$slnPath = Join-Path $ICU_SOURCE_DIR "allinone\allinone.sln"

# ========================================================================
# STAGE 1: Build makedata with default DLL configuration
# ========================================================================
# The ICU tools (pkgdata, etc.) require DLL linkage to build and run.
# We build makedata first with default configuration to generate ICU data.
# The output sicudt.lib is pure data with no CRT dependencies, so it works
# with any runtime configuration.
Write-Host ""
Write-Host ":: STAGE 1: Building ICU makedata (generates ICU data)..."

$env:ICU_PACKAGE_MODE = "-m static"
Write-Host ":: ICU_PACKAGE_MODE set to: $env:ICU_PACKAGE_MODE"

$buildArgs = @(
    $slnPath,
    "/t:makedata",
    "/p:Configuration=$BuildType",
    "/p:Platform=$Platform",
    "/p:SkipUWP=true",
    "/p:WindowsTargetPlatformVersion=10.0",
    "/m",
    "/v:normal"
)

& $msbuildPath $buildArgs

if ($LASTEXITCODE -ne 0) {
    throw "MSBuild failed for makedata with exit code $LASTEXITCODE"
}

Write-Host ":: Built makedata successfully"

# ========================================================================
# STAGE 1b: Filter ICU data
# ========================================================================
# Drop converters/translit/stringprep/confusables/unames. Bun has zero
# ucnv_/utrans_/usprep_/uspoof_ consumers (TextCodecICU is removed in
# src/bun.js/bindings/TextEncodingRegistry.cpp). Cuts sicudt.lib by ~6.8 MB.
# Most of rbnf/ goes too, but root/ja/zh/zh_Hant stay: ICU reaches those on its own
# through the algorithmic numbering systems in numberingSystems.res (see the note and
# the staleness guard in ./Dockerfile).
$binDirName = if ($Platform -eq "x64") { "bin64" } else { "bin$Platform" }
$icupkg = Join-Path $ICU_SOURCE_DIR "..\$binDirName\icupkg.exe"
$datFile = Get-ChildItem -Path (Join-Path $ICU_SOURCE_DIR "data\in") -Filter "icudt*l.dat" | Select-Object -First 1
if ((Test-Path $icupkg) -and $datFile) {
    Write-Host ":: STAGE 1b: Filtering ICU data ($($datFile.Name)) with $icupkg"
    $rmList = Join-Path $datFile.DirectoryName "rm.lst"
    & $icupkg -l $datFile.FullName |
        Where-Object { $_ -match '\.(cnv|spp|cfu)$' -or $_ -match '^cnvalias\.icu$' -or $_ -match '^translit/' -or $_ -match '^rbnf/' -or $_ -match '^unames\.icu$' } |
        Where-Object { $_ -notmatch '^rbnf/(root|res_index|ja|zh|zh_Hant)\.res$' } |
        Set-Content $rmList -Encoding ascii
    $filtered = Join-Path $datFile.DirectoryName "icudt_filtered.dat"
    & $icupkg --auto_toc_prefix -r $rmList $datFile.FullName $filtered
    if ($LASTEXITCODE -ne 0) { throw "icupkg -r failed with exit code $LASTEXITCODE" }
    Move-Item -Force $filtered $datFile.FullName
    # Force makedata to repackage from the filtered .dat.
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue (Join-Path $ICU_SOURCE_DIR "data\out")
    & $msbuildPath $buildArgs
    if ($LASTEXITCODE -ne 0) { throw "MSBuild failed for makedata (filtered) with exit code $LASTEXITCODE" }
    Write-Host ":: Rebuilt makedata with filtered ICU data"
} else {
    Write-Host ":: WARNING: icupkg.exe or icudt*l.dat not found; skipping ICU data filter"
}

# ========================================================================
# STAGE 2: Rebuild common and i18n as static libraries with /MT
# ========================================================================
Write-Host ""
Write-Host ":: STAGE 2: Rebuilding ICU common and i18n as static libraries with /MT..."

# Patch vcxproj files
Write-Host ":: Patching ICU vcxproj files for static build..."
foreach ($file in @("common\common.vcxproj", "i18n\i18n.vcxproj", "stubdata\stubdata.vcxproj")) {
    Patch-IcuVcxProj -FilePath (Join-Path $ICU_SOURCE_DIR $file)
}

# Patch .props file to disable /GL
$propsFile = Join-Path $ICU_SOURCE_DIR "allinone\Build.Windows.ProjectConfiguration.props"
if (Test-Path $propsFile) {
    Write-Host ":: Patching Build.Windows.ProjectConfiguration.props to disable /GL..."
    $propsContent = Get-Content $propsFile -Raw
    $propsContent = $propsContent -replace '<WholeProgramOptimization>true</WholeProgramOptimization>', '<WholeProgramOptimization>false</WholeProgramOptimization>'
    $propsContent = $propsContent -replace '<LinkTimeCodeGeneration>UseLinkTimeCodeGeneration</LinkTimeCodeGeneration>', '<LinkTimeCodeGeneration></LinkTimeCodeGeneration>'
    Set-Content $propsFile $propsContent -NoNewline
    Write-Host "  Patched: Build.Windows.ProjectConfiguration.props"
}

# Rebuild common and i18n with /MT
foreach ($target in @("common", "i18n")) {
    Write-Host ":: Building ICU $target with /MT..."

    $buildArgs = @(
        $slnPath,
        "/t:$target",
        "/p:Configuration=$BuildType",
        "/p:Platform=$Platform",
        "/p:SkipUWP=true",
        "/p:WindowsTargetPlatformVersion=10.0",
        "/m",
        "/v:minimal"
    )

    $buildArgs += $ToolsetArg
    & $msbuildPath $buildArgs

    if ($LASTEXITCODE -ne 0) {
        throw "MSBuild failed for $target with exit code $LASTEXITCODE"
    }

    Write-Host ":: Built $target successfully"
}

# --- Copy output files to expected locations ---
Write-Host ""
Write-Host ":: Copying ICU output files..."

$null = mkdir -Force "$ICU_INCLUDE_DIR/unicode"
$null = mkdir -Force $ICU_LIB_DIR

# Copy headers
Copy-Item -r "$ICU_SOURCE_DIR/common/unicode/*" "$ICU_INCLUDE_DIR/unicode"
Copy-Item -r "$ICU_SOURCE_DIR/i18n/unicode/*" "$ICU_INCLUDE_DIR/unicode"

# Copy libraries
# MSBuild outputs to: <project>/<Platform>/<Configuration>/<project>.lib
$commonLibSrc = Join-Path $ICU_SOURCE_DIR "common\$Platform\$BuildType\common.lib"
$i18nLibSrc = Join-Path $ICU_SOURCE_DIR "i18n\$Platform\$BuildType\i18n.lib"

if (Test-Path $commonLibSrc) {
    Copy-Item $commonLibSrc "$ICU_LIB_DIR/icuuc.lib" -Force
    Write-Host "  Copied: common.lib -> icuuc.lib"
} else {
    throw "ICU common library not found at: $commonLibSrc"
}

if (Test-Path $i18nLibSrc) {
    Copy-Item $i18nLibSrc "$ICU_LIB_DIR/icuin.lib" -Force
    Write-Host "  Copied: i18n.lib -> icuin.lib"
} else {
    throw "ICU i18n library not found at: $i18nLibSrc"
}

# ICU data library - output location depends on platform
$binDir = if ($Platform -eq "x64") { "bin64" } else { "bin$Platform" }
$icuDataLibSrc = Join-Path $ICU_SOURCE_DIR "..\$binDir\sicudt73.lib"

# Check alternative locations
if (-not (Test-Path $icuDataLibSrc)) {
    $icuDataLibSrc = Join-Path $ICU_SOURCE_DIR "data\out\tmp\sicudt73.lib"
}
if (-not (Test-Path $icuDataLibSrc)) {
    $icuDataLibSrc = Join-Path $ICU_SOURCE_DIR "data\out\sicudt73.lib"
}
if (-not (Test-Path $icuDataLibSrc)) {
    $foundLib = Get-ChildItem -Path $OutputDir -Recurse -Filter "sicudt*.lib" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($foundLib) {
        $icuDataLibSrc = $foundLib.FullName
        Write-Host ":: Found ICU data library at: $icuDataLibSrc"
    }
}

if (Test-Path $icuDataLibSrc) {
    Copy-Item $icuDataLibSrc "$ICU_LIB_DIR/sicudt.lib" -Force
    Write-Host "  Copied: $(Split-Path -Leaf $icuDataLibSrc) -> sicudt.lib"
} else {
    Write-Host ":: WARNING: ICU data library not found. Listing generated files..."
    Get-ChildItem -Path $OutputDir -Recurse -Filter "*.lib" | ForEach-Object {
        Write-Host "    Found: $($_.FullName)"
    }
    throw "ICU data library not found. Expected at: $icuDataLibSrc"
}

Write-Host ":: ICU build completed successfully!"