#include "app.h"

#include <shobjidl.h>
#include <windowsx.h>
#include <shellapi.h>
#include <imm.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace app {
namespace {

constexpr wchar_t kWindowClass[] = L"LogScopeWindow";
constexpr UINT kMsgLoadDone = WM_APP + 1;
constexpr UINT_PTR kTimerSpin = 1;
constexpr UINT_PTR kTimerCaret = 2;

// 파싱 스레드에서 불린다. 화면은 건드리지 않고 숫자만 남긴다.
int LC_CALL LoadProgress(void* user, unsigned long long done, unsigned long long total) {
    auto* job = static_cast<LoadJob*>(user);
    job->done.store(done, std::memory_order_relaxed);
    job->total.store(total, std::memory_order_relaxed);
    return job->cancel.load(std::memory_order_relaxed) ? 0 : 1;
}

// 시간축 눈금 후보 (밀리초). 시간 형식일 때 사람이 읽기 좋은 간격만 쓴다.
const double kTimeSteps[] = {1, 2, 5, 10, 20, 50, 100, 200, 500, 1e3, 2e3, 5e3,
                             1e4, 15e3, 3e4, 6e4, 12e4, 3e5, 6e5, 9e5, 18e5, 36e5};

// 선/도형 하나를 만드는 최소 도우미. D2D 는 폴리라인 API 가 없어서 경로를 만든다.
struct Path {
    Ptr<ID2D1PathGeometry> geo;
    Ptr<ID2D1GeometrySink> sink;
    bool figure = false;

    bool Begin(ID2D1Factory* f) {
        if (FAILED(f->CreatePathGeometry(geo.put()))) return false;
        return SUCCEEDED(geo->Open(sink.put()));
    }
    void Move(float x, float y) {
        if (figure) sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->BeginFigure(D2D1::Point2F(x, y), D2D1_FIGURE_BEGIN_HOLLOW);
        figure = true;
    }
    void MoveFilled(float x, float y) {
        if (figure) sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->BeginFigure(D2D1::Point2F(x, y), D2D1_FIGURE_BEGIN_FILLED);
        figure = true;
    }
    bool EndClosed() {
        if (figure) { sink->EndFigure(D2D1_FIGURE_END_CLOSED); figure = false; }
        return SUCCEEDED(sink->Close());
    }
    void Line(float x, float y) {
        if (figure) sink->AddLine(D2D1::Point2F(x, y));
    }
    bool End() {
        if (figure) { sink->EndFigure(D2D1_FIGURE_END_OPEN); figure = false; }
        return SUCCEEDED(sink->Close());
    }
};

float MeasureText(IDWriteFactory* dw, const std::wstring& s, IDWriteTextFormat* f) {
    if (!dw || !f || s.empty()) return 0.0f;
    Ptr<IDWriteTextLayout> layout;
    if (FAILED(dw->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), f,
                                    4096.0f, 64.0f, layout.put()))) {
        return 0.0f;
    }
    DWRITE_TEXT_METRICS m{};
    if (FAILED(layout->GetMetrics(&m))) return 0.0f;
    return m.widthIncludingTrailingWhitespace;
}

// 폭이 좁으면 뒤를 잘라 말줄임표를 붙인다.
std::wstring Ellipsize(IDWriteFactory* dw, std::wstring s, IDWriteTextFormat* f, float maxw) {
    if (MeasureText(dw, s, f) <= maxw) return s;
    while (s.size() > 1 && MeasureText(dw, s + L"…", f) > maxw) s.pop_back();
    return s + L"…";
}

float Px(float v) { return std::floor(v) + 0.5f; }

bool Inside(const D2D1_RECT_F& r, float x, float y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

D2D1_RECT_F Rect(float l, float t, float r, float b) { return D2D1::RectF(l, t, r, b); }

std::wstring Fmt(const wchar_t* fmt, ...) {
    wchar_t buf[512];
    va_list ap;
    va_start(ap, fmt);
    const int n = _vsnwprintf_s(buf, 512, _TRUNCATE, fmt, ap);
    va_end(ap);
    return std::wstring(buf, n > 0 ? static_cast<size_t>(n) : 0);
}

// 유효숫자를 적당히 맞춘 숫자 문자열.
std::wstring FormatNumber(double v) {
    if (!std::isfinite(v)) return L"—";
    const double a = std::fabs(v);
    if (a == 0.0) return L"0";
    if (a >= 1e6 || a < 1e-4) return Fmt(L"%.2e", v);
    if (a >= 100.0) return Fmt(L"%.1f", v);
    if (a >= 1.0) return Fmt(L"%.2f", v);
    return Fmt(L"%.3f", v);
}

struct Ticks {
    std::vector<double> at;
    double step = 1.0;
};

Ticks MakeTicks(double lo, double hi, int target, bool time_like) {
    Ticks t;
    const double span = hi - lo;
    if (!(span > 0.0) || target < 1) { t.at.push_back(lo); return t; }

    if (time_like) {
        const double want = span / target;
        t.step = kTimeSteps[sizeof(kTimeSteps) / sizeof(kTimeSteps[0]) - 1];
        for (double s : kTimeSteps) {
            if (s >= want) { t.step = s; break; }
        }
        if (want > t.step) t.step = std::ceil(want / 36e5) * 36e5;
    } else {
        const double raw = span / target;
        const double mag = std::pow(10.0, std::floor(std::log10(raw)));
        const double nm = raw / mag;
        t.step = (nm < 1.5 ? 1.0 : nm < 3.0 ? 2.0 : nm < 7.0 ? 5.0 : 10.0) * mag;
    }
    if (!(t.step > 0.0)) { t.at.push_back(lo); return t; }

    const double first = std::ceil(lo / t.step) * t.step;
    for (double v = first; v <= hi + t.step * 1e-9 && t.at.size() < 512; v += t.step) {
        t.at.push_back(v);
    }
    if (t.at.empty()) t.at.push_back(lo);
    return t;
}

// system32 에서만 DLL 을 찾아 올린다. 현재 디렉터리에 심어 둔 동명의 DLL 을
// 올리는 하이재킹을 막는다.
HMODULE LoadSystemLibrary(const wchar_t* name) {
    return LoadLibraryExW(name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
}

}  // namespace

// ===========================================================================
// 창 만들기 · 자원
// ===========================================================================

bool App::Create(HINSTANCE inst, int show, const wchar_t* initialPath,
                 const wchar_t* comparePath) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = &App::WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;  // 배경은 Direct2D 가 칠한다
    wc.lpszClassName = kWindowClass;
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    if (!RegisterClassExW(&wc)) return false;

    ApplySystemTheme();

    hwnd_ = CreateWindowExW(WS_EX_ACCEPTFILES, kWindowClass, L"IO Log Scope",
                            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                            1360, 860, nullptr, nullptr, inst, this);
    if (!hwnd_) return false;

    // 창별 DPI 를 읽는다 (Win10 1607+). 없으면 96 을 쓴다.
    if (HMODULE u32 = GetModuleHandleW(L"user32.dll")) {
        using PfnGetDpi = UINT(WINAPI*)(HWND);
        if (auto fn = reinterpret_cast<PfnGetDpi>(
                reinterpret_cast<void*>(GetProcAddress(u32, "GetDpiForWindow")))) {
            UpdateDpi(fn(hwnd_));
        }
    }

    // 제목 표시줄도 앱 테마를 따르게 한다 (Win10 1809+).
    if (HMODULE dwm = LoadSystemLibrary(L"dwmapi.dll")) {
        using PfnSet = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
        if (auto fn = reinterpret_cast<PfnSet>(
                reinterpret_cast<void*>(GetProcAddress(dwm, "DwmSetWindowAttribute")))) {
            const BOOL on = dark_ ? TRUE : FALSE;
            fn(hwnd_, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &on, sizeof(on));
        }
        FreeLibrary(dwm);
    }

    CreateTextFormats();

    ShowWindow(hwnd_, show);
    UpdateWindow(hwnd_);

    if (initialPath && *initialPath) LoadPath(initialPath);
    if (comparePath && *comparePath) LoadComparePath(comparePath);
    return true;
}

void App::ApplySystemTheme() {
    DWORD light = 1, size = sizeof(light);
    const LSTATUS st = RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    dark_ = (st == ERROR_SUCCESS) && (light == 0);
    pal_ = dark_ ? DarkPalette() : LightPalette();

}

void App::UpdateDpi(UINT dpi) {
    if (dpi < 48 || dpi > 480) return;
    dpi_ = static_cast<float>(dpi);
    fUi_.reset();
    CreateTextFormats();
}

bool App::CreateTextFormats() {
    if (!dw_) {
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                       reinterpret_cast<IUnknown**>(dw_.put())))) {
            return false;
        }
    }
    struct Spec { Ptr<IDWriteTextFormat>* slot; const wchar_t* family; float size;
                  DWRITE_TEXT_ALIGNMENT align; DWRITE_FONT_WEIGHT weight; };
    const Spec specs[] = {
        {&fUi_,        L"Segoe UI", 12.5f, DWRITE_TEXT_ALIGNMENT_LEADING,  DWRITE_FONT_WEIGHT_NORMAL},
        {&fUiCenter_,  L"Segoe UI", 12.5f, DWRITE_TEXT_ALIGNMENT_CENTER,   DWRITE_FONT_WEIGHT_NORMAL},
        {&fTitle_,     L"Segoe UI", 14.0f, DWRITE_TEXT_ALIGNMENT_LEADING,  DWRITE_FONT_WEIGHT_SEMI_BOLD},
        {&fMono_,      L"Consolas", 12.0f, DWRITE_TEXT_ALIGNMENT_LEADING,  DWRITE_FONT_WEIGHT_NORMAL},
        {&fMonoRight_, L"Consolas", 12.0f, DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_FONT_WEIGHT_NORMAL},
        {&fSmall_,     L"Consolas", 10.5f, DWRITE_TEXT_ALIGNMENT_LEADING,  DWRITE_FONT_WEIGHT_NORMAL},
        {&fSmallRight_,L"Consolas", 10.5f, DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_FONT_WEIGHT_NORMAL},
    };
    for (const Spec& s : specs) {
        if (FAILED(dw_->CreateTextFormat(s.family, nullptr, s.weight, DWRITE_FONT_STYLE_NORMAL,
                                         DWRITE_FONT_STRETCH_NORMAL, S(s.size), L"",
                                         s.slot->put()))) {
            return false;
        }
        (*s.slot)->SetTextAlignment(s.align);
        (*s.slot)->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        (*s.slot)->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    return true;
}

bool App::CreateDeviceResources() {
    if (rt_) return true;
    if (!d2d_) {
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.put()))) {
            return false;
        }
    }
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const D2D1_SIZE_U size = D2D1::SizeU(static_cast<UINT32>((std::max)(rc.right - rc.left, 1L)),
                                         static_cast<UINT32>((std::max)(rc.bottom - rc.top, 1L)));
    if (FAILED(d2d_->CreateHwndRenderTarget(D2D1::RenderTargetProperties(),
                                            D2D1::HwndRenderTargetProperties(hwnd_, size),
                                            rt_.put()))) {
        return false;
    }
    return SUCCEEDED(rt_->CreateSolidColorBrush(pal_.ink, brush_.put()));
}

void App::DiscardDeviceResources() {
    brush_.reset();
    rt_.reset();
}

// ===========================================================================
// 레이아웃
// ===========================================================================


// 왼쪽 목록 위쪽 머리 부분의 높이. 밴드를 아래로 차곡차곡 쌓아 계산한다.
// 예전에는 각 요소를 따로 계산해서 필터 칩과 개수 표시가 서로 겹쳤다.
//
//   [검색 라벨]  16
//   [검색 상자]  kSearchH
//   [칩 1행]     22        전체 / DIG / ANA / STATE
//   [칩 2행]     22        비교 중일 때만 — 달라진 것만
//   [선택 줄]    22        전체 선택 / 전체 해제 / 그룹 펴기·접기 / n개 선택
//
float App::RailHeaderHeight() const {
    float h = S(6.0f) + S(16.0f) + S(metrics::kSearchH) + S(8.0f);
    h += S(22.0f);                          // 칩 1행
    if (HasCompare()) h += S(26.0f);        // 칩 2행 (줄바꿈 여유 포함)
    h += S(6.0f) + S(22.0f) + S(8.0f);      // 선택 줄
    return h;
}

Rects App::CalcRects() const {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const float w = static_cast<float>(rc.right - rc.left);
    const float h = static_cast<float>(rc.bottom - rc.top);

    Rects r;
    r.toolbar = Rect(0, 0, w, S(metrics::kToolbarH));
    // 버튼이 늘어나 한 줄에 안 들어가므로 보기·확대 컨트롤은 아래 줄로 뺀다.
    r.controls = Rect(0, r.toolbar.bottom, w, r.toolbar.bottom + controlsH_);
    r.status = Rect(0, h - S(metrics::kStatusH), w, h);
    const float railW = (std::min)(S(metrics::kRailW), w * 0.42f);
    r.rail = Rect(0, r.controls.bottom, railW, r.status.top);
    r.axis = Rect(r.rail.right, r.status.top - S(metrics::kAxisH), w, r.status.top);
    r.plot = Rect(r.rail.right, r.controls.bottom, w, r.axis.top);

    r.railList = Rect(r.rail.left, r.rail.top + RailHeaderHeight(), r.rail.right,
                      r.rail.bottom);
    return r;
}

D2D1_RECT_F App::SearchRect(const Rects& r) const {
    const float pad = S(10.0f);
    const float top = r.rail.top + S(6.0f) + S(16.0f);
    return Rect(r.rail.left + pad, top, r.rail.right - pad, top + S(metrics::kSearchH));
}

void App::DrawSearchBox(const Rects& r) {
    const D2D1_RECT_F box = SearchRect(r);
    const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(box, S(4.0f), S(4.0f));
    brush_->SetColor(pal_.surface);
    rt_->FillRoundedRectangle(rr, brush_.get());
    brush_->SetColor(searchFocused_ ? pal_.accent : pal_.hair);
    rt_->DrawRoundedRectangle(rr, brush_.get(), searchFocused_ ? S(1.6f) : 1.0f);

    const float tx = box.left + S(8.0f);
    const D2D1_RECT_F textBox = Rect(tx, box.top, box.right - S(8.0f), box.bottom);
    if (query_.empty() && !searchFocused_) {
        DrawLabel(L"IO 이름으로 거르기", fUi_.get(), textBox, pal_.ink3);
        return;
    }
    DrawLabel(query_, fUi_.get(), textBox, pal_.ink);

    if (!searchFocused_) return;
    // 글자 커서. 0.5초 주기로 깜빡인다.
    if (((GetTickCount64() - caretTick_) / 530) % 2 == 0) {
        const std::wstring upto = query_.substr(0, (std::min)(caret_, query_.size()));
        const float cx = tx + MeasureText(dw_.get(), upto, fUi_.get());
        StrokeLine(Px(cx), box.top + S(4.0f), Px(cx), box.bottom - S(4.0f), pal_.ink, S(1.4f));
    }
}

// 한글은 IME 가 조합해서 WM_CHAR 로 완성된 글자를 보내 준다. 조합 중인 글자가
// 뜨는 위치만 글자 커서 옆으로 옮겨 준다.
void App::UpdateImePosition() {
    if (!searchFocused_) return;
    const Rects r = CalcRects();
    const D2D1_RECT_F box = SearchRect(r);
    const std::wstring upto = query_.substr(0, (std::min)(caret_, query_.size()));
    const float cx = box.left + S(8.0f) + MeasureText(dw_.get(), upto, fUi_.get());
    if (HIMC imc = ImmGetContext(hwnd_)) {
        COMPOSITIONFORM cf{};
        cf.dwStyle = CFS_POINT;
        cf.ptCurrentPos.x = static_cast<LONG>(cx);
        cf.ptCurrentPos.y = static_cast<LONG>(box.top);
        ImmSetCompositionWindow(imc, &cf);
        ImmReleaseContext(hwnd_, imc);
    }
}

void App::InsertSearchText(wchar_t c) {
    if (query_.size() >= 128) return;
    caret_ = (std::min)(caret_, query_.size());
    query_.insert(query_.begin() + static_cast<std::ptrdiff_t>(caret_), c);
    ++caret_;
    scrollRail_ = 0.0f;
    caretTick_ = GetTickCount64();
}

