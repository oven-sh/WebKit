param(
    # Configuration type: Debug, Release, or ASAN
    [ValidateSet("Debug", "Release", "ASAN")]
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"

# Set up MSVC environment variables. This is taken from Bun's 'scripts\env.ps1'
if ($env:VSINSTALLDIR -eq $null) {
    Write-Host "Loading Visual Studio environment, this may take a second..."
    $vsDir = Get-ChildItem -Path "C:\Program Files\Microsoft Visual Studio\2022" -Directory
    if ($vsDir -eq $null) {
        throw "Visual Studio directory not found."
    } 
    Push-Location $vsDir.FullName
    try {
        . (Join-Path -Path $vsDir.FullName -ChildPath "Common7\Tools\Launch-VsDevShell.ps1") -Arch amd64 -HostArch amd64
    }
    finally { Pop-Location }
}

if ($Env:VSCMD_ARG_TGT_ARCH -eq "x86") {
    # Please do not try to compile Bun for 32 bit. It will not work. I promise.
    throw "Visual Studio environment is targetting 32 bit. This configuration is definitely a mistake."
}

# Fix up $PATH - remove problematic paths
$SplitPath = $env:PATH -split ";"
$MSVCPaths = $SplitPath | Where-Object { $_ -like "*Microsoft Visual Studio*" }
$CleanPaths = $SplitPath | Where-Object { 
    $_ -notlike "*mingw*" -and 
    $_ -notlike "*strawberry*" -and 
    $_ -notlike "*cygwin*"
}
$env:PATH = ($MSVCPaths + $CleanPaths) -join ';'

Write-Host "Cleaned PATH: $env:PATH"

# Verify tools
(Get-Command link).Path
clang-cl.exe --version

# Detect LLVM installation for ASAN support
$ASANLibPath = $null
$ClangPath = (Get-Command clang-cl -ErrorAction SilentlyContinue).Path
if ($ClangPath) {
    $LLVMRoot = Split-Path (Split-Path $ClangPath)
    $LLVMLib = Join-Path $LLVMRoot "lib"
    $ClangLib = Join-Path $LLVMLib "clang"
    
    # Find clang version directory (e.g., "19" or "19.1.7")
    if (Test-Path $ClangLib) {
        $ClangVersion = Get-ChildItem $ClangLib | Select-Object -First 1
        $ASANLibPath = Join-Path $ClangVersion.FullName "lib\windows"
        
        if (Test-Path $ASANLibPath) {
            Write-Host ":: Found ASAN libraries at $ASANLibPath"
            
            # Set environment variables for the linker to find ASAN libraries
            $env:LIB = "$ASANLibPath;$env:LIB"
            $env:LINK = "/LIBPATH:`"$ASANLibPath`" $env:LINK"
            
            # Also set for CMake
            $env:CMAKE_PREFIX_PATH = "$LLVMRoot;$env:CMAKE_PREFIX_PATH"
            
            # For ASAN builds, we need to link the ASAN runtime explicitly
            if ($Configuration -eq "ASAN") {
                $env:LDFLAGS = "/LIBPATH:`"$ASANLibPath`" -fsanitize=address $env:LDFLAGS"
                $env:CFLAGS = "-fsanitize=address $env:CFLAGS"
                $env:CXXFLAGS = "-fsanitize=address $env:CXXFLAGS"
            }
        } else {
            Write-Warning "ASAN library path not found at $ASANLibPath"
            $ASANLibPath = $null
        }
    }
}

$env:CC = "clang-cl"
$env:CXX = "clang-cl"

# Configuration variables
$output = if ($env:WEBKIT_OUTPUT_DIR) { $env:WEBKIT_OUTPUT_DIR } else { "bun-webkit" }
$WebKitBuild = if ($env:WEBKIT_BUILD_DIR) { $env:WEBKIT_BUILD_DIR } else { "WebKitBuild" }
$CMAKE_BUILD_TYPE = $Configuration
$BUN_WEBKIT_VERSION = if ($env:BUN_WEBKIT_VERSION) { $env:BUN_WEBKIT_VERSION } else { $(git rev-parse HEAD) }

# Handle ASAN builds
$EnableSanitizers = $false
if ($Configuration -eq "ASAN") {
    $EnableSanitizers = $true
    $CMAKE_BUILD_TYPE = "Release"  # ASAN uses Release mode for better performance
    Write-Host ":: Building with AddressSanitizer enabled (Release)"
}

$null = mkdir $WebKitBuild -ErrorAction SilentlyContinue

# vcpkg setup - prefer standalone installation over Visual Studio integrated one
$VcpkgRoot = if ($env:VCPKG_ROOT) { 
    $env:VCPKG_ROOT 
} elseif (Test-Path "C:\vcpkg\vcpkg.exe") {
    "C:\vcpkg"
} else { 
    "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\vcpkg" 
}
$VcpkgExe = Join-Path $VcpkgRoot "vcpkg.exe"

