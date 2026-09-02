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

LcStatus build_dataset(Grid& grid, uint32_t orientation, const Limits& lim, Dataset& out) {
    if (orientation == 1) transpose(grid);

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

    size_t skipped = 0;
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
        if (ch.name.empty()) ch.name = L"IO_" + to_wstr(out.channels.size() + 1);

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
    return LC_OK;
}

}  // namespace lc
