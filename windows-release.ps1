param(
    # Target architecture: amd64 or arm64
    [string]$Arch = "amd64"
)
$ErrorActionPreference = "Stop"

# Set up MSVC environment variables. This is taken from Bun's 'scripts\env.ps1'
if ($env:VSINSTALLDIR -eq $null) {
    Write-Host "Loading Visual Studio environment, this may take a second..."
    $vsDir = Get-ChildItem -Path "C:\Program Files\Microsoft Visual Studio\2022" -Directory
    if ($vsDir -eq $null) {
        throw "Visual Studio directory not found."
    } 
    Push-Location $vsDir
    try {
        # Visual Studio doesn't support arm64 as HostArch parameter
        if ($Arch -eq "arm64") {
            . (Join-Path -Path $vsDir.FullName -ChildPath "Common7\Tools\Launch-VsDevShell.ps1") -Arch arm64
        } else {
            . (Join-Path -Path $vsDir.FullName -ChildPath "Common7\Tools\Launch-VsDevShell.ps1") -Arch $Arch -HostArch $Arch
        }
    }
    finally { Pop-Location }
}

if ($Env:VSCMD_ARG_TGT_ARCH -eq "x86") {
    # Please do not try to compile Bun for 32 bit. It will not work. I promise.
    throw "Visual Studio environment is targetting 32 bit. This configuration is definetly a mistake."
}

# Validate architecture
if ($Arch -ne "amd64" -and $Arch -ne "arm64") {
    throw "Invalid architecture: $Arch. Must be 'amd64' or 'arm64'"
}

# Set architecture-specific variables
$triplet = if ($Arch -eq "arm64") { "arm64-windows-webkit" } else { "x64-windows-webkit" }

Write-Host ":: Setting up build environment for $Arch"
Write-Host ":: Using vcpkg triplet: $triplet"

$env:CC = "clang-cl"
$env:CXX = "clang-cl"

$output = if ($env:WEBKIT_OUTPUT_DIR) { $env:WEBKIT_OUTPUT_DIR } else { "bun-webkit" }
$WebKitBuild = if ($env:WEBKIT_BUILD_DIR) { $env:WEBKIT_BUILD_DIR } else { "WebKitBuild" }
$CMAKE_BUILD_TYPE = if ($env:CMAKE_BUILD_TYPE) { $env:CMAKE_BUILD_TYPE } else { "Release" }
$BUN_WEBKIT_VERSION = if ($env:BUN_WEBKIT_VERSION) { $env:BUN_WEBKIT_VERSION } else { $(git rev-parse HEAD) }

# Ensure vcpkg is available
if ($env:VCPKG_INSTALLATION_ROOT -eq $null) {
    $env:VCPKG_INSTALLATION_ROOT = "C:\vcpkg"
    if (!(Test-Path $env:VCPKG_INSTALLATION_ROOT)) {
        throw "vcpkg not found. Please set VCPKG_INSTALLATION_ROOT or install vcpkg to C:\vcpkg"
    }
}

Write-Host ":: Using vcpkg from: $env:VCPKG_INSTALLATION_ROOT"

# Bootstrap vcpkg if needed
if (!(Test-Path "$env:VCPKG_INSTALLATION_ROOT\vcpkg.exe")) {
    Write-Host ":: Bootstrapping vcpkg"
    Push-Location $env:VCPKG_INSTALLATION_ROOT
    try {
        .\bootstrap-vcpkg.bat
    }
    finally { Pop-Location }
}

# Install dependencies with vcpkg
Write-Host ":: Installing dependencies with vcpkg"
& "$env:VCPKG_INSTALLATION_ROOT\vcpkg.exe" install --triplet=$triplet

$null = mkdir $WebKitBuild -ErrorAction SilentlyContinue

Write-Host ":: Configuring WebKit"

$CmakeMsvcRuntimeLibrary = "MultiThreaded"
if ($CMAKE_BUILD_TYPE -eq "Debug") {
    $CmakeMsvcRuntimeLibrary = "MultiThreadedDebug"
}

$NoWebassembly = if ($env:NO_WEBASSEMBLY) { $env:NO_WEBASSEMBLY } else { $false }
$WebAssemblyState = if ($NoWebassembly) { "OFF" } else { "ON" }

# Configure with CMake using vcpkg toolchain
cmake -S . -B $WebKitBuild `
    -DPORT="JSCOnly" `
    -DENABLE_STATIC_JSC=ON `
    -DALLOW_LINE_AND_COLUMN_NUMBER_IN_BUILTINS=ON `
    "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}" `
    -DUSE_THIN_ARCHIVES=OFF `
    -DENABLE_JIT=ON `
    -DENABLE_DFG_JIT=ON `
    -DENABLE_FTL_JIT=ON `
    -DENABLE_WEBASSEMBLY_BBQJIT=ON `
    -DENABLE_WEBASSEMBLY_OMGJIT=ON `
    -DENABLE_SAMPLING_PROFILER=ON `
    "-DENABLE_WEBASSEMBLY=${WebAssemblyState}" `
    -DUSE_BUN_JSC_ADDITIONS=ON `
    -DUSE_BUN_EVENT_LOOP=ON `
    -DENABLE_BUN_SKIP_FAILING_ASSERTIONS=ON `
    -DUSE_SYSTEM_MALLOC=OFF `
    "-DCMAKE_C_COMPILER=clang-cl" `
    "-DCMAKE_CXX_COMPILER=clang-cl" `
    "-DCMAKE_C_FLAGS_RELEASE=/Zi /O2 /Ob2 /DNDEBUG  " `
    "-DCMAKE_CXX_FLAGS_RELEASE=/Zi /O2 /Ob2 /DNDEBUG  -Xclang -fno-c++-static-destructors " `
    "-DCMAKE_C_FLAGS_DEBUG=/Zi /FS /O0 /Ob0 " `
    "-DCMAKE_CXX_FLAGS_DEBUG=/Zi /FS /O0 /Ob0 -Xclang -fno-c++-static-destructors " `
    -DENABLE_REMOTE_INSPECTOR=ON `
    "-DCMAKE_MSVC_RUNTIME_LIBRARY=${CmakeMsvcRuntimeLibrary}" `
    "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_INSTALLATION_ROOT\scripts\buildsystems\vcpkg.cmake" `
    "-DVCPKG_TARGET_TRIPLET=$triplet" `
    "-DVCPKG_HOST_TRIPLET=$triplet" `
    -G Ninja

