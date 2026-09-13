param(
    [ValidateRange(1, 256)][int]$Concurrency = 5
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$experiment = Join-Path $root 'build\history-qsearch-tt'
$devEngine = Join-Path $experiment 'dev.exe'
$opponentEngine = Join-Path $experiment 'base.exe'
$openingBook = 'E:\books-master\books-master\UHO_Lichess_4852_v1.epd'
$fastchess = 'F:\fastchess-windows-x86-64\fastchess.exe'
foreach ($requiredPath in @($devEngine, $opponentEngine, $openingBook, $fastchess)) {
    if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
        throw "Required file not found: $requiredPath"
    }
}
$matchDir = Join-Path $experiment ('base-120+1-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $matchDir | Out-Null
$matchArgs = @(
    '-engine', 'name=Dragon of MORS-0.2.0(avx2+bmi2)', "cmd=$devEngine", 'option.UCI_Chess960=false',
    '-engine', 'name=MORS-0.1.0(avx2+bmi2)', "cmd=$opponentEngine",
    '-each', 'proto=uci', 'tc=120+1', 'option.Hash=256', 'option.Threads=1',
    '-variant', 'standard',
    '-openings', "file=$openingBook", 'format=epd', 'order=random',
    '-resign', 'movecount=5', 'score=683', 'twosided=true',
    '-draw', 'movenumber=25', 'movecount=7', 'score=55',
    '-rounds', '10000', '-games', '2', '-concurrency', "$Concurrency",
    '-srand', '20260913',
    '-pgnout', "file=$matchDir\dragon-of-mors-0.2.0-vs-mors-0.1.0.pgn",
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
