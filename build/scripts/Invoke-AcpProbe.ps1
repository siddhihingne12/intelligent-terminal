[CmdletBinding()]
param(
    [string]$Prompt = 'Reply with exactly PONG and nothing else.',

    [string]$Cwd = (Get-Location).Path,

    [string]$AgentPath,

    [string[]]$AgentArgs = @('--acp', '--stdio')
)

$ErrorActionPreference = 'Stop'

function Write-Probe {
    param(
        [string]$Direction,
        [string]$Message
    )

    $timestamp = Get-Date -Format 'HH:mm:ss.fff'
    Write-Host "[$timestamp] $Direction $Message"
}

function Resolve-AgentPath {
    param([string]$RequestedPath)

    if (-not [string]::IsNullOrWhiteSpace($RequestedPath)) {
        return $RequestedPath
    }

    foreach ($candidate in @('copilot.exe', 'copilot.cmd', 'copilot')) {
        $command = Get-Command $candidate -ErrorAction SilentlyContinue
        if ($command -and -not [string]::IsNullOrWhiteSpace($command.Source)) {
            return $command.Source
        }
    }

    throw 'Could not resolve Copilot CLI. Pass -AgentPath explicitly.'
}

function Send-JsonRpc {
    param(
        [System.IO.StreamWriter]$Writer,
        [int]$Id,
        [string]$Method,
        [hashtable]$Params
    )

    $json = @{
        jsonrpc = '2.0'
        id = $Id
        method = $Method
        params = $Params
    } | ConvertTo-Json -Depth 20 -Compress

    Write-Probe '>>>' $json
    $Writer.WriteLine($json)
    $Writer.Flush()
}

function Read-UntilResponse {
    param(
        [System.IO.StreamReader]$Reader,
        [int]$ExpectedId
    )

    while ($true) {
        $line = $Reader.ReadLine()
        if ($null -eq $line) {
            throw "ACP process exited before response id=$ExpectedId."
        }

        Write-Probe '<<<' $line

        try {
            $obj = $line | ConvertFrom-Json -Depth 50
        }
        catch {
            continue
        }

        if ($null -ne $obj.id -and [int]$obj.id -eq $ExpectedId) {
            return $obj
        }
    }
}

$resolvedAgentPath = Resolve-AgentPath -RequestedPath $AgentPath
Write-Probe '===' "AgentPath=$resolvedAgentPath"
Write-Probe '===' "AgentArgs=$($AgentArgs -join ' ')"
Write-Probe '===' "Cwd=$Cwd"
Write-Probe '===' "Prompt=$Prompt"

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $resolvedAgentPath
$psi.Arguments = ($AgentArgs -join ' ')
$psi.WorkingDirectory = $Cwd
$psi.UseShellExecute = $false
$psi.RedirectStandardInput = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.CreateNoWindow = $true

$proc = New-Object System.Diagnostics.Process
$proc.StartInfo = $psi

if (-not $proc.Start()) {
    throw 'Failed to start ACP agent process.'
}

Write-Probe '===' "Spawned pid=$($proc.Id)"

try {
    Send-JsonRpc -Writer $proc.StandardInput -Id 1 -Method 'initialize' -Params @{
        protocolVersion = 1
        clientCapabilities = @{}
        clientInfo = @{
            name = 'acp-probe'
            version = '0.1.0'
        }
    }
    $initialize = Read-UntilResponse -Reader $proc.StandardOutput -ExpectedId 1

    Send-JsonRpc -Writer $proc.StandardInput -Id 2 -Method 'session/new' -Params @{
        cwd = [System.IO.Path]::GetFullPath($Cwd)
        mcpServers = @()
    }
    $newSession = Read-UntilResponse -Reader $proc.StandardOutput -ExpectedId 2

    $sessionId = $newSession.result.sessionId
    if ([string]::IsNullOrWhiteSpace($sessionId)) {
        throw 'session/new succeeded but no sessionId was returned.'
    }

    Write-Probe '===' "SessionId=$sessionId"

    Send-JsonRpc -Writer $proc.StandardInput -Id 3 -Method 'session/prompt' -Params @{
        sessionId = $sessionId
        prompt = @(
            @{
                type = 'text'
                text = $Prompt
            }
        )
    }
    $promptResponse = Read-UntilResponse -Reader $proc.StandardOutput -ExpectedId 3

    $stopReason = $promptResponse.result.stopReason
    Write-Probe '===' "Prompt stopReason=$stopReason"
}
finally {
    try {
        if (-not $proc.HasExited) {
            $proc.Kill($true)
        }
    }
    catch {
    }

    $stderrText = $proc.StandardError.ReadToEnd()
    if (-not [string]::IsNullOrWhiteSpace($stderrText)) {
        Write-Probe 'ERR' $stderrText.TrimEnd()
    }

    $proc.Dispose()
}
