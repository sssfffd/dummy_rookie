// app.h — 창 하나짜리 애플리케이션. 상태를 모두 들고 있으면서 Direct2D 로 직접
// 그리고 입력을 처리한다. 데이터는 전부 logcore.dll 이 소유한다.
#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "logcore/logcore.h"
#include "theme.h"

namespace app {

// 최소 COM 스마트 포인터 (코어 쪽과 같은 이유로 직접 둔다).
template <class T>
class Ptr {
public:
    Ptr() = default;
    Ptr(const Ptr&) = delete;
    Ptr& operator=(const Ptr&) = delete;
    ~Ptr() { reset(); }
    void reset() { if (p_) { p_->Release(); p_ = nullptr; } }
    T** put() { reset(); return &p_; }
    void** put_void() { reset(); return reinterpret_cast<void**>(&p_); }
    T* get() const { return p_; }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
private:
    T* p_ = nullptr;
};

enum class ButtonId {
    None, Open, OpenCompare, CloseCompare,
    OrientAuto, OrientRows, OrientCols,
    ModeLanes, ModeOverlay, Normalize,
    CompareBoth, CompareDiff,
    ZoomIn, ZoomOut, Fit, ClearCursors,
    SelectAll, SelectNone, FilterAll, FilterDigital, FilterAnalog, FilterState,
    FilterChanged,
    GroupsExpand, GroupsCollapse,
    AlignAuto, AlignReset, AlignLeft, AlignRight, Stagger,
    YFitVisible, CancelLoad,
    MetricSamples, MetricTimeFrac, MetricPeak, MetricMean, MetricRms, MetricArea, MetricRuns,
    ToleranceCycle
};

// 파일 읽기는 별도 스레드에서 한다. UI 스레드에서 하면 큰 파일에서 창이 통째로
// 멎어 버려서, 읽는 중인지 죽은 것인지 구분할 수가 없다.
struct LoadJob {
    std::wstring path;
    uint32_t orientation = 0;
    int slot = 0;                    // 0 = 이전 로그, 1 = 이후 로그
    LcStatus status = LC_OK;
    LcDataset* ds = nullptr;
    std::atomic<unsigned long long> done{0};   // 지금까지 읽은 줄 수
    std::atomic<unsigned long long> total{0};
    std::atomic<bool> cancel{false};
};

// 레인: 채널마다 한 줄, 각자 세로 눈금. 파형을 훑어볼 때.
// 겹쳐보기: 가로축 시간, 세로축 값 하나를 여러 채널이 공유. 값을 비교할 때.
enum class PlotMode { Lanes, Overlay };

// 비교 로그를 열었을 때 무엇을 그릴지.
//   Both — 이전과 이후를 겹쳐 그린다. 무엇이 어떻게 달라졌는지 눈으로 본다.
//   Diff — 이후에서 이전을 뺀 값만 그린다. 달라진 구간만 도드라진다.
enum class CompareMode { Both, Diff };

// 두 로그의 차이를 무엇으로 재는가.
//
// 처음에는 "다른 샘플 수" 하나만 있었는데, +1/−1 이 빠르게 오가는 진동이 있으면
// 샘플마다 카운트가 올라가서 실제로는 사소한 차이가 가장 큰 값으로 보인다.
// 성질이 다른 척도를 함께 두고 고를 수 있게 한다.
enum class DiffMetric {
    Samples,   // 다른 샘플 수. 얼마나 자주 어긋났는가
    TimeFrac,  // 다른 시간 비율(%). 샘플 수가 다른 로그끼리도 견줄 수 있다
    Peak,      // 최대 |Δ|. 진동 폭이 작으면 작게 나온다 — 진동에 가장 둔감
    Mean,      // 평균 |Δ|. 전체적으로 얼마나 벌어졌는가
    Rms,       // RMS. 큰 차이에 더 무게를 준다
    Area,      // ∫|Δ|dt. 시간으로 가중 — 짧은 진동은 작게 남는다
    Runs       // 다른 구간의 개수. 값이 크면 한 번의 변화가 아니라 진동이다
};

// 채널 하나에 대한 차이 통계. 한 번 훑으면서 모두 채운다.
struct DiffStats {
    uint32_t samples = 0;
    uint32_t runs = 0;
    double timeFrac = 0.0;   // 0..1
    double peak = 0.0;
    double mean = 0.0;
    double rms = 0.0;
    double area = 0.0;
    bool matched = false;    // 이후 로그에 짝이 있는가
};

// 겹쳐보기에서 한 번에 그릴 수 있는 채널 수. 범주형 색이 여덟 개까지만
// 서로 구분되므로 그 이상은 색으로 구별이 안 된다.
constexpr uint32_t kMaxOverlay = 8;

// 채널 목록을 이 개수씩 묶어 접었다 폈다 한다. 200채널 로그에서 목록을 훑는
// 것보다 그룹 단위로 여닫는 쪽이 빠르다.
constexpr uint32_t kGroupSize = 10;

// 왼쪽 목록의 한 줄. 그룹 머리, 채널, 그리고 "이후 로그에만 있는" 채널까지
// 한 배열로 다뤄야 스크롤과 클릭 판정이 한곳에 모인다.
struct RailRow {
    enum class Kind : uint8_t { Group, Channel, ExtraHeader, ExtraChannel };
    Kind kind = Kind::Channel;
    uint32_t index = 0;   // Group: 그룹 번호, Channel: 이전 로그 채널, ExtraChannel: 이후 로그 채널
};

struct Button {
    ButtonId id = ButtonId::None;
    D2D1_RECT_F rect{};
    std::wstring label;
    bool pressed = false;   // 세그먼트 토글의 선택 상태
    bool accent = false;
    bool isLabel = false;   // 누를 수 없는 묶음 이름 ("보기", "비교" 등)
};

struct Rects {
    D2D1_RECT_F toolbar{}, controls{}, rail{}, railList{}, plot{}, axis{}, status{};
};

class App {
public:
    // 명령줄 인자. 첫 번째는 이전 로그, 두 번째가 있으면 비교할 이후 로그.
    bool Create(HINSTANCE inst, int show, const wchar_t* initialPath,
                const wchar_t* comparePath);
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

private:
    // ---- 창 / 자원 ----
    bool CreateDeviceResources();
    void DiscardDeviceResources();
    bool CreateTextFormats();
    void ApplySystemTheme();
    void UpdateDpi(UINT dpi);
    float S(float v) const { return v * dpi_ / 96.0f; }

