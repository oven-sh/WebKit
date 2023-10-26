$ErrorActionPreference = "Stop"

# Set default OUT_DIR to WebKitBuild
if ($env:WEBKIT_BUILD_DIR) {
    $WebKitBuild = $env:WEBKIT_BUILD_DIR
}
else {
    $WebKitBuild = "WebKitBuild"
}

$ICUURL = "https://github.com/unicode-org/icu/releases/download/release-73-1/icu4c-73_1-Win64-MSVC2019.zip"

if (!(Test-Path -Path $WebKitBuild)) {
    $null = mkdir $WebKitBuild
}

if (!(Test-Path -Path $WebKitBuild/libicu)) {
    # Download and extract URL
    $icuZipPath = Join-Path $WebKitBuild "icu.zip"
    if (!(Test-Path -Path $icuZipPath)) {
        Invoke-WebRequest -Uri $ICUURL -OutFile $icuZipPath
    }
    $null = New-Item -ItemType Directory -Path $WebKitBuild\libicu
    $null = Expand-Archive $icuZipPath $WebKitBuild\libicu
    Remove-Item -Path $icuZipPath
}

$env:ICU_ROOT = Join-Path $WebKitBuild "libicu"
$env:ICU_LIBRARY = Join-Path $env:ICU_ROOT "lib64"
$env:ICU_INCLUDE_DIR = Join-Path $env:ICU_ROOT "include"

cmake `
    -DPORT="JSCOnly" `
    -DENABLE_STATIC_JSC=ON `
    -DENABLE_SINGLE_THREADED_VM_ENTRY_SCOPE=ON `
    -DALLOW_LINE_AND_COLUMN_NUMBER_IN_BUILTINS=ON `
    -DCMAKE_BUILD_TYPE=Release `
    -DUSE_THIN_ARCHIVES=OFF `
    -DENABLE_DFG_JIT=ON `
    -DENABLE_FTL_JIT=OFF `
    -DENABLE_WEBASSEMBLY=OFF `
    -DUSE_BUN_JSC_ADDITIONS=ON `
    -S . `
    -B $WebKitBuild `
    -DUSE_SYSTEM_MALLOC=ON `
    -DSTATICALLY_LINKED_WITH_WTF=ON `
    "-DICU_ROOT=${env:ICU_ROOT}" `
    "-DICU_LIBRARY=${env:ICU_LIBRARY}" `
    "-DICU_INCLUDE_DIR=${env:ICU_INCLUDE_DIR}"

cmake --build $WebKitBuild --config Release --target jsc

$output = "bun-webkit-x64"
if ($env:WEBKIT_OUTPUT_DIR) {
    $output = $env:WEBKIT_OUTPUT_DIR
}

Remove-Item -r -Force $output
$null = mkdir -Force $output
$null = mkdir -Force $output/lib
$null = mkdir -Force $output/include
$null = mkdir -Force $output/include/JavaScriptCore
$null = mkdir -Force $output/include/wtf

Copy-Item $WebKitBuild/cmakeconfig.h $output/include/cmakeconfig.h
Copy-Item $WebKitBuild/lib64/JavaScriptCore.lib $output/lib/
Copy-Item $WebKitBuild/lib64/JavaScriptCore.pdb $output/lib/
Copy-Item $WebKitBuild/lib64/WTF.pdb $output/lib/
Copy-Item $WebKitBuild/lib64/WTF.lib $output/lib/

$BUN_WEBKIT_VERSION = $(git rev-parse HEAD)

# Append #define BUN_WEBKIT_VERSION "${BUN_WEBKIT_VERSION}" to $output/include/cmakeconfig.h
Add-Content -Path $output/include/cmakeconfig.h -Value "`#define BUN_WEBKIT_VERSION `"$BUN_WEBKIT_VERSION`""

Copy-Item -r -Force $WebKitBuild/JavaScriptCore/DerivedSources/* $output/include/JavaScriptCore
Copy-Item -r -Force $WebKitBuild/JavaScriptCore/Headers/JavaScriptCore/* $output/include/JavaScriptCore/
Copy-Item -r -Force $WebKitBuild/JavaScriptCore/PrivateHeaders/JavaScriptCore/* $output/include/JavaScriptCore/

Copy-Item -r $WebKitBuild/WTF/DerivedSources/* $output/include/wtf/
Copy-Item -r $WebKitBuild/WTF/Headers/wtf/* $output/include/wtf/

(Get-Content -Path $output/include/JavaScriptCore/JSValueInternal.h) -replace "#import <JavaScriptCore/JSValuePrivate.h>", "#include <JavaScriptCore/JSValuePrivate.h>" | Set-Content -Path $output/include/JavaScriptCore/JSValueInternal.h

Copy-Item $WebKitBuild/libicu/lib64/* $output/lib/
