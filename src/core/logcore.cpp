// logcore.cpp — 공개 C ABI 구현.
//
// 경계 규칙: 이 파일 밖으로 나가는 것은 POD 와 데이터셋이 소유한 const 포인터뿐이다.
// 힙 소유권이 DLL 경계를 넘지 않으므로 EXE 와 DLL 의 CRT 가 달라도 안전하다.

#include "logcore/logcore.h"

#include <windows.h>
#include <shlwapi.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <new>
#include <vector>

#include "com_ptr.h"
#include "dataset.h"
#include "readers.h"
#include "text_util.h"

namespace {

using lc::Dataset;
using lc::Grid;
using lc::Limits;

const double kNaN = std::numeric_limits<double>::quiet_NaN();

// COM 을 이 호출 동안만 켠다. 호출자가 이미 다른 모드로 초기화했다면 그대로 쓴다.
struct ComScope {
    bool owned = false;
    ComScope() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        owned = SUCCEEDED(hr);
    }
    ~ComScope() { if (owned) CoUninitialize(); }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
};

enum class Format { Unknown, Delimited, Xlsx, OldXls };

Format format_from_path(const wchar_t* path) {
    if (!path) return Format::Unknown;
    const size_t n = wcsnlen(path, 32768);
    size_t dot = n;
    for (size_t i = n; i-- > 0;) {
        if (path[i] == L'.') { dot = i; break; }
        if (path[i] == L'\\' || path[i] == L'/') break;
    }
    if (dot >= n) return Format::Unknown;

    wchar_t ext[16] = {0};
    const size_t len = n - dot - 1;
    if (len == 0 || len >= 16) return Format::Unknown;
    for (size_t i = 0; i < len; ++i) {
        const wchar_t c = path[dot + 1 + i];
        ext[i] = (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c;
    }

    if (!wcscmp(ext, L"xlsx") || !wcscmp(ext, L"xlsm") || !wcscmp(ext, L"xltx")) return Format::Xlsx;
    if (!wcscmp(ext, L"xls")) return Format::OldXls;
    if (!wcscmp(ext, L"csv") || !wcscmp(ext, L"tsv") || !wcscmp(ext, L"txt") ||
        !wcscmp(ext, L"tab") || !wcscmp(ext, L"log")) {
        return Format::Delimited;
    }
    return Format::Unknown;
}

LcStatus read_whole_file(const wchar_t* path, uint64_t cap, std::vector<uint8_t>& out) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return LC_ERR_OPEN;

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart < 0) { CloseHandle(h); return LC_ERR_OPEN; }
    if (static_cast<uint64_t>(size.QuadPart) > cap) { CloseHandle(h); return LC_ERR_TOO_LARGE; }

    try {
        out.resize(static_cast<size_t>(size.QuadPart));
    } catch (const std::bad_alloc&) {
        CloseHandle(h);
        return LC_ERR_MEMORY;
    }

    size_t done = 0;
    while (done < out.size()) {
        const DWORD chunk = static_cast<DWORD>(
            (std::min)(static_cast<uint64_t>(out.size() - done), uint64_t{1} << 24));
        DWORD got = 0;
        if (!ReadFile(h, out.data() + done, chunk, &got, nullptr) || got == 0) {
            CloseHandle(h);
            return LC_ERR_OPEN;
        }
        done += got;
    }
    CloseHandle(h);
    return LC_OK;
}

LcStatus finish(Grid& grid, uint32_t orientation, const Limits& lim, LcDataset** out) {
    auto ds = std::unique_ptr<Dataset>(new (std::nothrow) Dataset());
    if (!ds) return LC_ERR_MEMORY;
    const LcStatus st = lc::build_dataset(grid, orientation, lim, *ds);
    if (st != LC_OK) return st;
    *out = reinterpret_cast<LcDataset*>(ds.release());
    return LC_OK;
}

const Dataset* cast(const LcDataset* ds) { return reinterpret_cast<const Dataset*>(ds); }

bool valid_channel(const Dataset* d, uint32_t ch) {
    return d != nullptr && ch < d->channels.size();
}

