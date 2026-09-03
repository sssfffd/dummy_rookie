# 3.18 = check_linker_flag 가 들어온 버전. CMakePresets 를 쓰려면 3.21 이상이
# 필요하지만, 프리셋 없이 -G 로 직접 구성하는 경로는 3.18 에서도 동작한다.
cmake_minimum_required(VERSION 3.18)
project(LogScope VERSION 1.0.0 LANGUAGES CXX)

# ---------------------------------------------------------------------------
# LogScope — 엑셀/CSV 로그를 IO 채널 파형으로 그리는 Windows 데스크톱 앱.
#
#   logcore.dll   파일 파싱 · 데이터 모델 · 다운샘플링 (UI 없음)
#   logscope.exe  Win32 창 + Direct2D 렌더러 (UI 만)
#
# 서드파티 라이브러리를 쓰지 않습니다. 링크하는 것은 Windows SDK 뿐입니다.
# 자세한 내용과 위험 분석은 docs/SECURITY.md 를 보세요.
# ---------------------------------------------------------------------------

if(NOT WIN32)
  message(FATAL_ERROR
    "LogScope 는 Windows 전용입니다. Windows 11 + Visual Studio 2022 에서 빌드하거나, "
    "mingw-w64 크로스 툴체인 파일을 지정해 구성하세요.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# logscope.exe 와 logcore.dll 이 같은 폴더에 놓이게 한다. EXE 는 앱 폴더와
# System32 에서만 DLL 을 찾도록 링크돼 있어서(docs/SECURITY.md 3.1), 둘이 흩어져
# 있으면 실행 자체가 안 된다. $<CONFIG> 를 쓰면 Visual Studio 처럼 구성이 여러 개인
# 생성기에서도 Debug 와 Release 산출물이 서로 덮어쓰지 않는다.
#   build/<preset>/bin/Debug/{logscope.exe, logcore.dll}
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/$<CONFIG>")

if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

  # /std:c++17 은 Visual Studio 2017 15.3 (MSVC 1911) 부터다. 그 아래에서는
  # CMake 가 C++17 을 요청해도 컴파일러가 받지 못해 엉뚱한 오류가 난다.
  if(MSVC_VERSION LESS 1911)
    message(FATAL_ERROR
      "Visual Studio 2017 15.3 이상이 필요합니다 (C++17 지원). "
      "감지된 MSVC_VERSION=${MSVC_VERSION}. Visual Studio 설치 관리자에서 업데이트하세요.")
  endif()
endif()

include(CheckCXXCompilerFlag)
include(CheckLinkerFlag)

# 두 타깃이 공유하는 컴파일/링크 설정 (경고 수준 + 보안 완화 기능)
add_library(logscope_hardening INTERFACE)
if(MSVC)
  # 어느 VS 버전에나 있는 것들
  target_compile_options(logscope_hardening INTERFACE
    /W4
    /GS                 # 스택 버퍼 오버런 검사
    /guard:cf           # 제어 흐름 보호
    /sdl                # 추가 보안 검사, 관련 경고를 오류로
    /utf-8              # 소스에 한글 주석이 있다
    $<$<CONFIG:Release>:/O2>
  )
  target_compile_definitions(logscope_hardening INTERFACE
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN NOMINMAX
    _CRT_SECURE_NO_WARNINGS=0   # 안전하지 않은 CRT 함수를 쓰지 않는다
  )
  target_link_options(logscope_hardening INTERFACE
    /DYNAMICBASE        # ASLR
    /NXCOMPAT           # DEP
    /GUARD:CF
  )
  if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    target_link_options(logscope_hardening INTERFACE /HIGHENTROPYVA)
  endif()

  # 버전에 따라 없을 수 있는 것들. 버전 번호로 짐작하지 않고 컴파일러에게 직접
  # 물어본다. (/permissive- 와 /Zc:__cplusplus 는 VS2017 중간 업데이트부터,
  # /Zc:preprocessor 는 VS2019 16.5 부터 들어왔다.)
  foreach(opt /permissive- /Zc:__cplusplus /Zc:preprocessor)
    string(MAKE_C_IDENTIFIER "LOGSCOPE_HAS${opt}" have_var)
    check_cxx_compiler_flag(${opt} ${have_var})
    if(${have_var})
      target_compile_options(logscope_hardening INTERFACE ${opt})
    endif()
  endforeach()

  # /DEPENDENTLOADFLAG 는 정적으로 임포트하는 DLL 을 어디서 찾을지 정한다.
  # 현재 디렉터리와 %PATH% 를 탐색에서 빼는 것이 목적이다 (docs/SECURITY.md 3.1).
  #   0x800 = SEARCH_SYSTEM32          (시스템 DLL 만 쓰는 logcore.dll)
  #   0xA00 = SEARCH_SYSTEM32 | APPLICATION_DIR
  #           (EXE 는 옆에 있는 logcore.dll 도 찾아야 하므로 앱 폴더를 더한다)
  # 옛 링커에는 없는 옵션이라 역시 직접 확인한다.
  check_linker_flag(CXX "/DEPENDENTLOADFLAG:0x800" LOGSCOPE_HAS_DEPENDENTLOADFLAG)
  if(LOGSCOPE_HAS_DEPENDENTLOADFLAG)
    set(LOGSCOPE_DEPLOAD_DLL /DEPENDENTLOADFLAG:0x800)
    set(LOGSCOPE_DEPLOAD_EXE /DEPENDENTLOADFLAG:0xA00)
  else()
    set(LOGSCOPE_DEPLOAD_DLL "")
    set(LOGSCOPE_DEPLOAD_EXE "")
    message(WARNING
      "이 링커는 /DEPENDENTLOADFLAG 를 지원하지 않습니다. 정적으로 임포트하는 DLL 의 "
      "검색 경로가 제한되지 않으므로, 반드시 쓰기 권한이 없는 폴더(Program Files 등)에 "
      "설치해서 실행하세요. 실행 중 LoadLibrary 는 SetDefaultDllDirectories 로 계속 "
      "보호됩니다. 자세한 내용은 docs/SECURITY.md 3.1 을 보세요.")
  endif()
else()
  # mingw-w64 크로스 빌드 (CI/문법 검증용 경로)
  # -municode 는 여기 넣지 않는다. wmain/wWinMain 진입점을 강제하기 때문에
  # 콘솔 테스트 실행 파일까지 wWinMain 을 요구하게 된다. 필요한 곳(logscope.exe)
  # 에서만 붙인다.
  target_compile_options(logscope_hardening INTERFACE
    -Wall -Wextra -Wconversion -Wshadow)
  target_compile_definitions(logscope_hardening INTERFACE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_link_options(logscope_hardening INTERFACE
    -Wl,--dynamicbase -Wl,--nxcompat -Wl,--high-entropy-va)
endif()

add_subdirectory(src/core)
add_subdirectory(src/app)

include(CTest)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

message(STATUS "LogScope ${PROJECT_VERSION} — 산출물: ${CMAKE_BINARY_DIR}/bin/<Config>/ (logscope.exe + logcore.dll)")
