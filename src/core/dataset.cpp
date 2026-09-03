#include "dataset.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <cstdio>
#include <unordered_map>

#include "text_util.h"

namespace lc {
namespace {

const double kNaN = std::numeric_limits<double>::quiet_NaN();

std::wstring to_wstr(size_t v) {
    wchar_t buf[24];
    int n = 0;
    if (v == 0) { buf[n++] = L'0'; }
    wchar_t tmp[24];
    int t = 0;
    while (v > 0) { tmp[t++] = static_cast<wchar_t>(L'0' + (v % 10)); v /= 10; }
    while (t > 0) buf[n++] = tmp[--t];
    return std::wstring(buf, static_cast<size_t>(n));
}

void transpose(Grid& g) {
    size_t w = 0;
    for (const Row& r : g) w = (std::max)(w, r.size());
    Grid out(w);
    for (size_t c = 0; c < w; ++c) {
        out[c].resize(g.size());
        for (size_t r = 0; r < g.size(); ++r) {
            if (c < g[r].size()) out[c][r] = std::move(g[r][c]);
        }
    }
    g.swap(out);
}

bool row_has_content(const Row& r) {
    for (const Cell& c : r) {
        if (!c.empty()) return true;
    }
    return false;
}

const Cell* cell_at(const Row& r, size_t i) {
    return i < r.size() ? &r[i] : nullptr;
}

// 헤더 행(첫 행)의 1..n 열을 시간축으로 읽는다.
// 서로 다른 해석이 섞이면 가장 많이 나온 해석을 쓰고, 그마저 60% 미만이면
// 샘플 번호로 물러선다. 값이 증가하지 않아도 샘플 번호로 물러선다.
LcTimeKind parse_time_axis(const Row& header, size_t n, std::vector<double>& times,
                           Dataset& ds) {
    times.assign(n, kNaN);
    std::vector<LcTimeKind> kinds(n, LC_TIME_INDEX);
    size_t counts[4] = {0, 0, 0, 0};

    for (size_t i = 0; i < n; ++i) {
        const Cell* c = cell_at(header, i + 1);
        if (!c || c->empty()) continue;
        double v = 0;
        LcTimeKind k = LC_TIME_INDEX;
        switch (c->kind) {
            case Cell::Kind::DateMs: v = c->num; k = LC_TIME_DATE_MS; break;
            case Cell::Kind::Number:
            case Cell::Kind::Bool:   v = c->num; k = LC_TIME_NUMBER; break;
            case Cell::Kind::Text:
                if (parse_clock_ms(c->text, v)) k = LC_TIME_CLOCK_MS;
                else if (parse_iso_ms(c->text, v)) k = LC_TIME_DATE_MS;
                else if (parse_number(c->text, v)) k = LC_TIME_NUMBER;
                break;
            default: break;
        }
        if (k == LC_TIME_INDEX) continue;
        times[i] = v;
        kinds[i] = k;
        counts[static_cast<int>(k)]++;
    }

    LcTimeKind best = LC_TIME_INDEX;
    size_t best_n = 0;
    for (int k = 1; k < 4; ++k) {
        if (counts[k] > best_n) { best_n = counts[k]; best = static_cast<LcTimeKind>(k); }
    }

    auto fall_back = [&](const wchar_t* why) {
        for (size_t i = 0; i < n; ++i) times[i] = static_cast<double>(i);
        ds.add_note(why);
        return LC_TIME_INDEX;
    };

    if (best == LC_TIME_INDEX || best_n * 5 < n * 3) {
        return fall_back(L"첫 행의 시간 값을 해석하지 못해 샘플 번호를 시간축으로 사용합니다.");
    }

    // 다른 해석으로 읽힌 칸과 빈 칸은 이웃 값으로 메운다.
    double last = 0.0;
    bool have_last = false;
    for (size_t i = 0; i < n; ++i) {
        if (kinds[i] == best && std::isfinite(times[i])) {
            last = times[i];
            have_last = true;
        } else {
            times[i] = have_last ? last : 0.0;
        }
    }
    for (size_t i = 1; i < n; ++i) {
        if (times[i] < times[i - 1]) {
            return fall_back(L"시간 값이 증가하지 않아 샘플 번호를 시간축으로 사용합니다.");
        }
    }
    return best;
}

// 값 묶음을 보고 채널 종류를 정한다.
LcChannelType classify(const Row& row, size_t n, bool& any_value) {
    bool numeric = true, digital = true;
    any_value = false;
    for (size_t i = 0; i < n; ++i) {
        const Cell* c = cell_at(row, i + 1);
        if (!c || c->empty()) continue;
        any_value = true;
        if (c->kind == Cell::Kind::Bool) continue;  // 0/1 로 읽힌다
        if (c->kind == Cell::Kind::Number || c->kind == Cell::Kind::DateMs) {
            if (c->num != 0.0 && c->num != 1.0) digital = false;
            continue;
        }
        double v = 0;
        if (parse_number(c->text, v)) {
            if (v != 0.0 && v != 1.0) digital = false;
            continue;
        }
        numeric = false;
        if (parse_digital(c->text) < 0) digital = false;
    }
    if (digital) return LC_CH_DIGITAL;
    if (numeric) return LC_CH_ANALOG;
    return LC_CH_STATE;
}


// ---------------------------------------------------------------------------
// 데이터 블록 찾기
//
// 로그 시트는 A1 부터 표가 시작하지 않는 경우가 많다. 장비 이름, 측정 일시,
// 설정값 같은 머리말이 앞에 붙고 실제 표는 시트 한참 아래·오른쪽에서 시작한다.
// 게다가 배치가 두 가지다 — 행이 채널이거나, 열이 채널이거나.
//
// 두 가지를 한 번에 정한다. 시간축은 "값이 줄지 않고 이어지는 가장 긴 구간"
// 이라는 성질이 있으므로, 모든 행과 모든 열에서 그런 구간을 찾아 가장 긴 쪽이
// 시간축이라고 본다. 시간축이 행이면 행이 채널, 열이면 열이 채널이다. 구간이
// 시작하는 위치가 곧 데이터가 시작하는 위치이고, 그 바로 앞이 이름 행/열이다.
// ---------------------------------------------------------------------------

// 후보로 훑어볼 앞쪽 행/열 수. 머리말이 이보다 길면 자동 판정을 포기한다.
constexpr size_t kScanLead = 256;
// 한 후보에서 시간축 구간을 얼마나 길게까지 세어 볼지. 어느 쪽이 더 긴지만
// 알면 되므로 무한정 셀 필요가 없다.
constexpr size_t kRunCap = 4096;
// 이만큼은 이어져야 시간축으로 인정한다.
constexpr uint32_t kMinRun = 3;

// 셀 하나를 시간 값으로 읽어 본다. Bool 은 제외한다 — 0/1 이 늘어선 디지털
// 채널을 시간축으로 오인하면 안 된다.
bool cell_as_time(const Cell& c, double& out) {
    switch (c.kind) {
        case Cell::Kind::DateMs:
        case Cell::Kind::Number:
            out = c.num;
            return true;
        case Cell::Kind::Text:
            return parse_clock_ms(c.text, out) || parse_iso_ms(c.text, out) ||
                   parse_number(c.text, out);
        default:
            return false;
    }
}

struct Run {
    size_t start = 0;
    uint32_t length = 0;
};

// 한 줄을 훑어 값이 줄지 않고 이어지는 가장 긴 시간 구간을 찾는다.
// at(i) 는 i 번째 셀을 주거나, 없으면 nullptr 을 준다.
template <class Fn>
Run longest_time_run(size_t count, Fn at) {
    Run best;
    size_t run_start = 0;
    uint32_t run_len = 0;
    double prev = 0.0;

    for (size_t i = 0; i < count; ++i) {
        const Cell* c = at(i);
        double v = 0.0;
        const bool ok = c && !c->empty() && cell_as_time(*c, v);
        if (ok && run_len > 0 && v < prev) {
            // 값이 줄었다. 시간축이 아니므로 여기서 끊고 이 자리에서 다시 센다.
            if (run_len > best.length) { best.start = run_start; best.length = run_len; }
            run_start = i;
            run_len = 1;
            prev = v;
            continue;
        }
        if (!ok) {
            if (run_len > best.length) { best.start = run_start; best.length = run_len; }
            run_len = 0;
            continue;
        }
        if (run_len == 0) run_start = i;
        prev = v;
        if (++run_len >= kRunCap) break;
    }
    if (run_len > best.length) { best.start = run_start; best.length = run_len; }
    return best;
}

struct Candidate {
    size_t line = 0;     // 후보가 된 행(또는 열) 번호
    size_t start = 0;    // 시간축이 시작하는 열(또는 행) 번호
    uint32_t length = 0;
    bool valid() const { return length >= kMinRun; }
};

// 여러 후보 중 하나를 고른다. 가장 긴 것의 90% 이상이면서 가장 앞선 것을 쓴다.
// 헤더 행 대신 뒤쪽의 단조 증가하는 데이터 행(예: 누적 카운터)이 뽑히는 것을
// 막기 위한 규칙이다.
Candidate pick(const std::vector<Candidate>& cands) {
    uint32_t best = 0;
    for (const Candidate& c : cands) best = (std::max)(best, c.length);
    if (best < kMinRun) return Candidate();
    const uint32_t floor_len = (std::max)(kMinRun, static_cast<uint32_t>(best * 9 / 10));
    for (const Candidate& c : cands) {
        if (c.length >= floor_len) return c;
    }
    return Candidate();
}

size_t grid_width(const Grid& g) {
    size_t w = 0;
    for (const Row& r : g) w = (std::max)(w, r.size());
    return w;
}

struct Layout {
    LcOrientation orientation = LC_ORIENT_ROWS;
    size_t row0 = 0;   // 데이터 블록의 첫 행 (이름 행 또는 헤더 행)
    size_t col0 = 0;   // 데이터 블록의 첫 열
    bool detected = false;
};

// 시간축을 못 찾았을 때 쓰는 대비책. 채워진 칸이 갑자기 많아지는 첫 행을 표의
// 시작으로 본다. 시간이 "T1", "T2" 같은 문자열이어도 머리말은 건너뛸 수 있다.
Layout dense_block_fallback(const Grid& g) {
    Layout lay;
    size_t widest = 0;
    for (const Row& r : g) {
        size_t filled = 0;
        for (const Cell& c : r) filled += c.empty() ? 0u : 1u;
        widest = (std::max)(widest, filled);
    }
    if (widest < 2) return lay;

    const size_t need = std::max<size_t>(2, widest / 2);
    for (size_t r = 0; r < g.size(); ++r) {
        size_t filled = 0;
        for (const Cell& c : g[r]) filled += c.empty() ? 0u : 1u;
        if (filled < need) continue;
        lay.row0 = r;
        // 이 행에서 처음 값이 있는 열을 표의 왼쪽 끝으로 본다.
        for (size_t c = 0; c < g[r].size(); ++c) {
            if (!g[r][c].empty()) { lay.col0 = c; break; }
        }
        return lay;
    }
    return lay;
}


// 이 배치를 택했을 때 채널 이름이 실제로 채워지는 비율.
//
// 길이만으로는 부족하다. 샘플이 몇 개 안 되는 시트에서는 데이터 줄이 우연히
// 증가 순서가 되어 시간축과 길이가 같아진다. 그때 잘못된 쪽을 택하면 이름 줄이
// 통째로 빈 칸이 되어 채널 이름이 전부 IO_1, IO_2 ... 로 나온다. 실제로 겪은
// 증상이라, 이름이 붙는지를 길이보다 먼저 본다.
double name_ratio_rows(const Grid& g, size_t header_row, size_t start_col) {
    if (start_col == 0) return 0.0;
    const size_t name_col = start_col - 1;
    size_t total = 0, named = 0;
    for (size_t r = header_row + 1; r < g.size(); ++r) {
        if (!row_has_content(g[r])) continue;
        ++total;
        const Cell* c = cell_at(g[r], name_col);
        if (c && !c->empty()) ++named;
    }
    return total ? static_cast<double>(named) / static_cast<double>(total) : 0.0;
}

double name_ratio_cols(const Grid& g, size_t width, size_t time_col, size_t start_row) {
    if (start_row == 0 || start_row - 1 >= g.size()) return 0.0;
    const Row& name_row = g[start_row - 1];
    size_t total = 0, named = 0;
    for (size_t c = time_col + 1; c < width; ++c) {
        // 그 열에 값이 하나라도 있어야 채널로 센다
        bool has_value = false;
        for (size_t r = start_row; r < g.size() && !has_value; ++r) {
            const Cell* cell = cell_at(g[r], c);
            has_value = cell && !cell->empty();
        }
        if (!has_value) continue;
        ++total;
        const Cell* n = cell_at(name_row, c);
        if (n && !n->empty()) ++named;
    }
    return total ? static_cast<double>(named) / static_cast<double>(total) : 0.0;
}

Layout detect_layout(const Grid& g, uint32_t requested) {
    Layout lay;
    const size_t width = grid_width(g);
    if (g.empty() || width < 2) return lay;

    const size_t row_lim = (std::min)(g.size(), kScanLead);
    const size_t col_lim = (std::min)(width, kScanLead);
    const size_t depth = (std::min)(g.size(), kRunCap + kScanLead);

    // 행 방향 후보: 각 행에서 가로로 이어지는 시간축
    std::vector<Candidate> by_row;
    for (size_t r = 0; r < row_lim; ++r) {
        const Row& row = g[r];
        const Run run = longest_time_run(row.size(), [&](size_t i) -> const Cell* {
            return i < row.size() ? &row[i] : nullptr;
        });
        // 이름 열이 왼쪽에 한 칸은 있어야 한다. 0 열부터 시작하는 구간이면
        // 한 칸 오른쪽으로 밀어서 본다.
        Candidate c;
        c.line = r;
        c.start = (run.start == 0) ? 1 : run.start;
        c.length = (run.start == 0) ? (run.length > 0 ? run.length - 1 : 0) : run.length;
        // 이 행을 헤더로 삼으면 아래에 채널이 최소 하나는 남아야 한다. 이 조건이
        // 없으면 마지막 행처럼 단조 증가하는 데이터 행이 헤더로 뽑혀서, 잘라낸
        // 뒤에 채널이 하나도 안 남는다.
        if (r + 1 >= g.size()) c.length = 0;
        by_row.push_back(c);
    }

    // 열 방향 후보: 각 열에서 세로로 이어지는 시간축
    std::vector<Candidate> by_col;
    for (size_t cx = 0; cx < col_lim; ++cx) {
        const Run run = longest_time_run(depth, [&](size_t i) -> const Cell* {
            if (i >= g.size()) return nullptr;
            const Row& row = g[i];
            return cx < row.size() ? &row[cx] : nullptr;
        });
        Candidate c;
        c.line = cx;
        c.start = (run.start == 0) ? 1 : run.start;
        c.length = (run.start == 0) ? (run.length > 0 ? run.length - 1 : 0) : run.length;
        // 마찬가지로 이 열을 시간축으로 삼으면 오른쪽에 채널이 남아야 한다.
        if (cx + 1 >= width) c.length = 0;
        by_col.push_back(c);
    }

    const Candidate row_pick = pick(by_row);
    const Candidate col_pick = pick(by_col);

    LcOrientation choice = LC_ORIENT_AUTO;
    if (requested == LC_ORIENT_ROWS) {
        choice = row_pick.valid() ? LC_ORIENT_ROWS : LC_ORIENT_AUTO;
    } else if (requested == LC_ORIENT_COLS) {
        choice = col_pick.valid() ? LC_ORIENT_COLS : LC_ORIENT_AUTO;
    } else if (row_pick.valid() || col_pick.valid()) {
        const double rows_named = row_pick.valid()
                                      ? name_ratio_rows(g, row_pick.line, row_pick.start)
                                      : -1.0;
        const double cols_named = col_pick.valid()
                                      ? name_ratio_cols(g, width, col_pick.line, col_pick.start)
                                      : -1.0;
        // 이름이 붙는 쪽을 먼저 본다. 한쪽만 이름이 제대로 채워지면 길이와
        // 무관하게 그쪽이다. 둘 다 비슷하면 시간축이 더 긴 쪽을 택한다.
        if (rows_named >= 0.5 && cols_named < 0.5) {
            choice = LC_ORIENT_ROWS;
        } else if (cols_named >= 0.5 && rows_named < 0.5) {
            choice = LC_ORIENT_COLS;
        } else {
            choice = (col_pick.length > row_pick.length) ? LC_ORIENT_COLS : LC_ORIENT_ROWS;
        }
    }

    if (choice == LC_ORIENT_ROWS) {
        lay.orientation = LC_ORIENT_ROWS;
        lay.row0 = row_pick.line;        // 헤더 행 (첫 칸은 이름 열 제목)
        lay.col0 = row_pick.start - 1;   // 이름 열
        lay.detected = true;
    } else if (choice == LC_ORIENT_COLS) {
        lay.orientation = LC_ORIENT_COLS;
        lay.row0 = col_pick.start - 1;   // 이름 행 (IO 이름이 가로로 늘어선 행)
        lay.col0 = col_pick.line;        // 시간 열
        lay.detected = true;
    } else {
        lay = dense_block_fallback(g);
        // 사용자가 배치를 지정했으면 그대로 따른다.
        lay.orientation = (requested == LC_ORIENT_COLS) ? LC_ORIENT_COLS : LC_ORIENT_ROWS;
    }
    return lay;
}

// 블록 시작 위치 앞쪽을 잘라낸다.
void crop(Grid& g, size_t row0, size_t col0) {
    if (row0 > 0) g.erase(g.begin(), g.begin() + static_cast<std::ptrdiff_t>((std::min)(row0, g.size())));
    if (col0 == 0) return;
    for (Row& r : g) {
        if (r.size() <= col0) { r.clear(); continue; }
        r.erase(r.begin(), r.begin() + static_cast<std::ptrdiff_t>(col0));
    }
}

// 0 기반 열 번호를 엑셀 열 이름으로. 2 -> "C"
std::wstring column_label(size_t col) {
    std::wstring out;
    size_t n = col + 1;
    while (n > 0) {
        const size_t rem = (n - 1) % 26;
        out.insert(out.begin(), static_cast<wchar_t>(L'A' + rem));
        n = (n - 1) / 26;
    }
    return out;
}

}  // namespace

Limits limits_from(const LcOpenOptions* opt) {
    Limits lim;
    if (!opt) return lim;
    if (opt->max_channels) lim.max_channels = opt->max_channels;
    if (opt->max_samples) lim.max_samples = opt->max_samples;
    if (opt->max_state_values) lim.max_state_values = opt->max_state_values;
    if (opt->max_cells) lim.max_cells = opt->max_cells;
    if (opt->max_uncompressed_bytes) lim.max_uncompressed_bytes = opt->max_uncompressed_bytes;
    return lim;
}


int32_t find_channel(const Dataset& ds, const std::wstring& name) {
    for (size_t i = 0; i < ds.channels.size(); ++i) {
        if (ds.channels[i].name == name) return static_cast<int32_t>(i);
    }
    const std::wstring want = fold_name(name);
    if (want.empty()) return -1;
    for (size_t i = 0; i < ds.channels.size(); ++i) {
        if (fold_name(ds.channels[i].name) == want) return static_cast<int32_t>(i);
    }
    return -1;
}

double sample_at(const Dataset& ds, uint32_t ch, double t) {
    if (ch >= ds.channels.size() || !std::isfinite(t)) return kNaN;
    const std::vector<double>& T = ds.times;
    const std::vector<double>& V = ds.channels[ch].values;
    if (T.empty() || V.size() != T.size()) return kNaN;
    if (t < T.front() || t > T.back()) return kNaN;

    // t 이하인 마지막 샘플. t 가 샘플 시각과 정확히 같으면 그 샘플이어야 한다
    // (lower_bound 를 쓰면 전환이 일어난 바로 그 순간에 직전 값을 돌려준다).
    const size_t i =
        static_cast<size_t>(std::upper_bound(T.begin(), T.end(), t) - T.begin()) - 1;

    const double v0 = V[i];
    // 디지털과 상태는 계단이다. 보간하면 없던 중간 값이 생긴다.
    if (ds.channels[ch].type != LC_CH_ANALOG) return v0;
    if (i + 1 >= T.size()) return v0;
    if (!std::isfinite(v0)) return kNaN;

    const double v1 = V[i + 1];
    if (!std::isfinite(v1)) return v0;
    const double dt = T[i + 1] - T[i];
    if (!(dt > 0.0)) return v0;
    return v0 + (v1 - v0) * (t - T[i]) / dt;
}

LcStatus build_dataset(Grid& grid, uint32_t orientation, const Limits& lim, Dataset& out) {
    if (orientation > LC_ORIENT_COLS) return LC_ERR_ARG;

    // 시트 앞쪽 머리말을 건너뛰고 실제 표가 어디서 시작하는지, 그리고 행이
    // 채널인지 열이 채널인지 정한다.
    const Layout lay = detect_layout(grid, orientation);
    out.orientation = lay.orientation;
    out.first_row = static_cast<uint32_t>(lay.row0 + 1);
    out.first_col = static_cast<uint32_t>(lay.col0 + 1);

    crop(grid, lay.row0, lay.col0);
    if (lay.orientation == LC_ORIENT_COLS) transpose(grid);

    if (lay.row0 > 0 || lay.col0 > 0) {
        out.add_note(L"데이터 표가 " + to_wstr(lay.row0 + 1) + L"행 " +
                     column_label(lay.col0) + L"열에서 시작하는 것으로 보고 그 앞은 건너뛰었습니다.");
    }
    if (orientation == LC_ORIENT_AUTO) {
        out.add_note(lay.orientation == LC_ORIENT_COLS
                         ? L"각 열이 IO 채널인 배치로 읽었습니다."
                         : L"각 행이 IO 채널인 배치로 읽었습니다.");
        if (!lay.detected) {
            out.add_note(L"시간축을 찾지 못해 배치를 확신할 수 없습니다. "
                         L"결과가 이상하면 배치를 직접 지정해 보세요.");
        }
    }

    grid.erase(std::remove_if(grid.begin(), grid.end(),
                              [](const Row& r) { return !row_has_content(r); }),
               grid.end());
    if (grid.size() < 2) return LC_ERR_NO_DATA;

    size_t width = 0;
    for (const Row& r : grid) width = (std::max)(width, r.size());
    // 값이 하나도 없는 뒤쪽 열은 잘라낸다 (엑셀이 남긴 빈 열 방지)
    while (width > 1) {
        bool empty = true;
        for (const Row& r : grid) {
            const Cell* c = cell_at(r, width - 1);
            if (c && !c->empty()) { empty = false; break; }
        }
        if (!empty) break;
        --width;
    }
    if (width < 2) return LC_ERR_NO_DATA;

    const size_t n = width - 1;
    if (n > lim.max_samples) return LC_ERR_TOO_LARGE;
    if (grid.size() - 1 > lim.max_channels) return LC_ERR_TOO_LARGE;

    out.times.clear();
    out.time_kind = parse_time_axis(grid[0], n, out.times, out);
    out.time_unit = (out.time_kind == LC_TIME_NUMBER && !grid[0].empty() &&
                     grid[0][0].kind == Cell::Kind::Text)
                        ? unit_from_label(grid[0][0].text)
                        : std::wstring();

    out.channels.clear();
    out.channels.reserve(grid.size() - 1);

    size_t skipped = 0, unnamed = 0;
    for (size_t r = 1; r < grid.size(); ++r) {
        const Row& row = grid[r];
        bool any_value = false;
        const LcChannelType type = classify(row, n, any_value);
        if (!any_value) { ++skipped; continue; }

        Channel ch;
        ch.type = type;
        const Cell* name_cell = cell_at(row, 0);
        if (name_cell && !name_cell->empty()) {
            ch.name = (name_cell->kind == Cell::Kind::Text)
                          ? trim(name_cell->text)
                          : std::wstring();
            if (ch.name.empty() && name_cell->kind != Cell::Kind::Text) {
                // 이름 칸이 숫자면 그대로 이름으로 쓴다
                wchar_t buf[64];
                const int len = swprintf(buf, 64, L"%g", name_cell->num);
                if (len > 0) ch.name.assign(buf, static_cast<size_t>(len));
            }
        }
        if (ch.name.empty()) {
            ch.name = L"IO_" + to_wstr(out.channels.size() + 1);
            ++unnamed;
        }

        ch.values.assign(n, kNaN);
        std::unordered_map<std::wstring, uint32_t> state_ids;
        double mn = std::numeric_limits<double>::infinity();
        double mx = -std::numeric_limits<double>::infinity();

        for (size_t i = 0; i < n; ++i) {
            const Cell* c = cell_at(row, i + 1);
            if (!c || c->empty()) continue;
            double v = kNaN;

            if (type == LC_CH_STATE) {
                std::wstring key = (c->kind == Cell::Kind::Text)
                                       ? trim(c->text)
                                       : std::wstring();
                if (key.empty() && c->kind != Cell::Kind::Text) {
                    wchar_t buf[64];
                    const int len = swprintf(buf, 64, L"%g", c->num);
                    key.assign(buf, len > 0 ? static_cast<size_t>(len) : 0);
                }
                if (key.empty()) continue;
                auto it = state_ids.find(key);
                if (it == state_ids.end()) {
                    if (ch.states.size() >= lim.max_state_values) return LC_ERR_TOO_LARGE;
                    const uint32_t id = static_cast<uint32_t>(ch.states.size());
                    ch.states.push_back(key);
                    it = state_ids.emplace(std::move(key), id).first;
                }
                v = static_cast<double>(it->second);
            } else if (type == LC_CH_DIGITAL) {
                if (c->kind == Cell::Kind::Text) {
                    const int d = parse_digital(c->text);
                    if (d < 0) {
                        double num = 0;
                        if (!parse_number(c->text, num)) continue;
                        v = (num != 0.0) ? 1.0 : 0.0;
                    } else {
                        v = static_cast<double>(d);
                    }
                } else {
                    v = (c->num != 0.0) ? 1.0 : 0.0;
                }
            } else {
                if (c->kind == Cell::Kind::Text) {
                    double num = 0;
                    if (!parse_number(c->text, num)) continue;
                    v = num;
                } else {
                    v = c->num;
                }
            }

            ch.values[i] = v;
            if (v < mn) mn = v;
            if (v > mx) mx = v;
        }

        if (!std::isfinite(mn)) { ++skipped; continue; }
        ch.min = mn;
        ch.max = mx;
        out.channels.push_back(std::move(ch));
    }

    if (out.channels.empty()) return LC_ERR_NO_DATA;
    if (skipped > 0) {
        out.add_note(L"값이 없는 행 " + to_wstr(skipped) + L"개를 건너뛰었습니다.");
    }
    if (unnamed > 0) {
        // 조용히 IO_n 을 붙이면, 원래 이름이 없는 것인지 배치를 잘못 읽은 것인지
        // 구분할 수가 없다. 숫자를 보여 주면 사용자가 판단할 수 있다.
        out.add_note(L"이름 칸이 비어 있어 채널 " + to_wstr(unnamed) +
                     L"개에 임시 이름(IO_n)을 붙였습니다. 배치가 맞는지 확인해 보세요.");
    }
    return LC_OK;
}

}  // namespace lc
