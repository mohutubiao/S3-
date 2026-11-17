# 临时脚本（用完删除）：按 dafeiyu 现有配置增量编译
$ErrorActionPreference = 'Continue'
$env:IDF_TOOLS_PATH = 'C:\Espressif'
$py = 'C:\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe'
$activatePy = 'C:\Espressif\frameworks\esp-idf-v5.5.4\tools\activate.py'

$out = & $py $activatePy --export
$scriptPath = $out | Where-Object { "$_" -like '*.ps1' } | Select-Object -Last 1
if (-not $scriptPath) { Write-Output 'ACTIVATE_FAILED'; exit 1 }
. $scriptPath

Set-Location 'C:\Users\mohut\xiaozhi-s3-board'
& idf.py -B build_dafeiyu build 2>&1 | Tee-Object -FilePath 'C:\Users\mohut\xiaozhi-s3-board\build_log_dafeiyu.txt'
Write-Output "BUILD_EXIT=$LASTEXITCODE"
exit $LASTEXITCODE
