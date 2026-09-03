<#
  환경 점검. 빌드가 왜 안 되는지 알려 줍니다.
      powershell -NoProfile -ExecutionPolicy Bypass -File scripts\diagnose.ps1
#>
$ErrorActionPreference = 'Continue'
$repo = Split-Path -Parent $PSScriptRoot

function Show([string]$label, [string]$value, [bool]$ok) {
    $mark = if ($ok) { 'OK  ' } else { 'FAIL' }
    $color = if ($ok) { 'Green' } else { 'Red' }
    Write-Host ("[{0}] {1,-22} {2}" -f $mark, $label, $value) -ForegroundColor $color
}

Write-Host ''
Write-Host '=== LogScope 환경 점검 ===' -ForegroundColor Cyan
Write-Host ''

Show 'PowerShell' $PSVersionTable.PSVersion.ToString() $true
Show '저장소 경로' $repo (Test-Path (Join-Path $repo 'CMakeLists.txt'))

$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if ($cmake) {
    $ver = (& cmake --version | Select-Object -First 1) -replace 'cmake version ', ''
    $enough = [version]$ver -ge [version]'3.18'
    Show 'cmake' "$ver  ($($cmake.Source))" $enough
    if (-not $enough) { Write-Host '      -> 3.18 이상이 필요합니다. https://cmake.org/download/' -ForegroundColor Yellow }
} else {
    Show 'cmake' '없음' $false
    Write-Host '      -> https://cmake.org/download/ 에서 설치하고 PATH 에 추가하세요.' -ForegroundColor Yellow
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (Test-Path $vswhere) {
    $installs = & $vswhere -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
                    -format value -property installationVersion 2>$null
    if ($installs) {
        Show 'Visual Studio C++' (($installs | ForEach-Object { $_ }) -join ', ') $true
    } else {
        Show 'Visual Studio C++' 'C++ 워크로드가 설치되지 않음' $false
        Write-Host '      -> Visual Studio 설치 관리자에서 "C++를 사용한 데스크톱 개발" 을 추가하세요.' -ForegroundColor Yellow
    }
} else {
    Show 'vswhere' '없음 (Visual Studio 미설치로 보임)' $false
}

$ninja = Get-Command ninja -ErrorAction SilentlyContinue
Show 'ninja (선택)' $(if ($ninja) { $ninja.Source } else { '없음 - vs2017/vs2022 프리셋에는 불필요' }) $true

foreach ($f in @('build.bat', 'scripts\build.ps1')) {
    $path = Join-Path $repo $f
    if (-not (Test-Path $path)) { Show $f '없음' $false; continue }
    $bytes = [System.IO.File]::ReadAllBytes($path)
    $hasCrLf = $false
    for ($i = 0; $i -lt $bytes.Length - 1; $i++) {
        if ($bytes[$i] -eq 13 -and $bytes[$i + 1] -eq 10) { $hasCrLf = $true; break }
    }
    Show $f $(if ($hasCrLf) { 'CRLF' } else { 'LF 전용 - git 설정 확인 필요' }) $hasCrLf
}

Write-Host ''
Write-Host '다음 단계: .\build.bat  (PowerShell 에서는 앞의 .\ 가 반드시 필요합니다)' -ForegroundColor Cyan
Write-Host ''
