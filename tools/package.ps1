$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
Push-Location $projectRoot
try {
    if (!(Test-Path ym3ds.3dsx) -or !(Test-Path ym3ds.smdh)) { throw 'Сначала выполните make: нужны собранные .3dsx и .smdh.' }
    $sdDirectory = Join-Path $projectRoot 'release/3ds/ym3ds'
    New-Item -ItemType Directory -Force "$sdDirectory/config" | Out-Null
    Copy-Item ym3ds.3dsx,ym3ds.smdh $sdDirectory
    Copy-Item config/cacert.pem "$sdDirectory/config/"
    # Never copy a real OAuth token from the working tree.
    Copy-Item config/token.example.txt "$sdDirectory/config/token.txt"
    Copy-Item README.md,LICENSE.upstream,THIRD_PARTY.md release/
    Compress-Archive -Path release/3ds,release/README.md,release/LICENSE.upstream,release/THIRD_PARTY.md -DestinationPath release/ym3ds-sd.zip -Force
} finally { Pop-Location }
