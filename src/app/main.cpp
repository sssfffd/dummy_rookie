// main.cpp — 진입점. 프로세스가 안전한 상태로 시작하도록 먼저 손을 본 다음
// 창을 띄우고 메시지 루프를 돈다.

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include "app.h"

namespace {

// DLL 검색 경로를 좁힌다. 기본 경로에는 현재 작업 디렉터리와 %PATH% 가 들어 있어서,
// 공격자가 쓰기 가능한 폴더에 시스템 DLL 과 같은 이름의 파일을 두면 그것이 먼저
// 올라올 수 있다(DLL 하이재킹). 여기서는 System32 와 이 EXE 가 있는 폴더만 남긴다.
// 정적으로 임포트하는 DLL 은 이 함수가 실행되기 전에 이미 로더가 처리하므로,
// 그쪽은 링커의 /DEPENDENTLOADFLAG:0xA00 이 같은 일을 한다 (CMakeLists.txt 참고).
void HardenDllSearchPath() {
    if (HMODULE k32 = GetModuleHandleW(L"kernel32.dll")) {
        using PfnSetDirs = BOOL(WINAPI*)(DWORD);
        if (auto fn = reinterpret_cast<PfnSetDirs>(
                reinterpret_cast<void*>(GetProcAddress(k32, "SetDefaultDllDirectories")))) {
            fn(0x00000800 /* SEARCH_SYSTEM32 */ | 0x00000200 /* SEARCH_APPLICATION_DIR */);
        }
    }
}

// 모니터별 DPI 인식. 매니페스트가 있으면 그쪽이 우선이고, 없어도 여기서 켜진다.
void EnablePerMonitorDpi() {
    if (HMODULE u32 = GetModuleHandleW(L"user32.dll")) {
        using PfnSetCtx = BOOL(WINAPI*)(HANDLE);
        if (auto fn = reinterpret_cast<PfnSetCtx>(
                reinterpret_cast<void*>(GetProcAddress(u32, "SetProcessDpiAwarenessContext")))) {
            if (fn(reinterpret_cast<HANDLE>(-4) /* PER_MONITOR_AWARE_V2 */)) return;
        }
    }
    SetProcessDPIAware();
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR cmdLine, int show) {
    HardenDllSearchPath();
    EnablePerMonitorDpi();

    // 파일 대화상자와 Packaging API 가 COM 을 쓴다.
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr)) {
        MessageBoxW(nullptr, L"COM 을 초기화하지 못했습니다.", L"IO Log Scope", MB_ICONERROR);
        return 1;
    }

    // 명령줄 인자. 하나면 그 로그를, 둘이면 이전/이후 로그로 열어 비교한다.
    //   logscope.exe before.xlsx after.xlsx
    std::wstring initial, compare;
    {
        // CommandLineToArgvW 는 빈 문자열을 받으면 인자 대신 실행 파일 경로를
        // 돌려준다. 그대로 쓰면 인자 없이 실행했을 때 자기 자신을 로그로 열려고
        // 한다. 비어 있으면 아예 부르지 않는다.
        const std::wstring raw = cmdLine ? cmdLine : L"";
        const bool has_arg = raw.find_first_not_of(L" \t") != std::wstring::npos;
        if (has_arg) {
            int argc = 0;
            LPWSTR* argv = CommandLineToArgvW(raw.c_str(), &argc);
            if (argv) {
                if (argc > 0 && argv[0]) initial = argv[0];
                if (argc > 1 && argv[1]) compare = argv[1];
                LocalFree(argv);
            }
        }
    }

    int exit_code = 0;
    {
        app::App application;
        if (!application.Create(inst, show, initial.c_str(), compare.c_str())) {
            MessageBoxW(nullptr, L"창을 만들지 못했습니다.", L"IO Log Scope", MB_ICONERROR);
            CoUninitialize();
            return 1;
        }
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        exit_code = static_cast<int>(msg.wParam);
    }

    CoUninitialize();
    return exit_code;
}
