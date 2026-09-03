# 07 · `build.bat` 이 아무것도 출력하지 않는 문제

- **날짜** 2026-09-03
- **커밋** [`970b2d1`](../../../commit/970b2d1) Fix Windows script encoding and line endings
- **바뀐 파일** `.gitattributes`(신규), `build.bat`, `scripts/build.ps1`, `scripts/diagnose.ps1`(신규), `.vscode/tasks.json`, `README.md`

## 요청

> 방금 작업 성공적으로 된거임?

> vscode에서 build.bat하니까 아무것도 안뜨는데 뭐임

## 답변

먼저 앞 질문 — 커밋·푸시는 성공했지만 **MSVC 로는 한 번도 빌드해 본 적이 없다**고
말씀드렸고, 바로 그 미검증 구간에서 문제가 나왔습니다.

파일을 확인해 보니 **제가 만든 스크립트에 Windows 결함이 세 개** 있었습니다.
리눅스에서 파일을 만들었기 때문에 생긴 것들이고, 여기서는 드러나지 않습니다.

| # | 결함 | 왜 문제인가 |
|---|---|---|
| 1 | `build.bat` 이 **LF 줄바꿈** | `cmd.exe` 는 배치 파일에 CRLF 를 기대합니다. LF 전용이면 명령을 제대로 못 읽고 조용히 아무것도 안 하는 경우가 있습니다 |
| 2 | `build.ps1` 이 **BOM 없는 UTF-8** | Windows PowerShell 5.1 은 BOM 이 없는 `.ps1` 을 시스템 ANSI(한국어 Windows = CP949)로 읽습니다. 한글이 들어간 스크립트는 깨지거나 파싱이 어긋납니다 |
| 3 | `build.bat` 안의 **한글 주석** | 같은 이유로 CP949 콘솔에서 깨집니다. 배치 파일은 ASCII 로만 써야 안전합니다 |

### 고친 것

- **`.gitattributes` 추가** — `*.bat` `*.cmd` `*.ps1` `*.manifest` `*.sln` 등을
  CRLF 로 강제 체크아웃합니다. 근본 원인을 막는 부분입니다.
- **`build.bat` 을 ASCII 전용 + CRLF 로 다시 작성.** 시작하자마자 경로를 찍고,
  스크립트가 없으면 그 사실을 말하고, 종료 코드를 보고합니다.
  **이제 어떤 경우에도 "아무것도 안 뜨는" 상태는 나오지 않습니다.**
- **`build.ps1` 을 UTF-8 BOM + CRLF 로 다시 저장.**
- **`.vscode/tasks.json` 이 `build.bat` 을 거치지 않고 `powershell.exe` 를 직접
  호출**하도록 변경. `Ctrl+Shift+B` 는 cmd 의 배치 해석 문제와 무관해집니다.
- **`scripts/diagnose.ps1` 추가** — cmake · Visual Studio · 줄바꿈 문자를 한 번에
  점검하고 빠진 것을 알려 줍니다.

### 그런데 원인이 다른 것일 수도 있습니다

**가장 흔한 원인은 따로 있습니다.** VS Code 통합 터미널의 기본 셸은 PowerShell 인데,
PowerShell 은 현재 폴더의 실행 파일을 이름만으로 실행하지 않습니다.

```powershell
build.bat      # 동작하지 않음
.\build.bat    # 앞의 .\ 가 반드시 필요
```

이 경우 보통 빨간 오류가 뜨지만, 터미널 설정에 따라 눈에 안 띌 수 있습니다.

### 다음에 해 주실 것

```powershell
git pull
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\diagnose.ps1
.\build.bat
```

`diagnose.ps1` 출력을 그대로 붙여 주시면 어디서 막혔는지 바로 알 수 있습니다.

---

### 솔직한 상태

이번 수정도 **Windows 에서 실행해 검증하지 못했습니다.** 줄바꿈·인코딩·ASCII 는
파일 자체를 확인해서 고친 것이라 확실하지만, "이것이 원인이었는가"는 아직
확인되지 않았습니다. 원인이 위의 `.\` 문제였다면 이번 수정과 무관하게 해결됩니다.
어느 쪽이든 세 결함은 실제 결함이었고 고쳐 두는 게 맞습니다.
