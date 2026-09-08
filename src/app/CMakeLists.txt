# logscope.exe — Win32 창 + Direct2D 렌더러. 파일 해석은 전혀 하지 않고
# logcore.dll 에 맡긴다.

add_executable(logscope WIN32
  main.cpp
  app.cpp
  app.h
  theme.h
)

# 판 번호와 커밋 해시를 헤더 하나로 만들어 둔다. 화면에 띄우는 값과 빌드가
# 어긋나지 않게, 손으로 적는 자리를 만들지 않는다.
set(LOGSCOPE_GIT_HASH "unknown")
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE LOGSCOPE_GIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
  if(NOT LOGSCOPE_GIT_HASH)
    set(LOGSCOPE_GIT_HASH "unknown")
  endif()
endif()
string(TIMESTAMP LOGSCOPE_BUILD_DATE "%Y-%m-%d")
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/version.h.in"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/version.h"
  @ONLY)

target_include_directories(logscope PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(logscope PRIVATE logcore logscope_hardening logscope_platform)

# 여기도 전부 Windows 구성요소다.
#   d2d1/dwrite : 렌더링
#   ole32       : COM (파일 대화상자)
#   shell32     : 드래그 앤 드롭
#   user32/gdi32/advapi32 : 창, 검색 상자 글꼴, 테마 설정 읽기
#   uuid        : CLSID_FileOpenDialog 등 인터페이스 GUID
#   imm32       : 한글 IME 조합창 위치 지정
target_link_libraries(logscope PRIVATE d2d1 dwrite ole32 shell32 user32 gdi32 advapi32 uuid imm32)

if(MSVC)
  target_sources(logscope PRIVATE logscope.manifest)
  target_link_options(logscope PRIVATE ${LOGSCOPE_DEPLOAD_EXE})
else()
  # mingw 는 wWinMain 진입점을 쓰려면 -municode 가 필요하다. GUI 실행 파일에만 붙인다.
  target_compile_options(logscope PRIVATE -municode)
  target_link_options(logscope PRIVATE -municode -mwindows)
endif()

set_target_properties(logscope PROPERTIES OUTPUT_NAME "logscope")
