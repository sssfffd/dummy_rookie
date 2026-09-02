# 외부 라이브러리와 위험 분석

## 요약

**서드파티 라이브러리는 하나도 쓰지 않습니다.** 링크하는 것은 전부 Windows 자체
구성요소(Windows SDK / OS DLL)입니다. `vcpkg`, `NuGet`, `FetchContent`, 벤더링한
소스 트리 — 어느 것도 없고, 빌드할 때 네트워크를 타지 않습니다.

프로그램은 네트워크에 접속하지 않고, 임시 파일을 만들지 않으며, 레지스트리는
테마 설정 하나를 **읽기만** 합니다. 파일은 `STGM_READ | STGM_SHARE_DENY_WRITE` 로
열어 원본을 건드리지 않습니다.

---

## 1. 링크하는 Windows 구성요소

| 구성요소 | 어디에 | 무엇에 쓰나 | 왜 이걸 골랐나 |
|---|---|---|---|
| `opcservices.dll` (Packaging / OPC API, `msopc.h`) | logcore.dll | `.xlsx` 컨테이너 열기 | `.xlsx` 는 OPC 패키지(ZIP + 관계 XML)입니다. 이 API 가 컨테이너를 그대로 다루므로 **ZIP/DEFLATE 해제 코드를 직접 만들지 않아도 됩니다.** 압축 해제기는 CVE 가 가장 많이 나오는 부류라, 직접 만든 300줄보다 MS 가 패치하는 OS 구성요소에 맡기는 쪽이 낫다고 판단했습니다. |
| `xmllite.dll` (`IXmlReader`) | logcore.dll | 시트·공유문자열·스타일 XML 파싱 | 스트리밍 pull 파서이고 **DTD 를 아예 지원하지 않습니다.** 즉 XXE 와 엔티티 폭탄(billion laughs)이 구조적으로 불가능합니다. DOM 이 아니라서 큰 시트를 열어도 메모리가 파일 크기만큼 부풀지 않습니다. MSXML(DOM/SAX)은 DTD 를 지원해서 매번 꺼야 하므로 제외했습니다. |
| `ole32` / `oleaut32` / `uuid` | 양쪽 | COM 인스턴스 생성, 인터페이스 GUID | 위 두 API 가 COM 기반입니다. |
| `shlwapi` | logcore.dll | `SHCreateStreamOnFileEx`, `SHCreateMemStream` | 파일/메모리를 `IStream` 으로 감쌉니다. |
| `d2d1` / `dwrite` | logscope.exe | 파형 렌더링, 텍스트 | GPU 가속 2D. 차트 라이브러리를 끌어오지 않아도 됩니다. |
| `user32` / `gdi32` / `shell32` / `advapi32` | logscope.exe | 창·입력, 검색 상자 글꼴, 드래그 앤 드롭, 테마 설정 읽기 | 기본 Win32. |
| `dwmapi.dll` | logscope.exe | 제목 표시줄 다크 모드 | **정적 링크가 아니라** `LoadLibraryExW(..., LOAD_LIBRARY_SEARCH_SYSTEM32)` 로 필요할 때만 올립니다. |
| MSVC CRT (`/MD`) | 양쪽 | 표준 C++ 런타임 | 재배포 패키지가 필요합니다(아래 6번). |

## 2. 직접 만든 것 = 우리가 책임지는 코드

OS 에 맡기지 못해 직접 짠 파서는 다음뿐입니다. 검토는 여기에 집중하면 됩니다.

- `src/core/text_util.cpp` — 숫자 · `HH:MM:SS.mmm` · ISO 8601 · 엑셀 일련값 해석
- `src/core/csv_reader.cpp` — 인코딩 판별, 구분자 판별, 따옴표 처리
- `src/core/dataset.cpp` — 격자 → 시간축 + 채널 변환, 타입 판별
- `src/core/xlsx_reader.cpp` — OPC 관계 추적과 SpreadsheetML 해석 (XML 토큰화는 XmlLite 가 함)

`tests/logcore_test.cpp` 가 이 중 플랫폼 비의존 부분을 71개 항목으로 검사합니다.

