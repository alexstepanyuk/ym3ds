param([Parameter(Mandatory=$true)][string]$Zig, [string]$Mp3)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
Push-Location $projectRoot
try {
    New-Item -ItemType Directory -Force build/host | Out-Null
    & $Zig cc -std=c11 -UNDEBUG -Wall -Wextra -Werror -Iinclude -Ivendor/cjson source/model.c vendor/cjson/cJSON.c tests/model_test.c -o build/host/model_test.exe
    if ($LASTEXITCODE) { throw 'Не удалось скомпилировать проверки API' }
    & ./build/host/model_test.exe
    if ($LASTEXITCODE) { throw 'Проверки API завершились с ошибкой' }
    if ($Mp3) {
        & $Zig cc -std=c11 -O2 -UNDEBUG -Wall -Wextra -Werror -Ivendor/minimp3 tests/decode_test.c -o build/host/decode_test.exe
        if ($LASTEXITCODE) { throw 'Не удалось скомпилировать проверку MP3' }
        & ./build/host/decode_test.exe $Mp3
        if ($LASTEXITCODE) { throw 'Проверка MP3 завершилась с ошибкой' }
    }
} finally { Pop-Location }