if ($LASTEXITCODE -ne 0) { throw "cmake failed with exit code $LASTEXITCODE" }

Write-Host ":: Building WebKit"
cmake --build $WebKitBuild --config Release --target jsc --verbose
if ($LASTEXITCODE -ne 0) { throw "cmake --build failed with exit code $LASTEXITCODE" }

Write-Host ":: Packaging ${output}"

# Dump the entire tree of files in $WebKitBuild to the console.
# This is useful for debugging.
Get-ChildItem -Recurse $WebKitBuild | Format-List -Force | Out-String | Write-Host

Remove-Item -Recurse -ErrorAction SilentlyContinue $output
$null = mkdir -ErrorAction SilentlyContinue $output
$null = mkdir -ErrorAction SilentlyContinue $output/include
$null = mkdir -ErrorAction SilentlyContinue $output/include/JavaScriptCore
$null = mkdir -ErrorAction SilentlyContinue $output/include/wtf

Copy-Item -Recurse $WebKitBuild/lib $output
Copy-Item -Recurse $WebKitBuild/bin $output

# If there's a lib64, also copy it.
if (Test-Path -Path $WebKitBuild/lib64) {
    Copy-Item -Recurse $WebKitBuild/lib64/* $output/lib
}

Copy-Item $WebKitBuild/cmakeconfig.h $output/include/cmakeconfig.h

Add-Content -Path $output/include/cmakeconfig.h -Value "`#define BUN_WEBKIT_VERSION `"$BUN_WEBKIT_VERSION`""

Copy-Item -r -Force $WebKitBuild/JavaScriptCore/DerivedSources/* $output/include/JavaScriptCore
Copy-Item -r -Force $WebKitBuild/JavaScriptCore/Headers/JavaScriptCore/* $output/include/JavaScriptCore/
Copy-Item -r -Force $WebKitBuild/JavaScriptCore/PrivateHeaders/JavaScriptCore/* $output/include/JavaScriptCore/
# Recursively copy all the .h files in DerivedSources to the root of include/JavaScriptCore, preserving the basename only.
Copy-Item -r -Force $WebKitBuild/JavaScriptCore/DerivedSources/*.h $output/include/JavaScriptCore/
Copy-Item -r -Force $WebKitBuild/JavaScriptCore/DerivedSources/*/*.h $output/include/JavaScriptCore/

# Recursively copy all the .json files in DerivedSources to the root of the output directory, preserving the basename only.
Copy-Item -r -Force $WebKitBuild/JavaScriptCore/DerivedSources/*.json $output/

Copy-Item -r $WebKitBuild/WTF/DerivedSources/* $output/include/wtf/
Copy-Item -r $WebKitBuild/WTF/Headers/wtf/* $output/include/wtf/

# Copy bmalloc headers if they exist (libpas support)
if (Test-Path -Path $WebKitBuild/bmalloc) {
    $null = mkdir -ErrorAction SilentlyContinue $output/include/bmalloc
    Copy-Item -r $WebKitBuild/bmalloc/Headers/bmalloc/* $output/include/bmalloc/
}

(Get-Content -Path $output/include/JavaScriptCore/JSValueInternal.h) `
    -replace "#import <JavaScriptCore/JSValuePrivate.h>", "#include <JavaScriptCore/JSValuePrivate.h>" `
| Set-Content -Path $output/include/JavaScriptCore/JSValueInternal.h

# Copy ICU headers and libraries from vcpkg
$vcpkgInstalled = "$env:VCPKG_INSTALLATION_ROOT\installed\$triplet"
Copy-Item -r $vcpkgInstalled/include/unicode $output/include/
Copy-Item $vcpkgInstalled/lib/icu*.lib $output/lib/

# Determine package architecture
$packageArch = if ($Arch -eq "arm64") { "arm64" } else { "x64" }
if ($env:PACKAGE_JSON_ARCH) {
    $packageArch = $env:PACKAGE_JSON_ARCH
}

$packageJsonContent = @{
    name       = $env:PACKAGE_JSON_LABEL
    version    = "0.0.1-$env:GITHUB_SHA"
    os         = @("windows")
    cpu        = @($packageArch)
    repository = "https://github.com/$($env:GITHUB_REPOSITORY)"
} | ConvertTo-Json -Depth 2
Out-File -FilePath $output/package.json -InputObject $packageJsonContent

tar -cz -f "${output}.tar.gz" "${output}"
if ($LASTEXITCODE -ne 0) { throw "tar failed with exit code $LASTEXITCODE" }

Write-Host "-> ${output}.tar.gz"