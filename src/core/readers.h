// readers.h — 파일 형식별 파서. 각각 신뢰할 수 없는 바이트를 받아 셀 격자를 만든다.
#pragma once

#include <windows.h>
#include <objidl.h>

#include "dataset.h"

namespace lc {

// CSV / TSV / 탭 구분 텍스트. BOM 으로 UTF-8 / UTF-16 을 판별하고,
// BOM 이 없으면 UTF-8 로 시도한 뒤 실패하면 시스템 ANSI 코드페이지로 읽는다.
LcStatus read_delimited(const uint8_t* data, size_t size, const Limits& lim, Grid& out);

// .xlsx / .xlsm. Windows 내장 Packaging(OPC) API 로 컨테이너를 열고
// XmlLite 로 시트 XML 을 스트리밍 파싱한다. 서드파티 라이브러리는 쓰지 않는다.
LcStatus read_xlsx(IStream* stream, const Limits& lim, Grid& out);

}  // namespace lc
