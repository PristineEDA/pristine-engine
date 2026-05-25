param(
    [ValidateSet('amd64', 'x86')]
    [string]$Arch = 'amd64',

    [ValidateSet('amd64', 'x86')]
    [string]$HostArch = 'amd64',

    [switch]$PassThruInfo
)

function Enter-VS2026 {
    [CmdletBinding()]
    param(
        [ValidateSet('amd64', 'x86')]
        [string]$Arch = 'amd64',

        [ValidateSet('amd64', 'x86')]
        [string]$HostArch = 'amd64',

        [switch]$PassThruInfo
    )

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) {
        throw "vswhere.exe was not found at '$vswhere'. Install Visual Studio 2026 Build Tools first."
    }

    $installationPath = & $vswhere `
        -latest `
        -products * `
        -version '[18.0,19.0)' `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath

    if (-not $installationPath) {
        throw 'Visual Studio 2026 Build Tools with the C++ toolset was not found.'
    }

    $devShellModule = Join-Path $installationPath 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll'
    if (-not (Test-Path -LiteralPath $devShellModule)) {
        throw "The Visual Studio DevShell module was not found at '$devShellModule'."
    }

    Import-Module $devShellModule -ErrorAction Stop | Out-Null
    Enter-VsDevShell -VsInstallPath $installationPath -DevCmdArguments "-arch=$Arch -host_arch=$HostArch" | Out-Null
    Set-Location -LiteralPath $PSScriptRoot

    if ($PassThruInfo) {
        [pscustomobject]@{
            InstallationPath = $installationPath
            ProjectRoot      = $PSScriptRoot
            Arch             = $Arch
            HostArch         = $HostArch
        }
    }
}

if ($MyInvocation.InvocationName -ne '.') {
    $info = Enter-VS2026 -Arch $Arch -HostArch $HostArch -PassThruInfo:$PassThruInfo

    if ($PassThruInfo) {
        $info
    } else {
        Write-Host "Entered VS 2026 DevShell ($Arch/$HostArch) and switched to $PSScriptRoot." -ForegroundColor Green
    }
}