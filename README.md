# IO Log Scope

엑셀/CSV 로그를 IO 채널 파형으로 그려 주는 Windows 데스크톱 프로그램입니다.
C++17, Win32 + Direct2D, **서드파티 라이브러리 없음**.

```
build/bin/
  logcore.dll    파일 파싱 · 데이터 모델 · 다운샘플링 (UI 코드 없음)
  logscope.exe   Win32 창 + Direct2D 렌더러 (파싱 코드 없음)
```

## 프로젝트 구조에 대해

DLL 과 EXE 가 함께 나오는 이 구성은 이렇게 부릅니다.

- **Visual Studio 관점** — 하나의 **솔루션(solution)** 안에 **프로젝트(project)** 두 개.
  하나는 *동적 연결 라이브러리(DLL)* 프로젝트, 하나는 *Windows 데스크톱
  애플리케이션(EXE)* 프로젝트입니다. 통칭 **다중 프로젝트 솔루션**.
- **CMake 관점** — 하나의 프로젝트에 **타깃(target)** 두 개.
  `add_library(logcore SHARED)` + `add_executable(logscope WIN32)`.
- **설계 패턴으로서** — *코어 라이브러리 + 얇은 호스트 실행 파일* 구조.
  Windows 밖에서는 보통 이걸 "shared library + frontend" 라고 부릅니다.
- EXE 가 DLL 을 쓰는 방식은 **암시적 링크(implicit / load-time linking)** 입니다.
  빌드할 때 생기는 **가져오기 라이브러리(import library, `logcore.lib`)** 를 링크하고,
  실행할 때 로더가 `logcore.dll` 을 올립니다.
  (반대는 `LoadLibrary`/`GetProcAddress` 로 직접 올리는 **명시적 링크**입니다.)

이렇게 나눈 이유는 파싱 코드에 UI 가 섞이지 않게 하기 위해서입니다. 덕분에
파서는 콘솔 도구나 테스트에서 그대로 재사용되고 (`tests/logcore_test.cpp`),
UI 를 손봐도 파싱 로직이 흔들리지 않습니다.

## 빌드 (Windows 11 + Visual Studio 2022)

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release
```

산출물은 `build\bin\Release\` 에 `logscope.exe` 와 `logcore.dll` 두 개가 나란히
놓입니다. **두 파일은 같은 폴더에 있어야 합니다** (EXE 는 앱 폴더와 System32
에서만 DLL 을 찾도록 링크돼 있습니다 — `docs/SECURITY.md` 3.1 참고).

Visual Studio 에서 *폴더 열기* 로 저장소를 열어도 CMake 통합이 그대로 동작합니다.

필요한 것: MSVC v143 툴셋, Windows 10/11 SDK, CMake 3.21 이상. 그 외에는 없습니다.
`vcpkg` 나 NuGet 복원 단계가 없고, 빌드 중 네트워크를 타지 않습니다.

## 입력 데이터 형식

기본 배치는 **행 = IO 채널** 입니다.

| | B | C | D | … |
|---|---|---|---|---|
| **Time [s]** | 0.00 | 0.01 | 0.02 | … |
| **DI_00_CYCLE_START** | 0 | 0 | 1 | … |
| **AI_TEMP_BARREL1** | 182.4 | 182.6 | 182.5 | … |
| **SEQ_STATE** | IDLE | IDLE | RUN | … |

- **첫 행** = 시간축. 숫자, `HH:MM:SS.mmm` 문자열, ISO 8601, 엑셀 날짜/시간 셀
  (`styles.xml` 의 표시 형식을 보고 판별) 을 인식합니다. 해석에 실패하거나 값이
  증가하지 않으면 샘플 번호(0…N)로 물러서고 상태 표시줄에 그 사실을 알립니다.
  `Time [s]` 처럼 첫 셀에 단위를 적어 두면 축 라벨에 반영됩니다.
- **각 행의 첫 열** = IO 이름. 비어 있으면 `IO_1` 형태로 자동 부여합니다.
- 시간축이 세로로 내려가는 파일이면 툴바에서 **열 = IO** 로 바꾸면 됩니다.
- 지원 확장자: `.xlsx` `.xlsm` `.xltx` `.csv` `.tsv` `.txt` `.log`
  (`.xls` 구형 BIFF 는 지원하지 않습니다 — 이유는 `docs/SECURITY.md` 3.5)

### 채널 타입 자동 판별

| 타입 | 판별 기준 | 표현 |
|---|---|---|
| `DIG` | 값이 0/1, TRUE/FALSE, ON/OFF, HIGH/LOW 뿐 | 스텝 파형 + 하이 구간 채움 |
| `ANA` | 값이 모두 숫자 | 라인, 레인별 자동 스케일 |
| `STATE` | 문자열 상태값 | 구간 리본 + 상태 라벨 |

빈 셀은 0 이 아니라 **끊김**으로 처리해 선을 잇지 않습니다. 0 으로 채우면
없는 골짜기가 그래프에 생깁니다.

## 화면과 조작

왼쪽에 채널 목록(검색, `전체/DIG/ANA/STATE` 필터, 개별 체크), 오른쪽에 채널당 한
줄씩 쌓이는 레인 플롯. 레인 왼쪽에 IO 이름, 오른쪽 거터에 커서 위치의 값
(아날로그는 최소–최대 범위까지) 이 붙습니다.

| 동작 | 조작 |
|---|---|
| 커서 A / B 놓기 | 클릭 / Shift+클릭 (상태 표시줄에 Δt) |
| 시간축 이동 | 드래그, ← → |
| 시간축 확대/축소 | Ctrl+휠, `+` `-` |
| 채널 세로 스크롤 | 휠 |
| 전체 보기 | 더블클릭, `0`, Home, 툴바 버튼 |
| 파일 열기 | 툴바 버튼, Ctrl+O, 창에 드래그 앤 드롭, 명령줄 인자 |

채널이 40개를 넘으면 처음 40개만 켠 상태로 시작합니다. 200채널을 한 화면에
그리면 레인이 얇아져 아무것도 읽을 수 없기 때문입니다.

Windows 의 앱 테마(밝게/어둡게)와 모니터별 DPI 를 따라갑니다.

## 저장소 구조

```
CMakeLists.txt          공통 컴파일/링크 설정 (경고 수준, 보안 완화 옵션)
src/core/               logcore.dll
  include/logcore/logcore.h   공개 C ABI — DLL 경계를 넘는 유일한 계약
  logcore.cpp                 ABI 구현
  dataset.cpp                 격자 → 시간축 + 채널, 타입 판별
  csv_reader.cpp              인코딩·구분자 판별, 따옴표 처리
  xlsx_reader.cpp             OPC 컨테이너 + SpreadsheetML
  text_util.cpp               숫자·시간·디지털 값 해석
src/app/                logscope.exe
  main.cpp                    진입점, DLL 검색 경로 차단, DPI
  app.cpp / app.h             창·입력·Direct2D 렌더러
  theme.h                     팔레트와 치수
tests/logcore_test.cpp  코어 로직 회귀 테스트 (71개 검사)
docs/SECURITY.md        외부 라이브러리 목록과 위험 분석
prototype/web/          초기 웹 프로토타입 (참고용, 빌드에 포함되지 않음)
```

## 외부 라이브러리와 보안

서드파티 라이브러리는 하나도 쓰지 않습니다. 링크하는 것은 전부 Windows 자체
구성요소입니다. 무엇을 왜 쓰는지, 어디가 위험하고 무엇을 막아 뒀는지는
**[`docs/SECURITY.md`](docs/SECURITY.md)** 에 정리돼 있습니다.
