// app.h — 창 하나짜리 애플리케이션. 상태를 모두 들고 있으면서 Direct2D 로 직접
// 그리고 입력을 처리한다. 데이터는 전부 logcore.dll 이 소유한다.
#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

#include <string>
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
    None, Open, OrientAuto, OrientRows, OrientCols, Fit, ClearCursors,
    SelectAll, SelectNone, FilterAll, FilterDigital, FilterAnalog, FilterState
};

struct Button {
    ButtonId id = ButtonId::None;
    D2D1_RECT_F rect{};
    std::wstring label;
    bool pressed = false;   // 세그먼트 토글의 선택 상태
    bool accent = false;
};

struct Rects {
    D2D1_RECT_F toolbar{}, rail{}, railList{}, plot{}, axis{}, status{};
};

class App {
public:
    // initialPath 가 있으면 창을 띄운 뒤 그 파일을 연다 (명령줄 인자).
    bool Create(HINSTANCE inst, int show, const wchar_t* initialPath);
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
    void RebuildButtons(const Rects& r);
    void LayoutChildren(const Rects& r);
    float LaneHeight(LcChannelType t) const;
    float TotalLaneHeight() const;
    float TotalRailHeight() const;

    // ---- 그리기 ----
    void Render();
    void DrawToolbar(const Rects& r);
    void DrawRail(const Rects& r);
    void DrawPlot(const Rects& r);
    void DrawAxis(const Rects& r);
    void DrawStatus(const Rects& r);
    void DrawEmptyState(const Rects& r);
    void DrawLaneDigital(uint32_t ch, D2D1_RECT_F lane, const D2D1_RECT_F& plot);
    void DrawLaneAnalog(uint32_t ch, D2D1_RECT_F lane, const D2D1_RECT_F& plot);
    void DrawLaneState(uint32_t ch, D2D1_RECT_F lane, const D2D1_RECT_F& plot);
    // Win32 의 DrawText 매크로와 부딪히지 않도록 이름을 달리한다.
    void DrawLabel(const std::wstring& s, IDWriteTextFormat* fmt,
                   const D2D1_RECT_F& box, const D2D1_COLOR_F& color);
    void DrawButton(const Button& b, bool hot);
    void Fill(const D2D1_RECT_F& r, const D2D1_COLOR_F& c);
    void StrokeLine(float x0, float y0, float x1, float y1,
                    const D2D1_COLOR_F& c, float w = 1.0f);

    // ---- 데이터 ----
    void LoadPath(const std::wstring& path);
    void CloseDataset();
    void OpenFileDialog();
    void ResetViewToData();
    void ClampView(double a, double b);
    void ZoomAt(float clientX, double factor);
    void IndexRange(int& i0, int& i1) const;
    int  IndexAt(double t) const;
    bool ChannelVisibleInList(uint32_t ch) const;

    // ---- 좌표 ----
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
    void OnSearchChanged();

    LRESULT HandleMessage(UINT msg, WPARAM wp, LPARAM lp);

    // ---- 상태 ----
    HWND hwnd_ = nullptr;
    HWND search_ = nullptr;
    HFONT searchFont_ = nullptr;
    HBRUSH searchBg_ = nullptr;
    float dpi_ = 96.0f;
    bool dark_ = false;
    Palette pal_ = LightPalette();

    Ptr<ID2D1Factory> d2d_;
    Ptr<IDWriteFactory> dw_;
    Ptr<ID2D1HwndRenderTarget> rt_;
    Ptr<ID2D1SolidColorBrush> brush_;
    Ptr<IDWriteTextFormat> fUi_, fUiCenter_, fMono_, fMonoRight_, fSmall_, fSmallRight_, fTitle_;

    LcDataset* ds_ = nullptr;
    std::wstring fileName_, message_;
    bool messageIsError_ = false;
    std::vector<bool> selected_;
    uint32_t orientation_ = LC_ORIENT_AUTO;
    std::wstring lastPath_;

    double t0_ = 0.0, t1_ = 1.0;
    double curA_ = 0.0, curB_ = 0.0;
    bool hasA_ = false, hasB_ = false;

    float scrollPlot_ = 0.0f, scrollRail_ = 0.0f;
    float hoverX_ = -1.0f, hoverY_ = -1.0f;
    bool dragging_ = false, dragMoved_ = false, dragShift_ = false;
    float dragStartX_ = 0.0f;
    double dragT0_ = 0.0, dragT1_ = 0.0;

    std::wstring query_;
    int filter_ = -1;  // -1 = 전체, 아니면 LcChannelType
    std::vector<Button> buttons_;
    ButtonId hotButton_ = ButtonId::None;
};

}  // namespace app