---

## 3. 위험해 보이는 지점

### 3.1 DLL 하이재킹 — **이 구조에서 가장 큰 위험**

EXE 와 DLL 이 함께 배포되는 프로그램의 고전적 취약점입니다. Windows 의 기본 DLL
검색 경로에는 **현재 작업 디렉터리**와 **`%PATH%`** 가 들어 있어서, 공격자가 쓰기
가능한 폴더(예: 다운로드 폴더)에 시스템 DLL 과 같은 이름의 파일을 두고 그 폴더에서
프로그램을 실행시키면 그 DLL 이 먼저 올라옵니다.

막아 둔 것:

- 링커 `/DEPENDENTLOADFLAG` — 정적으로 임포트하는 DLL 은 프로그램 코드가 한 줄도
  돌기 전에 로더가 처리하므로, 런타임 API 로는 못 막고 링커 플래그로 막습니다.
  - `logcore.dll` → `0x800` (System32 만)
  - `logscope.exe` → `0xA00` (System32 + 앱 폴더). 옆에 있는 `logcore.dll` 을 찾아야
    하므로 앱 폴더를 더하되, **현재 디렉터리와 `%PATH%` 는 빠집니다.**
- 시작하자마자 `SetDefaultDllDirectories(SEARCH_SYSTEM32 | SEARCH_APPLICATION_DIR)` —
  이후의 모든 `LoadLibrary` 에 같은 규칙을 적용합니다.
- `dwmapi.dll` 은 `LOAD_LIBRARY_SEARCH_SYSTEM32` 를 명시해 올립니다.

**남는 위험:** 설치 폴더에 쓰기 권한이 있으면 `logcore.dll` 을 바꿔치기할 수 있습니다.
`Program Files` 처럼 관리자만 쓸 수 있는 위치에 설치해야 합니다. 사용자 폴더나
네트워크 공유에서 실행하면 이 방어가 무의미해집니다.

### 3.2 신뢰할 수 없는 파일 파싱

로그 파일은 결국 **남이 준 임의의 바이트**입니다. `.xlsx` 는 임의의 ZIP 이기도 합니다.

| 공격 | 막아 둔 것 | 남는 것 |
|---|---|---|
| 압축 폭탄 (작은 파일 → 거대한 압축 해제) | `LcOpenOptions` 상한: 셀 3,200만 · 채널 4,096 · 샘플 1,048,576 · 문자열당 64 KiB · 파일 512 MiB. 넘으면 `LC_ERR_TOO_LARGE` 로 **자르지 않고 거절**합니다. | OPC 가 파트 하나를 내부 버퍼에 펼치는 동안의 일시적 메모리 급증까지는 못 막습니다. 더 엄격하게 하려면 파싱을 Job 객체(메모리 상한)나 별도 프로세스로 분리해야 합니다. |
| 셀 좌표 폭탄 (`r="XFD1048576"`) | 열 참조를 16,384 에서 잘라 거절, 행 번호도 상한 검사 | — |
| 공유 문자열 인덱스 범위 초과 | 인덱스를 테이블 크기와 비교한 뒤에만 사용 | — |
| 스타일 인덱스 범위 초과 | 동일 | — |
| XXE · 엔티티 폭탄 | XmlLite 는 DTD 미지원. 추가로 `DtdProcessing_Prohibit` 와 최대 깊이 256 을 **명시적으로** 설정 | — |
| 패키지 밖을 가리키는 관계 (External 관계로 임의 URL/로컬 경로 열기) | `OPC_URI_TARGET_MODE_INTERNAL` 이 아닌 관계는 따라가지 않음 | — |
| 경로 탐색 (`../../windows/...`) | 파트 이름은 OPC API 가 사양에 맞게 정규화하고, 우리는 파일 시스템 경로를 조립하지 않음 | — |
| 형식 문자열 공격 | **파일에서 온 문자열을 `printf` 계열의 서식 인자로 절대 넘기지 않습니다.** 서식 문자열은 전부 리터럴입니다. | — |
| `inf` / `nan` 값으로 축 스케일 파괴 | `parse_number` 가 `inf`/`nan` 문자열과 뒤에 붙은 잉여 문자를 거절 | — |

