// dataset.h — 파서가 만든 셀 격자를 시간축 + 채널 배열로 바꾸는 내부 모델.
// 이 헤더는 DLL 밖으로 나가지 않는다. 공개 계약은 include/logcore/logcore.h 다.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "logcore/logcore.h"

namespace lc {

// 신뢰할 수 없는 파일에 대한 자원 상한. 넘으면 파싱을 멈춘다.
struct Limits {
    uint32_t max_channels = 4096;
    uint32_t max_samples = 1u << 20;
    uint32_t max_state_values = 4096;
    uint64_t max_cells = 32000000ull;
    uint64_t max_uncompressed_bytes = 512ull * 1024 * 1024;
};

Limits limits_from(const LcOpenOptions* opt);

// 한 셀. 숫자인지 문자열인지 구분을 유지해야 타입 판별이 정확해진다.
struct Cell {
    enum class Kind : uint8_t { Empty, Number, Text, Bool, DateMs };
    Kind kind = Kind::Empty;
    double num = 0.0;      // Number / Bool / DateMs 에서 유효
    std::wstring text;     // Text 에서 유효

    bool empty() const { return kind == Kind::Empty; }
};

using Row = std::vector<Cell>;
using Grid = std::vector<Row>;

struct Channel {
    std::wstring name;
    LcChannelType type = LC_CH_ANALOG;
    std::vector<double> values;          // NaN = 빈 셀(끊김)
    std::vector<std::wstring> states;    // LC_CH_STATE 에서만
    double min = 0.0;
    double max = 0.0;
};

struct Dataset {
    std::vector<double> times;
    LcTimeKind time_kind = LC_TIME_INDEX;
    std::wstring time_unit;
    std::vector<Channel> channels;
    std::wstring notes;

    void add_note(const std::wstring& n) {
        if (!notes.empty()) notes += L" ";
        notes += n;
    }
};

// grid 를 소비해 out 을 채운다. orientation 1 이면 먼저 전치한다.
LcStatus build_dataset(Grid& grid, uint32_t orientation, const Limits& lim, Dataset& out);

}  // namespace lc
