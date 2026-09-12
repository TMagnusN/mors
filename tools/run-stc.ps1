$ErrorActionPreference = 'Stop'
$matchDir = 'F:\MORS\build\stc-uho-0.2.0-dev-4t-vs-0.1.0-1t-10+0.1'
$devEngine = 'F:\MORS\build\release-avx2+bmi2\mors-avx2+bmi2.exe'
$baseEngine = 'C:\Users\Magnus\Downloads\releases\mors-avx2+bmi2.exe'
$openingBook = 'E:\books-master\books-master\UHO_Lichess_4852_v1.epd'
$fastchess = 'F:\fastchess-windows-x86-64\fastchess.exe'

foreach ($requiredPath in @($devEngine, $baseEngine, $openingBook, $fastchess)) {
  if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
    throw "Required file not found: $requiredPath"
  }
}
New-Item -ItemType Directory -Path $matchDir -Force | Out-Null
Set-Location -LiteralPath $matchDir

$matchArgs = @(
  '-engine', 'name=Dragon of MORS-0.2.0-Dev', "cmd=$devEngine", 'option.Threads=4',
  '-engine', 'name=MORS-0.1.0', "cmd=$baseEngine", 'option.Threads=1',
  '-each', 'proto=uci', 'tc=10+0.1', 'option.Hash=64', 'option.UCI_Chess960=false',
  '-variant', 'standard',
  '-openings', "file=$openingBook", 'format=epd', 'order=random',
  '-resign', 'movecount=5', 'score=683', 'twosided=true',
  '-draw', 'movenumber=25', 'movecount=7', 'score=55',
  '-tb', 'D:\syzygy5', '-tbpieces', '5',
  '-rounds', '10000', '-games', '2', '-concurrency', '5',
  '-srand', '20260912',
  '-pgnout', "file=$matchDir\dragon-of-mors-0.2.0-dev-vs-mors-0.1.0-uho.pgn",
  '-config', "outname=$matchDir\resume-uho.json"
)
& $fastchess @matchArgs
exit $LASTEXITCODE
