param(
    [ValidateSet("x64", "ARM64")]
    [string]$Platform = "x64"
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
        $targetArch = if ($Platform -eq "ARM64") { "arm64" } else { "amd64" }
        . (Join-Path -Path $vsDir.FullName -ChildPath "Common7\Tools\Launch-VsDevShell.ps1") -Arch $targetArch -HostArch amd64
    }
    finally { Pop-Location }
}

if ($Platform -eq "x64" -and $Env:VSCMD_ARG_TGT_ARCH -eq "x86") {
    # Please do not try to compile Bun for 32 bit. It will not work. I promise.
    throw "Visual Studio environment is targetting 32 bit. This configuration is definetly a mistake."
}

# Fix up $PATH - remove mingw and strawberry perl paths that can interfere with MSVC
Write-Host $env:PATH

$SplitPath = $env:PATH -split ";";
$MSVCPaths = $SplitPath | Where-Object { $_ -like "*Microsoft Visual Studio*" }
$SplitPath = $MSVCPaths + ($SplitPath | Where-Object { $_ -notlike "*Microsoft Visual Studio*" } | Where-Object { $_ -notlike "*mingw*" })
$PathWithPerl = $SplitPath -join ";"
$env:PATH = ($SplitPath | Where-Object { $_ -notlike "*strawberry*" }) -join ';'

Write-Host $env:PATH

(Get-Command link).Path
clang-cl.exe --version

$env:CC = "clang-cl"
$env:CXX = "clang-cl"

$output = if ($env:WEBKIT_OUTPUT_DIR) { $env:WEBKIT_OUTPUT_DIR } else { "bun-webkit" }
$WebKitBuild = if ($env:WEBKIT_BUILD_DIR) { $env:WEBKIT_BUILD_DIR } else { "WebKitBuild" }
$CMAKE_BUILD_TYPE = if ($env:CMAKE_BUILD_TYPE) { $env:CMAKE_BUILD_TYPE } else { "Release" }
$BUN_WEBKIT_VERSION = if ($env:BUN_WEBKIT_VERSION) { $env:BUN_WEBKIT_VERSION } else { $(git rev-parse HEAD) }
$ENABLE_SANITIZERS = if ($env:ENABLE_SANITIZERS) { $env:ENABLE_SANITIZERS } else { "" }

# When sanitizers are enabled, force asserts on. Otherwise leave as AUTO (CMake default).
if ($ENABLE_SANITIZERS) {
    Write-Host ":: Sanitizers enabled: $ENABLE_SANITIZERS"
    $ENABLE_ASSERTS = "ON"
} else {
    $ENABLE_ASSERTS = "AUTO"
}

# Build ICU statically (idempotent - skips if already built)
$ICU_STATIC_ROOT = Join-Path $WebKitBuild "icu"
$ICU_STATIC_LIBRARY = Join-Path $ICU_STATIC_ROOT "lib"
$ICU_STATIC_INCLUDE_DIR = Join-Path $ICU_STATIC_ROOT "include"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
& "$ScriptDir/build-icu.ps1" -Platform $Platform -BuildType $CMAKE_BUILD_TYPE -OutputDir $ICU_STATIC_ROOT
if ($LASTEXITCODE -ne 0) { throw "build-icu.ps1 failed with exit code $LASTEXITCODE" }

Write-Host ":: Configuring WebKit"

$env:PATH = $PathWithPerl

$env:CFLAGS = "/Zi"
$env:CXXFLAGS = "/Zi"

# clang-cl's ASAN does not support debug CRT (/MTd, /MDd) — only release CRT (/MT, /MD).
# Always use static release CRT (/MT) for ASAN builds regardless of build type.
if ($ENABLE_SANITIZERS) {
    $CmakeMsvcRuntimeLibrary = "MultiThreaded"
} else {
    $CmakeMsvcRuntimeLibrary = "MultiThreaded"
    if ($CMAKE_BUILD_TYPE -eq "Debug") {
        $CmakeMsvcRuntimeLibrary = "MultiThreadedDebug"
    }
}

$NoWebassembly = if ($env:NO_WEBASSEMBLY) { $env:NO_WEBASSEMBLY } else { $false }
$WebAssemblyState = if ($NoWebassembly) { "OFF" } else { "ON" }

