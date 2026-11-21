[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ToolchainPath,
    [string]$OutputDirectory = (Join-Path $PSScriptRoot 'build-native')
)

$ErrorActionPreference = 'Stop'
$hdrCompilerRoot = (Resolve-Path -LiteralPath $ToolchainPath).Path
$hdrCompiler = Join-Path $hdrCompilerRoot 'bin\x86_64-w64-mingw32-clang++.exe'
$hdrResourceCompiler = Join-Path $hdrCompilerRoot 'bin\x86_64-w64-mingw32-windres.exe'
foreach ($hdrTool in @($hdrCompiler, $hdrResourceCompiler)) {
    if (-not (Test-Path -LiteralPath $hdrTool -PathType Leaf)) {
        throw "Required LLVM-MinGW tool is missing: $hdrTool"
    }
}

$hdrOutputPath = [System.IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $hdrOutputPath -Force | Out-Null
$hdrSourcePath = Join-Path $PSScriptRoot 'toggleHDR'
$hdrResourceObject = Join-Path $hdrOutputPath 'Resources.o'
$hdrExecutable = Join-Path $hdrOutputPath 'toggleHDR.exe'

Push-Location $hdrSourcePath
try {
    & $hdrResourceCompiler '-i' 'Resources.rc' '-o' $hdrResourceObject '-O' 'coff'
    if ($LASTEXITCODE -ne 0) { throw 'Resource compilation failed.' }

    $hdrCompileArgs = @(
        '-std=c++17', '-O2', '-Wall', '-Wextra', '-municode', '-mwindows',
        '-static', '-static-libgcc', '-static-libstdc++',
        '-DUNICODE', '-D_UNICODE', '-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN', '-D_WIN32_WINNT=0x0A00',
        'toggleHDR.cpp', 'Display.cpp', 'SettingsWindow.cpp', $hdrResourceObject,
        '-o', $hdrExecutable, '-luser32', '-lshell32', '-lgdi32', '-lole32', '-ladvapi32'
    )
    & $hdrCompiler @hdrCompileArgs
    if ($LASTEXITCODE -ne 0) { throw 'Native executable compilation failed.' }
} finally {
    Pop-Location
}

Get-Item -LiteralPath $hdrExecutable | Select-Object FullName, Length, LastWriteTime
