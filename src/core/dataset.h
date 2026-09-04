// dataset.h — 파서가 만든 셀 격자를 시간축 + 채널 배열로 바꾸는 내부 모델.
// 이 헤더는 DLL 밖으로 나가지 않는다. 공개 계약은 include/logcore/logcore.h 다.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "logcore/logcore.h"

namespace lc {

// 파싱 중간에 진행 상황을 알리고, 취소 요청을 받는다.
struct Progress {
    LcProgressFn fn = nullptr;
    void* user = nullptr;

    // 계속해도 되면 true, 취소하라면 false.
    bool report(uint64_t done, uint64_t total) const {
        return fn == nullptr || fn(user, done, total) != 0;
    }
};

// 신뢰할 수 없는 파일에 대한 자원 상한. 넘으면 파싱을 멈춘다.
struct Limits {
    uint32_t max_channels = 4096;
    uint32_t max_samples = 1u << 20;
    uint32_t max_state_values = 4096;
    uint64_t max_cells = 32000000ull;
    uint64_t max_uncompressed_bytes = 512ull * 1024 * 1024;
    Progress progress;   // 기본은 알림 없음
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

    // 실제로 사용한 배치와, 시트에서 표가 시작한 위치 (1 기반, 엑셀과 동일)
    LcOrientation orientation = LC_ORIENT_ROWS;
    uint32_t first_row = 1;
    uint32_t first_col = 1;

    void add_note(const std::wstring& n) {
        if (!notes.empty()) notes += L" ";
        notes += n;
    }
};

// grid 를 소비해 out 을 채운다. orientation 1 이면 먼저 전치한다.
LcStatus build_dataset(Grid& grid, uint32_t orientation, const Limits& lim, Dataset& out);

// ---- 두 로그 비교용 --------------------------------------------------------
// 이름으로 채널 찾기. 정확히 일치하는 것을 먼저 보고, 없으면 공백·대소문자를
// 무시하고 다시 찾는다. 못 찾으면 -1.
int32_t find_channel(const Dataset& ds, const std::wstring& name);

// 임의의 시각 t 에서의 값. 아날로그는 선형 보간, 디지털·상태는 직전 값 유지.
// 기록 구간 밖이면 NaN — 없는 데이터를 지어내지 않는다.
double sample_at(const Dataset& ds, uint32_t ch, double t);

}  // namespace lc
