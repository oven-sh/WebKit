#Requires -RunAsAdministrator

if ($null -eq (Get-Command "choco" -ErrorAction SilentlyContinue)) 
{ 
    # FROM https://chocolatey.org/install
    Set-ExecutionPolicy Bypass -Scope Process -Force; [System.Net.ServicePointManager]::SecurityProtocol = [System.Net.ServicePointManager]::SecurityProtocol -bor 3072; iex ((New-Object System.Net.WebClient).DownloadString('https://community.chocolatey.org/install.ps1'))
}

if ($null -eq (Get-Command "git" -ErrorAction SilentlyContinue)) 
{ 
    choco install -y git
}

if($null -eq (Get-Command "python" -ErrorAction SilentlyContinue)) 
{ 
    choco install -y python3
}

if($null -eq (Get-Command "perl" -ErrorAction SilentlyContinue)) 
{ 
    choco install -y StrawberryPerl
}

if ($null -eq (Get-Command "ruby" -ErrorAction SilentlyContinue)) 
{ 
    choco install -y ruby
}

if ($null -eq (Get-Command "cmake" -ErrorAction SilentlyContinue)) 
{ 
    # SEE https://community.chocolatey.org/packages/cmake
    choco install cmake --installargs 'ADD_CMAKE_TO_PATH=System' -y
}

$VCPKG_ROOT = "$PSScriptRoot\vcpkg"
$VCPKG_EXE  = "$VCPKG_ROOT\vcpkg.exe"
if(!(Test-Path -Path $VCPKG_EXE -PathType leaf)) {
    .\vcpkg\bootstrap-vcpkg.bat -disableMetrics
}

& $VCPKG_EXE install icu --triplet "x64-windows-static"