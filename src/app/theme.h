// theme.h — 색과 치수. Windows 의 앱 테마(밝게/어둡게) 설정을 따라간다.
#pragma once

#include <d2d1.h>

namespace app {

struct Palette {
    D2D1_COLOR_F plane;       // 창 배경
    D2D1_COLOR_F surface;     // 플롯 바탕
    D2D1_COLOR_F panel;       // 툴바 · 레일 · 상태바
    D2D1_COLOR_F ink;         // 본문 글자
    D2D1_COLOR_F ink2;        // 보조 글자
    D2D1_COLOR_F ink3;        // 흐린 글자 (축, 라벨)
    D2D1_COLOR_F grid;        // 격자선
    D2D1_COLOR_F axis;        // 축선
    D2D1_COLOR_F hair;        // 구분선
    D2D1_COLOR_F laneAlt;     // 짝수 레인 배경
    D2D1_COLOR_F hover;       // 마우스 오버 / 눌린 버튼
    D2D1_COLOR_F accent;      // 파형 색
    D2D1_COLOR_F onAccent;    // 강조색 위 글자
    D2D1_COLOR_F cursorA;
    D2D1_COLOR_F cursorB;
    D2D1_COLOR_F series[8];   // 상태 리본용 범주 색
};

inline D2D1_COLOR_F Rgb(UINT32 hex, float a = 1.0f) {
    return D2D1::ColorF(hex, a);
}

// 라이트 팔레트. 중성색은 살짝 따뜻한 쪽으로 잡아 순회색 특유의 인상을 피한다.
inline Palette LightPalette() {
    Palette p{};
    p.plane    = Rgb(0xF1F0EC);
    p.surface  = Rgb(0xFCFCFB);
    p.panel    = Rgb(0xF7F6F3);
    p.ink      = Rgb(0x0B0B0B);
    p.ink2     = Rgb(0x52514E);
    p.ink3     = Rgb(0x898781);
    p.grid     = Rgb(0xE1E0D9);
    p.axis     = Rgb(0xC3C2B7);
    p.hair     = Rgb(0x0B0B0B, 0.10f);
    p.laneAlt  = Rgb(0x0B0B0B, 0.022f);
    p.hover    = Rgb(0x0B0B0B, 0.055f);
    p.accent   = Rgb(0x2A78D6);
    p.onAccent = Rgb(0xFFFFFF);
    p.cursorA  = Rgb(0x2A78D6);
    p.cursorB  = Rgb(0xEB6834);
    const UINT32 s[8] = {0x2A78D6, 0xEB6834, 0x1BAF7A, 0xEDA100,
                         0xE87BA4, 0x008300, 0x4A3AA7, 0xE34948};
    for (int i = 0; i < 8; ++i) p.series[i] = Rgb(s[i]);
    return p;
}

// 다크 팔레트. 같은 여덟 색을 어두운 바탕에 맞춰 단계만 옮긴 것이다.
inline Palette DarkPalette() {
    Palette p{};
    p.plane    = Rgb(0x0D0D0D);
    p.surface  = Rgb(0x1A1A19);
    p.panel    = Rgb(0x141413);
    p.ink      = Rgb(0xFFFFFF);
    p.ink2     = Rgb(0xC3C2B7);
    p.ink3     = Rgb(0x898781);
    p.grid     = Rgb(0x2C2C2A);
    p.axis     = Rgb(0x383835);
    p.hair     = Rgb(0xFFFFFF, 0.11f);
    p.laneAlt  = Rgb(0xFFFFFF, 0.026f);
    p.hover    = Rgb(0xFFFFFF, 0.065f);
    p.accent   = Rgb(0x3987E5);
    p.onAccent = Rgb(0xFFFFFF);
    p.cursorA  = Rgb(0x3987E5);
    p.cursorB  = Rgb(0xD95926);
    const UINT32 s[8] = {0x3987E5, 0xD95926, 0x199E70, 0xC98500,
                         0xD55181, 0x008300, 0x9085E9, 0xE66767};
    for (int i = 0; i < 8; ++i) p.series[i] = Rgb(s[i]);
    return p;
}

// 96 DPI 기준 치수. 실제 값은 App 이 DPI 배율을 곱해서 쓴다.
namespace metrics {
constexpr float kToolbarH = 44.f;
constexpr float kRailW = 292.f;
constexpr float kAxisH = 28.f;
constexpr float kStatusH = 26.f;
constexpr float kLaneDigital = 34.f;
constexpr float kLaneState = 34.f;
constexpr float kLaneAnalog = 58.f;
constexpr float kNameGutter = 150.f;
constexpr float kValueGutter = 108.f;
constexpr float kRowH = 24.f;      // 채널 목록 한 줄
constexpr float kSearchH = 26.f;
}  // namespace metrics

}  // namespace app
