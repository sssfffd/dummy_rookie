cmake_minimum_required(VERSION 3.21)
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

# EXE 옆에 DLL 이 놓이도록 산출물을 한곳에 모은다 (실행 시 DLL 탐색 문제 방지)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
foreach(cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${cfg} "${CMAKE_BINARY_DIR}/bin")
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${cfg} "${CMAKE_BINARY_DIR}/bin")
  set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${cfg} "${CMAKE_BINARY_DIR}/lib")
endforeach()

if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

# 두 타깃이 공유하는 컴파일/링크 설정 (경고 수준 + 보안 완화 기능)
add_library(logscope_hardening INTERFACE)
if(MSVC)
  target_compile_options(logscope_hardening INTERFACE
    /W4 /permissive- /Zc:__cplusplus /Zc:preprocessor
    /GS                 # 스택 버퍼 오버런 검사
    /guard:cf           # 제어 흐름 보호
    /sdl                # 추가 보안 검사, 관련 경고를 오류로
    /utf-8
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
  # /DEPENDENTLOADFLAG 는 타깃마다 다르다. 정적으로 임포트하는 DLL 을 어디서 찾을지
  # 정하는 값이라, 현재 디렉터리와 %PATH% 를 탐색에서 빼는 것이 목적이다.
  #   0x800 = SEARCH_SYSTEM32          (시스템 DLL 만 쓰는 logcore.dll)
  #   0xA00 = SEARCH_SYSTEM32 | APPLICATION_DIR
  #           (EXE 는 옆에 있는 logcore.dll 도 찾아야 하므로 앱 폴더를 더한다)
  set(LOGSCOPE_DEPLOAD_DLL /DEPENDENTLOADFLAG:0x800)
  set(LOGSCOPE_DEPLOAD_EXE /DEPENDENTLOADFLAG:0xA00)
  if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    target_link_options(logscope_hardening INTERFACE /HIGHENTROPYVA)
  endif()
else()
  # mingw-w64 크로스 빌드 (문법 검증용 경로)
  target_compile_options(logscope_hardening INTERFACE
    -Wall -Wextra -Wconversion -Wshadow -municode)
  target_compile_definitions(logscope_hardening INTERFACE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_link_options(logscope_hardening INTERFACE
    -municode -Wl,--dynamicbase -Wl,--nxcompat -Wl,--high-entropy-va)
endif()

add_subdirectory(src/core)
add_subdirectory(src/app)

include(CTest)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

message(STATUS "LogScope ${PROJECT_VERSION} — 산출물: ${CMAKE_BINARY_DIR}/bin (logcore.dll, logscope.exe)")