void App::OnSearchKey(WPARAM key) {
    caret_ = (std::min)(caret_, query_.size());
    switch (key) {
        case VK_LEFT:  if (caret_ > 0) --caret_; break;
        case VK_RIGHT: if (caret_ < query_.size()) ++caret_; break;
        case VK_HOME:  caret_ = 0; break;
        case VK_END:   caret_ = query_.size(); break;
        case VK_DELETE:
            if (caret_ < query_.size()) {
                query_.erase(query_.begin() + static_cast<std::ptrdiff_t>(caret_));
                scrollRail_ = 0.0f;
            }
            break;
        case VK_ESCAPE:
            if (query_.empty()) { searchFocused_ = false; }
            else { query_.clear(); caret_ = 0; scrollRail_ = 0.0f; }
            break;
        case VK_RETURN:
            searchFocused_ = false;
            break;
        default:
            return;
    }
    caretTick_ = GetTickCount64();
}

float App::LaneHeight(LcChannelType t) const {
    switch (t) {
        case LC_CH_DIGITAL: return S(metrics::kLaneDigital);
        case LC_CH_STATE:   return S(metrics::kLaneState);
        default:            return S(metrics::kLaneAnalog);
    }
}

float App::TotalLaneHeight() const {
    float total = 0.0f;
    for (uint32_t i = 0; i < selected_.size(); ++i) {
        if (selected_[i]) total += LaneHeight(lc_channel_type(ds_, i));
    }
    return total;
}

float App::TotalRailHeight() const {
    return static_cast<float>(railRows_.size()) * S(metrics::kRowH);
}

bool App::ChannelVisibleInList(uint32_t ch) const {
    if (!ds_) return false;
    // "달라진 것만" 은 값이 바뀐 채널과 이후 로그에서 사라진 채널을 모두 뜻한다.
    if (filter_ == -2) {
        const bool missing = ch >= matchB_.size() || matchB_[ch] < 0;
        if (!missing && !ChannelDiffers(ch)) return false;
    }
    if (filter_ >= 0 && static_cast<int>(lc_channel_type(ds_, ch)) != filter_) return false;
    if (query_.empty()) return true;
    std::wstring name = lc_channel_name(ds_, ch);
    std::wstring q = query_;
    std::transform(name.begin(), name.end(), name.begin(), ::towlower);
    std::transform(q.begin(), q.end(), q.begin(), ::towlower);
    return name.find(q) != std::wstring::npos;
}

// ===========================================================================
// 데이터
// ===========================================================================


// ===========================================================================
// 배경에서 파일 읽기
//
// lc_open_file 은 큰 xlsx 에서 몇 초가 걸린다. UI 스레드에서 부르면 그동안 창이
// 메시지를 처리하지 못해 Windows 가 "응답 없음" 으로 흐리게 만든다. 사용자
// 입장에서는 읽는 중인지 죽은 것인지 알 방법이 없다. 그래서 읽기는 따로 돌리고,
// 화면에는 경과 시간과 읽은 줄 수를 계속 갱신해서 보여 준다.
// ===========================================================================

void App::BeginLoad(const std::wstring& path, int slot) {
    if (IsLoading()) {
        // 이미 읽는 중이면 줄을 세운다. 그냥 버리면 파일 두 개를 한꺼번에
        // 떨어뜨렸을 때 두 번째가 조용히 사라진다.
        loadQueue_.emplace_back(path, slot);
        return;
    }
    if (slot == 1 && !ds_) {
        message_ = L"먼저 이전 로그를 여세요. 그 다음에 비교할 이후 로그를 엽니다.";
        messageIsError_ = true;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    auto job = std::make_shared<LoadJob>();
    job->path = path;
    job->orientation = orientation_;
    job->slot = slot;
    loadJob_ = job;
    loadStartTick_ = GetTickCount64();

    if (loadThread_.joinable()) loadThread_.join();
    HWND hwnd = hwnd_;
    loadThread_ = std::thread([job, hwnd]() {
        LcOpenOptions opt{};
        lc_default_options(&opt);
        opt.orientation = job->orientation;
        opt.progress = &LoadProgress;
        opt.progress_user = job.get();

        LcDataset* ds = nullptr;
        job->status = lc_open_file(job->path.c_str(), &opt, &ds);
        job->ds = ds;
        // shared_ptr 을 하나 더 띄워 UI 스레드가 받을 때까지 살려 둔다.
        auto* handoff = new std::shared_ptr<LoadJob>(job);
        if (!PostMessageW(hwnd, kMsgLoadDone, 0, reinterpret_cast<LPARAM>(handoff))) {
            // 창이 이미 없어졌다. 우리가 정리한다.
            if (ds) lc_close(ds);
            delete handoff;
        }
    });

    SetTimer(hwnd_, kTimerSpin, 80, nullptr);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::FinishLoad(const std::shared_ptr<LoadJob>& job) {
    KillTimer(hwnd_, kTimerSpin);
    loadJob_.reset();
    if (loadThread_.joinable()) loadThread_.join();

    // 대기열이 있으면 이번 것을 처리한 뒤 이어서 읽는다.
    auto start_queued = [this]() {
        if (loadQueue_.empty()) return;
        const auto next = loadQueue_.front();
        loadQueue_.erase(loadQueue_.begin());
        BeginLoad(next.first, next.second);
    };

    if (job->status != LC_OK || !job->ds) {
        if (job->ds) lc_close(job->ds);
        message_ = (job->slot == 1 ? std::wstring(L"이후 로그를 열지 못했습니다 — ")
                                   : std::wstring(L"로그를 열지 못했습니다 — ")) +
                   lc_status_text(job->status);
        messageIsError_ = true;
        loadQueue_.clear();   // 앞의 것이 실패했으면 뒤따르는 비교도 의미가 없다
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    const size_t slash = job->path.find_last_of(L"\\/");
    const std::wstring name =
        (slash == std::wstring::npos) ? job->path : job->path.substr(slash + 1);

    if (job->slot == 1) {
        const std::wstring keep = job->path;
        CloseCompare();
        dsB_ = job->ds;
        fileNameB_ = name;
        lastPathB_ = keep;
        RebuildComparison();
    } else {
        CloseCompare();
        CloseDataset();
        ds_ = job->ds;
        lastPath_ = job->path;
        fileName_ = name;

        const uint32_t n = lc_channel_count(ds_);
        // 아무것도 선택하지 않은 채로 시작한다. 200채널을 한꺼번에 그려 놓고
        // 지우는 것보다, 볼 것을 골라 담는 쪽이 빠르다.
        selected_.assign(n, false);
        anchorChannel_ = -1;
        groupOpen_.clear();
        ResetViewToData();

        message_ = lc_notes(ds_);
        if (!message_.empty()) message_ += L" ";
        message_ += Fmt(L"채널 %u개를 읽었습니다. 왼쪽에서 볼 IO 를 고르세요.", n);
        messageIsError_ = false;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
    start_queued();
}

void App::DrawLoadingOverlay(const Rects& r) {
    const float cx = (r.plot.left + r.plot.right) * 0.5f;
    const float cy = (r.plot.top + r.plot.bottom) * 0.5f;
    const float w = S(340.0f), h = S(104.0f);
    const D2D1_RECT_F box = Rect(cx - w * 0.5f, cy - h * 0.5f, cx + w * 0.5f, cy + h * 0.5f);

    Fill(r.plot, D2D1::ColorF(pal_.surface.r, pal_.surface.g, pal_.surface.b, 0.82f));
    const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(box, S(8.0f), S(8.0f));
    brush_->SetColor(pal_.panel);
    rt_->FillRoundedRectangle(rr, brush_.get());
    brush_->SetColor(pal_.hair);
    rt_->DrawRoundedRectangle(rr, brush_.get(), 1.0f);

    // 도는 표시. 이게 돌고 있으면 적어도 멈춘 것은 아니다.
    const unsigned long long ms = GetTickCount64() - loadStartTick_;
    const float angle = static_cast<float>(ms % 1000) / 1000.0f * 6.2831853f;
    const float rad = S(9.0f);
    const float sx = box.left + S(22.0f), sy = box.top + S(24.0f);
    for (int i = 0; i < 8; ++i) {
        const float a = angle + static_cast<float>(i) * 0.7853982f;
        const float alpha = 0.15f + 0.85f * (static_cast<float>(i) / 8.0f);
        const D2D1_RECT_F dot =
            Rect(sx + std::cos(a) * rad - S(1.6f), sy + std::sin(a) * rad - S(1.6f),
                 sx + std::cos(a) * rad + S(1.6f), sy + std::sin(a) * rad + S(1.6f));
        Fill(dot, D2D1::ColorF(pal_.accent.r, pal_.accent.g, pal_.accent.b, alpha));
    }

    const std::wstring name =
        loadJob_ ? loadJob_->path.substr(loadJob_->path.find_last_of(L"\\/") + 1)
                 : std::wstring();
    DrawLabel(Ellipsize(dw_.get(), name, fMono_.get(), w - S(60.0f)), fMono_.get(),
              Rect(box.left + S(42.0f), box.top + S(12.0f), box.right - S(14.0f),
                   box.top + S(36.0f)),
              pal_.ink);

    const unsigned long long rows = loadJob_ ? loadJob_->done.load() : 0;
    std::wstring detail = Fmt(L"%.1f초 경과", static_cast<double>(ms) / 1000.0);
    if (rows > 0) detail += Fmt(L" · %llu줄 읽음", rows);
    DrawLabel(detail, fSmall_.get(),
              Rect(box.left + S(42.0f), box.top + S(38.0f), box.right - S(14.0f),
                   box.top + S(58.0f)),
              pal_.ink2);
    DrawLabel(L"읽는 동안에도 창은 그대로 움직입니다.", fSmall_.get(),
              Rect(box.left + S(42.0f), box.top + S(58.0f), box.right - S(14.0f),
                   box.top + S(76.0f)),
              pal_.ink3);

    for (const Button& b : buttons_) {
        if (b.id == ButtonId::CancelLoad) DrawButton(b, hotButton_ == b.id);
    }
}

void App::CloseDataset() {
    if (ds_) { lc_close(ds_); ds_ = nullptr; }
    matchB_.clear();
    diffCount_.clear();
    selected_.clear();
    hasA_ = hasB_ = false;
    scrollPlot_ = scrollRail_ = 0.0f;
}

void App::LoadPath(const std::wstring& path) { BeginLoad(path, 0); }

void App::CloseCompare() {
    if (dsB_) { lc_close(dsB_); dsB_ = nullptr; }
    fileNameB_.clear();
    lastPathB_.clear();
    extraB_.clear();
    compareOffset_ = 0.0;
    matchB_.clear();
    diffCount_.clear();
    cmpLo_.clear(); cmpHi_.clear();
    diffLo_.clear(); diffHi_.clear();
    compareSummary_.clear();
    if (filter_ == -2) filter_ = -1;
}

std::wstring App::PickLogFile(const wchar_t* title) {
    Ptr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(dlg.put())))) {
        return std::wstring();
    }
    const COMDLG_FILTERSPEC filters[] = {
        {L"로그 파일 (*.xlsx;*.xlsm;*.csv;*.tsv;*.txt)", L"*.xlsx;*.xlsm;*.csv;*.tsv;*.txt"},
        {L"엑셀 통합 문서 (*.xlsx;*.xlsm)", L"*.xlsx;*.xlsm"},
        {L"구분 텍스트 (*.csv;*.tsv;*.txt)", L"*.csv;*.tsv;*.txt"},
        {L"모든 파일 (*.*)", L"*.*"},
    };
    dlg->SetFileTypes(ARRAYSIZE(filters), filters);
    dlg->SetTitle(title);
    if (FAILED(dlg->Show(hwnd_))) return std::wstring();

    Ptr<IShellItem> item;
    if (FAILED(dlg->GetResult(item.put()))) return std::wstring();
    PWSTR raw = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) || !raw) return std::wstring();
    std::wstring path = raw;
    CoTaskMemFree(raw);
    return path;
}

void App::OpenFileDialog() {
    const std::wstring path = PickLogFile(L"로그 파일 열기 (이전 로그)");
    if (!path.empty()) LoadPath(path);
}

