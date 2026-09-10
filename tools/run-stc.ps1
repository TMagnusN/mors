Set-Location 'F:\fastchess-windows-x86-64'

.\fastchess.exe `
  -engine name=dev cmd=.\dev.exe `
  -engine name=base cmd=.\base.exe `
  -each proto=uci tc=10+0.1 option.Hash=64 option.Threads=1 `
  -openings file="E:\books-master\books-master\UHO_4060_v4.epd" format=epd order=random `
  -resign movecount=5 score=583 twosided=true `
  -draw movenumber=25 movecount=7 score=55 `
  -tb "D:\syzygy5" `
  -tbpieces 5 `
  -rounds 10000 `
  -games 2 `
  -concurrency 14 `
  -pgnout file=dev-vs-base.pgn
