# logcore.dll — 파일 파싱과 데이터 모델. UI 코드가 전혀 없어서 콘솔 도구나
# 테스트에서도 그대로 쓸 수 있다.

add_library(logcore SHARED
  logcore.cpp
  dataset.cpp
  csv_reader.cpp
  xlsx_reader.cpp
  text_util.cpp
)

target_include_directories(logcore
  PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

# LOGCORE_BUILD 는 dllexport 를, 소비자 쪽에서는 dllimport 를 켠다.
target_compile_definitions(logcore PRIVATE LOGCORE_BUILD)
target_link_libraries(logcore PRIVATE logscope_hardening)
if(MSVC)
  target_link_options(logcore PRIVATE ${LOGSCOPE_DEPLOAD_DLL})
endif()

# 링크하는 것은 전부 Windows 구성요소다. 서드파티 없음.
#   ole32/oleaut32 : COM (Packaging API 인스턴스 생성)
#   xmllite        : IXmlReader (스트리밍 XML, DTD 미지원)
#   shlwapi        : SHCreateStreamOnFileEx / SHCreateMemStream
#   uuid           : CLSID_OpcFactory 등 인터페이스 GUID
target_link_libraries(logcore PRIVATE ole32 oleaut32 xmllite shlwapi uuid)

set_target_properties(logcore PROPERTIES OUTPUT_NAME "logcore")