void App::OpenCompareDialog() {
    if (!ds_) {
        message_ = L"먼저 이전 로그를 여세요. 그 다음에 비교할 이후 로그를 엽니다.";
        messageIsError_ = true;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    const std::wstring path = PickLogFile(L"비교할 이후 로그 열기");
    if (!path.empty()) LoadComparePath(path);
}

void App::LoadComparePath(const std::wstring& path) { BeginLoad(path, 1); }

// 이후 로그를 이전 로그의 시각 t 에서 읽는다.
//
// 두 로그의 시간값이 같은 기준일 거라고 가정하지 않는다. 절대 시각으로 찍힌
// 로그라면 측정한 날짜부터 다르다. 그래서 각자의 시작점을 0 으로 놓고, 시작
// 이후 경과 시간으로 맞춘다.
double App::CompareValueAt(uint32_t ch, double t) const {
    if (!dsB_ || ch >= matchB_.size() || matchB_[ch] < 0) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const double* ta = lc_times(ds_);
    const double* tb = lc_times(dsB_);
    if (!ta || !tb) return std::numeric_limits<double>::quiet_NaN();
    // compareOffset_ 은 사용자가 맞춘 보정값. 두 로그의 시작 시각이 정말로 다를 때
    // (트리거 시점이 어긋난 경우 등) 이걸로 밀어 맞춘다.
    return lc_sample_at(dsB_, static_cast<uint32_t>(matchB_[ch]),
                        (t - ta[0]) + tb[0] + compareOffset_);
}

double App::DiffValueAt(uint32_t ch, double t) const {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const int idx = IndexAt(t);
    if (idx < 0) return nan;
    const double* av = lc_channel_values(ds_, ch);
    if (!av) return nan;
    const double a = av[idx];
    const double b = CompareValueAt(ch, t);
    if (!std::isfinite(a) || !std::isfinite(b)) return nan;

    if (lc_channel_type(ds_, ch) == LC_CH_STATE) {
        // 상태 값은 로그마다 번호가 다르게 매겨진다. 이름으로 비교해야 한다.
        const std::wstring sa = lc_state_name(ds_, ch, static_cast<uint32_t>(a));
        const std::wstring sb =
            lc_state_name(dsB_, static_cast<uint32_t>(matchB_[ch]), static_cast<uint32_t>(b));
        return (sa == sb) ? 0.0 : 1.0;
    }
    return b - a;
}

bool App::ChannelDiffers(uint32_t ch) const {
    return ch < diffCount_.size() && diffCount_[ch] > 0;
}

void App::RebuildComparison() {
    const uint32_t n = ds_ ? lc_channel_count(ds_) : 0u;
    matchB_.assign(n, -1);
    diffCount_.assign(n, 0u);
    diffStats_.assign(n, DiffStats{});
    cmpLo_.assign(n, 0.0);
    cmpHi_.assign(n, 1.0);
    diffLo_.assign(n, 0.0);
    diffHi_.assign(n, 1.0);
    if (!dsB_ || n == 0) return;

    const double* ta = lc_times(ds_);
    const uint32_t samples = lc_sample_count(ds_);
    const double totalSpan = (samples > 1) ? (ta[samples - 1] - ta[0]) : 0.0;
    uint32_t matched = 0, changed = 0;

    for (uint32_t ch = 0; ch < n; ++ch) {
        const int32_t b = lc_find_channel(dsB_, lc_channel_name(ds_, ch));
        matchB_[ch] = b;
        cmpLo_[ch] = lc_channel_min(ds_, ch);
        cmpHi_[ch] = lc_channel_max(ds_, ch);
        if (b < 0) continue;
        ++matched;
        DiffStats& st = diffStats_[ch];
        st.matched = true;

        // 값 범위 대비 허용 오차. 아날로그는 반올림 차이가 늘 있으므로 그대로
        // 세면 전부 "달라짐" 이 된다. 디지털·상태는 정확히 비교한다.
        const bool analog = lc_channel_type(ds_, ch) == LC_CH_ANALOG;
        const double span = lc_channel_max(ds_, ch) - lc_channel_min(ds_, ch);
        const double tol = analog ? (std::max)(std::fabs(span) * tolerance_, 1e-12) : 0.0;

        double dlo = std::numeric_limits<double>::infinity();
        double dhi = -std::numeric_limits<double>::infinity();
        double sumAbs = 0.0, sumSq = 0.0, area = 0.0, diffTime = 0.0;
        uint32_t counted = 0;
        bool prevDiffer = false;
        const double* av = lc_channel_values(ds_, ch);

        for (uint32_t i = 0; i < samples; ++i) {
            const double bv = CompareValueAt(ch, ta[i]);
            if (std::isfinite(bv)) {
                cmpLo_[ch] = (std::min)(cmpLo_[ch], bv);
                cmpHi_[ch] = (std::max)(cmpHi_[ch], bv);
            }
            const double d = DiffValueAt(ch, ta[i]);
            if (!std::isfinite(d)) { prevDiffer = false; continue; }
            dlo = (std::min)(dlo, d);
            dhi = (std::max)(dhi, d);

            const double ad = std::fabs(d);
            ++counted;
            sumAbs += ad;
            sumSq += ad * ad;

            // 이 샘플이 대표하는 시간 폭 (사다리꼴처럼 앞뒤 절반씩)
            const double left = (i > 0) ? (ta[i] - ta[i - 1]) * 0.5 : 0.0;
            const double right = (i + 1 < samples) ? (ta[i + 1] - ta[i]) * 0.5 : 0.0;
            const double width = left + right;
            area += ad * width;

            const bool differ = ad > tol;
            if (differ) {
                ++st.samples;
                diffTime += width;
                if (!prevDiffer) ++st.runs;
                st.peak = (std::max)(st.peak, ad);
            }
            prevDiffer = differ;
            (void)av;
        }

        if (counted > 0) {
            st.mean = sumAbs / counted;
            st.rms = std::sqrt(sumSq / counted);
        }
        st.area = area;
        st.timeFrac = (totalSpan > 0.0) ? (diffTime / totalSpan) : 0.0;
        diffCount_[ch] = st.samples;

        if (std::isfinite(dlo) && std::isfinite(dhi)) {
            if (dhi <= dlo) { dlo -= 0.5; dhi += 0.5; }
            diffLo_[ch] = dlo;
            diffHi_[ch] = dhi;
        }
        if (st.samples > 0) ++changed;
    }

    // 이후 로그에만 있는 채널 (새로 생긴 IO). 이전 로그에 짝이 없어 그릴 수는
    // 없지만, 무엇이 새로 생겼는지는 두 로그를 견줄 때 꼭 알아야 한다.
    extraB_.clear();
    const uint32_t nb = lc_channel_count(dsB_);
    for (uint32_t bx = 0; bx < nb; ++bx) {
        if (lc_find_channel(ds_, lc_channel_name(dsB_, bx)) < 0) extraB_.push_back(bx);
    }
    const uint32_t removed = n - matched;

    compareSummary_ =
        Fmt(L"이후 로그 %s · 이름이 맞은 채널 %u/%u · 값이 달라진 채널 %u · "
            L"이전에만 있음 %u · 이후에만 있음 %u · 차이 기준 %s · 허용 오차 %.1f%%",
            fileNameB_.c_str(), matched, n, changed, removed,
            static_cast<uint32_t>(extraB_.size()), MetricName(), tolerance_ * 100.0);
    if (compareOffset_ != 0.0) {
        compareSummary_ += L" · 시간 보정 " + FormatSpan(std::fabs(compareOffset_)) +
                           (compareOffset_ < 0 ? L" 당김" : L" 밀음");
    }
    message_ = compareSummary_;
    if (matched == 0) {
        message_ = L"두 로그에서 이름이 같은 채널을 하나도 찾지 못했습니다. "
                   L"같은 양식의 로그가 맞는지, 배치 설정이 같은지 확인하세요.";
        messageIsError_ = true;
    } else {
        messageIsError_ = false;
    }
}

const wchar_t* App::MetricName() const {
    switch (metric_) {
        case DiffMetric::Samples:  return L"다른 샘플 수";
        case DiffMetric::TimeFrac: return L"다른 시간 비율";
        case DiffMetric::Peak:     return L"최대 차이";
        case DiffMetric::Mean:     return L"평균 차이";
        case DiffMetric::Rms:      return L"RMS 차이";
        case DiffMetric::Area:     return L"차이 면적";
        case DiffMetric::Runs:     return L"다른 구간 수";
    }
    return L"";
}

double App::MetricValue(uint32_t ch) const {
    if (ch >= diffStats_.size()) return 0.0;
    const DiffStats& st = diffStats_[ch];
    switch (metric_) {
        case DiffMetric::Samples:  return st.samples;
        case DiffMetric::TimeFrac: return st.timeFrac;
        case DiffMetric::Peak:     return st.peak;
        case DiffMetric::Mean:     return st.mean;
        case DiffMetric::Rms:      return st.rms;
        case DiffMetric::Area:     return st.area;
        case DiffMetric::Runs:     return st.runs;
    }
    return 0.0;
}

std::wstring App::MetricBadge(uint32_t ch) const {
    if (ch >= diffStats_.size() || !diffStats_[ch].matched) return L"삭제";
    const DiffStats& st = diffStats_[ch];
    if (st.samples == 0) return L"동일";
    switch (metric_) {
        case DiffMetric::Samples:  return Fmt(L"Δ%u", st.samples);
        case DiffMetric::TimeFrac: return Fmt(L"%.1f%%", st.timeFrac * 100.0);
        case DiffMetric::Peak:     return L"↕" + FormatNumber(st.peak);
        case DiffMetric::Mean:     return L"평균 " + FormatNumber(st.mean);
        case DiffMetric::Rms:      return L"RMS " + FormatNumber(st.rms);
        case DiffMetric::Area:     return L"∫ " + FormatNumber(st.area);
        case DiffMetric::Runs:     return Fmt(L"%u구간", st.runs);
    }
    return L"";
}


// ===========================================================================
// 왼쪽 목록 — 10개씩 묶은 그룹, Shift 범위 선택
// ===========================================================================

void App::GroupCounts(uint32_t group, uint32_t& visible, uint32_t& selected) const {
    visible = 0;
    selected = 0;
    if (!ds_) return;
    const uint32_t n = lc_channel_count(ds_);
    const uint32_t first = group * kGroupSize;
    for (uint32_t ch = first; ch < (std::min)(first + kGroupSize, n); ++ch) {
        if (!ChannelVisibleInList(ch)) continue;
        ++visible;
        if (selected_[ch]) ++selected;
    }
}

void App::ToggleGroup(uint32_t group) {
    if (group < groupOpen_.size()) groupOpen_[group] = !groupOpen_[group];
}

void App::SetGroupSelected(uint32_t group, bool on) {
    if (!ds_) return;
    const uint32_t n = lc_channel_count(ds_);
    const uint32_t first = group * kGroupSize;
    for (uint32_t ch = first; ch < (std::min)(first + kGroupSize, n); ++ch) {
        if (ChannelVisibleInList(ch)) selected_[ch] = on;
    }
}

// 목록에 실제로 그릴 줄들을 만든다. 그룹 머리, 펼쳐진 그룹의 채널, 그리고
// 마지막에 "이후 로그에만 있는" 채널 목록.
void App::RebuildRailRows() {
    railRows_.clear();
    if (!ds_) return;

    const uint32_t n = lc_channel_count(ds_);
    const uint32_t groups = (n + kGroupSize - 1) / kGroupSize;
    if (groupOpen_.size() != groups) groupOpen_.assign(groups, true);

    for (uint32_t g = 0; g < groups; ++g) {
        uint32_t visible = 0, selected = 0;
        GroupCounts(g, visible, selected);
        if (visible == 0) continue;   // 필터에 걸리는 채널이 없는 그룹은 숨긴다
        railRows_.push_back({RailRow::Kind::Group, g});
        if (!groupOpen_[g]) continue;
        const uint32_t first = g * kGroupSize;
        for (uint32_t ch = first; ch < (std::min)(first + kGroupSize, n); ++ch) {
            if (ChannelVisibleInList(ch)) railRows_.push_back({RailRow::Kind::Channel, ch});
        }
    }

    // 이후 로그에만 있는 채널. 이전 로그에 짝이 없으니 그릴 수는 없지만,
    // "무엇이 새로 생겼는지" 는 두 로그를 견줄 때 꼭 알아야 하는 정보다.
    if (!extraB_.empty()) {
        railRows_.push_back({RailRow::Kind::ExtraHeader, 0});
        for (uint32_t b : extraB_) railRows_.push_back({RailRow::Kind::ExtraChannel, b});
    }
}

void App::ClickChannel(uint32_t ch, bool shift, bool ctrl, bool onCheckbox) {
    if (!ds_ || ch >= selected_.size()) return;

    if (shift && anchorChannel_ >= 0) {
        // 기준점부터 여기까지 한 번에 켠다. 목록에 보이는 것만 대상으로 한다.
        const uint32_t a = static_cast<uint32_t>(anchorChannel_);
        const uint32_t lo = (std::min)(a, ch);
        const uint32_t hi = (std::max)(a, ch);
        for (uint32_t i = lo; i <= hi && i < selected_.size(); ++i) {
            if (ChannelVisibleInList(i)) selected_[i] = true;
        }
        return;
    }
    // 클릭은 그 채널만 켜고 끈다. 이미 고른 것들은 건드리지 않는다 — 하나를
    // 더 보려고 눌렀는데 보던 것이 사라지면 쓸 수가 없다. 끄고 싶으면 그 채널을
    // 다시 누르면 된다.
    (void)onCheckbox;
    (void)ctrl;
    selected_[ch] = !selected_[ch];
    anchorChannel_ = static_cast<int32_t>(ch);
}

// ===========================================================================
// 두 로그 시간 맞추기
// ===========================================================================

void App::ResetAlign() {
    compareOffset_ = 0.0;
    if (dsB_) RebuildComparison();
}

// 표본 간격 단위로 민다.
void App::NudgeAlign(int steps) {
    if (!ds_ || !dsB_) return;
    const uint32_t n = lc_sample_count(ds_);
    const double* t = lc_times(ds_);
    if (!t || n < 2) return;
    const double dt = (t[n - 1] - t[0]) / static_cast<double>(n - 1);
    compareOffset_ += dt * steps;
    RebuildComparison();
}

// 이후 로그를 조금씩 밀어 보면서 차이가 가장 작아지는 위치를 찾는다.
//
// 두 로그의 트리거 시점이 어긋나면 시작점만 맞춰서는 겹치지 않는다. 값이 가장
// 잘 맞는 지점을 직접 찾는 편이 확실하다. 전 구간을 다 보면 느리므로 표본을
// 솎아서 훑고, 대강 찾은 뒤 그 주변을 다시 촘촘히 본다.
void App::AutoAlignCompare() {
    if (!ds_ || !dsB_) return;
    const uint32_t n = lc_sample_count(ds_);
    const double* t = lc_times(ds_);
    if (!t || n < 4) return;

    std::vector<uint32_t> chans;
    for (uint32_t ch = 0; ch < lc_channel_count(ds_) && chans.size() < 12; ++ch) {
        if (ch < matchB_.size() && matchB_[ch] >= 0) chans.push_back(ch);
    }
    if (chans.empty()) {
        message_ = L"이름이 맞는 채널이 없어 정렬할 수 없습니다.";
        messageIsError_ = true;
        return;
    }

    const double span = t[n - 1] - t[0];
    const double dt = span / static_cast<double>(n - 1);
    const uint32_t stride = (std::max)(1u, n / 400u);   // 400점 정도만 훑는다

    // 채널마다 값 크기가 달라서, 자기 범위로 나눠 정규화한 뒤 더한다.
    auto cost = [&](double offset) {
        const double saved = compareOffset_;
        compareOffset_ = offset;
        double total = 0.0;
        uint32_t counted = 0;
        for (uint32_t ch : chans) {
            const double lo = lc_channel_min(ds_, ch);
            const double hi = lc_channel_max(ds_, ch);
            const double scale = (hi > lo) ? (hi - lo) : 1.0;
            const double* v = lc_channel_values(ds_, ch);
            if (!v) continue;
            for (uint32_t i = 0; i < n; i += stride) {
                const double b = CompareValueAt(ch, t[i]);
                if (!std::isfinite(b) || !std::isfinite(v[i])) continue;
                total += std::fabs(b - v[i]) / scale;
                ++counted;
            }
        }
        compareOffset_ = saved;
        // 겹치는 구간이 너무 적으면 억지로 맞춘 것이므로 점수를 나쁘게 준다.
        if (counted < 8) return std::numeric_limits<double>::infinity();
        return total / static_cast<double>(counted);
    };

    // 1단계: 전체 구간의 ±40% 를 성기게 훑는다
    const double range = span * 0.4;
    double best = 0.0;
    double bestCost = std::numeric_limits<double>::infinity();
    const int coarse = 80;
    for (int k = -coarse; k <= coarse; ++k) {
        const double off = range * k / coarse;
        const double c = cost(off);
        if (c < bestCost) { bestCost = c; best = off; }
    }
    // 2단계: 찾은 곳 주변을 한 표본 간격 단위로 다시 본다
    for (int k = -30; k <= 30; ++k) {
        const double off = best + dt * k;
        const double c = cost(off);
        if (c < bestCost) { bestCost = c; best = off; }
    }

    if (!std::isfinite(bestCost)) {
        message_ = L"겹치는 구간을 찾지 못했습니다. 두 로그가 같은 구간을 담고 있는지 확인하세요.";
        messageIsError_ = true;
        return;
    }
    compareOffset_ = best;
    RebuildComparison();
    message_ = compareSummary_ + Fmt(L" · 자동 정렬 %s", FormatSpan(std::fabs(best)).c_str()) +
               (best < 0 ? L" 당김" : L" 밀음");
    messageIsError_ = false;
}

void App::DrawDifferenceBand(uint32_t ch, const D2D1_RECT_F& plot, float top, float bottom,
                             double lo, double hi, const D2D1_COLOR_F& color) {
    if (!dsB_ || !(hi > lo)) return;
    const double* t = lc_times(ds_);
    const double* av = lc_channel_values(ds_, ch);
    if (!t || !av) return;
    int i0 = 0, i1 = 0;
    IndexRange(i0, i1);
    if (i0 < 0 || i1 < i0) return;

    auto yOf = [&](double val) {
        return bottom - static_cast<float>((val - lo) / (hi - lo)) * (bottom - top);
    };
    const bool step = lc_channel_type(ds_, ch) != LC_CH_ANALOG;

    // 두 값이 모두 있는 구간을 이어서 하나의 닫힌 도형으로 만든다.
    // 위쪽은 이전 로그를 따라가고, 아래쪽은 이후 로그를 거꾸로 되짚는다.
    std::vector<D2D1_POINT_2F> fwd, back;
    Path p;
    if (!p.Begin(d2d_.get())) return;
    bool any = false;

    auto flush = [&]() {
        if (fwd.size() < 2) { fwd.clear(); back.clear(); return; }
        p.MoveFilled(fwd[0].x, fwd[0].y);
        for (size_t k = 1; k < fwd.size(); ++k) p.Line(fwd[k].x, fwd[k].y);
        for (size_t k = back.size(); k-- > 0;) p.Line(back[k].x, back[k].y);
        any = true;
        fwd.clear();
        back.clear();
    };

    float lastA = 0.0f, lastB = 0.0f;
    bool open = false;
    for (int i = i0; i <= i1; ++i) {
        const double a = av[i];
        const double b = CompareValueAt(ch, t[i]);
        if (!std::isfinite(a) || !std::isfinite(b)) { flush(); open = false; continue; }
        const float x = XOfTime(t[i], plot);
        const float ya = yOf(a), yb = yOf(b);
        if (open && step) {
            fwd.push_back(D2D1::Point2F(x, lastA));
            back.push_back(D2D1::Point2F(x, lastB));
        }
        fwd.push_back(D2D1::Point2F(x, ya));
        back.push_back(D2D1::Point2F(x, yb));
        lastA = ya;
        lastB = yb;
        open = true;
    }
    flush();
    if (!p.EndClosed() || !any) return;

    brush_->SetColor(D2D1::ColorF(color.r, color.g, color.b, 0.22f));
    rt_->FillGeometry(p.geo.get(), brush_.get());
}

void App::DrawResampled(uint32_t ch, const D2D1_RECT_F& plot, float top, float bottom,
                        double lo, double hi, const D2D1_COLOR_F& color, bool diff,
                        float thickness) {
    if (!dsB_ || !(hi > lo)) return;
    const double* t = lc_times(ds_);
    if (!t) return;
    int i0 = 0, i1 = 0;
    IndexRange(i0, i1);
    if (i0 < 0 || i1 < i0) return;

    auto yOf = [&](double val) {
        return bottom - static_cast<float>((val - lo) / (hi - lo)) * (bottom - top);
    };
    const bool step = lc_channel_type(ds_, ch) != LC_CH_ANALOG;

    Path p;
    if (!p.Begin(d2d_.get())) return;
    bool pen = false;
    float lastY = 0.0f;
    for (int i = i0; i <= i1; ++i) {
        const double v = diff ? DiffValueAt(ch, t[i]) : CompareValueAt(ch, t[i]);
        if (!std::isfinite(v)) { pen = false; continue; }
        const float x = XOfTime(t[i], plot);
        const float y = yOf(v);
        if (!pen) { p.Move(x, y); pen = true; }
        else if (step) { p.Line(x, lastY); p.Line(x, y); }
        else { p.Line(x, y); }
        lastY = y;
    }
    if (!p.End()) return;
    brush_->SetColor(color);
    rt_->DrawGeometry(p.geo.get(), brush_.get(), thickness);
}

void App::ResetViewToData() {
    if (!ds_ || lc_sample_count(ds_) == 0) { t0_ = 0.0; t1_ = 1.0; return; }
    const double* t = lc_times(ds_);
    const uint32_t n = lc_sample_count(ds_);
    t0_ = t[0];
    t1_ = (n > 1 && t[n - 1] > t[0]) ? t[n - 1] : t[0] + 1.0;
}

void App::ClampView(double a, double b) {
    if (!ds_ || lc_sample_count(ds_) == 0) return;
    const double* t = lc_times(ds_);
    const uint32_t n = lc_sample_count(ds_);
    const double lo = t[0];
    const double hi = (n > 1 && t[n - 1] > t[0]) ? t[n - 1] : t[0] + 1.0;
    const double full = hi - lo;
    const double minSpan = full / 20000.0;

    if (b - a < minSpan) {
        const double mid = (a + b) * 0.5;
        a = mid - minSpan * 0.5;
        b = mid + minSpan * 0.5;
    }
    if (b - a > full) { a = lo; b = hi; }
    if (a < lo) { b += lo - a; a = lo; }
    if (b > hi) { a -= b - hi; b = hi; }
    if (a < lo) a = lo;
    t0_ = a;
    t1_ = b;
}

// 버튼으로 확대할 때의 기준점. 화면 가운데를 고정하고 양옆을 당긴다.
float App::ZoomAnchorX() const {
    const Rects r = CalcRects();
    float left = 0.0f, w = 10.0f;
    PlotSpan(r.plot, left, w);
    return left + w * 0.5f;
}

void App::ZoomAt(float clientX, double factor) {
    const Rects r = CalcRects();
    float left = 0.0f, w = 10.0f;
    PlotSpan(r.plot, left, w);
    const float x = (std::min)((std::max)(clientX, left), left + w);
    const double anchor = TimeOfX(x, r.plot);
    ClampView(anchor - (anchor - t0_) * factor, anchor + (t1_ - anchor) * factor);
}

void App::IndexRange(int& i0, int& i1) const {
    i0 = i1 = -1;
    if (!ds_) return;
    const uint32_t n = lc_sample_count(ds_);
    if (n == 0) return;
    const double* t = lc_times(ds_);
    const double* lo = std::lower_bound(t, t + n, t0_);
    const double* hi = std::lower_bound(t, t + n, t1_);
    i0 = (std::max)(0, static_cast<int>(lo - t) - 1);
    i1 = (std::min)(static_cast<int>(n) - 1, static_cast<int>(hi - t) + 1);
}

int App::IndexAt(double t) const { return ds_ ? lc_index_at(ds_, t) : -1; }

// ===========================================================================
// 좌표와 문자열
// ===========================================================================

// 두 모드가 가로 여백이 다르다. 레인은 왼쪽에 이름, 오른쪽에 값 거터를 두고,
// 겹쳐보기는 왼쪽에 세로 눈금만 둔다. 시간 <-> 화면 좌표 변환을 한곳에 모은다.
void App::PlotSpan(const D2D1_RECT_F& plot, float& left, float& width) const {
    if (mode_ == PlotMode::Overlay) {
        left = plot.left + S(metrics::kOverlayAxisW);
        width = (std::max)(plot.right - S(12.0f) - left, 10.0f);
    } else {
        left = plot.left + S(metrics::kNameGutter);
        width = (std::max)(plot.right - S(metrics::kValueGutter) - left, 10.0f);
    }
}

float App::XOfTime(double t, const D2D1_RECT_F& plot) const {
    float left = 0.0f, w = 10.0f;
    PlotSpan(plot, left, w);
    if (!(t1_ > t0_)) return left;
    return left + static_cast<float>((t - t0_) / (t1_ - t0_)) * w;
}

double App::TimeOfX(float x, const D2D1_RECT_F& plot) const {
    float left = 0.0f, w = 10.0f;
    PlotSpan(plot, left, w);
    return t0_ + static_cast<double>((x - left) / w) * (t1_ - t0_);
}

std::wstring App::FormatTime(double t, double step) const {
    const LcTimeKind kind = ds_ ? lc_time_kind(ds_) : LC_TIME_INDEX;
    if (kind == LC_TIME_CLOCK_MS || kind == LC_TIME_DATE_MS) {
        double ms = t;
        if (kind == LC_TIME_DATE_MS) {
            // 하루 안의 시각만 보여 준다. 로그를 읽을 때는 그것으로 충분하다.
            ms = std::fmod(t, 86400000.0);
            if (ms < 0) ms += 86400000.0;
        }
        const long long total = static_cast<long long>(std::llround(ms));
        const long long h = total / 3600000LL;
        const long long m = (total % 3600000LL) / 60000LL;
        const long long s = (total % 60000LL) / 1000LL;
        const long long f = total % 1000LL;
        if (step > 0.0 && step < 1000.0) return Fmt(L"%lld:%02lld:%02lld.%03lld", h, m, s, f);
        return Fmt(L"%lld:%02lld:%02lld", h, m, s);
    }
    if (kind == LC_TIME_INDEX) return Fmt(L"%.0f", t);

    int digits = 3;
    if (step > 0.0) {
        digits = static_cast<int>(-std::floor(std::log10(step))) + 1;
        digits = (std::min)((std::max)(digits, 0), 6);
    }
    std::wstring s = Fmt(L"%.*f", digits, t);
    const std::wstring unit = ds_ ? lc_time_unit(ds_) : L"";
    if (!unit.empty()) { s += L" "; s += unit; }
    return s;
}

std::wstring App::FormatSpan(double span) const {
    const LcTimeKind kind = ds_ ? lc_time_kind(ds_) : LC_TIME_INDEX;
    if (kind == LC_TIME_CLOCK_MS || kind == LC_TIME_DATE_MS) {
        if (span < 1000.0) return Fmt(L"%.0f ms", span);
        if (span < 60000.0) return Fmt(L"%.3f s", span / 1000.0);
        return Fmt(L"%.0f분 %.0f초", std::floor(span / 60000.0),
                   std::fmod(span, 60000.0) / 1000.0);
    }
    if (kind == LC_TIME_INDEX) return Fmt(L"%.0f 샘플", span);
    const std::wstring unit = ds_ ? lc_time_unit(ds_) : L"";
    return FormatNumber(span) + (unit.empty() ? L"" : L" " + unit);
}

std::wstring App::FormatValue(uint32_t ch, double v) const {
    if (!ds_ || !std::isfinite(v)) return L"—";
    switch (lc_channel_type(ds_, ch)) {
        case LC_CH_DIGITAL: return (v != 0.0) ? L"1" : L"0";
        case LC_CH_STATE: {
            const uint32_t idx = static_cast<uint32_t>(v);
            return idx < lc_state_count(ds_, ch) ? lc_state_name(ds_, ch, idx) : L"—";
        }
        default: return FormatNumber(v);
    }
}

}  // namespace app