### 3.3 DLL 경계의 ABI · 힙

EXE 와 DLL 이 서로 다른 CRT(또는 Debug/Release 혼합)로 빌드되면, 한쪽에서 할당한
메모리를 다른 쪽에서 해제할 때 **힙이 깨집니다.** 크래시가 원인에서 멀리 떨어져
나타나 디버깅이 특히 어렵습니다.

막아 둔 것: 공개 헤더는 **C 링키지만** 노출하고, `std::string`/`std::vector` 같은 C++
타입이나 힙 소유권이 경계를 넘지 않습니다. 반환값은 전부 POD 아니면 데이터셋이
소유한 `const` 포인터이고, 해제는 `lc_close()` 하나뿐입니다. 호출 규약도 `__cdecl` 로
고정했습니다. `LcOpenOptions` 는 `struct_size` 를 확인해 구조체가 커졌을 때 예전
호출자가 조용히 깨지지 않게 합니다.

**남는 위험:** 반환된 포인터는 `lc_close()` 이후 무효입니다. 지금은 UI 가 단일
스레드라 문제가 없지만, 백그라운드 로딩을 넣는다면 수명 관리를 다시 봐야 합니다.

### 3.4 빌드 시 켜 둔 완화 기능

`/GS`(스택 쿠키) · `/guard:cf`(CFG) · `/sdl` · `/permissive-` · `/W4` ·
`/DYNAMICBASE`(ASLR) · `/HIGHENTROPYVA` · `/NXCOMPAT`(DEP) · `/GUARD:CF`.

이건 방어선이지 대책이 아닙니다. 실제 안전은 위 3.2 의 경계 검사에서 나옵니다.

### 3.5 아직 안 한 것 (권장)

1. **코드 서명이 없습니다.** SmartScreen 경고가 뜨고, 배포본이 변조돼도 알 수 없습니다.
   사내 배포라면 조직 인증서로 `signtool` 서명을 붙이는 것을 권합니다.
2. **퍼징을 안 했습니다.** 파일 파서는 퍼징 대상 1순위입니다. `build_dataset` 과
   `read_delimited` 는 순수 함수라 libFuzzer/WinAFL 을 붙이기 쉽습니다.
3. **ASan 빌드 구성이 없습니다.** MSVC 는 `/fsanitize=address` 를 지원하므로
   테스트 구성에 하나 추가해 두면 좋습니다.
4. **파싱을 낮은 권한 프로세스로 분리하지 않았습니다.** 정말 적대적인 파일을
   다뤄야 한다면 파싱 전용 자식 프로세스(AppContainer 또는 Job 메모리 상한)로
   격리하는 것이 다음 단계입니다.
5. `.xls`(구형 BIFF)는 **일부러 지원하지 않습니다.** 지원하려면 OLE 복합 문서
   파서를 직접 짜야 하는데, 그 순간 이 문서의 "직접 만든 코드" 항목이 크게 늘어납니다.
   `.xlsx` 나 CSV 로 저장해서 쓰는 편이 안전합니다.

### 3.6 환경 가정

`opcservices.dll` 과 `xmllite.dll` 은 Windows 7 이후 모든 데스크톱 SKU 에 들어
있습니다. Windows 11 에서는 확인할 필요가 없지만, Server Core 나 다듬어진 이미지에
배포한다면 존재 여부를 먼저 확인하세요. 없으면 `.xlsx` 열기가
`LC_ERR_INTERNAL` 로 실패하고 CSV 경로는 계속 동작합니다.

---

## 4. 의존성을 더 줄이고 싶다면

`.xlsx` 지원을 빼고 **CSV/TSV 전용**으로 가면 `opcservices` · `xmllite` · `shlwapi` ·
`ole32` 의존이 통째로 사라지고, 남는 파싱 코드는 `csv_reader.cpp` 200줄 남짓입니다.
공격면이 가장 작아지는 구성입니다. 엑셀에서 "다른 이름으로 저장 → CSV" 한 단계를
사용자가 감수할 수 있다면 검토할 만합니다.
