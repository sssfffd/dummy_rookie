# logcore_test — 코어 로직 회귀 테스트 (콘솔).
# DLL 을 링크하지 않고 소스를 직접 컴파일한다. 내부 헤더(dataset.h)를 봐야 하고,
# 그래야 공개 ABI 를 넓히지 않고도 내부 규칙을 검사할 수 있다.

add_executable(logcore_test
  logcore_test.cpp
  ${CMAKE_SOURCE_DIR}/src/core/dataset.cpp
  ${CMAKE_SOURCE_DIR}/src/core/text_util.cpp
)
target_include_directories(logcore_test PRIVATE
  ${CMAKE_SOURCE_DIR}/src/core
  ${CMAKE_SOURCE_DIR}/src/core/include
)
target_link_libraries(logcore_test PRIVATE logscope_hardening)

add_test(NAME logcore_test COMMAND logcore_test)
