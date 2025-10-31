# DXVK Logging results
are in dxvk_logs particularly d3d11.log
problem: they dont show anything interestinglets 

# DXVK Logging (Maximum Verbosity)

**IMPORTANT:** The log directory MUST exist before launching the game, or no logs will be created.

## Setup

1. Create the log directory:
```bash
mkdir -p ~/dxvk_logs
chmod 777 ~/dxvk_logs
```

2. Steam Launch Options (Right-click game → Properties → General → Launch Options):
```bash
DXVK_LOG_LEVEL=trace DXVK_LOG_PATH=/home/klappec/dxvk_logs PROTON_LOG=1 WINEDEBUG=+d3d11 %command%
```

## Log Levels
- `trace` - Maximum verbosity (most detailed)
- `debug` - Debug information
- `info` - General information (default)
- `warn` - Warnings only
- `error` - Errors only

## Output Files

After launching the game, logs will be created at:
- **DXVK D3D11 log:** `~/dxvk_logs/A13V_x64_Release_en_d3d11.log` (primary log for menu lag analysis)
- **DXVK D3D9 log:** `~/dxvk_logs/A13V_x64_Release_en_d3d9.log`
- **DXVK DXGI log:** `~/dxvk_logs/A13V_x64_Release_en_dxgi.log`
- **Proton/Wine log:** `~/steam-936190.log` (only when PROTON_LOG=1)

The log files are named after the game's executable (A13V_x64_Release_en.exe).

## Troubleshooting

If no logs are created:
1. Verify the directory exists: `ls -la ~/dxvk_logs`
2. Check permissions: `chmod 777 ~/dxvk_logs`
3. Use absolute path (not /tmp or relative paths)
4. Verify game launched through Proton (check Steam shows "Play" not "Install")
