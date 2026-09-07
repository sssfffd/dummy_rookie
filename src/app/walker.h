// walker.h — 다른 로그의 값을 시간 순서대로 읽어 가는 커서.
//
// 같은 일을 lc_sample_at 으로 하면 표본마다 이진 탐색 한 번과 DLL 호출 한 번이
// 붙는다. 묻는 시각이 늘 커지기만 하는 자리(그리기, 비교 통계)에서는 커서를 앞으로
// 밀기만 하면 되므로, 표본 수 n·m 에 대해 O(n log m) 이 O(n + m) 이 된다.
//
// 값을 고르는 규칙은 lc_sample_at 과 **똑같아야 한다**: t 이하인 마지막 표본을
// 쓰고, 아날로그만 다음 표본과 선형 보간한다. 창이 없는 곳에서도 시험할 수 있게
// 이 헤더에는 Windows 의존이 없다.
#pragma once

#include <cmath>
#include <cstdint>
#include <limits>

namespace app {

struct Walker {
    const double* t = nullptr;
    const double* v = nullptr;
    uint32_t n = 0;
    bool analog = false;
    uint32_t i = 0;

    double At(double x) {
        const double nan = std::numeric_limits<double>::quiet_NaN();
        if (!t || !v || n == 0 || !std::isfinite(x)) return nan;
        if (x < t[0] || x > t[n - 1]) return nan;
        while (i + 1 < n && t[i + 1] <= x) ++i;
        while (i > 0 && t[i] > x) --i;
        const double v0 = v[i];
        if (!analog || i + 1 >= n || !std::isfinite(v0)) return v0;
        const double v1 = v[i + 1];
        if (!std::isfinite(v1)) return v0;
        const double dt = t[i + 1] - t[i];
        if (!(dt > 0.0)) return v0;
        return v0 + (v1 - v0) * (x - t[i]) / dt;
    }
};

}  // namespace app