// times 에서 v 이상인 첫 인덱스.
size_t lower_index(const std::vector<double>& t, double v) {
    return static_cast<size_t>(std::lower_bound(t.begin(), t.end(), v) - t.begin());
}

}  // namespace

extern "C" {

void LC_CALL lc_default_options(LcOpenOptions* opt) {
    if (!opt) return;
    const Limits d;
    opt->struct_size = sizeof(LcOpenOptions);
    opt->orientation = LC_ORIENT_AUTO;
    opt->max_channels = d.max_channels;
    opt->max_samples = d.max_samples;
    opt->max_cells = d.max_cells;
    opt->max_uncompressed_bytes = d.max_uncompressed_bytes;
    opt->max_state_values = d.max_state_values;
    opt->reserved = 0;
}

LcStatus LC_CALL lc_open_file(const wchar_t* path, const LcOpenOptions* opt, LcDataset** out) {
    if (!path || !out) return LC_ERR_ARG;
    if (opt && opt->struct_size != sizeof(LcOpenOptions)) return LC_ERR_ARG;
    *out = nullptr;

    const Limits lim = lc::limits_from(opt);
    const uint32_t orientation = opt ? opt->orientation : 0u;
    if (orientation > LC_ORIENT_COLS) return LC_ERR_ARG;

    switch (format_from_path(path)) {
        case Format::Delimited: {
            std::vector<uint8_t> bytes;
            const LcStatus st = read_whole_file(path, lim.max_uncompressed_bytes, bytes);
            if (st != LC_OK) return st;
            Grid grid;
            const LcStatus rs = lc::read_delimited(bytes.data(), bytes.size(), lim, grid);
            if (rs != LC_OK) return rs;
            return finish(grid, orientation, lim, out);
        }
        case Format::Xlsx: {
            ComScope com;
            lc::ComPtr<IStream> stream;
            const HRESULT hr = SHCreateStreamOnFileEx(
                path, STGM_READ | STGM_SHARE_DENY_WRITE, FILE_ATTRIBUTE_NORMAL,
                FALSE, nullptr, stream.put());
            if (FAILED(hr)) return LC_ERR_OPEN;
            Grid grid;
            const LcStatus rs = lc::read_xlsx(stream.get(), lim, grid);
            if (rs != LC_OK) return rs;
            return finish(grid, orientation, lim, out);
        }
        case Format::OldXls:
            return LC_ERR_UNSUPPORTED;
        default:
            return LC_ERR_UNSUPPORTED;
    }
}

LcStatus LC_CALL lc_open_memory(const void* bytes, size_t size, const wchar_t* hint_name,
                                const LcOpenOptions* opt, LcDataset** out) {
    if (!bytes || size == 0 || !out) return LC_ERR_ARG;
    if (opt && opt->struct_size != sizeof(LcOpenOptions)) return LC_ERR_ARG;
    *out = nullptr;

    const Limits lim = lc::limits_from(opt);
    const uint32_t orientation = opt ? opt->orientation : 0u;
    if (orientation > LC_ORIENT_COLS) return LC_ERR_ARG;
    if (static_cast<uint64_t>(size) > lim.max_uncompressed_bytes) return LC_ERR_TOO_LARGE;

    const Format fmt = hint_name ? format_from_path(hint_name) : Format::Delimited;
    if (fmt == Format::Xlsx) {
        if (size > static_cast<size_t>(UINT_MAX)) return LC_ERR_TOO_LARGE;
        ComScope com;
        lc::ComPtr<IStream> stream;
        *stream.put() = SHCreateMemStream(static_cast<const BYTE*>(bytes),
                                          static_cast<UINT>(size));
        if (!stream) return LC_ERR_MEMORY;
        Grid grid;
        const LcStatus rs = lc::read_xlsx(stream.get(), lim, grid);
        if (rs != LC_OK) return rs;
        return finish(grid, orientation, lim, out);
    }
    if (fmt == Format::OldXls) return LC_ERR_UNSUPPORTED;

    Grid grid;
    const LcStatus rs =
        lc::read_delimited(static_cast<const uint8_t*>(bytes), size, lim, grid);
    if (rs != LC_OK) return rs;
    return finish(grid, orientation, lim, out);
}

void LC_CALL lc_close(LcDataset* ds) {
    delete reinterpret_cast<Dataset*>(ds);
}

const wchar_t* LC_CALL lc_status_text(LcStatus st) {
    switch (st) {
        case LC_OK:              return L"성공";
        case LC_ERR_ARG:         return L"잘못된 인자입니다.";
        case LC_ERR_OPEN:        return L"파일을 열 수 없습니다.";
        case LC_ERR_FORMAT:      return L"파일 형식을 해석하지 못했습니다.";
        case LC_ERR_NO_DATA:     return L"시간 행이나 IO 채널을 찾지 못했습니다. 배치 설정을 바꿔 보세요.";
        case LC_ERR_TOO_LARGE:   return L"파일이 허용 한도를 넘습니다.";
        case LC_ERR_MEMORY:      return L"메모리가 부족합니다.";
        case LC_ERR_UNSUPPORTED: return L"지원하지 않는 형식입니다. .xlsx 또는 CSV 로 저장해 주세요.";
        case LC_ERR_INTERNAL:    return L"내부 오류입니다.";
    }
    return L"알 수 없는 오류입니다.";
}

const wchar_t* LC_CALL lc_notes(const LcDataset* ds) {
    const Dataset* d = cast(ds);
    return d ? d->notes.c_str() : L"";
}

LcOrientation LC_CALL lc_orientation(const LcDataset* ds) {
    const Dataset* d = cast(ds);
    return d ? d->orientation : LC_ORIENT_ROWS;
}

uint32_t LC_CALL lc_data_first_row(const LcDataset* ds) {
    const Dataset* d = cast(ds);
    return d ? d->first_row : 1u;
}

uint32_t LC_CALL lc_data_first_column(const LcDataset* ds) {
    const Dataset* d = cast(ds);
    return d ? d->first_col : 1u;
}

uint32_t LC_CALL lc_sample_count(const LcDataset* ds) {
    const Dataset* d = cast(ds);
    return d ? static_cast<uint32_t>(d->times.size()) : 0u;
}

const double* LC_CALL lc_times(const LcDataset* ds) {
    const Dataset* d = cast(ds);
    return (d && !d->times.empty()) ? d->times.data() : nullptr;
}

LcTimeKind LC_CALL lc_time_kind(const LcDataset* ds) {
    const Dataset* d = cast(ds);
    return d ? d->time_kind : LC_TIME_INDEX;
}

const wchar_t* LC_CALL lc_time_unit(const LcDataset* ds) {
    const Dataset* d = cast(ds);
    return d ? d->time_unit.c_str() : L"";
}

int32_t LC_CALL lc_index_at(const LcDataset* ds, double t) {
    const Dataset* d = cast(ds);
    if (!d || d->times.empty()) return -1;
    const std::vector<double>& T = d->times;
    const size_t i = lower_index(T, t);
    if (i == 0) return 0;
    if (i >= T.size()) return static_cast<int32_t>(T.size() - 1);
    return static_cast<int32_t>((t - T[i - 1] <= T[i] - t) ? i - 1 : i);
}

uint32_t LC_CALL lc_channel_count(const LcDataset* ds) {
    const Dataset* d = cast(ds);
    return d ? static_cast<uint32_t>(d->channels.size()) : 0u;
}

const wchar_t* LC_CALL lc_channel_name(const LcDataset* ds, uint32_t ch) {
    const Dataset* d = cast(ds);
    return valid_channel(d, ch) ? d->channels[ch].name.c_str() : L"";
}

LcChannelType LC_CALL lc_channel_type(const LcDataset* ds, uint32_t ch) {
    const Dataset* d = cast(ds);
    return valid_channel(d, ch) ? d->channels[ch].type : LC_CH_ANALOG;
}

double LC_CALL lc_channel_min(const LcDataset* ds, uint32_t ch) {
    const Dataset* d = cast(ds);
    return valid_channel(d, ch) ? d->channels[ch].min : 0.0;
}

double LC_CALL lc_channel_max(const LcDataset* ds, uint32_t ch) {
    const Dataset* d = cast(ds);
    return valid_channel(d, ch) ? d->channels[ch].max : 0.0;
}

const double* LC_CALL lc_channel_values(const LcDataset* ds, uint32_t ch) {
    const Dataset* d = cast(ds);
    if (!valid_channel(d, ch) || d->channels[ch].values.empty()) return nullptr;
    return d->channels[ch].values.data();
}

uint32_t LC_CALL lc_state_count(const LcDataset* ds, uint32_t ch) {
    const Dataset* d = cast(ds);
    return valid_channel(d, ch) ? static_cast<uint32_t>(d->channels[ch].states.size()) : 0u;
}

const wchar_t* LC_CALL lc_state_name(const LcDataset* ds, uint32_t ch, uint32_t state) {
    const Dataset* d = cast(ds);
    if (!valid_channel(d, ch)) return L"";
    const lc::Channel& c = d->channels[ch];
    return state < c.states.size() ? c.states[state].c_str() : L"";
}

uint32_t LC_CALL lc_decimate(const LcDataset* ds, uint32_t ch, double t0, double t1,
                             uint32_t columns, double* out_min, double* out_max) {
    const Dataset* d = cast(ds);
    if (!valid_channel(d, ch) || !out_min || !out_max || columns == 0) return 0;
    for (uint32_t c = 0; c < columns; ++c) { out_min[c] = kNaN; out_max[c] = kNaN; }
    if (!(t1 > t0) || !std::isfinite(t0) || !std::isfinite(t1)) return columns;

    const std::vector<double>& T = d->times;
    const std::vector<double>& V = d->channels[ch].values;
    if (T.empty() || V.size() != T.size()) return columns;

    size_t i = lower_index(T, t0);
    if (i > 0) --i;  // 왼쪽 화면 밖 샘플 한 개를 포함해 선이 끊기지 않게 한다
    const double scale = static_cast<double>(columns) / (t1 - t0);

    for (; i < T.size(); ++i) {
        if (T[i] > t1) break;
        const double v = V[i];
        if (!std::isfinite(v)) continue;
        const double fc = (T[i] - t0) * scale;
        if (fc < 0.0 || fc >= static_cast<double>(columns)) continue;
        const uint32_t c = static_cast<uint32_t>(fc);
        if (!std::isfinite(out_min[c]) || v < out_min[c]) out_min[c] = v;
        if (!std::isfinite(out_max[c]) || v > out_max[c]) out_max[c] = v;
    }
    return columns;
}

int32_t LC_CALL lc_find_channel(const LcDataset* ds, const wchar_t* name) {
    const Dataset* d = cast(ds);
    if (!d || !name) return -1;
    return lc::find_channel(*d, name);
}

double LC_CALL lc_sample_at(const LcDataset* ds, uint32_t ch, double t) {
    const Dataset* d = cast(ds);
    if (!d) return kNaN;
    return lc::sample_at(*d, ch, t);
}

uint32_t LC_CALL lc_edge_count(const LcDataset* ds, uint32_t ch, double t0, double t1) {
    const Dataset* d = cast(ds);
    if (!valid_channel(d, ch)) return 0;
    if (t1 < t0) std::swap(t0, t1);

    const std::vector<double>& T = d->times;
    const std::vector<double>& V = d->channels[ch].values;
    if (T.empty() || V.size() != T.size()) return 0;

    size_t i = lower_index(T, t0);
    uint32_t edges = 0;
    bool have_prev = false;
    double prev = 0.0;
    for (; i < T.size() && T[i] <= t1; ++i) {
        const double v = V[i];
        if (!std::isfinite(v)) { have_prev = false; continue; }
        if (have_prev && v != prev) ++edges;
        prev = v;
        have_prev = true;
    }
    return edges;
}

}  // extern "C"

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(inst);
    return TRUE;
}