if (!(Test-Path $VcpkgExe)) {
    throw "vcpkg not found at $VcpkgExe. Please install vcpkg and set VCPKG_ROOT environment variable."
}

Write-Host ":: Using vcpkg at $VcpkgRoot"
Write-Host ":: VcpkgExe is $VcpkgExe"
Write-Host ":: VCPKG_ROOT env var is $env:VCPKG_ROOT"

# Ensure Ruby is in PATH if installed via scoop
$RubyPath = "C:\Users\window\scoop\apps\ruby\current\bin"
if (Test-Path $RubyPath) {
    if ($env:PATH -notlike "*$RubyPath*") {
        $env:PATH = "$RubyPath;$env:PATH"
        Write-Host ":: Added Ruby to PATH: $RubyPath"
    }
} else {
    Write-Warning "Ruby not found at expected scoop path: $RubyPath"
}

# Use dynamic triplet to avoid ICU linking issues with static libraries
$VcpkgTriplet = "x64-windows"

# Install dependencies via vcpkg (manifest mode)
Write-Host ":: Checking ICU installation via vcpkg"

# In manifest mode, vcpkg install without arguments installs dependencies from vcpkg.json
# The vcpkg.json in the root already has ICU as a dependency
Write-Host ":: Installing dependencies from vcpkg.json (including ICU)"
Push-Location $PSScriptRoot
try {
    & $VcpkgExe install --triplet=$VcpkgTriplet
    if ($LASTEXITCODE -ne 0) { 
        Write-Warning "vcpkg install returned exit code $LASTEXITCODE - attempting to continue"
    }
} finally {
    Pop-Location
}

# Verify ICU is installed
$IcuCheck = & $VcpkgExe list 2>&1 | Select-String "icu:"
if ($IcuCheck) {
    Write-Host ":: ICU confirmed installed via vcpkg"
} else {
    Write-Warning "Could not confirm ICU installation - build may fail"
}

# vcpkg toolchain file
$VcpkgToolchain = Join-Path $VcpkgRoot "scripts/buildsystems/vcpkg.cmake"

Write-Host ":: Configuring WebKit with vcpkg"

# Set compiler flags for static runtime linking
$env:CFLAGS = "/Zi"
$env:CXXFLAGS = "/Zi"

# Determine MSVC runtime library setting - use dynamic runtime since we're using dynamic ICU
$CmakeMsvcRuntimeLibrary = "MultiThreadedDLL"
if ($CMAKE_BUILD_TYPE -eq "Debug") {
    $CmakeMsvcRuntimeLibrary = "MultiThreadedDebugDLL"
}

# ASAN-specific flags
$AsanFlags = ""
if ($EnableSanitizers) {
    $AsanFlags = " -fsanitize=address"
    # ASAN requires dynamic runtime and works best with release runtime
    $CmakeMsvcRuntimeLibrary = "MultiThreadedDLL"  # /MD
}

$NoWebassembly = if ($env:NO_WEBASSEMBLY) { $env:NO_WEBASSEMBLY } else { $false }
$WebAssemblyState = if ($NoWebassembly) { "OFF" } else { "ON" }

# Build CMake command
$CmakeArgs = @(
    "-S", ".",
    "-B", $WebKitBuild,
    "-DPORT=JSCOnly",
    "-DENABLE_STATIC_JSC=ON",
    "-DALLOW_LINE_AND_COLUMN_NUMBER_IN_BUILTINS=ON",
    "-DCMAKE_BUILD_TYPE=$CMAKE_BUILD_TYPE",
    "-DUSE_THIN_ARCHIVES=OFF",
    "-DENABLE_JIT=ON",
    "-DENABLE_DFG_JIT=ON",
    "-DENABLE_FTL_JIT=ON",
    "-DENABLE_WEBASSEMBLY_BBQJIT=ON",
    "-DENABLE_WEBASSEMBLY_OMGJIT=ON",
    "-DENABLE_SAMPLING_PROFILER=ON",
    "-DENABLE_WEBASSEMBLY=$WebAssemblyState",
    "-DUSE_BUN_JSC_ADDITIONS=ON",
    "-DUSE_BUN_EVENT_LOOP=ON",
    "-DENABLE_BUN_SKIP_FAILING_ASSERTIONS=ON",
    "-DUSE_SYSTEM_MALLOC=ON",
    "-DCMAKE_C_COMPILER=clang-cl",
    "-DCMAKE_CXX_COMPILER=clang-cl",
    "-DCMAKE_C_FLAGS_RELEASE=/Zi /O2 /Ob2 /DNDEBUG$AsanFlags",
    "-DCMAKE_CXX_FLAGS_RELEASE=/Zi /O2 /Ob2 /DNDEBUG$AsanFlags -Xclang -fno-c++-static-destructors",
    "-DCMAKE_C_FLAGS_DEBUG=/Zi /FS /O0 /Ob0$AsanFlags",
    "-DCMAKE_CXX_FLAGS_DEBUG=/Zi /FS /O0 /Ob0$AsanFlags -Xclang -fno-c++-static-destructors",
    "-DENABLE_REMOTE_INSPECTOR=ON",
    "-DCMAKE_MSVC_RUNTIME_LIBRARY=$CmakeMsvcRuntimeLibrary",
    "-DCMAKE_TOOLCHAIN_FILE=$VcpkgToolchain",
    "-DVCPKG_TARGET_TRIPLET=$VcpkgTriplet",
    "-G", "Ninja"
)

