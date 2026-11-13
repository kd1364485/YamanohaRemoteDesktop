# Start the Yamanoha host app in local mode (signaling server on this PC) for latency measurement.
# Start the signaling server first (C:\Users\user\tools\yamanoha\start-signaling.ps1).
# VITE_CLOUD_HOST=local makes src/types.ts use the local server without editing the source.
$env:VITE_CLOUD_HOST = 'local'
Set-Location (Join-Path $PSScriptRoot '..\..')
npm run tauri dev
