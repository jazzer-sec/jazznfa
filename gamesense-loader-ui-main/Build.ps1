param([ValidateSet('Release', 'Debug')][string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'
$projectRoot = $PSScriptRoot
$backendProject = Join-Path $projectRoot '../nfatool-v0.5.5/NfaLoader/NfaLoader.csproj'
$buildRoot = Join-Path $projectRoot 'build-native'
$distribution = Join-Path $projectRoot 'dist-single'
$payloadRoot = Join-Path $buildRoot 'payload'
$bundleRoot = Join-Path $buildRoot 'bundle'

if (!(Test-Path -LiteralPath $backendProject)) { throw "NFA source missing: $backendProject" }
# Every GUI object and library is compiled from src/ and external/* source.
# No supplied GUI executable, object, static library, DLL or precompiled header is used.
& dotnet publish $backendProject -c $Configuration -r win-x64 -p:Platform=x64 -p:PublishAot=false -p:DebugType=None --self-contained true -o $payloadRoot
if ($LASTEXITCODE -ne 0) { throw 'NFA service compilation failed.' }

$licenses = Join-Path $payloadRoot 'licenses'
New-Item -ItemType Directory -Path $licenses -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $projectRoot 'external/imgui/LICENSE.txt') -Destination (Join-Path $licenses 'Dear-ImGui.txt') -Force
Copy-Item -LiteralPath (Join-Path $projectRoot 'external/freetype/docs/FTL.TXT') -Destination (Join-Path $licenses 'FreeType.txt') -Force
Copy-Item -LiteralPath (Join-Path $projectRoot '../nfatool-v0.5.5/LICENSE') -Destination (Join-Path $licenses 'NfaLoader.txt') -Force

# CAB is extracted through the Windows Setup API; friends need neither an
# archiver nor .NET installed. Numbered members map to manifest-relative paths.
New-Item -ItemType Directory -Path $bundleRoot -Force | Out-Null
$files = @(Get-ChildItem -LiteralPath $payloadRoot -Recurse -File | Sort-Object FullName)
$manifest = [System.Collections.Generic.List[string]]::new()
$ddf = [System.Collections.Generic.List[string]]::new()
@('.OPTION EXPLICIT', '.Set Cabinet=ON', '.Set Compress=ON', '.Set CompressionType=LZX', '.Set CompressionMemory=21',
  '.Set CabinetNameTemplate=runtime.cab', ".Set DiskDirectoryTemplate=`"$bundleRoot`"", '.Set MaxDiskSize=0',
  '.Set MaxCabinetSize=0') | ForEach-Object { $ddf.Add($_) }
for ($index = 0; $index -lt $files.Count; $index++) {
    $file = $files[$index]
    $relative = [System.IO.Path]::GetRelativePath($payloadRoot, $file.FullName)
    $manifest.Add("$relative`t$($file.Length)")
    $ddf.Add("`"$($file.FullName)`" `"$index`"")
}
$ddfPath = Join-Path $bundleRoot 'runtime.ddf'
[System.IO.File]::WriteAllLines($ddfPath, $ddf, [System.Text.Encoding]::Default)
$manifestPath = Join-Path $bundleRoot 'manifest.txt'
[System.IO.File]::WriteAllText($manifestPath, ($manifest -join "`n") + "`n", [System.Text.UTF8Encoding]::new($false))
Write-Host "Compressing $($files.Count) runtime files..."
& makecab.exe /F $ddfPath | Out-File -LiteralPath (Join-Path $bundleRoot 'makecab.log')
if ($LASTEXITCODE -ne 0) { throw "Runtime compression failed. See $bundleRoot/makecab.log" }
$cabinetPath = Join-Path $bundleRoot 'runtime.cab'
$version = (Get-FileHash -LiteralPath $cabinetPath -Algorithm SHA256).Hash.ToLowerInvariant().Substring(0, 24)
$cabinetRc = $cabinetPath.Replace('\', '/')
$manifestRc = $manifestPath.Replace('\', '/')
[System.IO.File]::WriteAllText((Join-Path $bundleRoot 'bundle.rc'), "101 RCDATA `"$cabinetRc`"`n102 RCDATA `"$manifestRc`"`n")
[System.IO.File]::WriteAllText((Join-Path $bundleRoot 'bundle_version.h'), "#pragma once`n#define BUNDLE_VERSION L`"$version`"`n#define BUNDLE_VERSION_NARROW `"$version`"`n")

& cmake -S $projectRoot -B $buildRoot -A x64 "-DBUNDLE_DIRECTORY=$bundleRoot"
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
& cmake --build $buildRoot --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) { throw 'Native GUI compilation failed.' }
New-Item -ItemType Directory -Path $distribution -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $buildRoot "$Configuration/JazzNfa.exe") -Destination (Join-Path $distribution 'JazzNfa.exe') -Force
$result = Get-Item -LiteralPath (Join-Path $distribution 'JazzNfa.exe')
Write-Host "One-file build: $($result.FullName) ($([Math]::Round($result.Length / 1MB, 1)) MB)"