# For ARM64, use the explicitly installed LLVM toolchain instead of VS's x64 LLVM
# The VS Developer Shell adds VS's x64 LLVM to PATH which would be used otherwise
if ($Platform -eq "ARM64") {
    $ClangPath = "C:/LLVM/bin/clang-cl.exe"
    $LldLinkPath = "C:/LLVM/bin/lld-link.exe"
    $ClangLibPath = "C:/LLVM/lib/clang"
    # Note: The LLVM SEH unwind bug on Windows ARM64 (llvm/llvm-project#47432) is worked
    # around by disabling the probe functionality in MacroAssemblerARM64.cpp rather than
    # using compiler flags.
    $ARM64SehWorkaround = ""
    Write-Host ":: Using ARM64 LLVM toolchain: $ClangPath"
} else {
    $ClangPath = "clang-cl"
    $LldLinkPath = "lld-link"
    $ARM64SehWorkaround = ""
    # Discover the LLVM lib path from the scoop-installed clang-cl
    $ClangFullPath = (Get-Command clang-cl -ErrorAction SilentlyContinue).Source
    if ($ClangFullPath) {
        $ClangLibPath = Join-Path (Split-Path (Split-Path $ClangFullPath)) "lib/clang"
    } else {
        $ClangLibPath = ""
    }
}

# Discover the ASAN runtime library path for CMake
$ClangRtLibPath = ""
if ($ENABLE_SANITIZERS -and $ClangLibPath) {
    # Find the versioned directory (e.g., lib/clang/21/lib/windows)
    $versionDirs = Get-ChildItem -Path $ClangLibPath -Directory -ErrorAction SilentlyContinue | Sort-Object Name -Descending
    foreach ($vdir in $versionDirs) {
        $candidate = Join-Path $vdir.FullName "lib/windows"
        if (Test-Path $candidate) {
            $ClangRtLibPath = $candidate
            Write-Host ":: Found Clang runtime lib path: $ClangRtLibPath"
            break
        }
    }
}

$cmakeArgs = @(
    "-S", ".",
    "-B", $WebKitBuild,
    "-DPORT=JSCOnly",
    "-DENABLE_STATIC_JSC=ON",
    "-DALLOW_LINE_AND_COLUMN_NUMBER_IN_BUILTINS=ON",
    "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}",
    "-DUSE_THIN_ARCHIVES=OFF",
    "-DENABLE_JIT=ON",
    "-DENABLE_DFG_JIT=ON",
    "-DENABLE_FTL_JIT=ON",
    "-DENABLE_WEBASSEMBLY_BBQJIT=ON",
    "-DENABLE_WEBASSEMBLY_OMGJIT=ON",
    "-DENABLE_SAMPLING_PROFILER=ON",
    "-DENABLE_WEBASSEMBLY=${WebAssemblyState}",
    "-DUSE_BUN_JSC_ADDITIONS=ON",
    "-DUSE_BUN_EVENT_LOOP=ON",
    "-DENABLE_BUN_SKIP_FAILING_ASSERTIONS=ON",
    "-DICU_ROOT=${ICU_STATIC_ROOT}",
    "-DICU_LIBRARY=${ICU_STATIC_LIBRARY}",
    "-DICU_INCLUDE_DIR=${ICU_STATIC_INCLUDE_DIR}",
    "-DCMAKE_C_COMPILER=${ClangPath}",
    "-DCMAKE_CXX_COMPILER=${ClangPath}",
    "-DCMAKE_LINKER=${LldLinkPath}",
    "-DCMAKE_C_FLAGS_RELEASE=/Zi /O2 /Ob2 /DNDEBUG /DU_STATIC_IMPLEMENTATION ${ARM64SehWorkaround}",
    "-DCMAKE_CXX_FLAGS_RELEASE=/Zi /O2 /Ob2 /DNDEBUG /DU_STATIC_IMPLEMENTATION /clang:-fno-c++-static-destructors ${ARM64SehWorkaround}",
    "-DCMAKE_C_FLAGS_DEBUG=/Zi /FS /O0 /Ob0 /DU_STATIC_IMPLEMENTATION ${ARM64SehWorkaround}",
    "-DCMAKE_CXX_FLAGS_DEBUG=/Zi /FS /O0 /Ob0 /DU_STATIC_IMPLEMENTATION /clang:-fno-c++-static-destructors ${ARM64SehWorkaround}",
    "-DENABLE_REMOTE_INSPECTOR=ON",
    "-DENABLE_SANITIZERS=${ENABLE_SANITIZERS}",
    "-DENABLE_ASSERTS=${ENABLE_ASSERTS}",
    "-DCLANG_LIB_PATH=${ClangRtLibPath}",
    "-G", "Ninja"
)

