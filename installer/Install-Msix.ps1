#Requires -RunAsAdministrator
param([switch]$Force)

$ErrorActionPreference = 'Stop'
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

# Remove old unpackaged install if present
$unpackagedPath = "$env:LOCALAPPDATA\Programs\AgenticTerminal"
if (Test-Path $unpackagedPath) {
    Write-Host "Removing old unpackaged install at $unpackagedPath..."
    Remove-Item $unpackagedPath -Recurse -Force
}

# Trust the dev certificate
$cer = Get-Item "$scriptDir\AgenticTerminalDev.cer" -ErrorAction Stop
Write-Host "Importing dev certificate: $($cer.Name)"
Import-Certificate -FilePath $cer.FullName -CertStoreLocation Cert:\LocalMachine\TrustedPeople | Out-Null

# Install XAML dependency
$xaml = Get-Item "$scriptDir\Dependencies\Microsoft.UI.Xaml.2.8.appx" -ErrorAction SilentlyContinue
if ($xaml) {
    Write-Host "Installing XAML dependency..."
    Add-AppxPackage -Path $xaml.FullName -ErrorAction SilentlyContinue
}

# Install the MSIX
$msix = Get-Item "$scriptDir\CascadiaPackage_*.msix" -ErrorAction Stop | Select-Object -First 1
Write-Host "Installing $($msix.Name)..."
Add-AppxPackage -Path $msix.FullName

Write-Host "Done. Launch 'Agentic Terminal (Dev)' from the Start menu."
