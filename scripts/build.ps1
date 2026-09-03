<#
.SYNOPSIS
    LogScope 를 구성하고 빌드합니다. VS Code 확장이 없어도, CMake 프리셋을 몰라도
    이 스크립트 하나로 빌드됩니다.

.DESCRIPTION
    설치된 Visual Studio 를 찾아 알맞은 CMake 생성기를 고르고, 구성 → 빌드 →
    (선택) 테스트 → (선택) 실행까지 합니다. Visual Studio 2017 / 2019 / 2022 를
    모두 지원합니다.

.EXAMPLE
    .\scripts\build.ps1
    Debug 로 빌드합니다.

.EXAMPLE
    .\scripts\build.ps1 -Config Release -Test
    Release 로 빌드하고 테스트까지 돌립니다.

.EXAMPLE
    .\scripts\build.ps1 -Clean -Run
    빌드 폴더를 지우고 처음부터 빌드한 뒤 실행합니다.
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Config = 'Debug',

    # 자동 감지 대신 생성기를 직접 지정할 때. 예: "Visual Studio 15 2017"
    [string]$Generator = '',

    # 빌드 폴더를 지우고 처음부터
    [switch]$Clean,

    # 빌드 후 ctest 실행
    [switch]$Test,

    # 빌드 후 logscope.exe 실행
    [switch]$Run
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot

function Find-VisualStudioGenerator {
    # vswhere 는 Visual Studio 2017 부터 항상 이 경로에 설치된다.
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { return $null }

    $version = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationVersion 2>$null
    if (-not $version) { return $null }

    switch ([int](($version -split '\.')[0])) {
        15      { 'Visual Studio 15 2017' }
        16      { 'Visual Studio 16 2019' }
        17      { 'Visual Studio 17 2022' }
        default { $null }
    }
}

# ---- 사전 확인 --------------------------------------------------------------

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    throw @'
cmake 을 찾지 못했습니다.

  https://cmake.org/download/ 에서 3.18 이상을 설치하고,
  설치 중 "Add CMake to the system PATH" 를 선택하세요.

  (Visual Studio 에 딸려 오는 CMake 는 PATH 에 없을 수 있습니다. 그 경우
   "x64 Native Tools Command Prompt" 에서 이 스크립트를 실행해도 됩니다.)
'@
}

if (-not $Generator) { $Generator = Find-VisualStudioGenerator }
if (-not $Generator) {
    throw @'
Visual Studio 의 C++ 도구를 찾지 못했습니다.

  Visual Studio 설치 관리자에서 "C++를 사용한 데스크톱 개발" 워크로드를 설치하세요.
  이미 설치돼 있는데도 안 잡히면 생성기를 직접 지정하세요:

    .\scripts\build.ps1 -Generator "Visual Studio 15 2017"
'@
}

# "Visual Studio 15 2017" -> "vs2017"
$year = ($Generator -split '\s+')[-1]
$buildDir = Join-Path $repo "build\vs$year"

Write-Host "생성기 : $Generator"   -ForegroundColor Cyan
Write-Host "구성   : $Config"      -ForegroundColor Cyan
Write-Host "빌드   : $buildDir"    -ForegroundColor Cyan
Write-Host ''

# ---- 구성 -------------------------------------------------------------------

if ($Clean -and (Test-Path $buildDir)) {
    Write-Host '빌드 폴더를 지웁니다...' -ForegroundColor Yellow
    Remove-Item -Recurse -Force $buildDir
}

cmake -S $repo -B $buildDir -G $Generator -A x64
if ($LASTEXITCODE -ne 0) { throw "CMake 구성 실패 (종료 코드 $LASTEXITCODE)" }

# ---- 빌드 -------------------------------------------------------------------

cmake --build $buildDir --config $Config
if ($LASTEXITCODE -ne 0) { throw "빌드 실패 (종료 코드 $LASTEXITCODE)" }

$binDir = Join-Path $buildDir "bin\$Config"
Write-Host ''
Write-Host "빌드 완료: $binDir" -ForegroundColor Green
Get-ChildItem $binDir -Filter '*.exe' | ForEach-Object { Write-Host "  $($_.Name)" }
Get-ChildItem $binDir -Filter '*.dll' | ForEach-Object { Write-Host "  $($_.Name)" }

# ---- 테스트 / 실행 ------------------------------------------------------------

if ($Test) {
    Write-Host ''
    Push-Location $buildDir
    try {
        ctest -C $Config --output-on-failure
        if ($LASTEXITCODE -ne 0) { throw "테스트 실패 (종료 코드 $LASTEXITCODE)" }
    } finally { Pop-Location }
}

if ($Run) {
    $exe = Join-Path $binDir 'logscope.exe'
    if (-not (Test-Path $exe)) { throw "실행 파일이 없습니다: $exe" }
    Write-Host ''
    Write-Host "실행: $exe" -ForegroundColor Green
    Start-Process $exe
}