# Add sanitizer flags if enabled
if ($EnableSanitizers) {
    # For ASAN on Windows with Clang, we need to:
    # 1. Use -fsanitize=address for compilation (already in AsanFlags)
    # 2. Link against the ASAN runtime libraries
    # 3. Use dynamic CRT (/MD) - already configured
    
    Write-Host ":: Configuring ASAN build with linker flags"
    
    $CmakeArgs += "-DENABLE_SANITIZERS=address"
    
    if ($ASANLibPath) {
        # Set up linking flags with explicit ASAN library path
        # The compiler flags already have -fsanitize=address from AsanFlags
        # We just need to ensure the linker can find the ASAN runtime
        
        $ASANLinkFlags = "/LIBPATH:`"$ASANLibPath`""
        
        # Add linker flags with ASAN library path
        $CmakeArgs += "-DCMAKE_EXE_LINKER_FLAGS=$ASANLinkFlags"
        $CmakeArgs += "-DCMAKE_SHARED_LINKER_FLAGS=$ASANLinkFlags"
        $CmakeArgs += "-DCMAKE_MODULE_LINKER_FLAGS=$ASANLinkFlags"
        
        # Skip CMake's compiler test since it doesn't inherit our linker flags properly
        $CmakeArgs += "-DCMAKE_C_COMPILER_WORKS=1"
        $CmakeArgs += "-DCMAKE_CXX_COMPILER_WORKS=1"
        
        Write-Host ":: ASAN libraries configured at $ASANLibPath"
    } else {
        Write-Warning "ASAN library path not found - build may fail during linking"
    }
}

# Run CMake
& cmake @CmakeArgs
if ($LASTEXITCODE -ne 0) { throw "cmake failed with exit code $LASTEXITCODE" }

# Workaround for what is probably a CMake bug
Write-Host ":: Applying batch file workaround"
$batFiles = Get-ChildItem -Path $WebKitBuild -Filter "*.bat" -File -Recurse
foreach ($file in $batFiles) {
    $content = Get-Content $file.FullName -Raw
    $newContent = $content -replace "(\|\| \(set FAIL_LINE=\d+& goto :ABORT\))", ""
    if ($content -ne $newContent) {
        Set-Content -Path $file.FullName -Value $newContent
        Write-Host ":: Patched $($file.FullName)"
    }
}

Write-Host ":: Building WebKit ($CMAKE_BUILD_TYPE configuration)"
$BuildTarget = "jsc"
cmake --build $WebKitBuild --config $CMAKE_BUILD_TYPE --target $BuildTarget --verbose
if ($LASTEXITCODE -ne 0) { throw "cmake --build failed with exit code $LASTEXITCODE" }

Write-Host ":: Packaging $output"

# Dump the entire tree of files in $WebKitBuild to the console.
# This is useful for debugging.
Write-Host ":: Build output directory contents:"
Get-ChildItem -Recurse $WebKitBuild | Format-List -Force | Out-String | Write-Host

# Clean and create output directories
Remove-Item -Recurse -ErrorAction SilentlyContinue $output
$null = mkdir -ErrorAction SilentlyContinue $output
$null = mkdir -ErrorAction SilentlyContinue $output/include
$null = mkdir -ErrorAction SilentlyContinue $output/include/JavaScriptCore
$null = mkdir -ErrorAction SilentlyContinue $output/include/wtf

# Copy build artifacts
Copy-Item -Recurse $WebKitBuild/lib $output
Copy-Item -Recurse $WebKitBuild/bin $output

# If there's a lib64, also copy it.
if (Test-Path -Path $WebKitBuild/lib64) {
    Copy-Item -Recurse $WebKitBuild/lib64/* $output/lib
}

Copy-Item $WebKitBuild/cmakeconfig.h $output/include/cmakeconfig.h

# Add BUN_WEBKIT_VERSION to cmakeconfig.h
Add-Content -Path $output/include/cmakeconfig.h -Value "#define BUN_WEBKIT_VERSION `"$BUN_WEBKIT_VERSION`""

# Copy JavaScriptCore headers
Copy-Item -r -Force $WebKitBuild/JavaScriptCore/DerivedSources/* $output/include/JavaScriptCore
Copy-Item -r -Force $WebKitBuild/JavaScriptCore/Headers/JavaScriptCore/* $output/include/JavaScriptCore/
Copy-Item -r -Force $WebKitBuild/JavaScriptCore/PrivateHeaders/JavaScriptCore/* $output/include/JavaScriptCore/

# Recursively copy all the .h files in DerivedSources to the root of include/JavaScriptCore, preserving the basename only.
Copy-Item -r -Force $WebKitBuild/JavaScriptCore/DerivedSources/*.h $output/include/JavaScriptCore/
Copy-Item -r -Force $WebKitBuild/JavaScriptCore/DerivedSources/*/*.h $output/include/JavaScriptCore/

# Recursively copy all the .json files in DerivedSources to the root of the output directory, preserving the basename only.
Copy-Item -r -Force $WebKitBuild/JavaScriptCore/DerivedSources/*.json $output/

# Copy WTF headers
Copy-Item -r $WebKitBuild/WTF/DerivedSources/* $output/include/wtf/
Copy-Item -r $WebKitBuild/WTF/Headers/wtf/* $output/include/wtf/

# Copy bmalloc headers if they exist (libpas support)
if (Test-Path -Path $WebKitBuild/bmalloc) {
    $null = mkdir -ErrorAction SilentlyContinue $output/include/bmalloc
    Copy-Item -r $WebKitBuild/bmalloc/Headers/bmalloc/* $output/include/bmalloc/
}

# Fix header import/include syntax
(Get-Content -Path $output/include/JavaScriptCore/JSValueInternal.h) `
    -replace "#import <JavaScriptCore/JSValuePrivate.h>", "#include <JavaScriptCore/JSValuePrivate.h>" `
| Set-Content -Path $output/include/JavaScriptCore/JSValueInternal.h

# Copy ICU headers and libraries from vcpkg
$VcpkgInstalled = Join-Path $VcpkgRoot "installed/$VcpkgTriplet"
$IcuInclude = Join-Path $VcpkgInstalled "include"
$IcuLib = Join-Path $VcpkgInstalled "lib"

if (Test-Path $IcuInclude) {
    Copy-Item -r $IcuInclude/* $output/include/
    Write-Host ":: Copied ICU headers from vcpkg"
} else {
    Write-Warning "ICU headers not found at $IcuInclude"
}

if (Test-Path $IcuLib) {
    # Copy ICU libraries
    $IcuLibFiles = Get-ChildItem -Path $IcuLib -Filter "icu*.lib"
    foreach ($lib in $IcuLibFiles) {
        Copy-Item $lib.FullName $output/lib/
    }
    Write-Host ":: Copied ICU libraries from vcpkg"
} else {
    Write-Warning "ICU libraries not found at $IcuLib"
}

# Create package.json for artifact identification
$ConfigSuffix = if ($EnableSanitizers) { "asan" } else { $CMAKE_BUILD_TYPE.ToLower() }
$packageJsonContent = @{
    name       = if ($env:PACKAGE_JSON_LABEL) { $env:PACKAGE_JSON_LABEL } else { "webkit-$ConfigSuffix" }
    version    = "0.0.1-$env:GITHUB_SHA"
    os         = @("win32")
    cpu        = @(if ($env:PACKAGE_JSON_ARCH) { $env:PACKAGE_JSON_ARCH } else { "x64" })
    repository = "https://github.com/$($env:GITHUB_REPOSITORY)"
    config     = $ConfigSuffix
    asan       = $EnableSanitizers
} | ConvertTo-Json -Depth 2

Out-File -FilePath $output/package.json -InputObject $packageJsonContent

# Create tarball
$OutputArchive = if ($EnableSanitizers) { "${output}-asan.tar.gz" } else { "${output}.tar.gz" }
tar -cz -f $OutputArchive $output
if ($LASTEXITCODE -ne 0) { throw "tar failed with exit code $LASTEXITCODE" }

Write-Host "-> $OutputArchive"
Write-Host ":: Build completed successfully!"

# Summary
Write-Host ""
Write-Host "=== Build Summary ==="
Write-Host "Configuration: $Configuration"
Write-Host "ASAN Enabled: $EnableSanitizers"
Write-Host "Output: $OutputArchive"
Write-Host "WebKit Version: $BUN_WEBKIT_VERSION"