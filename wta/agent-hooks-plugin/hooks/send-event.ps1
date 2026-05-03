# send-event.ps1 — Forward Copilot CLI hook events to WTA via wtcli
param([string]$EventType = "agent.hook")

# Skip if not running inside Windows Terminal
if (-not $env:WT_COM_CLSID) { exit 0 }

# Skip if wtcli not available
$wtcliPath = (Get-Command wtcli -ErrorAction SilentlyContinue).Source
if (-not $wtcliPath) { exit 0 }

# Read hook JSON from stdin
$hookData = [Console]::In.ReadToEnd()
if (-not $hookData -or -not $hookData.Trim()) { exit 0 }

# Wrap payload and send via ProcessStartInfo to avoid PowerShell argument mangling
try {
    $parsed = $hookData | ConvertFrom-Json

    # Extract agent_session_id from stdin JSON (Claude/Gemini), env (Copilot), or empty.
    $agentSessionId = ""
    if ($parsed.PSObject.Properties.Name -contains "session_id") {
        $agentSessionId = [string]$parsed.session_id
    } elseif ($env:COPILOT_SESSION_ID) {
        $agentSessionId = $env:COPILOT_SESSION_ID
    } elseif ($env:CLAUDE_SESSION_ID) {
        $agentSessionId = $env:CLAUDE_SESSION_ID
    } elseif ($env:GEMINI_SESSION_ID) {
        $agentSessionId = $env:GEMINI_SESSION_ID
    }

    # Detect CLI source: prefer WTA_CLI_SOURCE (set by bash hooks); fall back
    # to env-var sniffing; default to "copilot" for backward compat.
    $cliSource = $env:WTA_CLI_SOURCE
    if (-not $cliSource) {
        if ($env:CLAUDE_PLUGIN_ROOT) { $cliSource = "claude" }
        elseif ($env:GEMINI_CLI) { $cliSource = "gemini" }
        elseif ($env:COPILOT_CLI) { $cliSource = "copilot" }
        else { $cliSource = "copilot" }
    }

    $wrapper = @{
        cli_source       = $cliSource
        agent_session_id = $agentSessionId
        payload          = $parsed
    }

    $payload = $wrapper | ConvertTo-Json -Compress -Depth 5

    # Escape quotes for raw command line: each " becomes \"
    $escaped = $payload.Replace('"', '\"')
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $wtcliPath
    $psi.Arguments = "send-event -e $EventType `"$escaped`""
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardError = $true
    $proc = [System.Diagnostics.Process]::Start($psi)
    $proc.WaitForExit(5000)
} catch {
    # Silently ignore errors — hooks must not block the agent.
}
