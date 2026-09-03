# 03 · C++ 재작성 — DLL+EXE 구조 용어, 외부 라이브러리·보안 분석

- **날짜** 2026-09-02
- **커밋** [`eae6729`](../../../commit/eae6729) Rebuild IO Log Scope as a C++ Windows app (logcore.dll + logscope.exe)
- **바뀐 파일** `CMakeLists.txt`, `docs/SECURITY.md`, `src/core/*` (7파일), `src/app/*` (6파일), `tests/logcore_test.cpp`, `README.md`, `.gitignore`, `index.html` → `prototype/web/index.html`

## 요청

> 근데 거의 다시 만들어야될듯
> 일단 언어는 c++인데 필요시에는 다른 언어도 가능해
> 그리고 win11에서 빌드해서 exe파일로 쓸 수 있는 dll과 exe가 나오는 프로젝트로 만들어야되고 <- 이런걸 뭐라하지
> 그리고 외부 라이브러리는 보안때문에 사용자제해야되는데 사용했으면 뭐 사용하는 지 그리고 위험점 있어 보이는 부분 설명해줘

## 시작 전에 물어본 것

| 질문 | 선택 |
|---|---|
| DLL 의 성격 | 내부 분리만 (EXE 와 함께 쓰는 코어 모듈) |
| 입력 파일 범위 | `.xlsx` + CSV 둘 다 |
| 빌드 구성 | CMake |

## 답변

C++로 다시 만들어서 푸시했습니다. 질문부터 답변드립니다.

### 1. "DLL과 EXE가 나오는 프로젝트" 를 뭐라 하는가

부르는 층위가 몇 개 있습니다:

- **Visual Studio 기준** — **솔루션(solution)** 안에 **프로젝트(project)** 두 개. 하나는 *동적 연결 라이브러리(DLL)* 프로젝트, 하나는 *Windows 데스크톱 애플리케이션(EXE)* 프로젝트. 통칭 **다중 프로젝트 솔루션**
- **CMake 기준** — 한 프로젝트에 **타깃(target)** 두 개. `add_library(... SHARED)` + `add_executable(... WIN32)`
- **설계 패턴으로서** — *코어 라이브러리 + 얇은 호스트 실행 파일*
- EXE가 DLL을 쓰는 방식은 **암시적 링크(implicit/load-time linking)** 입니다. 빌드 때 생기는 **가져오기 라이브러리(import library, `.lib`)** 를 링크하고 실행 시 로더가 DLL을 올립니다. 반대는 `LoadLibrary`로 직접 올리는 **명시적 링크**

### 2. 외부 라이브러리 — 0개입니다

링크하는 건 전부 Windows 자체 구성요소입니다. vcpkg/NuGet 없고, 빌드 중 네트워크를 안 탑니다.

| 쓰는 것 | 용도 | 왜 이걸 골랐나 |
|---|---|---|
| **Packaging(OPC) API** `opcservices.dll` | `.xlsx` 컨테이너 | `.xlsx`는 OPC 패키지(ZIP+관계XML)라 이 API가 그대로 엽니다. **ZIP/DEFLATE 해제기를 직접 안 짜도 됩니다** — 압축 해제기는 CVE가 가장 많이 나오는 부류라, 직접 만든 300줄보다 MS가 패치하는 OS 구성요소가 낫다고 판단 |
| **XmlLite** `IXmlReader` | 시트 XML | 스트리밍 pull 파서고 **DTD를 아예 지원 안 합니다** → XXE·엔티티 폭탄이 구조적으로 불가능. MSXML은 DTD를 지원해서 매번 꺼야 하므로 제외 |
| Direct2D / DirectWrite | 렌더링 | 차트 라이브러리 불필요 |
| ole32 / shlwapi / user32 / gdi32 / shell32 / advapi32 / uuid | COM, 스트림, 창·입력 | 기본 Win32 |

### 3. 위험해 보이는 부분

**가장 큰 건 DLL 하이재킹입니다.** DLL+EXE 구성의 고전 취약점 — Windows 기본 DLL 검색 경로에 *현재 작업 디렉터리*와 *%PATH%*가 들어 있어서, 쓰기 가능한 폴더에 동명의 DLL을 심어두면 그게 먼저 올라옵니다. 정적 임포트는 코드가 한 줄도 돌기 전에 로더가 처리하므로 런타임 API로는 못 막습니다 → 링커 `/DEPENDENTLOADFLAG` (DLL은 `0x800` System32만, EXE는 `0xA00` System32+앱폴더)로 막고, 시작 시 `SetDefaultDllDirectories`로 이후 로드도 좁혔습니다. **남는 위험: 설치 폴더에 쓰기 권한이 있으면 무의미합니다 — `Program Files`에 설치해야 합니다.**

나머지:
- **압축 폭탄 / 셀 폭탄** — 셀 3200만·채널 4096·샘플 100만·문자열 64KiB·파일 512MiB 상한. 넘으면 **자르지 않고 거절**. 남는 것: OPC가 파트를 내부 버퍼에 펼치는 동안의 일시적 메모리 급증
- **패키지 밖을 가리키는 관계** — External 관계는 따라가지 않음 (임의 URL/경로 열기 차단)
- **DLL 경계 힙 손상** — EXE/DLL의 CRT가 다르면 힙이 깨집니다. C ABI만 노출하고 힙 소유권이 경계를 안 넘게 했습니다
- **형식 문자열** — 파일에서 온 문자열을 `printf` 계열 서식 인자로 절대 안 넘깁니다
- **안 한 것**: 코드 서명 없음(SmartScreen 경고 + 변조 탐지 불가), 퍼징 안 함, ASan 구성 없음, 파싱 프로세스 격리 안 함

전체 목록과 각각의 완화·잔여 위험은 `docs/SECURITY.md`에 있습니다.

---

### 검증 상태 — 여기서 확인한 것과 못 한 것

**확인했습니다:** mingw-w64 크로스 컴파일러로 `-Wall -Wextra -Wshadow -Wconversion` 전체 통과, `logcore.dll`과 `logscope.exe` 링크 성공, 21개 공개 함수 export 확인. 코어 로직 테스트 **71개 검사 전부 통과** (숫자/시간/디지털 해석, 타입 판별, 배치 전환, 시간축 대체 경로, 상한 처리).

**못 했습니다:** 이 환경은 Linux라 **MSVC 빌드도, 실행도 못 했습니다.** 특히 `.xlsx` 파싱(OPC API)과 Direct2D 렌더링은 실제로 돌려본 적이 없습니다 — mingw로 컴파일·링크만 통과한 상태입니다. Win11에서 `cmake -B build -G "Visual Studio 17 2022" -A x64` 후 첫 실행 때 문제가 나면 알려주세요.

`prototype/web/index.html`은 참고용으로 남겨뒀고 빌드에는 포함되지 않습니다. 겹쳐보기 모드와 하단 판독 표는 아직 C++로 안 옮겼습니다 — 필요하시면 추가하겠습니다.