    // ---- 레이아웃 ----
    Rects CalcRects() const;
    // 버튼 배치는 두 단계다. 위쪽(툴바 + 컨트롤 줄)은 창 너비만 알면 되고,
    // 그 결과로 컨트롤 줄 높이가 정해져야 나머지 영역을 계산할 수 있다.
    void RebuildTopButtons(float clientWidth);
    void RebuildRailButtons(const Rects& r);
    // 검색 상자는 직접 그린다. Win32 자식 컨트롤을 쓰면 Direct2D 가 매 프레임
    // 창 전체를 다시 올리면서 그 위를 덮어써서 계속 깜빡인다.
    D2D1_RECT_F SearchRect(const Rects& r) const;
    void DrawSearchBox(const Rects& r);
    void InsertSearchText(wchar_t c);
    void OnSearchKey(WPARAM key);
    void UpdateImePosition();
    float LaneHeight(LcChannelType t) const;
    float TotalLaneHeight() const;
    float TotalRailHeight() const;

    // ---- 그리기 ----
    void Render();
    void DrawToolbar(const Rects& r);
    void DrawControls(const Rects& r);
    void DrawRail(const Rects& r);
    void DrawPlot(const Rects& r);
    void DrawLanesView(const Rects& r);
    void DrawOverlayView(const Rects& r);
    void DrawOverlayReadout(const Rects& r, const std::vector<uint32_t>& shown,
                            double lo, double hi);
    void DrawAxis(const Rects& r);
    void DrawStatus(const Rects& r);
    void DrawEmptyState(const Rects& r);
    void DrawLaneDigital(uint32_t ch, D2D1_RECT_F lane, const D2D1_RECT_F& plot);
    void DrawLaneAnalog(uint32_t ch, D2D1_RECT_F lane, const D2D1_RECT_F& plot);
    void DrawLaneState(uint32_t ch, D2D1_RECT_F lane, const D2D1_RECT_F& plot);
    // 이전 로그의 시간 격자를 따라가며 이후 로그(또는 차이)를 그린다.
    void DrawResampled(uint32_t ch, const D2D1_RECT_F& plot, float top, float bottom,
                       double lo, double hi, const D2D1_COLOR_F& color, bool diff,
                       float thickness);
    // 이전과 이후 사이를 옅게 칠해 벌어진 만큼을 면적으로 보여 준다.
    void DrawDifferenceBand(uint32_t ch, const D2D1_RECT_F& plot, float top, float bottom,
                            double lo, double hi, const D2D1_COLOR_F& color);
    // 겹쳐보기용: 하나의 세로 눈금 위에 채널 하나를 그린다.
    void DrawSeries(uint32_t ch, const D2D1_RECT_F& plot, float top, float bottom,
                    double lo, double hi, const D2D1_COLOR_F& color);
    // Win32 의 DrawText 매크로와 부딪히지 않도록 이름을 달리한다.
    void DrawLabel(const std::wstring& s, IDWriteTextFormat* fmt,
                   const D2D1_RECT_F& box, const D2D1_COLOR_F& color);
    void DrawButton(const Button& b, bool hot);
    void Fill(const D2D1_RECT_F& r, const D2D1_COLOR_F& c);
    void StrokeLine(float x0, float y0, float x1, float y1,
                    const D2D1_COLOR_F& c, float w = 1.0f);

