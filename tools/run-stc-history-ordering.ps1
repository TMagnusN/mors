param(
    [ValidateRange(1, 256)][int]$Concurrency = 5
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$experiment = Join-Path $root 'build\history-ordering'
$devEngine = Join-Path $experiment 'dev.exe'
$baseEngine = Join-Path $experiment 'base.exe'
$openingBook = 'E:\books-master\books-master\UHO_Lichess_4852_v1.epd'
$fastchess = 'F:\fastchess-windows-x86-64\fastchess.exe'
foreach ($requiredPath in @($devEngine, $baseEngine, $openingBook, $fastchess)) {
    if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
        throw "Required file not found: $requiredPath"
    }
}
$matchDir = Join-Path $experiment ('stc-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $matchDir | Out-Null
$matchArgs = @(
    '-engine', 'name=dev', "cmd=$devEngine",
    '-engine', 'name=base', "cmd=$baseEngine",
    '-each', 'proto=uci', 'tc=10+0.1', 'option.Hash=64', 'option.Threads=1', 'option.UCI_Chess960=false',
    '-variant', 'standard',
    '-openings', "file=$openingBook", 'format=epd', 'order=random',
    '-resign', 'movecount=5', 'score=683', 'twosided=true',
    '-draw', 'movenumber=25', 'movecount=7', 'score=55',
    '-sprt', 'elo0=0', 'elo1=5', 'alpha=0.05', 'beta=0.05', 'model=normalized',
    '-rounds', '10000', '-games', '2', '-concurrency', "$Concurrency",
    '-srand', '20260913',
    '-pgnout', "file=$matchDir\dev-vs-base.pgn",
    '-config', "outname=$matchDir\resume.json"
)
if (Test-Path -LiteralPath 'D:\syzygy5' -PathType Container) {
    $matchArgs += @('-tb', 'D:\syzygy5', '-tbpieces', '5')
}
Push-Location -LiteralPath $matchDir
try {
    & $fastchess @matchArgs
    $matchExitCode = $LASTEXITCODE
} finally {
    Pop-Location
}
exit $matchExitCode