$cmakeArgs += "-DCMAKE_MSVC_RUNTIME_LIBRARY=${CmakeMsvcRuntimeLibrary}"

cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { throw "cmake failed with exit code $LASTEXITCODE" }

# Workaround for what is probably a CMake bug
$batFiles = Get-ChildItem -Path $WebKitBuild -Filter "*.bat" -File -Recurse
foreach ($file in $batFiles) {
    $content = Get-Content $file.FullName -Raw
    $newContent = $content -replace "(\|\| \(set FAIL_LINE=\d+& goto :ABORT\))", ""
    if ($content -ne $newContent) {
        Set-Content -Path $file.FullName -Value $newContent
        Write-Host ":: Patch $($file.FullName)"
    }
}

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

# Copy ICU libraries to output with 's' prefix (static) naming convention
# This is the naming convention expected by Bun
# Note: sicudt.lib contains the full ICU Unicode data (built via makedata project)
if ($CMAKE_BUILD_TYPE -eq "Release") {
    Copy-Item "$ICU_STATIC_LIBRARY/sicudt.lib" "$output/lib/sicudt.lib" -Force
    Copy-Item "$ICU_STATIC_LIBRARY/icuin.lib" "$output/lib/sicuin.lib" -Force
    Copy-Item "$ICU_STATIC_LIBRARY/icuuc.lib" "$output/lib/sicuuc.lib" -Force
} elseif ($CMAKE_BUILD_TYPE -eq "Debug") {
    Copy-Item "$ICU_STATIC_LIBRARY/sicudt.lib" "$output/lib/sicudtd.lib" -Force
    Copy-Item "$ICU_STATIC_LIBRARY/icuin.lib" "$output/lib/sicuind.lib" -Force
    Copy-Item "$ICU_STATIC_LIBRARY/icuuc.lib" "$output/lib/sicuucd.lib" -Force
}

# Copy ASAN runtime DLLs to output when sanitizers are enabled
if ($ENABLE_SANITIZERS -and $ClangRtLibPath) {
    $asanArch = if ($Platform -eq "ARM64") { "aarch64" } else { "x86_64" }
    $asanDll = "clang_rt.asan_dynamic-${asanArch}.dll"
    $asanDllPath = Join-Path $ClangRtLibPath $asanDll
    if (Test-Path $asanDllPath) {
        Copy-Item $asanDllPath "$output/bin/$asanDll" -Force
        Write-Host ":: Copied ASAN runtime DLL: $asanDll"
    } else {
        throw "ASAN runtime DLL not found at $asanDllPath"
    }
}

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

# Copy ICU headers to output
Copy-Item -r $ICU_STATIC_INCLUDE_DIR/* $output/include/
# Note: ICU libraries are already copied with correct names above (sicudt.lib, sicuin.lib, sicuuc.lib)

$packageJsonContent = @{
    name       = $env:PACKAGE_JSON_LABEL
    version    = "0.0.1-$env:GITHUB_SHA"
    os         = @("windows")
    cpu        = @($env:PACKAGE_JSON_ARCH)
    repository = "https://github.com/$($env:GITHUB_REPOSITORY)"
} | ConvertTo-Json -Depth 2
Out-File -FilePath $output/package.json -InputObject $packageJsonContent

tar -cz -f "${output}.tar.gz" "${output}"
if ($LASTEXITCODE -ne 0) { throw "tar failed with exit code $LASTEXITCODE" }

Write-Host "-> ${output}.tar.gz"
