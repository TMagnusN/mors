param(
    [ValidateRange(1, 256)][int]$Concurrency = 14,
    [ValidateSet("Uho", "Endgames")][string]$Book = "Uho"
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$experiment = Join-Path $root 'build\syzygy-stc'
$devEngine = Join-Path $experiment 'dev.exe'
$baseEngine = Join-Path $experiment 'base.exe'
$openingBook = if ($Book -eq 'Endgames') {
    'E:\books-master\books-master\endgames.epd'
} else {
    'E:\books-master\books-master\UHO_Lichess_4852_v1.epd'
}
if (-not (Test-Path -LiteralPath 'D:\syzygy5' -PathType Container)) {
    throw 'Tablebase directory is missing: D:\syzygy5'
}
$fastchess = 'F:\fastchess-windows-x86-64\fastchess.exe'
foreach ($requiredPath in @($devEngine, $baseEngine, $openingBook, $fastchess)) {
    if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
        throw "Required file not found: $requiredPath"
    }
}
$matchDir = Join-Path $experiment ('stc-' + $Book.ToLowerInvariant() + '-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $matchDir | Out-Null
$matchArgs = @(
    '-engine', 'name=syzygy-dev', "cmd=$devEngine", 'option.SyzygyPath=D:\syzygy5', 'option.SyzygyProbeLimit=5',
    '-engine', 'name=pre-syzygy-base', "cmd=$baseEngine",
    '-each', 'proto=uci', 'tc=10+0.1', 'option.Hash=64', 'option.Threads=1', 'option.UCI_Chess960=false',
    '-variant', 'standard',
    '-openings', "file=$openingBook", 'format=epd', 'order=random',
    '-resign', 'movecount=5', 'score=683', 'twosided=true',
    '-draw', 'movenumber=25', 'movecount=7', 'score=55',
    '-rounds', '10000', '-games', '2', '-concurrency', "$Concurrency",
    '-srand', '20260914',
    '-pgnout', "file=$matchDir\dev-vs-base.pgn", 'tbhits=true',
    '-config', "outname=$matchDir\resume.json"
)
# No tournament-level tablebase adjudication: only dev receives SyzygyPath.
Push-Location -LiteralPath $matchDir
try {
    & $fastchess @matchArgs
    $matchExitCode = $LASTEXITCODE
} finally {
    Pop-Location
}
exit $matchExitCode