    // ---- 데이터 ----
    void LoadPath(const std::wstring& path);
    // ---- 배경에서 읽기 ----
    void BeginLoad(const std::wstring& path, int slot);
    void FinishLoad(const std::shared_ptr<LoadJob>& job);
    bool IsLoading() const { return loadJob_ != nullptr; }
    void DrawLoadingOverlay(const Rects& r);
    void CloseDataset();
    void OpenFileDialog();
    std::wstring PickLogFile(const wchar_t* title);

    // ---- 두 로그 비교 ----
    void OpenCompareDialog();
    void LoadComparePath(const std::wstring& path);
    void CloseCompare();
    // 채널을 이름으로 맞추고, 채널마다 다른 샘플 수와 값 범위를 미리 구해 둔다.
    void RebuildComparison();
    bool HasCompare() const { return dsB_ != nullptr; }
    // 이전 로그의 시각 t 에 대응하는 이후 로그의 값. 없으면 NaN.
    double CompareValueAt(uint32_t ch, double t) const;
    // 이후 - 이전. 상태 채널은 값이 다르면 1, 같으면 0.
    double DiffValueAt(uint32_t ch, double t) const;
    bool ChannelDiffers(uint32_t ch) const;
    // 지금 고른 척도로 잰 값과, 목록에 넣을 짧은 표시
    double MetricValue(uint32_t ch) const;
    std::wstring MetricBadge(uint32_t ch) const;
    const wchar_t* MetricName() const;
    void ResetViewToData();
    void ClampView(double a, double b);
    float ZoomAnchorX() const;
    void ZoomAt(float clientX, double factor);
    void IndexRange(int& i0, int& i1) const;
    int  IndexAt(double t) const;
    bool ChannelVisibleInList(uint32_t ch) const;
    // ---- 왼쪽 목록 ----
    void RebuildRailRows();
    float RailHeaderHeight() const;
    void ToggleGroup(uint32_t group);
    void SetGroupSelected(uint32_t group, bool on);
    // 그룹 안에서 목록에 보이는 채널 수와 그중 선택된 수
    void GroupCounts(uint32_t group, uint32_t& visible, uint32_t& selected) const;
    // Shift/Ctrl 조합에 따른 선택 갱신
    void ClickChannel(uint32_t ch, bool shift, bool ctrl, bool onCheckbox);

    // ---- 두 로그 시간 맞추기 ----
    void AutoAlignCompare();
    void NudgeAlign(int steps);
    void ResetAlign();

    // 겹쳐보기에 실제로 그릴 채널 목록 (선택된 것 중 앞에서부터 kMaxOverlay 개)
    std::vector<uint32_t> OverlayChannels() const;
    // 현재 보이는 시간 구간에서의 값 범위. 확대하면 세로 눈금도 따라 좁혀진다.
    void OverlayRange(const std::vector<uint32_t>& shown, double& lo, double& hi) const;
    // 채널 하나의 값을 세로 눈금 좌표로. 정규화가 켜져 있으면 0..1 로 접는다.
    double SeriesValue(uint32_t ch, double raw) const;

    // ---- 좌표 ----
    // 모드에 따라 달라지는 플롯 가로 구간 (왼쪽 끝, 폭)
    void PlotSpan(const D2D1_RECT_F& plot, float& left, float& width) const;
    float XOfTime(double t, const D2D1_RECT_F& plot) const;
    double TimeOfX(float x, const D2D1_RECT_F& plot) const;

    // ---- 문자열 ----
    std::wstring FormatTime(double t, double step) const;
    std::wstring FormatSpan(double span) const;
    std::wstring FormatValue(uint32_t ch, double v) const;

