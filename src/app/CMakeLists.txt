# logscope.exe — Win32 창 + Direct2D 렌더러. 파일 해석은 전혀 하지 않고
# logcore.dll 에 맡긴다.

add_executable(logscope WIN32
  main.cpp
  app.cpp
  app.h
  theme.h
)

target_include_directories(logscope PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(logscope PRIVATE logcore logscope_hardening)

# 여기도 전부 Windows 구성요소다.
#   d2d1/dwrite : 렌더링
#   ole32       : COM (파일 대화상자)
#   shell32     : 드래그 앤 드롭
#   user32/gdi32/advapi32 : 창, 검색 상자 글꼴, 테마 설정 읽기
#   uuid        : CLSID_FileOpenDialog 등 인터페이스 GUID
target_link_libraries(logscope PRIVATE d2d1 dwrite ole32 shell32 user32 gdi32 advapi32 uuid)

if(MSVC)
  target_sources(logscope PRIVATE logscope.manifest)
  target_link_options(logscope PRIVATE ${LOGSCOPE_DEPLOAD_EXE})
endif()

set_target_properties(logscope PROPERTIES OUTPUT_NAME "logscope")
