# 06 · Visual Studio 2017 지원, VS Code 확장 없이 쓰기

- **날짜** 2026-09-03
- **커밋** [`95ba55e`](../../../commit/95ba55e) Support Visual Studio 2017 and a build path with no VS Code extensions
- **바뀐 파일** `CMakeLists.txt`, `CMakePresets.json`, `scripts/build.ps1`(신규), `build.bat`(신규), `.vscode/tasks.json`, `.vscode/launch.json`, `.vscode/settings.json`, `src/core/xlsx_reader.cpp`, `README.md`

## 요청

> 그리고 visual studio 2017버전까지 사용 가능함
> 그리고 vscode extention은 사용 안할껀데 괜찮아?

## 답변

둘 다 손봐야 했습니다. **VS 2017은 지금 설정 그대로는 구성 단계에서 깨졌을 겁니다.**

### 1. VS 2017 — 실제로 깨지던 것

`CMakeLists.txt` 에 **`/Zc:preprocessor` 가 무조건 붙어 있었습니다. 이건 VS 2019 16.5부터라 VS 2017에서는 컴파일러가 거부합니다.** `/permissive-`, `/Zc:__cplusplus`, `/DEPENDENTLOADFLAG` 도 VS2017 중간 업데이트마다 있고 없고가 갈립니다.

버전 번호로 짐작하는 대신 **구성할 때 컴파일러·링커에게 직접 물어보고** 받아 주는 것만 붙이도록 바꿨습니다 (`check_cxx_compiler_flag` / `check_linker_flag`). 제 기억 속 버전 번호가 틀려도 빌드가 안 깨지는 방식입니다.

`/DEPENDENTLOADFLAG` 가 없는 링커면 그냥 넘어가되 **구성 로그에 경고를 냅니다** — 정적 임포트 DLL 검색 경로가 안 좁혀지므로 `Program Files` 같은 쓰기 불가 폴더에 설치해야 한다는 내용입니다.

그 외:
- `vs2017` 프리셋 추가 (`Visual Studio 15 2017`, x64) — 이제 이게 기본입니다
- `cmake_minimum_required` 를 3.21 → **3.18** 로 낮췄습니다. 프리셋을 쓰려면 여전히 3.21+ 가 필요하지만, `-G` 로 직접 구성하는 경로는 3.18에서도 됩니다
- **VS 2017 15.3 미만이면 구성 단계에서 명확한 한국어 오류**를 냅니다 (`/std:c++17` 없음). 엉뚱한 컴파일 오류 수십 개 대신 이유를 바로 보게

**CMake는 따로 설치하셔야 합니다.** VS2017 번들 CMake는 대체로 3.12 이하라 이 프로젝트 최소 버전에 못 미치고, PATH에도 잘 안 잡힙니다. [cmake.org](https://cmake.org/download/) 에서 받아 *Add CMake to the system PATH* 를 켜세요.

### 2. 확장 없이 — 빌드는 됩니다, 두 가지가 안 됩니다

**빌드는 완전히 됩니다.** `tasks.json` 을 전부 다시 썼습니다. 원래는 `"type": "cmake"` 라 CMake Tools 확장이 있어야 돌아갔는데, 지금은 전부 **VS Code 내장 shell 작업**입니다.

- **`Ctrl+Shift+B`** — 설치된 Visual Studio 를 자동으로 찾아 구성하고 Debug 빌드
- 컴파일 오류는 VS Code 내장 `$msCompile` 표시기가 잡아서 **클릭하면 해당 줄로 이동**합니다 (이것도 확장 아닙니다)
- 터미널파를 위해 `build.bat` 도 추가했습니다:
  ```
  build.bat                     Debug
  build.bat -Config Release     Release
  build.bat -Test               빌드 후 테스트
  build.bat -Clean -Run         처음부터 빌드하고 실행
  ```
  `scripts/build.ps1` 이 `vswhere` 로 VS 2017/2019/2022 를 자동 감지합니다. PowerShell 실행 정책이 막혀 있어도 되게 `-ExecutionPolicy Bypass` 를 넣었습니다.

**안 되는 것 두 가지:**

| 안 되는 것 | 왜 | 대안 |
|---|---|---|
| VS Code 안에서 `F5` 디버깅 | `cppvsdbg` 디버거 자체가 C/C++ 확장에서 옵니다 | **Visual Studio 2017 로 `build\vs2017\LogScope.sln` 을 열어 디버깅.** 어차피 VS 디버거가 VS Code 쪽보다 낫습니다 |
| C++ IntelliSense (정의로 이동, 자동완성) | 확장이 제공하는 기능 | 코드 탐색이 필요할 때만 Visual Studio 사용 |

즉 **VS Code = 편집 + 빌드, Visual Studio 2017 = 디버깅** 조합이 되고, 이건 충분히 정상적인 작업 방식입니다. 어차피 `.sln` 이 생성되니 왔다 갔다 하는 비용도 거의 없습니다.

확장을 못 쓰는 이유가 사내 정책이나 마켓플레이스 차단이라면, C/C++ 와 CMake Tools 는 둘 다 Microsoft 배포라 `.vsix` 오프라인 설치가 됩니다 (`code --install-extension <파일>`).

`launch.json` 은 순서를 바꿔서, 앞의 세 구성이 **경로를 직접 가리키도록** 했습니다. CMake Tools 없이 C/C++ 확장만 있어도 동작합니다. (다른 VS 버전으로 빌드했으면 경로의 `vs2017` 부분만 바꾸면 됩니다.)

`settings.json` 의 `cmake.*` / `C_Cpp.*` 값들은 확장이 없으면 그냥 무시됩니다. 지워도 빌드에 영향 없다고 주석을 달아 뒀습니다.

### 3. 덤으로 고친 것

`__uuidof` 대신 이름 있는 GUID 상수(`CLSID_OpcFactory` 등)를 쓰면 옛 SDK 에서 더 안전할까 싶어 바꿔 봤는데, **링크가 깨졌습니다** — 그 GUID 들이 툴체인마다 다른 `.lib` 에 들어 있습니다. `__uuidof` 로 되돌리고, 왜 그게 더 나은지 주석으로 남겼습니다. `__uuidof` 는 MSVC 확장이지만 VS2017 을 포함한 MSVC 전 버전과 mingw/clang 이 모두 지원합니다.

---

### 검증 상태

mingw 크로스 툴체인으로 CMake 구성부터 링크까지 Debug/Release 둘 다 통과했습니다 (`logcore.dll`, `logscope.exe`, `logcore_test.exe`).

**여전히 MSVC 로는 못 돌려봤습니다.** 특히 이번에 넣은 `check_cxx_compiler_flag` / `check_linker_flag` 는 MSVC 브랜치 안에 있어서 여기서는 아예 실행되지 않았습니다. VS2017 에서 첫 구성 때 로그를 보시면 어떤 옵션이 빠졌는지 나옵니다 — 이상하면 그 출력을 붙여 주세요.
