# 04 · VS Code 빌드 환경

- **날짜** 2026-09-03
- **커밋** [`73829a4`](../../../commit/73829a4) Add CMake presets and VS Code workspace configuration
- **바뀐 파일** `CMakePresets.json`, `.vscode/*` (5파일), `.clang-format`, `CMakeLists.txt`, `src/app/CMakeLists.txt`, `src/core/CMakeLists.txt`, `README.md`

## 요청

> vscode로 개발할려고 함 빌드 어케해

## 답변

VS Code에서 바로 열고 빌드되도록 설정을 넣어 푸시했습니다.

### 먼저 깔아야 하는 것

**VS Code에는 컴파일러가 없습니다.** 두 가지가 필요합니다:

1. **[Visual Studio 2022 Build Tools](https://visualstudio.microsoft.com/downloads/)** — 설치기에서 **"C++를 사용한 데스크톱 개발"** 워크로드 선택. MSVC v143 + Windows SDK + CMake + Ninja가 함께 들어옵니다
2. **확장 2개** — 폴더를 열면 VS Code가 알아서 권합니다 (`.vscode/extensions.json`)
   - `ms-vscode.cpptools`, `ms-vscode.cmake-tools`

### 빌드 순서

폴더를 열면 CMake Tools가 `CMakePresets.json`을 읽어 자동 구성합니다.

1. `Ctrl+Shift+P` → **CMake: Select Configure Preset** → **Visual Studio 2022 (x64)**
2. `Ctrl+Shift+P` → **CMake: Select Build Preset** → **VS 2022 · Debug**
3. **`F5`** — 빌드하고 바로 디버그 실행 (`F7`은 빌드만)

터미널파라면:
```powershell
cmake --preset vs2022
cmake --build --preset vs2022-debug
ctest --preset vs2022-debug
```

### 프리셋 2종

| 프리셋 | 언제 |
|---|---|
| `vs2022` | **기본값.** 별도 환경 설정 불필요. `build/vs2022/LogScope.sln`을 Visual Studio로 열 수도 있음 |
| `ninja-debug` / `ninja-release` | 빌드가 훨씬 빠름. MSVC 개발자 환경이 필요한데 CMake Tools가 보통 알아서 잡음. 안 잡히면 **개발자 명령 프롬프트**에서 `code .` |

### 디버그 구성 3개

- **logscope 실행** — 그냥 띄움
- **logscope 실행 (파일 지정)** — 로그 파일 경로를 물어보고 그 파일을 연 채로 시작. 파일 대화상자를 안 거쳐서 **파서 반복 디버깅할 때 편합니다**
- **logcore_test 디버그** — 코어 테스트를 콘솔에서

IntelliSense는 CMake 구성에서 가져오므로 `c_cpp_properties.json`을 손댈 일 없습니다. 한글 주석 때문에 편집기 인코딩은 UTF-8로 고정해 뒀습니다(MSVC에는 이미 `/utf-8`).

**산출물**: `build\vs2022\bin\Debug\` 에 `logscope.exe` + `logcore.dll`. 두 파일은 반드시 같은 폴더에 있어야 합니다 — EXE가 앱 폴더와 System32에서만 DLL을 찾도록 링크돼 있습니다.

---

**설정하면서 실제 버그 2개가 나와서 같이 고쳤습니다:**

1. 산출물 경로가 모든 구성에서 `bin/` 하나였습니다 → Visual Studio 생성기에서 Debug와 Release가 서로 덮어씁니다. `bin/$<CONFIG>/`로 분리했습니다.
2. `-municode`가 공통 옵션에 있어서 **콘솔 테스트 타깃이 링크에 실패**했습니다(`wWinMain` 요구). GUI 실행 파일에만 붙게 옮겼습니다.

이건 mingw 크로스 툴체인으로 CMake 구성부터 링크까지 실제로 돌려서 잡은 것이고, Debug/Release 둘 다 3개 타깃이 정상 빌드되는 것을 확인했습니다. 다만 **MSVC 경로는 여전히 여기서 못 돌려봤습니다** — Visual Studio 생성기와 `/DEPENDENTLOADFLAG` 같은 MSVC 전용 옵션은 첫 빌드 때 확인이 필요합니다. 막히면 에러 그대로 붙여주세요.
