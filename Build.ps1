param([ValidateSet('Release', 'Debug')][string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'gamesense-loader-ui-main/Build.ps1') -Configuration $Configuration