    // ---- 입력 ----
    void OnLButtonDown(float x, float y, bool shift);
    void OnLButtonUp(float x, float y, bool shift);
    void OnMouseMove(float x, float y, bool dragging);
    void OnWheel(float x, float y, int delta, bool ctrl);
    void OnKey(WPARAM key);
    void OnButton(ButtonId id);

    LRESULT HandleMessage(UINT msg, WPARAM wp, LPARAM lp);

    // ---- 상태 ----
    HWND hwnd_ = nullptr;
    bool searchFocused_ = false;
    size_t caret_ = 0;                 // 검색 문자열 안에서의 글자 위치
    unsigned long long caretTick_ = 0; // 깜빡임 기준 시각
    float dpi_ = 96.0f;
    bool dark_ = false;
    Palette pal_ = LightPalette();

    Ptr<ID2D1Factory> d2d_;
    Ptr<IDWriteFactory> dw_;
    Ptr<ID2D1HwndRenderTarget> rt_;
    Ptr<ID2D1SolidColorBrush> brush_;
    Ptr<IDWriteTextFormat> fUi_, fUiCenter_, fMono_, fMonoRight_, fSmall_, fSmallRight_, fTitle_;

    std::shared_ptr<LoadJob> loadJob_;
    std::thread loadThread_;
    unsigned long long loadStartTick_ = 0;
    // 읽기는 한 번에 하나만 돈다. 파일 두 개를 한꺼번에 떨어뜨렸거나 배치를
    // 바꿔 둘 다 다시 읽어야 할 때를 위해 대기열을 둔다.
    std::vector<std::pair<std::wstring, int>> loadQueue_;

    LcDataset* ds_ = nullptr;          // 이전 로그 (비교하지 않을 때는 그냥 열린 로그)
    LcDataset* dsB_ = nullptr;         // 이후 로그
    std::wstring fileNameB_;
    std::vector<int32_t> matchB_;      // 이전 채널 -> 이후 채널 (없으면 -1)
    std::vector<uint32_t> diffCount_;  // 채널마다 값이 다른 샘플 수 (호환용)
    std::vector<DiffStats> diffStats_;
    DiffMetric metric_ = DiffMetric::Peak;
    // 값 범위 대비 이 비율보다 작은 차이는 같다고 본다. 진동을 걸러내는 손잡이.
    double tolerance_ = 0.001;
    std::vector<double> cmpLo_, cmpHi_;    // 이전·이후를 함께 담는 값 범위
    std::vector<double> diffLo_, diffHi_;  // 차이 값의 범위
    CompareMode compareMode_ = CompareMode::Both;
    std::vector<uint32_t> extraB_;     // 이후 로그에만 있는 채널
    double compareOffset_ = 0.0;       // 이후 로그 시간에 더할 보정값
    bool stagger_ = false;             // 겹칠 때 이후 선을 살짝 띄워 그린다
    std::wstring compareSummary_;
    std::wstring fileName_, message_;
    bool messageIsError_ = false;
    std::vector<bool> selected_;
    uint32_t orientation_ = LC_ORIENT_AUTO;
    std::wstring lastPath_, lastPathB_;

    double t0_ = 0.0, t1_ = 1.0;
    double curA_ = 0.0, curB_ = 0.0;
    bool hasA_ = false, hasB_ = false;

    float scrollPlot_ = 0.0f, scrollRail_ = 0.0f;
    float hoverX_ = -1.0f, hoverY_ = -1.0f;
    bool hoverWasInPlot_ = false;   // 플롯 밖으로 나간 첫 순간에만 한 번 더 그리려고
    bool dragging_ = false, dragMoved_ = false, dragShift_ = false;
    float dragStartX_ = 0.0f;
    double dragT0_ = 0.0, dragT1_ = 0.0;

    PlotMode mode_ = PlotMode::Lanes;
    bool normalize_ = false;
    // 세로 눈금을 보이는 구간에 맞출지. 꺼 두면 선택한 채널의 전체 범위로
    // 고정되어, 시간축을 옮겨도 배율이 갑자기 바뀌지 않는다.
    bool yFitVisible_ = false;
    float controlsH_ = 34.0f;   // 줄바꿈 결과로 정해지는 컨트롤 줄 높이 (픽셀)

    std::vector<RailRow> railRows_;
    std::vector<bool> groupOpen_;
    int32_t anchorChannel_ = -1;   // Shift 범위 선택의 기준점

    std::wstring query_;
    int filter_ = -1;  // -1 = 전체, -2 = 달라진 채널만, 아니면 LcChannelType
    std::vector<Button> buttons_;
    ButtonId hotButton_ = ButtonId::None;
};

}  // namespace app
