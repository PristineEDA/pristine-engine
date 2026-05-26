[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'Medium')]
param(
    [string]$Remote = 'origin',
    [string]$TagPrefix = 'v',
    [switch]$Push,
    [switch]$AllowDirty
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Invoke-Git {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Arguments,
        [switch]$AllowFailure
    )

    $output = & git @Arguments 2>&1
    $exitCode = $LASTEXITCODE
    $text = ($output | Out-String).Trim()
    if ($exitCode -ne 0 -and -not $AllowFailure) {
        throw "git $($Arguments -join ' ') failed with exit code $exitCode.`n$text"
    }

    [pscustomobject]@{
        ExitCode = $exitCode
        Output = $text
    }
}

function Get-RequiredMatch {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Content,
        [Parameter(Mandatory = $true)]
        [string]$Pattern,
        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    $match = [regex]::Match($Content, $Pattern)
    if (-not $match.Success) {
        throw "Unable to find $Description."
    }
    $match.Groups[1].Value
}

$repoRoot = (Invoke-Git -Arguments @('rev-parse', '--show-toplevel')).Output
Set-Location $repoRoot

$cmakeLists = Join-Path $repoRoot 'CMakeLists.txt'
$mainCpp = Join-Path $repoRoot 'src/main.cpp'

$cmakeVersion = Get-RequiredMatch `
    -Content (Get-Content -Raw -Path $cmakeLists) `
    -Pattern '(?s)project\s*\(\s*pristine-engine\b.*?\bVERSION\s+([0-9]+\.[0-9]+\.[0-9]+(?:[-+][A-Za-z0-9.-]+)?)' `
    -Description 'the CMake project version'

$serverVersion = Get-RequiredMatch `
    -Content (Get-Content -Raw -Path $mainCpp) `
    -Pattern 'kServerVersion\s*=\s*"([^"]+)"' `
    -Description 'kServerVersion in src/main.cpp'

if ($serverVersion -ne $cmakeVersion) {
    throw "Version mismatch: CMakeLists.txt has $cmakeVersion, but src/main.cpp has $serverVersion."
}

$tagName = "$TagPrefix$cmakeVersion"
if ($tagName -notmatch '^v?[0-9]+\.[0-9]+\.[0-9]+(?:[-+][A-Za-z0-9.-]+)?$') {
    throw "Refusing to create non-version tag '$tagName'."
}

$status = (Invoke-Git -Arguments @('status', '--porcelain')).Output
if (-not $AllowDirty -and -not [string]::IsNullOrWhiteSpace($status)) {
    throw "Working tree is not clean. Commit or stash changes, or pass -AllowDirty."
}

$existingLocalTag = Invoke-Git -Arguments @('rev-parse', '-q', '--verify', "refs/tags/$tagName") -AllowFailure
if ($existingLocalTag.ExitCode -eq 0) {
    throw "Local tag '$tagName' already exists."
}

if ($Push) {
    $existingRemoteTag = Invoke-Git -Arguments @('ls-remote', '--exit-code', '--tags', $Remote, "refs/tags/$tagName") -AllowFailure
    if ($existingRemoteTag.ExitCode -eq 0) {
        throw "Remote tag '$tagName' already exists on '$Remote'."
    }
    if ($existingRemoteTag.ExitCode -ne 2) {
        throw "Unable to check remote tag '$tagName' on '$Remote'.`n$($existingRemoteTag.Output)"
    }
}

$message = "pristine-engine $cmakeVersion"
if ($PSCmdlet.ShouldProcess($tagName, "Create annotated git tag for version $cmakeVersion")) {
    Invoke-Git -Arguments @('tag', '-a', $tagName, '-m', $message) | Out-Null
    Write-Host "Created tag $tagName for pristine-engine $cmakeVersion."
}
else {
    Write-Host "Would create tag $tagName for pristine-engine $cmakeVersion."
}

if ($Push) {
    if ($PSCmdlet.ShouldProcess("$Remote/$tagName", "Push git tag")) {
        Invoke-Git -Arguments @('push', $Remote, $tagName) | Out-Null
        Write-Host "Pushed tag $tagName to $Remote."
    }
    else {
        Write-Host "Would push tag $tagName to $Remote."
    }
}
