param(
    [string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'

$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$buildDirectory = Join-Path $projectRoot 'build'
$distDirectory = Join-Path $projectRoot 'dist'
$cpackConfig = Join-Path $buildDirectory 'CPackConfig.cmake'

if (-not (Test-Path -LiteralPath $cpackConfig -PathType Leaf)) {
    throw "CMake package configuration was not found: $cpackConfig"
}

$cmake = Get-Command cmake -ErrorAction Stop
& $cmake.Source --build $buildDirectory --config Release --target bcnminer_portable_launcher
if ($LASTEXITCODE -ne 0) {
    throw "CMake failed while building the portable launcher (exit code $LASTEXITCODE)."
}

$cpack = Get-Command cpack -ErrorAction Stop
& $cpack.Source --config $cpackConfig -G ZIP
if ($LASTEXITCODE -ne 0) {
    throw "CPack failed while creating the portable ZIP (exit code $LASTEXITCODE)."
}

$configText = Get-Content -LiteralPath $cpackConfig -Raw
$packageNameMatch = [regex]::Match(
    $configText,
    '(?m)^set\(CPACK_PACKAGE_FILE_NAME "([^"]+)"\)'
)
if (-not $packageNameMatch.Success) {
    throw "Could not read CPACK_PACKAGE_FILE_NAME from $cpackConfig"
}

$packageName = $packageNameMatch.Groups[1].Value
$archivePath = Join-Path $distDirectory "$packageName.zip"
if (-not (Test-Path -LiteralPath $archivePath -PathType Leaf)) {
    throw "CPack did not create the expected portable ZIP: $archivePath"
}

$archive = [System.IO.Compression.ZipFile]::OpenRead($archivePath)
try {
    if ($archive.Entries | Where-Object { $_.FullName -match '(^|/)myconfig\.txt$' }) {
        throw 'The portable ZIP contains myconfig.txt; refusing to bundle user configuration.'
    }
} finally {
    $archive.Dispose()
}

$launcherCandidates = @(
    (Join-Path $buildDirectory 'Release\bcnminer_portable_launcher.exe'),
    (Join-Path $buildDirectory 'bcnminer_portable_launcher.exe')
)
$launcherPath = $launcherCandidates |
    Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
    Select-Object -First 1
if (-not $launcherPath) {
    throw 'The portable launcher binary was not found in the build output.'
}

if (-not $OutputDirectory) {
    $OutputDirectory = $distDirectory
}
$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$outputPath = Join-Path $OutputDirectory ($packageName -replace '-win64$', '-portable.exe')

$stagingDirectory = Join-Path ([System.IO.Path]::GetTempPath()) "bcnminer-sfx-$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $stagingDirectory | Out-Null

try {
    $stagedLauncher = Join-Path $stagingDirectory 'bcnminer-portable.exe'
    Copy-Item -LiteralPath $launcherPath -Destination $stagedLauncher

    $archiveInfo = Get-Item -LiteralPath $archivePath
    $folderName = [System.IO.Path]::GetFileNameWithoutExtension($archiveInfo.Name)
    $folderBytes = [System.Text.Encoding]::UTF8.GetBytes($folderName)
    if ($folderBytes.Length -gt 256) {
        throw 'The portable package folder name is unexpectedly long.'
    }

    $outputStream = [System.IO.File]::Open(
        $stagedLauncher,
        [System.IO.FileMode]::Append,
        [System.IO.FileAccess]::Write,
        [System.IO.FileShare]::None
    )
    try {
        $archiveStream = [System.IO.File]::OpenRead($archivePath)
        try {
            $archiveLength = $archiveStream.Length
            $archiveStream.CopyTo($outputStream)
        } finally {
            $archiveStream.Dispose()
        }

        $writer = New-Object System.IO.BinaryWriter($outputStream)
        try {
            $writer.Write($folderBytes)
            $writer.Write([UInt16]$folderBytes.Length)
            $writer.Write([Int64]$archiveLength)
            $writer.Write([System.Text.Encoding]::ASCII.GetBytes('BCNSFX01'))
            $writer.Flush()
        } finally {
            $writer.Dispose()
        }
    } finally {
        $outputStream.Dispose()
    }

    [System.IO.File]::Copy($stagedLauncher, $outputPath, $true)
    Write-Output "Created portable launcher: $outputPath"
} finally {
    Remove-Item -LiteralPath $stagingDirectory -Recurse -Force
}