// ===========================================================================
// 그리기
// ===========================================================================

namespace app {
namespace {

}  // namespace

void App::Fill(const D2D1_RECT_F& r, const D2D1_COLOR_F& c) {
    brush_->SetColor(c);
    rt_->FillRectangle(r, brush_.get());
}

void App::StrokeLine(float x0, float y0, float x1, float y1, const D2D1_COLOR_F& c, float w) {
    brush_->SetColor(c);
    rt_->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), brush_.get(), w);
}

void App::DrawLabel(const std::wstring& s, IDWriteTextFormat* fmt, const D2D1_RECT_F& box,
                    const D2D1_COLOR_F& color) {
    if (s.empty() || !fmt) return;
    brush_->SetColor(color);
    rt_->DrawTextW(s.c_str(), static_cast<UINT32>(s.size()), fmt, box, brush_.get(),
                   D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void App::DrawButton(const Button& b, bool hot) {
    if (b.isLabel) {
        // 묶음 이름. 버튼처럼 보이면 누를 수 있다고 오해한다.
        DrawLabel(b.label, fSmall_.get(), b.rect, pal_.ink3);
        return;
    }
    const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(b.rect, S(4.0f), S(4.0f));
    if (b.accent || b.pressed) {
        brush_->SetColor(pal_.accent);
        rt_->FillRoundedRectangle(rr, brush_.get());
    } else {
        brush_->SetColor(hot ? pal_.hover : pal_.surface);
        rt_->FillRoundedRectangle(rr, brush_.get());
        brush_->SetColor(pal_.hair);
        rt_->DrawRoundedRectangle(rr, brush_.get(), 1.0f);
    }
    const D2D1_COLOR_F fg = (b.accent || b.pressed) ? pal_.onAccent : pal_.ink2;
    DrawLabel(b.label, fUiCenter_.get(), b.rect, fg);
}

void App::RebuildTopButtons(float clientWidth) {
    buttons_.clear();
    const float pad = S(10.0f);

    // ---- 툴바: 파일 열기 ----------------------------------------------------
    const float h = S(26.0f);
    const float y = S(metrics::kToolbarH) * 0.5f - h * 0.5f;
    float x = pad;
    auto add = [&](ButtonId id, const wchar_t* label, bool accent, float gap) {
        Button b;
        b.id = id;
        b.label = label;
        b.accent = accent;
        const float w = MeasureText(dw_.get(), b.label, fUiCenter_.get()) + S(22.0f);
        b.rect = Rect(x, y, x + w, y + h);
        x += w + gap;
        buttons_.push_back(std::move(b));
    };
    if (IsLoading()) {
        add(ButtonId::CancelLoad, L"읽기 취소", false, S(14.0f));
    } else {
        add(ButtonId::Open, L"이전 로그 열기", true, S(4.0f));
        add(ButtonId::OpenCompare, L"이후 로그 열기", false, S(4.0f));
        if (HasCompare()) add(ButtonId::CloseCompare, L"비교 해제", false, S(14.0f));
    }

    // ---- 컨트롤 줄: 자리가 모자라면 다음 줄로 넘긴다 --------------------------
    // 버튼을 한 줄에 밀어 넣던 것이 지금까지 겹침의 주된 원인이었다. 이제는
    // 넘치면 줄을 바꾸고, 그 결과 줄 수만큼 컨트롤 줄 높이를 늘린다.
    const float ctlH = S(24.0f);
    const float gapY = S(4.0f);
    const float padY = S(5.0f);
    const float right = clientWidth - pad;
    float cx = pad;
    float cy = S(metrics::kToolbarH) + padY;
    int rows = 1;

    auto place = [&](float w) {
        if (cx > pad && cx + w > right) {   // 줄 바꿈
            cx = pad;
            cy += ctlH + gapY;
            ++rows;
        }
        const D2D1_RECT_F rc = Rect(cx, cy, cx + w, cy + ctlH);
        cx += w;
        return rc;
    };
    auto addLabel = [&](const wchar_t* text) {
        Button b;
        b.id = ButtonId::None;
        b.isLabel = true;
        b.label = text;
        b.rect = place(MeasureText(dw_.get(), b.label, fUiCenter_.get()) + S(12.0f));
        cx += S(2.0f);
        buttons_.push_back(std::move(b));
    };
    auto addCtl = [&](ButtonId id, const wchar_t* label, bool pressed, float gap) {
        Button b;
        b.id = id;
        b.label = label;
        b.pressed = pressed;
        b.rect = place(MeasureText(dw_.get(), b.label, fUiCenter_.get()) + S(18.0f));
        cx += gap;
        buttons_.push_back(std::move(b));
    };

    addLabel(L"보기");
    addCtl(ButtonId::ModeLanes, L"레인", mode_ == PlotMode::Lanes, S(2.0f));
    addCtl(ButtonId::ModeOverlay, L"겹쳐보기", mode_ == PlotMode::Overlay, S(14.0f));

    addLabel(L"배치");
    addCtl(ButtonId::OrientAuto, L"자동", orientation_ == LC_ORIENT_AUTO, S(2.0f));
    addCtl(ButtonId::OrientRows, L"행 = IO", orientation_ == LC_ORIENT_ROWS, S(2.0f));
    addCtl(ButtonId::OrientCols, L"열 = IO", orientation_ == LC_ORIENT_COLS, S(14.0f));

    if (mode_ == PlotMode::Overlay) {
        // 레인은 채널마다 눈금이 따로라 이 설정이 뜻을 갖지 않는다.
        addLabel(L"세로 눈금");
        addCtl(ButtonId::Normalize, L"0–1 정규화", normalize_, S(2.0f));
        addCtl(ButtonId::YFitVisible, L"보이는 구간에 맞춤", yFitVisible_, S(14.0f));
    }

    addLabel(L"확대");
    addCtl(ButtonId::ZoomOut, L"축소 −", false, S(2.0f));
    addCtl(ButtonId::ZoomIn, L"확대 ＋", false, S(2.0f));
    addCtl(ButtonId::Fit, L"전체 보기", false, S(14.0f));

    addLabel(L"커서");
    addCtl(ButtonId::ClearCursors, L"해제", false, S(14.0f));

    if (HasCompare()) {
        addLabel(L"비교");
        addCtl(ButtonId::CompareBoth, L"이전+이후", compareMode_ == CompareMode::Both, S(2.0f));
        addCtl(ButtonId::CompareDiff, L"차이 Δ", compareMode_ == CompareMode::Diff, S(2.0f));
        addCtl(ButtonId::Stagger, L"벌려 그리기", stagger_, S(14.0f));

        // 화살표만 있으면 무엇을 미는 건지 알 수 없다. 무엇이 어느 쪽으로
        // 움직이는지 글자로 적는다.
        addLabel(L"차이 기준");
        addCtl(ButtonId::MetricPeak, L"최대", metric_ == DiffMetric::Peak, S(2.0f));
        addCtl(ButtonId::MetricMean, L"평균", metric_ == DiffMetric::Mean, S(2.0f));
        addCtl(ButtonId::MetricRms, L"RMS", metric_ == DiffMetric::Rms, S(2.0f));
        addCtl(ButtonId::MetricArea, L"면적", metric_ == DiffMetric::Area, S(2.0f));
        addCtl(ButtonId::MetricTimeFrac, L"시간%", metric_ == DiffMetric::TimeFrac, S(2.0f));
        addCtl(ButtonId::MetricSamples, L"샘플 수", metric_ == DiffMetric::Samples, S(2.0f));
        addCtl(ButtonId::MetricRuns, L"구간 수", metric_ == DiffMetric::Runs, S(2.0f));
        addCtl(ButtonId::ToleranceCycle, Fmt(L"허용 오차 %.1f%%", tolerance_ * 100.0).c_str(),
               false, S(14.0f));

        addLabel(L"이후 로그 시간 맞추기");
        addCtl(ButtonId::AlignLeft, L"◀ 앞당김", false, S(2.0f));
        addCtl(ButtonId::AlignRight, L"뒤로 밀기 ▶", false, S(2.0f));
        addCtl(ButtonId::AlignAuto, L"자동 정렬", false, S(2.0f));
        addCtl(ButtonId::AlignReset, L"정렬 해제", false, S(2.0f));
    }

    controlsH_ = static_cast<float>(rows) * ctlH + static_cast<float>(rows - 1) * gapY +
                 padY * 2.0f;
}

void App::RebuildRailButtons(const Rects& r) {
    const float pad = S(10.0f);
    float ry = r.rail.top + S(6.0f) + S(16.0f) + S(metrics::kSearchH) + S(8.0f);
    const float chipH = S(22.0f);
    float rx = r.rail.left + pad;
    auto addChip = [&](ButtonId id, const wchar_t* label, bool pressed) {
        Button b;
        b.id = id;
        b.label = label;
        b.pressed = pressed;
        const float w = MeasureText(dw_.get(), b.label, fUiCenter_.get()) + S(16.0f);
        // 레일도 넘치면 줄을 바꾼다.
        if (rx > r.rail.left + pad && rx + w > r.rail.right - pad) {
            rx = r.rail.left + pad;
            ry += chipH + S(4.0f);
        }
        b.rect = Rect(rx, ry, rx + w, ry + chipH);
        rx += w + S(5.0f);
        buttons_.push_back(std::move(b));
    };
    addChip(ButtonId::FilterAll, L"전체", filter_ < 0);
    addChip(ButtonId::FilterDigital, L"DIG", filter_ == LC_CH_DIGITAL);
    addChip(ButtonId::FilterAnalog, L"ANA", filter_ == LC_CH_ANALOG);
    addChip(ButtonId::FilterState, L"STATE", filter_ == LC_CH_STATE);
    if (HasCompare()) addChip(ButtonId::FilterChanged, L"달라진 것만", filter_ == -2);

    const float sy = ry + chipH + S(6.0f);
    float sx = r.rail.left + pad;
    auto addLink = [&](ButtonId id, const wchar_t* label) {
        Button b;
        b.id = id;
        b.label = label;
        const float w = MeasureText(dw_.get(), b.label, fUiCenter_.get()) + S(12.0f);
        b.rect = Rect(sx, sy, sx + w, sy + S(22.0f));
        sx += w + S(4.0f);
        buttons_.push_back(std::move(b));
    };
    addLink(ButtonId::SelectAll, L"전체 선택");
    addLink(ButtonId::SelectNone, L"전체 해제");
    addLink(ButtonId::GroupsExpand, L"펴기");
    addLink(ButtonId::GroupsCollapse, L"접기");
}

void App::Render() {
    if (!CreateDeviceResources()) return;

    RECT rc{};
    GetClientRect(hwnd_, &rc);
    RebuildTopButtons(static_cast<float>(rc.right - rc.left));
    const Rects r = CalcRects();
    RebuildRailButtons(r);

    rt_->BeginDraw();
    rt_->Clear(pal_.plane);

    DrawPlot(r);
    DrawAxis(r);
    DrawRail(r);
    DrawToolbar(r);
    DrawControls(r);
    DrawStatus(r);
    if (IsLoading()) DrawLoadingOverlay(r);

    // 글자 커서가 깜빡이려면 검색 중일 때만 주기적으로 다시 그리면 된다.
    if (searchFocused_) SetTimer(hwnd_, kTimerCaret, 260, nullptr);
    else KillTimer(hwnd_, kTimerCaret);

    if (rt_->EndDraw() == static_cast<HRESULT>(D2DERR_RECREATE_TARGET)) DiscardDeviceResources();
}

void App::DrawToolbar(const Rects& r) {
    Fill(r.toolbar, pal_.panel);
    StrokeLine(r.toolbar.left, Px(r.toolbar.bottom), r.toolbar.right, Px(r.toolbar.bottom),
               pal_.hair);

    for (const Button& b : buttons_) {
        if (b.rect.top < r.toolbar.bottom) DrawButton(b, hotButton_ == b.id);
    }

    // 파일 요약
    float x = buttons_.empty() ? S(10.0f) : 0.0f;
    for (const Button& b : buttons_) {
        if (b.rect.top < r.toolbar.bottom) x = (std::max)(x, b.rect.right);
    }
    x += S(18.0f);
    if (x >= r.toolbar.right - S(40.0f)) return;

    std::wstring meta;
    if (ds_) {
        const uint32_t n = lc_sample_count(ds_);
        const double* t = lc_times(ds_);
        const double span = (n > 1 && t) ? t[n - 1] - t[0] : 0.0;
        meta = fileName_ + L"   ·   " +
               Fmt(L"%u 채널", lc_channel_count(ds_)) + L"   ·   " +
               Fmt(L"%u 샘플", n) + L"   ·   " + FormatSpan(span);
    } else {
        meta = L"파일이 열려 있지 않습니다";
    }
    DrawLabel(Ellipsize(dw_.get(), meta, fMono_.get(), r.toolbar.right - x - S(10.0f)),
              fMono_.get(), Rect(x, r.toolbar.top, r.toolbar.right - S(10.0f), r.toolbar.bottom),
              ds_ ? pal_.ink2 : pal_.ink3);
}

void App::DrawControls(const Rects& r) {
    Fill(r.controls, pal_.surface);
    StrokeLine(r.controls.left, Px(r.controls.bottom), r.controls.right,
               Px(r.controls.bottom), pal_.hair);
    for (const Button& b : buttons_) {
        if (b.rect.top >= r.controls.top && b.rect.bottom <= r.controls.bottom) {
            DrawButton(b, hotButton_ == b.id);
        }
    }
}

void App::DrawRail(const Rects& r) {
    Fill(r.rail, pal_.panel);
    StrokeLine(Px(r.rail.right), r.rail.top, Px(r.rail.right), r.rail.bottom, pal_.hair);

    DrawLabel(L"IO 이름 검색", fSmall_.get(),
              Rect(r.rail.left + S(10.0f), r.rail.top + S(4.0f),
                   r.rail.right, r.rail.top + S(20.0f)),
              pal_.ink3);
    DrawSearchBox(r);

    // 선택 개수는 검색 라벨과 같은 줄 오른쪽 끝에 둔다. 아래 버튼 줄에 두면
    // "펴기 / 접기" 와 자리를 다툰다 (실제로 겹쳤다).
    if (ds_) {
        uint32_t sel = 0;
        for (bool b : selected_) sel += b ? 1u : 0u;
        DrawLabel(Fmt(L"%u / %u 선택", sel, lc_channel_count(ds_)), fSmallRight_.get(),
                  Rect(r.rail.left + S(60.0f), r.rail.top + S(4.0f),
                       r.rail.right - S(10.0f), r.rail.top + S(20.0f)),
                  sel ? pal_.accent : pal_.ink3);
    }

    for (const Button& b : buttons_) {
        if (b.rect.top >= r.rail.top && b.rect.bottom <= r.railList.top) {
            DrawButton(b, hotButton_ == b.id);
        }
    }
    if (!ds_) return;

    RebuildRailRows();

    rt_->PushAxisAlignedClip(r.railList, D2D1_ANTIALIAS_MODE_ALIASED);
    const float rowH = S(metrics::kRowH);
    const float box = S(12.0f);
    float y = r.railList.top - scrollRail_;

    for (const RailRow& row : railRows_) {
        const float top = y;
        y += rowH;
        if (top + rowH < r.railList.top || top > r.railList.bottom) continue;

        if (row.kind == RailRow::Kind::Group) {
            uint32_t visible = 0, selected = 0;
            GroupCounts(row.index, visible, selected);
            const bool open = row.index < groupOpen_.size() && groupOpen_[row.index];

            Fill(Rect(r.rail.left, top, r.rail.right, top + rowH), pal_.hover);

            // 펼침 표시 삼각형
            const float tx = r.rail.left + S(10.0f);
            const float ty = top + rowH * 0.5f;
            brush_->SetColor(pal_.ink2);
            Path tri;
            if (tri.Begin(d2d_.get())) {
                if (open) {
                    tri.Move(tx, ty - S(2.0f));
                    tri.Line(tx + S(8.0f), ty - S(2.0f));
                    tri.Line(tx + S(4.0f), ty + S(3.0f));
                    tri.Line(tx, ty - S(2.0f));
                } else {
                    tri.Move(tx + S(1.0f), ty - S(4.0f));
                    tri.Line(tx + S(6.0f), ty);
                    tri.Line(tx + S(1.0f), ty + S(4.0f));
                    tri.Line(tx + S(1.0f), ty - S(4.0f));
                }
                if (tri.End()) rt_->DrawGeometry(tri.geo.get(), brush_.get(), S(1.4f));
            }

            // 그룹 전체 선택 상자 (일부만 선택되면 반쯤 찬 표시)
            const D2D1_RECT_F cb = Rect(r.rail.left + S(26.0f), top + (rowH - box) * 0.5f,
                                        r.rail.left + S(26.0f) + box, top + (rowH + box) * 0.5f);
            const D2D1_ROUNDED_RECT crr = D2D1::RoundedRect(cb, S(3.0f), S(3.0f));
            if (selected == visible && visible > 0) {
                brush_->SetColor(pal_.accent);
                rt_->FillRoundedRectangle(crr, brush_.get());
                brush_->SetColor(pal_.onAccent);
                rt_->DrawLine(D2D1::Point2F(cb.left + box * 0.24f, cb.top + box * 0.52f),
                              D2D1::Point2F(cb.left + box * 0.44f, cb.top + box * 0.74f),
                              brush_.get(), S(1.6f));
                rt_->DrawLine(D2D1::Point2F(cb.left + box * 0.44f, cb.top + box * 0.74f),
                              D2D1::Point2F(cb.left + box * 0.78f, cb.top + box * 0.28f),
                              brush_.get(), S(1.6f));
            } else {
                brush_->SetColor(pal_.hair);
                rt_->DrawRoundedRectangle(crr, brush_.get(), 1.0f);
                if (selected > 0) {
                    Fill(Rect(cb.left + S(3.0f), top + rowH * 0.5f - S(1.0f),
                              cb.right - S(3.0f), top + rowH * 0.5f + S(1.0f)),
                         pal_.accent);
                }
            }

            const uint32_t first = row.index * kGroupSize + 1;
            const uint32_t last = (std::min)(first + kGroupSize - 1, lc_channel_count(ds_));
            DrawLabel(Fmt(L"%03u – %03u", first, last), fMono_.get(),
                      Rect(cb.right + S(9.0f), top, r.rail.right - S(60.0f), top + rowH),
                      pal_.ink);
            DrawLabel(Fmt(L"%u/%u", selected, visible), fSmallRight_.get(),
                      Rect(r.rail.right - S(58.0f), top, r.rail.right - S(12.0f), top + rowH),
                      selected ? pal_.accent : pal_.ink3);
            continue;
        }

        if (row.kind == RailRow::Kind::ExtraHeader) {
            Fill(Rect(r.rail.left, top, r.rail.right, top + rowH), pal_.hover);
            DrawLabel(Fmt(L"이후 로그에만 있음  (%u)", static_cast<uint32_t>(extraB_.size())),
                      fSmall_.get(),
                      Rect(r.rail.left + S(12.0f), top, r.rail.right - S(10.0f), top + rowH),
                      pal_.cursorB);
            continue;
        }

        if (row.kind == RailRow::Kind::ExtraChannel) {
            // 이전 로그에 짝이 없어 그릴 수는 없다. 이름만 보여 준다.
            DrawLabel(Ellipsize(dw_.get(), lc_channel_name(dsB_, row.index), fMono_.get(),
                                r.rail.right - r.rail.left - S(70.0f)),
                      fMono_.get(),
                      Rect(r.rail.left + S(38.0f), top, r.rail.right - S(56.0f), top + rowH),
                      pal_.ink3);
            DrawLabel(L"신규", fSmallRight_.get(),
                      Rect(r.rail.right - S(54.0f), top, r.rail.right - S(12.0f), top + rowH),
                      pal_.cursorB);
            continue;
        }

        // 일반 채널 줄
        const uint32_t ch = row.index;
        const bool on = selected_[ch];
        if (anchorChannel_ == static_cast<int32_t>(ch)) {
            Fill(Rect(r.rail.left, top, r.rail.right, top + rowH), pal_.hover);
        }
        const float cx = r.rail.left + S(26.0f);
        const D2D1_RECT_F cb = Rect(cx, top + (rowH - box) * 0.5f, cx + box,
                                    top + (rowH + box) * 0.5f);
        const D2D1_ROUNDED_RECT crr = D2D1::RoundedRect(cb, S(3.0f), S(3.0f));
        if (on) {
            brush_->SetColor(pal_.accent);
            rt_->FillRoundedRectangle(crr, brush_.get());
            brush_->SetColor(pal_.onAccent);
            rt_->DrawLine(D2D1::Point2F(cb.left + box * 0.24f, cb.top + box * 0.52f),
                          D2D1::Point2F(cb.left + box * 0.44f, cb.top + box * 0.74f),
                          brush_.get(), S(1.6f));
            rt_->DrawLine(D2D1::Point2F(cb.left + box * 0.44f, cb.top + box * 0.74f),
                          D2D1::Point2F(cb.left + box * 0.78f, cb.top + box * 0.28f),
                          brush_.get(), S(1.6f));
        } else {
            brush_->SetColor(pal_.hair);
            rt_->DrawRoundedRectangle(crr, brush_.get(), 1.0f);
        }

        const float tagW = HasCompare() ? S(76.0f) : S(48.0f);
        const float nameL = cb.right + S(9.0f);
        const float nameR = r.rail.right - tagW - S(12.0f);
        DrawLabel(Ellipsize(dw_.get(), lc_channel_name(ds_, ch), fMono_.get(), nameR - nameL),
                  fMono_.get(), Rect(nameL, top, nameR, top + rowH),
                  on ? pal_.ink : pal_.ink2);

        // 비교 중이면 타입 대신 "달라진 샘플 수"를 보여 준다. 어느 IO 가 바뀌었는지가
        // 두 로그를 견줄 때 가장 먼저 알고 싶은 것이다.
        std::wstring tag;
        D2D1_COLOR_F tagColor = pal_.ink3;
        if (HasCompare()) {
            tag = MetricBadge(ch);
            if (tag != L"동일") tagColor = pal_.cursorB;
        } else {
            switch (lc_channel_type(ds_, ch)) {
                case LC_CH_DIGITAL: tag = L"DIG"; break;
                case LC_CH_STATE:   tag = L"STATE"; break;
                default:            tag = L"ANA"; break;
            }
        }
        DrawLabel(tag, fSmallRight_.get(),
                  Rect(nameR, top, r.rail.right - S(12.0f), top + rowH), tagColor);
    }
    rt_->PopAxisAlignedClip();
}

void App::DrawEmptyState(const Rects& r) {
    const float cy = (r.plot.top + r.plot.bottom) * 0.5f;
    const float w = r.plot.right - r.plot.left;
    DrawLabel(L"엑셀 로그 파일을 열어 주세요", fTitle_.get(),
              Rect(r.plot.left, cy - S(34.0f), r.plot.right, cy - S(10.0f)), pal_.ink2);
    DrawLabel(L"첫 행에 시간, 각 행의 첫 열에 IO 이름이 있는 형식입니다.", fUi_.get(),
              Rect(r.plot.left + w * 0.5f - S(220.0f), cy - S(6.0f),
                   r.plot.right, cy + S(14.0f)), pal_.ink3);
    DrawLabel(L"파일을 창에 끌어다 놓아도 됩니다.", fUi_.get(),
              Rect(r.plot.left + w * 0.5f - S(220.0f), cy + S(16.0f),
                   r.plot.right, cy + S(36.0f)), pal_.ink3);
}

void App::DrawPlot(const Rects& r) {
    Fill(r.plot, pal_.surface);
    if (!ds_ || lc_channel_count(ds_) == 0) { DrawEmptyState(r); return; }
    if (mode_ == PlotMode::Overlay) { DrawOverlayView(r); return; }
    DrawLanesView(r);
}

void App::DrawLanesView(const Rects& r) {
    const float gutterX = r.plot.left + S(metrics::kNameGutter);
    const float rightX = r.plot.right - S(metrics::kValueGutter);
    const bool timeLike = lc_time_kind(ds_) == LC_TIME_CLOCK_MS ||
                          lc_time_kind(ds_) == LC_TIME_DATE_MS;
    const Ticks ticks = MakeTicks(t0_, t1_,
                                  (std::max)(2, static_cast<int>((rightX - gutterX) / S(110.0f))),
                                  timeLike);

    rt_->PushAxisAlignedClip(r.plot, D2D1_ANTIALIAS_MODE_ALIASED);

    for (double tv : ticks.at) {
        const float x = XOfTime(tv, r.plot);
        if (x < gutterX || x > rightX) continue;
        StrokeLine(Px(x), r.plot.top, Px(x), r.plot.bottom, pal_.grid);
    }
    StrokeLine(Px(gutterX), r.plot.top, Px(gutterX), r.plot.bottom, pal_.axis);
    StrokeLine(Px(rightX), r.plot.top, Px(rightX), r.plot.bottom, pal_.axis);

    const int hoverIdx = (hoverX_ >= gutterX && hoverX_ <= rightX)
                             ? IndexAt(TimeOfX(hoverX_, r.plot))
                             : -1;

    float y = r.plot.top - scrollPlot_;
    int laneIndex = 0;
    for (uint32_t ch = 0; ch < lc_channel_count(ds_); ++ch) {
        if (!selected_[ch]) continue;
        const LcChannelType type = lc_channel_type(ds_, ch);
        const float h = LaneHeight(type);
        const D2D1_RECT_F lane = Rect(r.plot.left, y, r.plot.right, y + h);
        y += h;
        const int idx = laneIndex++;
        if (lane.bottom < r.plot.top || lane.top > r.plot.bottom) continue;

        if (idx % 2 == 1) Fill(lane, pal_.laneAlt);
        StrokeLine(r.plot.left, Px(lane.bottom), r.plot.right, Px(lane.bottom), pal_.hair);

        rt_->PushAxisAlignedClip(Rect(gutterX, lane.top, rightX, lane.bottom),
                                 D2D1_ANTIALIAS_MODE_ALIASED);
        const bool compare = HasCompare() && ch < matchB_.size() && matchB_[ch] >= 0;
        const float pad = S(8.0f);
        if (compare && compareMode_ == CompareMode::Diff) {
            // 차이만 그린다. 0 선을 같이 그어야 어느 쪽으로 벌어졌는지 읽힌다.
            const double dlo = diffLo_[ch], dhi = diffHi_[ch];
            if (dhi > dlo && dlo <= 0.0 && dhi >= 0.0) {
                const float zero = lane.bottom - pad -
                                   static_cast<float>((0.0 - dlo) / (dhi - dlo)) *
                                       (lane.bottom - pad - (lane.top + pad));
                StrokeLine(gutterX, Px(zero), rightX, Px(zero), pal_.axis);
            }
            DrawResampled(ch, r.plot, lane.top + pad, lane.bottom - pad, dlo, dhi,
                          pal_.accent, true, S(2.0f));
        } else if (compare) {
            // 두 로그를 같은 눈금에 겹친다. 눈금은 둘을 모두 담는 범위로.
            const double lo = cmpLo_[ch], hi = cmpHi_[ch];
            // 벌어진 만큼을 먼저 면적으로 칠하고 그 위에 두 선을 실선으로 얹는다.
            DrawDifferenceBand(ch, r.plot, lane.top + pad, lane.bottom - pad, lo, hi,
                               pal_.cursorB);
            DrawSeries(ch, r.plot, lane.top + pad, lane.bottom - pad, lo, hi,
                       pal_.accent);
            // 값이 완전히 같으면 두 선이 포개져 하나로 보인다. 그때만 확인하고
            // 싶다면 "벌려 그리기" 로 이후 선을 살짝 띄울 수 있다.
            const float shift = stagger_ ? (lane.bottom - lane.top) * 0.10f : 0.0f;
            DrawResampled(ch, r.plot, lane.top + pad + shift, lane.bottom - pad + shift,
                          lo, hi, pal_.cursorB, false, S(1.8f));
        } else {
            switch (type) {
                case LC_CH_DIGITAL: DrawLaneDigital(ch, lane, r.plot); break;
                case LC_CH_STATE:   DrawLaneState(ch, lane, r.plot); break;
                default:            DrawLaneAnalog(ch, lane, r.plot); break;
            }
        }
        rt_->PopAxisAlignedClip();

        // 왼쪽 이름 거터
        DrawLabel(Ellipsize(dw_.get(), lc_channel_name(ds_, ch), fMono_.get(),
                            S(metrics::kNameGutter) - S(20.0f)),
                  fMonoRight_.get(),
                  Rect(r.plot.left + S(6.0f), lane.top, gutterX - S(10.0f), lane.bottom),
                  pal_.ink);

        // 오른쪽 값 거터: 커서(없으면 화면 오른쪽 끝) 위치의 값
        const double* vals = lc_channel_values(ds_, ch);
        int vi = hoverIdx;
        if (vi < 0) {
            int i0 = 0, i1 = 0;
            IndexRange(i0, i1);
            vi = i1;
        }
        if (vals && vi >= 0) {
            const double* times = lc_times(ds_);
            if (compare) {
                // 이전 / 이후 / 차이를 위아래로 쌓아 보여 준다.
                const double tv = times ? times[vi] : 0.0;
                DrawLabel(FormatValue(ch, vals[vi]), fSmallRight_.get(),
                          Rect(rightX + S(4.0f), lane.top + S(2.0f), r.plot.right - S(10.0f),
                               lane.top + h * 0.36f),
                          pal_.accent);
                DrawLabel(FormatValue(ch, CompareValueAt(ch, tv)), fSmallRight_.get(),
                          Rect(rightX + S(4.0f), lane.top + h * 0.34f, r.plot.right - S(10.0f),
                               lane.top + h * 0.68f),
                          pal_.cursorB);
                const double d = DiffValueAt(ch, tv);
                DrawLabel(std::isfinite(d) ? (L"Δ " + FormatNumber(d)) : L"Δ —",
                          fSmallRight_.get(),
                          Rect(rightX + S(4.0f), lane.top + h * 0.66f, r.plot.right - S(10.0f),
                               lane.bottom - S(2.0f)),
                          pal_.ink2);
            } else {
                const D2D1_RECT_F vg =
                    Rect(rightX + S(8.0f), lane.top, r.plot.right - S(10.0f),
                         type == LC_CH_ANALOG ? lane.top + h * 0.55f : lane.bottom);
                DrawLabel(FormatValue(ch, vals[vi]), fMonoRight_.get(), vg, pal_.ink);
                if (type == LC_CH_ANALOG) {
                    DrawLabel(FormatNumber(lc_channel_min(ds_, ch)) + L" – " +
                                  FormatNumber(lc_channel_max(ds_, ch)),
                              fSmallRight_.get(),
                              Rect(rightX + S(4.0f), lane.top + h * 0.52f,
                                   r.plot.right - S(10.0f), lane.bottom),
                              pal_.ink3);
                }
            }
        }
    }

    if (laneIndex == 0) {
        DrawLabel(L"표시할 채널이 없습니다 — 왼쪽 목록에서 선택하세요", fUi_.get(),
                  Rect(gutterX + S(16.0f), r.plot.top + S(16.0f), r.plot.right,
                       r.plot.top + S(40.0f)),
                  pal_.ink2);
    }

    // 커서와 십자선
    if (hoverIdx >= 0) {
        StrokeLine(Px(hoverX_), r.plot.top, Px(hoverX_), r.plot.bottom, pal_.ink3);
    }
    auto cursor = [&](double t, const D2D1_COLOR_F& c, const wchar_t* tag) {
        const float x = XOfTime(t, r.plot);
        if (x < gutterX - 1.0f || x > rightX + 1.0f) return;
        StrokeLine(Px(x), r.plot.top, Px(x), r.plot.bottom, c, S(1.5f));
        const D2D1_RECT_F tab = Rect(x - S(9.0f), r.plot.top, x + S(9.0f),
                                     r.plot.top + S(15.0f));
        Fill(tab, c);
        DrawLabel(tag, fSmall_.get(),
                  Rect(tab.left + S(6.0f), tab.top, tab.right, tab.bottom), pal_.onAccent);
    };
    if (hasA_) cursor(curA_, pal_.cursorA, L"A");
    if (hasB_) cursor(curB_, pal_.cursorB, L"B");

    rt_->PopAxisAlignedClip();
}

void App::DrawLaneDigital(uint32_t ch, D2D1_RECT_F lane, const D2D1_RECT_F& plot) {
    const double* v = lc_channel_values(ds_, ch);
    const double* t = lc_times(ds_);
    if (!v || !t) return;
    int i0 = 0, i1 = 0;
    IndexRange(i0, i1);
    if (i0 < 0 || i1 < i0) return;

    const float pad = S(8.0f);
    const float hi = lane.top + pad, lo = lane.bottom - pad;
    const float rightX = plot.right - S(metrics::kValueGutter);

    // 하이 구간 채움
    brush_->SetColor(D2D1::ColorF(pal_.accent.r, pal_.accent.g, pal_.accent.b, 0.14f));
    int runStart = -1;
    for (int i = i0; i <= i1; ++i) {
        const bool on = (v[i] == 1.0);
        if (on && runStart < 0) runStart = i;
        if ((!on || i == i1) && runStart >= 0) {
            const float xa = XOfTime(t[runStart], plot);
            const float xb = XOfTime(t[i], plot);
            rt_->FillRectangle(Rect(xa, hi, (std::max)(xb, xa + 0.7f), lo), brush_.get());
            runStart = -1;
        }
    }

    Path p;
    if (!p.Begin(d2d_.get())) return;
    bool started = false;
    float prevY = lo;
    for (int i = i0; i <= i1; ++i) {
        if (!std::isfinite(v[i])) { started = false; continue; }
        const float x = XOfTime(t[i], plot);
        const float yy = (v[i] != 0.0) ? hi : lo;
        if (!started) { p.Move(x, yy); started = true; }
        else { p.Line(x, prevY); p.Line(x, yy); }
        prevY = yy;
    }
    if (started) p.Line(rightX, prevY);
    if (!p.End()) return;
    brush_->SetColor(pal_.accent);
    rt_->DrawGeometry(p.geo.get(), brush_.get(), S(2.0f));
}

void App::DrawLaneAnalog(uint32_t ch, D2D1_RECT_F lane, const D2D1_RECT_F& plot) {
    const double* v = lc_channel_values(ds_, ch);
    const double* t = lc_times(ds_);
    if (!v || !t) return;
    int i0 = 0, i1 = 0;
    IndexRange(i0, i1);
    if (i0 < 0 || i1 < i0) return;

    const float pad = S(8.0f);
    const float top = lane.top + pad, bot = lane.bottom - pad;
    const float leftX = plot.left + S(metrics::kNameGutter);
    const float rightX = plot.right - S(metrics::kValueGutter);
    const float width = (std::max)(rightX - leftX, 10.0f);

    double mn = lc_channel_min(ds_, ch), mx = lc_channel_max(ds_, ch);
    if (!(mx > mn)) mx = mn + 1.0;
    auto yOf = [&](double val) {
        return bot - static_cast<float>((val - mn) / (mx - mn)) * (bot - top);
    };

    const int count = i1 - i0 + 1;
    // 샘플이 픽셀보다 촘촘하면 코어의 다운샘플러로 픽셀당 최소/최대만 뽑는다.
    if (static_cast<float>(count) > width * 2.0f) {
        const uint32_t cols = static_cast<uint32_t>(width);
        std::vector<double> lo(cols), hi(cols);
        lc_decimate(ds_, ch, t0_, t1_, cols, lo.data(), hi.data());
        Path p;
        if (!p.Begin(d2d_.get())) return;
        for (uint32_t c = 0; c < cols; ++c) {
            if (!std::isfinite(lo[c])) continue;
            const float x = leftX + static_cast<float>(c) + 0.5f;
            p.Move(x, yOf(hi[c]));
            p.Line(x, yOf(lo[c]) + 0.8f);
        }
        if (!p.End()) return;
        brush_->SetColor(pal_.accent);
        rt_->DrawGeometry(p.geo.get(), brush_.get(), 1.0f);
        return;
    }

    Path p;
    if (!p.Begin(d2d_.get())) return;
    bool pen = false;
    for (int i = i0; i <= i1; ++i) {
        if (!std::isfinite(v[i])) { pen = false; continue; }
        const float x = XOfTime(t[i], plot);
        const float yy = yOf(v[i]);
        if (!pen) { p.Move(x, yy); pen = true; } else { p.Line(x, yy); }
    }
    if (!p.End()) return;
    brush_->SetColor(pal_.accent);
    rt_->DrawGeometry(p.geo.get(), brush_.get(), S(2.0f));
}

void App::DrawLaneState(uint32_t ch, D2D1_RECT_F lane, const D2D1_RECT_F& plot) {
    const double* v = lc_channel_values(ds_, ch);
    const double* t = lc_times(ds_);
    if (!v || !t) return;
    int i0 = 0, i1 = 0;
    IndexRange(i0, i1);
    if (i0 < 0 || i1 < i0) return;

    const float pad = S(7.0f);
    const float top = lane.top + pad, bot = lane.bottom - pad;
    const float rightX = plot.right - S(metrics::kValueGutter);

    int start = i0;
    double cur = v[i0];
    for (int i = i0 + 1; i <= i1 + 1; ++i) {
        const bool last = (i > i1);
        const double val = last ? std::nan("") : v[i];
        const bool same = (!last && ((std::isnan(val) && std::isnan(cur)) || val == cur));
        if (same) continue;

        if (std::isfinite(cur)) {
            const float xa = XOfTime(t[start], plot);
            const float xb = last ? rightX : XOfTime(t[i], plot);
            const uint32_t si = static_cast<uint32_t>(cur);
            const D2D1_COLOR_F base = (si < 8) ? pal_.series[si] : pal_.ink3;
            brush_->SetColor(D2D1::ColorF(base.r, base.g, base.b, 0.82f));
            rt_->FillRectangle(Rect(xa, top, (std::max)(xb - S(2.0f), xa + 0.7f), bot),
                               brush_.get());

            const std::wstring label =
                si < lc_state_count(ds_, ch) ? lc_state_name(ds_, ch, si) : L"";
            const float w = MeasureText(dw_.get(), label, fSmall_.get());
            if (!label.empty() && xb - xa > w + S(14.0f)) {
                DrawLabel(label, fSmall_.get(),
                          Rect(xa + S(6.0f), top, xb - S(4.0f), bot),
                          D2D1::ColorF(D2D1::ColorF::White));
            }
        }
        start = i;
        cur = last ? cur : val;
    }
}


std::vector<uint32_t> App::OverlayChannels() const {
    std::vector<uint32_t> out;
    if (!ds_) return out;
    for (uint32_t ch = 0; ch < lc_channel_count(ds_) && out.size() < kMaxOverlay; ++ch) {
        if (selected_[ch]) out.push_back(ch);
    }
    return out;
}

double App::SeriesValue(uint32_t ch, double raw) const {
    if (!normalize_) return raw;
    const double lo = lc_channel_min(ds_, ch);
    const double hi = lc_channel_max(ds_, ch);
    const double span = (hi > lo) ? (hi - lo) : 1.0;
    return (raw - lo) / span;
}

void App::OverlayRange(const std::vector<uint32_t>& shown, double& lo, double& hi) const {
    const bool diff = HasCompare() && compareMode_ == CompareMode::Diff;
    if (normalize_ && !diff) { lo = 0.0; hi = 1.0; return; }

    lo = std::numeric_limits<double>::infinity();
    hi = -std::numeric_limits<double>::infinity();

    // 어느 구간의 값을 볼지.
    //
    // 보이는 구간에만 맞추면 시간축을 조금만 옮겨도 세로 배율이 통째로 바뀐다.
    // 확대해 놓고 뒤로 넘기면 배율이 풀린 것처럼 보여서 쓰기가 어렵다. 그래서
    // 기본은 선택한 채널의 전체 범위로 고정하고, 보이는 구간에 맞추는 동작은
    // 버튼으로 켤 때만 쓴다.
    int i0 = 0, i1 = 0;
    const double* t = lc_times(ds_);
    const uint32_t n = lc_sample_count(ds_);
    if (yFitVisible_) {
        IndexRange(i0, i1);
    } else {
        i0 = 0;
        i1 = static_cast<int>(n) - 1;
    }
    if (!t || i0 < 0 || i1 < i0) { lo = 0.0; hi = 1.0; return; }

    for (uint32_t ch : shown) {
        const double* v = lc_channel_values(ds_, ch);
        if (!v) continue;
        if (diff) {
            for (int i = i0; i <= i1; ++i) {
                const double d = DiffValueAt(ch, t[i]);
                if (!std::isfinite(d)) continue;
                lo = (std::min)(lo, d);
                hi = (std::max)(hi, d);
            }
            continue;
        }
        // 전체 범위라면 채널이 이미 들고 있는 최소·최대를 쓰면 된다. 샘플을
        // 다시 훑을 이유가 없다.
        if (!yFitVisible_) {
            lo = (std::min)(lo, HasCompare() && ch < cmpLo_.size() ? cmpLo_[ch]
                                                                   : lc_channel_min(ds_, ch));
            hi = (std::max)(hi, HasCompare() && ch < cmpHi_.size() ? cmpHi_[ch]
                                                                   : lc_channel_max(ds_, ch));
            continue;
        }
        for (int i = i0; i <= i1; ++i) {
            if (std::isfinite(v[i])) {
                lo = (std::min)(lo, v[i]);
                hi = (std::max)(hi, v[i]);
            }
            if (HasCompare()) {
                const double b = CompareValueAt(ch, t[i]);
                if (std::isfinite(b)) {
                    lo = (std::min)(lo, b);
                    hi = (std::max)(hi, b);
                }
            }
        }
    }
    if (!std::isfinite(lo) || !std::isfinite(hi)) { lo = 0.0; hi = 1.0; return; }
    if (hi <= lo) { hi = lo + 1.0; }
    const double pad = (hi - lo) * 0.08;
    lo -= pad;
    hi += pad;
}

void App::DrawSeries(uint32_t ch, const D2D1_RECT_F& plot, float top, float bottom,
                     double lo, double hi, const D2D1_COLOR_F& color) {
    const double* v = lc_channel_values(ds_, ch);
    const double* t = lc_times(ds_);
    if (!v || !t || !(hi > lo)) return;
    int i0 = 0, i1 = 0;
    IndexRange(i0, i1);
    if (i0 < 0 || i1 < i0) return;

    auto yOf = [&](double val) {
        return bottom - static_cast<float>((SeriesValue(ch, val) - lo) / (hi - lo)) *
                            (bottom - top);
    };
    // 디지털과 상태 채널은 값이 순간적으로 바뀌므로 계단으로 그린다. 비스듬한
    // 선으로 이으면 없는 중간 값이 있는 것처럼 보인다.
    const bool step = lc_channel_type(ds_, ch) != LC_CH_ANALOG;

    Path p;
    if (!p.Begin(d2d_.get())) return;
    bool pen = false;
    float lastY = 0.0f;
    for (int i = i0; i <= i1; ++i) {
        if (!std::isfinite(v[i])) { pen = false; continue; }
        const float x = XOfTime(t[i], plot);
        const float y = yOf(v[i]);
        if (!pen) { p.Move(x, y); pen = true; }
        else if (step) { p.Line(x, lastY); p.Line(x, y); }
        else { p.Line(x, y); }
        lastY = y;
    }
    if (!p.End()) return;
    brush_->SetColor(color);
    rt_->DrawGeometry(p.geo.get(), brush_.get(), S(2.0f));
}

void App::DrawOverlayReadout(const Rects& r, const std::vector<uint32_t>& shown,
                             double lo, double hi) {
    (void)lo;
    (void)hi;
    const float gutterX = r.plot.left + S(metrics::kOverlayAxisW);
    const float rightX = r.plot.right - S(12.0f);
    if (hoverX_ < gutterX || hoverX_ > rightX) return;
    const int idx = IndexAt(TimeOfX(hoverX_, r.plot));
    if (idx < 0) return;

    // 커서 위치의 값들을 한 상자에 모아 보여 준다.
    const float lineH = S(16.0f);
    const float boxH = lineH * (static_cast<float>(shown.size()) + 1) + S(10.0f);
    float boxW = S(150.0f);
    auto value_at = [&](uint32_t ch) {
        const double* v = lc_channel_values(ds_, ch);
        return v ? v[idx] : std::numeric_limits<double>::quiet_NaN();
    };
    for (uint32_t ch : shown) {
        const std::wstring row = std::wstring(lc_channel_name(ds_, ch)) + L"  " +
                                 FormatValue(ch, value_at(ch));
        boxW = (std::max)(boxW, MeasureText(dw_.get(), row, fMono_.get()) + S(40.0f));
    }
    float bx = hoverX_ + S(14.0f);
    if (bx + boxW > r.plot.right - S(6.0f)) bx = hoverX_ - boxW - S(14.0f);
    bx = (std::max)(bx, r.plot.left + S(4.0f));
    const float by = r.plot.top + S(8.0f);

    const D2D1_RECT_F box = Rect(bx, by, bx + boxW, by + boxH);
    const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(box, S(5.0f), S(5.0f));
    brush_->SetColor(pal_.surface);
    rt_->FillRoundedRectangle(rr, brush_.get());
    brush_->SetColor(pal_.hair);
    rt_->DrawRoundedRectangle(rr, brush_.get(), 1.0f);

    const double* t = lc_times(ds_);
    DrawLabel(FormatTime(t[idx], 0.0), fSmall_.get(),
              Rect(bx + S(9.0f), by + S(3.0f), box.right, by + S(3.0f) + lineH), pal_.ink3);

    float y = by + S(3.0f) + lineH;
    for (uint32_t ch : shown) {
        const D2D1_COLOR_F col = pal_.series[ch % 8];
        const D2D1_RECT_F dot = Rect(bx + S(9.0f), y + lineH * 0.5f - S(3.0f),
                                     bx + S(9.0f) + S(7.0f), y + lineH * 0.5f + S(4.0f));
        Fill(dot, col);
        DrawLabel(lc_channel_name(ds_, ch), fMono_.get(),
                  Rect(dot.right + S(7.0f), y, box.right - S(70.0f), y + lineH), pal_.ink2);
        if (HasCompare()) {
            const double tv = lc_times(ds_)[idx];
            const double d = DiffValueAt(ch, tv);
            DrawLabel(std::isfinite(d) ? (L"Δ " + FormatNumber(d)) : L"Δ —", fMonoRight_.get(),
                      Rect(box.right - S(72.0f), y, box.right - S(9.0f), y + lineH),
                      std::isfinite(d) && d != 0.0 ? pal_.cursorB : pal_.ink3);
        } else {
            DrawLabel(FormatValue(ch, value_at(ch)), fMonoRight_.get(),
                      Rect(box.right - S(72.0f), y, box.right - S(9.0f), y + lineH), pal_.ink);
        }
        y += lineH;
    }
}

void App::DrawOverlayView(const Rects& r) {
    const std::vector<uint32_t> shown = OverlayChannels();
    const float axisW = S(metrics::kOverlayAxisW);
    const float gutterX = r.plot.left + axisW;
    const float rightX = r.plot.right - S(12.0f);
    const float legendH = S(24.0f);
    const float top = r.plot.top + legendH + S(10.0f);
    const float bottom = r.plot.bottom - S(10.0f);

    const bool timeLike = lc_time_kind(ds_) == LC_TIME_CLOCK_MS ||
                          lc_time_kind(ds_) == LC_TIME_DATE_MS;
    const Ticks xt = MakeTicks(t0_, t1_,
                               (std::max)(2, static_cast<int>((rightX - gutterX) / S(110.0f))),
                               timeLike);

    if (shown.empty()) {
        DrawLabel(L"표시할 채널이 없습니다 — 왼쪽 목록에서 IO 를 선택하세요", fUi_.get(),
                  Rect(gutterX + S(16.0f), r.plot.top + S(20.0f), r.plot.right,
                       r.plot.top + S(44.0f)),
                  pal_.ink2);
        return;
    }

    double lo = 0.0, hi = 1.0;
    OverlayRange(shown, lo, hi);
    const Ticks yt = MakeTicks(lo, hi, 5, false);

    rt_->PushAxisAlignedClip(r.plot, D2D1_ANTIALIAS_MODE_ALIASED);

    // 세로 눈금과 가로 격자
    for (double yv : yt.at) {
        if (yv < lo || yv > hi) continue;
        const float y = bottom - static_cast<float>((yv - lo) / (hi - lo)) * (bottom - top);
        StrokeLine(gutterX, Px(y), rightX, Px(y), pal_.grid);
        DrawLabel(normalize_ ? FormatNumber(yv) : FormatNumber(yv), fSmallRight_.get(),
                  Rect(r.plot.left + S(4.0f), y - S(8.0f), gutterX - S(8.0f), y + S(8.0f)),
                  pal_.ink3);
    }
    // 세로 격자 (시간)
    for (double tv : xt.at) {
        const float x = XOfTime(tv, r.plot);
        if (x < gutterX || x > rightX) continue;
        StrokeLine(Px(x), top, Px(x), bottom, pal_.grid);
    }
    StrokeLine(Px(gutterX), top, Px(gutterX), bottom, pal_.axis);
    StrokeLine(gutterX, Px(bottom), rightX, Px(bottom), pal_.axis);

    // 파형
    rt_->PushAxisAlignedClip(Rect(gutterX, top, rightX, bottom), D2D1_ANTIALIAS_MODE_ALIASED);
    const bool cmpDiff = HasCompare() && compareMode_ == CompareMode::Diff;
    for (uint32_t ch : shown) {
        const D2D1_COLOR_F col = pal_.series[ch % 8];
        if (cmpDiff) {
            DrawResampled(ch, r.plot, top, bottom, lo, hi, col, true, S(2.0f));
            continue;
        }
        if (HasCompare()) DrawDifferenceBand(ch, r.plot, top, bottom, lo, hi, col);
        DrawSeries(ch, r.plot, top, bottom, lo, hi, col);
        if (HasCompare()) {
            // 같은 색을 옅고 얇게 그린다. 색은 채널을 뜻하고, 굵기가 이전/이후를 뜻한다.
            const float shift = stagger_ ? (bottom - top) * 0.02f : 0.0f;
            DrawResampled(ch, r.plot, top + shift, bottom + shift, lo, hi, col, false,
                          S(1.4f));
        }
    }
    if (cmpDiff && lo <= 0.0 && hi >= 0.0) {
        const float zero = bottom - static_cast<float>((0.0 - lo) / (hi - lo)) * (bottom - top);
        StrokeLine(gutterX, Px(zero), rightX, Px(zero), pal_.axis);
    }
    rt_->PopAxisAlignedClip();

    // 범례. 오른쪽에 붙는 안내를 먼저 재서 자리를 확보한 뒤, 남는 폭에만
    // 채널 이름을 채운다. 순서를 바꾸면 이름이 안내 글자 위로 넘어간다.
    uint32_t selected = 0;
    for (bool b : selected_) selected += b ? 1u : 0u;

    std::wstring note;
    if (selected > kMaxOverlay) {
        note = Fmt(L"선택한 %u개 중 앞 %u개만 표시", selected, kMaxOverlay);
    } else if (HasCompare()) {
        note = (compareMode_ == CompareMode::Diff)
                   ? L"이후 − 이전"
                   : L"굵은 선 = 이전 · 가는 선 = 이후 · 음영 = 차이";
    }
    float noteL = rightX;
    if (!note.empty()) {
        const float w = MeasureText(dw_.get(), note, fSmallRight_.get()) + S(12.0f);
        noteL = rightX - w;
        DrawLabel(note, fSmallRight_.get(),
                  Rect(noteL, r.plot.top, rightX, r.plot.top + legendH), pal_.ink3);
    }

    float lx = gutterX;
    for (uint32_t ch : shown) {
        const std::wstring name = lc_channel_name(ds_, ch);
        const float w = MeasureText(dw_.get(), name, fMono_.get());
        if (lx + w + S(30.0f) > noteL) break;
        const D2D1_RECT_F swatch = Rect(lx, r.plot.top + legendH * 0.5f - S(2.0f),
                                        lx + S(14.0f), r.plot.top + legendH * 0.5f + S(1.5f));
        Fill(swatch, pal_.series[ch % 8]);
        DrawLabel(name, fMono_.get(),
                  Rect(swatch.right + S(6.0f), r.plot.top, swatch.right + S(6.0f) + w + S(4.0f),
                       r.plot.top + legendH),
                  pal_.ink2);
        lx = swatch.right + S(6.0f) + w + S(18.0f);
    }

    // 커서
    if (hoverX_ >= gutterX && hoverX_ <= rightX) {
        StrokeLine(Px(hoverX_), top, Px(hoverX_), bottom, pal_.ink3);
    }
    auto cursor = [&](double t, const D2D1_COLOR_F& c, const wchar_t* tag) {
        const float x = XOfTime(t, r.plot);
        if (x < gutterX - 1.0f || x > rightX + 1.0f) return;
        StrokeLine(Px(x), top, Px(x), bottom, c, S(1.5f));
        const D2D1_RECT_F tab = Rect(x - S(9.0f), top, x + S(9.0f), top + S(15.0f));
        Fill(tab, c);
        DrawLabel(tag, fSmall_.get(), Rect(tab.left + S(6.0f), tab.top, tab.right, tab.bottom),
                  pal_.onAccent);
    };
    if (hasA_) cursor(curA_, pal_.cursorA, L"A");
    if (hasB_) cursor(curB_, pal_.cursorB, L"B");

    DrawOverlayReadout(r, shown, lo, hi);
    rt_->PopAxisAlignedClip();
}

void App::DrawAxis(const Rects& r) {
    Fill(r.axis, pal_.surface);
    StrokeLine(r.axis.left, Px(r.axis.top), r.axis.right, Px(r.axis.top), pal_.hair);
    if (!ds_ || lc_sample_count(ds_) == 0) return;

    float gutterX = 0.0f, spanW = 10.0f;
    PlotSpan(r.plot, gutterX, spanW);
    const float rightX = gutterX + spanW;
    const bool timeLike = lc_time_kind(ds_) == LC_TIME_CLOCK_MS ||
                          lc_time_kind(ds_) == LC_TIME_DATE_MS;
    const Ticks ticks = MakeTicks(t0_, t1_,
                                  (std::max)(2, static_cast<int>((rightX - gutterX) / S(110.0f))),
                                  timeLike);

    for (double tv : ticks.at) {
        const float x = XOfTime(tv, r.plot);
        if (x < gutterX || x > rightX) continue;
        StrokeLine(Px(x), r.axis.top, Px(x), r.axis.top + S(4.0f), pal_.axis);
        const std::wstring label = FormatTime(tv, ticks.step);
        const float w = MeasureText(dw_.get(), label, fSmall_.get());
        DrawLabel(label, fSmall_.get(),
                  Rect(x - w * 0.5f - S(2.0f), r.axis.top + S(4.0f), x + w * 0.5f + S(6.0f),
                       r.axis.bottom),
                  pal_.ink3);
    }

    const LcTimeKind kind = lc_time_kind(ds_);
    std::wstring unit;
    if (kind == LC_TIME_INDEX) unit = L"샘플 번호";
    else if (kind == LC_TIME_NUMBER && *lc_time_unit(ds_)) unit = std::wstring(L"시간 [") + lc_time_unit(ds_) + L"]";
    if (!unit.empty()) {
        DrawLabel(unit, fSmallRight_.get(),
                  Rect(rightX, r.axis.top, r.axis.right - S(10.0f), r.axis.bottom), pal_.ink3);
    }
}

void App::DrawStatus(const Rects& r) {
    Fill(r.status, pal_.panel);
    StrokeLine(r.status.left, Px(r.status.top), r.status.right, Px(r.status.top), pal_.hair);

    // 오른쪽 커서 판독을 먼저 그리고, 왼쪽 글은 남는 폭에 맞춰 잘라낸다.
    // 두 글이 같은 줄에 있으므로 순서를 지키지 않으면 겹친다.
    std::wstring right;
    if (hasA_) right += L"A " + FormatTime(curA_, 0.0);
    if (hasB_) right += (right.empty() ? L"" : L"    ") + std::wstring(L"B ") + FormatTime(curB_, 0.0);
    if (hasA_ && hasB_) right += L"    Δt " + FormatSpan(std::fabs(curB_ - curA_));

    float rightW = 0.0f;
    if (!right.empty()) {
        rightW = MeasureText(dw_.get(), right, fMonoRight_.get()) + S(20.0f);
        DrawLabel(right, fMonoRight_.get(),
                  Rect(r.status.right - rightW, r.status.top, r.status.right - S(12.0f),
                       r.status.bottom),
                  pal_.ink);
    }

    const std::wstring left =
        message_.empty()
            ? std::wstring(mode_ == PlotMode::Overlay
                               ? L"휠 = 확대·축소 · 드래그 = 이동 · 클릭 = 커서 A · Shift+클릭 = 커서 B"
                               : L"Ctrl+휠 = 확대·축소 · 휠 = 채널 스크롤 · 드래그 = 이동")
            : message_;
    const float leftL = r.status.left + S(10.0f);
    const float avail = (r.status.right - rightW) - leftL - S(8.0f);
    if (avail > S(40.0f)) {
        DrawLabel(Ellipsize(dw_.get(), left, fUi_.get(), avail), fUi_.get(),
                  Rect(leftL, r.status.top, leftL + avail, r.status.bottom),
                  messageIsError_ ? pal_.cursorB : pal_.ink3);
    }
}

// ===========================================================================
// 입력
// ===========================================================================

void App::OnButton(ButtonId id) {
    switch (id) {
        case ButtonId::Open: OpenFileDialog(); break;
        case ButtonId::CancelLoad:
            if (loadJob_) loadJob_->cancel.store(true);
            break;
        case ButtonId::OpenCompare: OpenCompareDialog(); break;
        case ButtonId::CloseCompare:
            CloseCompare();
            message_ = ds_ ? lc_notes(ds_) : L"";
            messageIsError_ = false;
            break;
        case ButtonId::CompareBoth: compareMode_ = CompareMode::Both; break;
        case ButtonId::CompareDiff: compareMode_ = CompareMode::Diff; break;
        case ButtonId::Stagger: stagger_ = !stagger_; break;
        case ButtonId::MetricSamples:  metric_ = DiffMetric::Samples; break;
        case ButtonId::MetricTimeFrac: metric_ = DiffMetric::TimeFrac; break;
        case ButtonId::MetricPeak:     metric_ = DiffMetric::Peak; break;
        case ButtonId::MetricMean:     metric_ = DiffMetric::Mean; break;
        case ButtonId::MetricRms:      metric_ = DiffMetric::Rms; break;
        case ButtonId::MetricArea:     metric_ = DiffMetric::Area; break;
        case ButtonId::MetricRuns:     metric_ = DiffMetric::Runs; break;
        case ButtonId::ToleranceCycle: {
            // 0.1% -> 1% -> 5% -> 0.1% . 진동을 얼마나 걸러낼지 손으로 정한다.
            tolerance_ = (tolerance_ < 0.005) ? 0.01 : (tolerance_ < 0.03 ? 0.05 : 0.001);
            if (dsB_) RebuildComparison();
            break;
        }
        case ButtonId::AlignLeft:  NudgeAlign(-1); break;
        case ButtonId::AlignRight: NudgeAlign(1); break;
        case ButtonId::AlignAuto:  AutoAlignCompare(); break;
        case ButtonId::AlignReset: ResetAlign(); break;
        case ButtonId::OrientAuto:
        case ButtonId::OrientRows:
        case ButtonId::OrientCols: {
            const uint32_t want = (id == ButtonId::OrientAuto)   ? LC_ORIENT_AUTO
                                : (id == ButtonId::OrientRows)   ? LC_ORIENT_ROWS
                                                                 : LC_ORIENT_COLS;
            if (orientation_ == want) break;
            orientation_ = want;
            // 두 로그를 같은 배치로 읽어야 채널이 맞는다. 비교 중이면 둘 다.
            if (!lastPath_.empty()) {
                const std::wstring b = lastPathB_;
                LoadPath(lastPath_);
                if (!b.empty()) LoadComparePath(b);
            }
            break;
        }
        case ButtonId::ModeLanes:
        case ButtonId::ModeOverlay: {
            const PlotMode want = (id == ButtonId::ModeLanes) ? PlotMode::Lanes
                                                              : PlotMode::Overlay;
            if (mode_ == want) break;
            mode_ = want;
            scrollPlot_ = 0.0f;
            break;
        }
        case ButtonId::Normalize: normalize_ = !normalize_; break;
        case ButtonId::ZoomIn:  if (ds_) ZoomAt(ZoomAnchorX(), 0.75); break;
        case ButtonId::ZoomOut: if (ds_) ZoomAt(ZoomAnchorX(), 1.0 / 0.75); break;
        case ButtonId::YFitVisible: yFitVisible_ = !yFitVisible_; break;
        case ButtonId::Fit: if (ds_) ResetViewToData(); break;
        case ButtonId::ClearCursors: hasA_ = hasB_ = false; break;
        case ButtonId::SelectAll:
            // 필터가 걸려 있으면 지금 목록에 보이는 것만 켠다.
            for (uint32_t i = 0; i < selected_.size(); ++i) {
                if (ChannelVisibleInList(i)) selected_[i] = true;
            }
            scrollPlot_ = 0.0f;
            break;
        case ButtonId::SelectNone:
            for (size_t i = 0; i < selected_.size(); ++i) selected_[i] = false;
            scrollPlot_ = 0.0f;
            break;
        case ButtonId::GroupsExpand:
            for (size_t i = 0; i < groupOpen_.size(); ++i) groupOpen_[i] = true;
            scrollRail_ = 0.0f;
            break;
        case ButtonId::GroupsCollapse:
            for (size_t i = 0; i < groupOpen_.size(); ++i) groupOpen_[i] = false;
            scrollRail_ = 0.0f;
            break;
        case ButtonId::FilterAll:     filter_ = -1; scrollRail_ = 0.0f; break;
        case ButtonId::FilterDigital: filter_ = LC_CH_DIGITAL; scrollRail_ = 0.0f; break;
        case ButtonId::FilterAnalog:  filter_ = LC_CH_ANALOG; scrollRail_ = 0.0f; break;
        case ButtonId::FilterState:   filter_ = LC_CH_STATE; scrollRail_ = 0.0f; break;
        case ButtonId::FilterChanged: filter_ = -2; scrollRail_ = 0.0f; break;
        default: break;
    }
    // 척도를 바꾸면 값은 이미 계산돼 있으므로 다시 훑을 필요가 없지만, 요약
    // 문구에 척도 이름이 들어가므로 그 줄만 다시 만든다.
    if (HasCompare() && !messageIsError_) {
        const uint32_t n = lc_channel_count(ds_);
        uint32_t matched = 0, changed = 0;
        for (uint32_t i = 0; i < n; ++i) {
            if (i < diffStats_.size() && diffStats_[i].matched) ++matched;
            if (i < diffStats_.size() && diffStats_[i].samples > 0) ++changed;
        }
        compareSummary_ = Fmt(
            L"이후 로그 %s · 이름이 맞은 채널 %u/%u · 값이 달라진 채널 %u · "
            L"이전에만 있음 %u · 이후에만 있음 %u · 차이 기준 %s · 허용 오차 %.1f%%",
            fileNameB_.c_str(), matched, n, changed, n - matched,
            static_cast<uint32_t>(extraB_.size()), MetricName(), tolerance_ * 100.0);
        if (compareOffset_ != 0.0) {
            compareSummary_ += L" · 시간 보정 " + FormatSpan(std::fabs(compareOffset_)) +
                               (compareOffset_ < 0 ? L" 당김" : L" 밀음");
        }
        message_ = compareSummary_;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::OnLButtonDown(float x, float y, bool shift) {
    SetFocus(hwnd_);

    // 검색 상자 안이면 그 자리에 글자 커서를 놓는다.
    {
        const Rects rr = CalcRects();
        const D2D1_RECT_F box = SearchRect(rr);
        if (Inside(box, x, y)) {
            searchFocused_ = true;
            caretTick_ = GetTickCount64();
            const float tx = box.left + S(8.0f);
            caret_ = query_.size();
            for (size_t i = 0; i <= query_.size(); ++i) {
                if (tx + MeasureText(dw_.get(), query_.substr(0, i), fUi_.get()) >= x) {
                    caret_ = (i > 0) ? i - 1 : 0;
                    break;
                }
            }
            UpdateImePosition();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (searchFocused_) {
            searchFocused_ = false;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }
    for (const Button& b : buttons_) {
        if (!b.isLabel && b.id != ButtonId::None && Inside(b.rect, x, y)) {
            OnButton(b.id);
            return;
        }
    }

    const Rects r = CalcRects();

    if (ds_ && Inside(r.railList, x, y)) {
        const float rowH = S(metrics::kRowH);
        float ry = r.railList.top - scrollRail_;
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;

        for (const RailRow& row : railRows_) {
            const bool hit = (y >= ry && y < ry + rowH);
            ry += rowH;
            if (!hit) continue;

            if (row.kind == RailRow::Kind::Group) {
                // 왼쪽 삼각형 자리를 누르면 펼침, 체크 상자를 누르면 그룹 전체 선택,
                // 나머지 이름 부분은 펼침으로 친다 (더 자주 쓰는 동작).
                const float boxL = r.rail.left + S(26.0f);
                const float boxR = boxL + S(12.0f);
                if (x >= boxL - S(3.0f) && x <= boxR + S(3.0f)) {
                    uint32_t visible = 0, selected = 0;
                    GroupCounts(row.index, visible, selected);
                    SetGroupSelected(row.index, selected < visible);
                } else {
                    ToggleGroup(row.index);
                }
            } else if (row.kind == RailRow::Kind::Channel) {
                const float boxL = r.rail.left + S(26.0f);
                const float boxR = boxL + S(12.0f);
                const bool onCheckbox = (x >= boxL - S(3.0f) && x <= boxR + S(3.0f));
                ClickChannel(row.index, shift, ctrl, onCheckbox);
            }
            // ExtraHeader / ExtraChannel 은 이전 로그에 짝이 없어 그릴 수 없다.

            const float maxScroll =
                (std::max)(0.0f, TotalLaneHeight() - (r.plot.bottom - r.plot.top));
            scrollPlot_ = (std::min)(scrollPlot_, maxScroll);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        return;
    }

    if (ds_ && Inside(r.plot, x, y)) {
        dragging_ = true;
        dragMoved_ = false;
        dragShift_ = shift;
        dragStartX_ = x;
        dragT0_ = t0_;
        dragT1_ = t1_;
        SetCapture(hwnd_);
    }
}

void App::OnLButtonUp(float x, float y, bool shift) {
    if (!dragging_) return;
    dragging_ = false;
    ReleaseCapture();

    const Rects r = CalcRects();
    const float gutterX = r.plot.left + S(metrics::kNameGutter);
    const float rightX = r.plot.right - S(metrics::kValueGutter);
    if (!dragMoved_ && ds_ && x >= gutterX && x <= rightX && Inside(r.plot, x, y)) {
        const double t = TimeOfX(x, r.plot);
        const double* times = lc_times(ds_);
        const uint32_t n = lc_sample_count(ds_);
        if (times && n > 0 && t >= times[0] && t <= times[n - 1]) {
            if (shift || dragShift_) { curB_ = t; hasB_ = true; }
            else { curA_ = t; hasA_ = true; }
        }
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::OnMouseMove(float x, float y, bool /*dragging*/) {
    hoverX_ = x;
    hoverY_ = y;

    ButtonId hot = ButtonId::None;
    for (const Button& b : buttons_) {
        if (!b.isLabel && b.id != ButtonId::None && Inside(b.rect, x, y)) {
            hot = b.id;
            break;
        }
    }
    const bool hotChanged = (hot != hotButton_);
    hotButton_ = hot;

    if (dragging_ && ds_) {
        const float dx = x - dragStartX_;
        if (std::fabs(dx) > S(3.0f)) dragMoved_ = true;
        if (dragMoved_) {
            const Rects r = CalcRects();
            const float leftX = r.plot.left + S(metrics::kNameGutter);
            const float w = (std::max)(r.plot.right - S(metrics::kValueGutter) - leftX, 10.0f);
            const double dt = static_cast<double>(dx / w) * (dragT1_ - dragT0_);
            ClampView(dragT0_ - dt, dragT1_ - dt);
        }
    }
    // 다시 그릴 이유가 있을 때만 그린다. 예전에는 데이터가 열려 있기만 하면
    // 마우스가 어디로 움직이든 창 전체를 다시 그렸다.
    const Rects rc = CalcRects();
    const bool overPlot = ds_ && Inside(rc.plot, x, y);
    if (hotChanged || dragging_ || overPlot || hoverWasInPlot_) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    hoverWasInPlot_ = overPlot;
}

void App::OnWheel(float x, float y, int delta, bool ctrl) {
    const Rects r = CalcRects();
    const float notches = static_cast<float>(delta) / WHEEL_DELTA;

    if (Inside(r.rail, x, y)) {
        const float maxScroll = (std::max)(0.0f, TotalRailHeight() -
                                                     (r.railList.bottom - r.railList.top));
        scrollRail_ = (std::min)((std::max)(scrollRail_ - notches * S(60.0f), 0.0f), maxScroll);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (!ds_) return;

    // 겹쳐보기는 세로로 스크롤할 것이 없으므로 휠을 바로 확대에 쓴다.
    if (ctrl || mode_ == PlotMode::Overlay) {
        // 한 칸에 18%씩 줄던 것을 8%로 낮췄다. 좁은 구간을 볼 때 한 번만 굴려도
        // 훌쩍 지나가 버려서 원하는 배율에 맞추기 어려웠다. 굴린 양에 비례하도록
        // 거듭제곱을 쓰므로, 빠르게 여러 칸 굴리면 그만큼 크게 움직인다.
        ZoomAt(x, std::pow(0.92, static_cast<double>(notches)));
    } else {
        const float maxScroll = (std::max)(0.0f, TotalLaneHeight() -
                                                     (r.plot.bottom - r.plot.top));
        scrollPlot_ = (std::min)((std::max)(scrollPlot_ - notches * S(70.0f), 0.0f), maxScroll);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::OnKey(WPARAM key) {
    if (!ds_) return;
    const Rects r = CalcRects();
    const float mid = (r.plot.left + S(metrics::kNameGutter) +
                       r.plot.right - S(metrics::kValueGutter)) * 0.5f;
    const double span = t1_ - t0_;

    switch (key) {
        case VK_OEM_PLUS: case VK_ADD:      ZoomAt(mid, 0.85); break;
        case VK_OEM_MINUS: case VK_SUBTRACT: ZoomAt(mid, 1.0 / 0.85); break;
        case VK_LEFT:  ClampView(t0_ - span * 0.15, t1_ - span * 0.15); break;
        case VK_RIGHT: ClampView(t0_ + span * 0.15, t1_ + span * 0.15); break;
        case VK_HOME: case '0': ResetViewToData(); break;
        case VK_PRIOR: OnWheel(hoverX_, hoverY_, WHEEL_DELTA * 3, false); return;
        case VK_NEXT:  OnWheel(hoverX_, hoverY_, -WHEEL_DELTA * 3, false); return;
        default: return;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ===========================================================================
// 메시지
// ===========================================================================

LRESULT App::HandleMessage(UINT msg, WPARAM wp, LPARAM lp) {
    const float mx = static_cast<float>(GET_X_LPARAM(lp));
    const float my = static_cast<float>(GET_Y_LPARAM(lp));

    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd_, &ps);
            Render();
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;  // Direct2D 가 전부 칠하므로 깜빡임을 막는다

        case WM_SIZE:
            if (rt_) {
                rt_->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;

        case WM_DPICHANGED: {
            UpdateDpi(HIWORD(wp));
            const RECT* target = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd_, nullptr, target->left, target->top,
                         target->right - target->left, target->bottom - target->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }

        case kMsgLoadDone: {
            auto* handoff = reinterpret_cast<std::shared_ptr<LoadJob>*>(lp);
            if (handoff) {
                FinishLoad(*handoff);
                delete handoff;
            }
            return 0;
        }

        case WM_TIMER:
            if (wp == kTimerSpin || wp == kTimerCaret) {
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            break;

        case WM_SETTINGCHANGE:
            ApplySystemTheme();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;

        case WM_LBUTTONDOWN:
            OnLButtonDown(mx, my, (wp & MK_SHIFT) != 0);
            return 0;
        case WM_LBUTTONUP:
            OnLButtonUp(mx, my, (wp & MK_SHIFT) != 0);
            return 0;
        case WM_LBUTTONDBLCLK:
            if (ds_) { ResetViewToData(); InvalidateRect(hwnd_, nullptr, FALSE); }
            return 0;
        case WM_MOUSEMOVE: {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
            TrackMouseEvent(&tme);
            OnMouseMove(mx, my, dragging_);
            return 0;
        }
        case WM_MOUSELEAVE:
            hoverX_ = hoverY_ = -1.0f;
            hotButton_ = ButtonId::None;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;

        case WM_MOUSEWHEEL: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd_, &pt);
            OnWheel(static_cast<float>(pt.x), static_cast<float>(pt.y),
                    GET_WHEEL_DELTA_WPARAM(wp), (wp & MK_CONTROL) != 0);
            return 0;
        }

        case WM_CHAR:
            if (searchFocused_) {
                const wchar_t c = static_cast<wchar_t>(wp);
                if (c == 8) {   // 백스페이스
                    caret_ = (std::min)(caret_, query_.size());
                    if (caret_ > 0) {
                        query_.erase(query_.begin() + static_cast<std::ptrdiff_t>(caret_ - 1));
                        --caret_;
                        scrollRail_ = 0.0f;
                        caretTick_ = GetTickCount64();
                    }
                } else if (c >= 0x20) {
                    InsertSearchText(c);
                }
                UpdateImePosition();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            break;

        case WM_KEYDOWN:
            if (wp == 'O' && (GetKeyState(VK_CONTROL) & 0x8000)) { OpenFileDialog(); return 0; }
            if (searchFocused_) {
                // 검색 중에는 방향키가 글자 커서를 옮긴다. 시간축으로 새어 나가면
                // 글자를 지우려다 그래프가 움직인다.
                OnSearchKey(wp);
                UpdateImePosition();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            OnKey(wp);
            return 0;

        case WM_DROPFILES: {
            HDROP drop = reinterpret_cast<HDROP>(wp);
            // 두 개를 한 번에 떨어뜨리면 첫 번째가 이전 로그, 두 번째가 이후 로그.
            const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            wchar_t first[MAX_PATH] = {0};
            wchar_t second[MAX_PATH] = {0};
            const bool got1 = count > 0 && DragQueryFileW(drop, 0, first, MAX_PATH) > 0;
            const bool got2 = count > 1 && DragQueryFileW(drop, 1, second, MAX_PATH) > 0;
            DragFinish(drop);
            if (got1) {
                // 이미 이전 로그가 있고 한 개만 떨어뜨렸으면 비교용으로 받는다.
                if (!got2 && ds_ != nullptr) LoadComparePath(first);
                else LoadPath(first);
            }
            if (got2) LoadComparePath(second);
            SetForegroundWindow(hwnd_);
            return 0;
        }

        case WM_DESTROY:
            // 읽는 중이면 멈추라고 알리고 스레드가 끝날 때까지 기다린다. 기다리지
            // 않으면 스레드가 사라진 창에 메시지를 보내려 한다.
            if (loadJob_) loadJob_->cancel.store(true);
            KillTimer(hwnd_, kTimerSpin);
            KillTimer(hwnd_, kTimerCaret);
            if (loadThread_.joinable()) loadThread_.join();
            loadJob_.reset();
            CloseCompare();
            CloseDataset();
            DiscardDeviceResources();
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

LRESULT CALLBACK App::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    App* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<App*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->HandleMessage(msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace app
