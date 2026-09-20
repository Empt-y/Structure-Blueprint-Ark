# Launch the local ARK test server.
#
# -NoBattlEye matters: ArkServerAPI works by loading a proxy version.dll next to
# the executable, and BattlEye objects to exactly that. This server is local and
# unlisted, so there is nothing for BattlEye to protect here anyway.

param(
    [string]$ServerRoot = "C:\ARKServer",
    [string]$Map = "ScorchedEarth_P",
    [string]$AdminPassword = "test123",
    [int]$Port = 7777,
    [int]$QueryPort = 27015,
    [int]$RconPort = 27020
)

$exe = "$ServerRoot\ShooterGame\Binaries\Win64\ShooterGameServer.exe"
if (-not (Test-Path $exe)) { throw "server not installed at $exe" }

$opts = "$Map" +
    "?listen" +
    "?SessionName=StructureBlueprintTest" +
    "?ServerAdminPassword=$AdminPassword" +
    "?Port=$Port" +
    "?QueryPort=$QueryPort" +
    "?RCONEnabled=True" +
    "?RCONPort=$RconPort" +
    "?MaxPlayers=5"

Write-Host "Starting $Map (admin password: $AdminPassword)" -ForegroundColor Cyan
Write-Host "In game: join via console 'open 127.0.0.1:$Port'" -ForegroundColor DarkGray
Write-Host "Then:    enablecheats $AdminPassword" -ForegroundColor DarkGray

& $exe $opts -server -log -NoBattlEye

