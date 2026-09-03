#include "text_util.h"

#include <cmath>
#include <cstdlib>
#include <cwctype>

namespace lc {
namespace {

bool is_space(wchar_t c) {
    return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == 0x00A0 || c == 0xFEFF;
}

// 자리수 문자열을 정수로. 오버플로 없이 최대 max_digits 자리까지만 읽는다.
bool read_uint(const wchar_t* p, size_t n, unsigned& out) {
    if (n == 0 || n > 9) return false;
    unsigned v = 0;
    for (size_t i = 0; i < n; ++i) {
        if (p[i] < L'0' || p[i] > L'9') return false;
        v = v * 10u + static_cast<unsigned>(p[i] - L'0');
    }
    out = v;
    return true;
}

// 1970-01-01 을 0 으로 하는 일수. Howard Hinnant 의 days_from_civil.
long long days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    // 3월을 0으로 놓은 달 번호. 원 논문은 (m + (m > 2 ? -3 : 9)) 로 쓰지만,
    // 부호 없는 값에 단항 빼기를 쓰면 MSVC 가 C4146 을 내고 /sdl 이 이를 오류로
    // 올린다. 뺄셈을 밖으로 빼서 부호 없는 연산만 남긴다.
    const unsigned mp = (m > 2) ? (m - 3u) : (m + 9u);
    const unsigned doy = (153u * mp + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return static_cast<long long>(era) * 146097LL + static_cast<long long>(doe) - 719468LL;
}

}  // namespace

std::wstring trim(const std::wstring& s) {
    size_t b = 0, e = s.size();
    while (b < e && is_space(s[b])) ++b;
    while (e > b && is_space(s[e - 1])) --e;
    return s.substr(b, e - b);
}

bool parse_number(const std::wstring& raw, double& out) {
    const std::wstring s = trim(raw);
    if (s.empty() || s.size() > 64) return false;

    // 콤마를 떼되, 숫자/부호/소수점/지수만 남는지 확인한다.
    std::wstring buf;
    buf.reserve(s.size());
    for (wchar_t c : s) {
        if (c == L',') continue;
        if ((c >= L'0' && c <= L'9') || c == L'+' || c == L'-' || c == L'.' ||
            c == L'e' || c == L'E') {
            buf.push_back(c);
        } else {
            return false;  // inf, nan, 단위 접미사 등은 숫자로 보지 않는다
        }
    }
    if (buf.empty()) return false;

    wchar_t* end = nullptr;
    const double v = std::wcstod(buf.c_str(), &end);
    if (end != buf.c_str() + buf.size()) return false;
    if (!std::isfinite(v)) return false;
    out = v;
    return true;
}

int parse_digital(const std::wstring& raw) {
    const std::wstring s = trim(raw);
    if (s.empty() || s.size() > 5) return -1;
    std::wstring k;
    k.reserve(s.size());
    for (wchar_t c : s) k.push_back(static_cast<wchar_t>(std::towlower(c)));

    if (k == L"1" || k == L"true" || k == L"on" || k == L"high" || k == L"yes" ||
        k == L"t" || k == L"h" || k == L"y")
        return 1;
    if (k == L"0" || k == L"false" || k == L"off" || k == L"low" || k == L"no" ||
        k == L"f" || k == L"l" || k == L"n")
        return 0;
    return -1;
}

bool parse_clock_ms(const std::wstring& raw, double& out) {
    const std::wstring s = trim(raw);
    if (s.size() < 4 || s.size() > 20) return false;

    // ':' 로 나눈다. 필드가 2개(MM:SS) 또는 3개(HH:MM:SS) 여야 한다.
    std::wstring tok[4];
    size_t nt = 0, start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == L':') {
            if (nt >= 4) return false;
            tok[nt++] = s.substr(start, i - start);
            start = i + 1;
        }
    }
    if (nt < 2 || nt > 3) return false;

    // 마지막 필드에만 소수점이 붙을 수 있다.
    unsigned frac_ms = 0;
    std::wstring& last = tok[nt - 1];
    const size_t dot = last.find_first_of(L".,");
    if (dot != std::wstring::npos) {
        std::wstring frac = last.substr(dot + 1);
        if (frac.empty() || frac.size() > 6) return false;
        for (wchar_t c : frac) {
            if (c < L'0' || c > L'9') return false;
        }
        while (frac.size() < 3) frac.push_back(L'0');
        if (!read_uint(frac.c_str(), 3, frac_ms)) return false;
        last.erase(dot);
    }

    unsigned v[3] = {0, 0, 0};
    for (size_t k = 0; k < nt; ++k) {
        if (!read_uint(tok[k].c_str(), tok[k].size(), v[k])) return false;
    }
    const unsigned h = (nt == 3) ? v[0] : 0u;
    const unsigned m = (nt == 3) ? v[1] : v[0];
    const unsigned sec = (nt == 3) ? v[2] : v[1];
    if (h > 9999 || m > 59 || sec > 59) return false;

    out = (static_cast<double>(h) * 3600.0 + m * 60.0 + sec) * 1000.0 + frac_ms;
    return true;
}

bool parse_iso_ms(const std::wstring& raw, double& out) {
    const std::wstring s = trim(raw);
    if (s.size() < 10 || s.size() > 32) return false;
    if (s[4] != L'-' || s[7] != L'-') return false;

    unsigned y = 0, mo = 0, d = 0;
    if (!read_uint(s.c_str() + 0, 4, y)) return false;
    if (!read_uint(s.c_str() + 5, 2, mo)) return false;
    if (!read_uint(s.c_str() + 8, 2, d)) return false;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || y < 1601 || y > 9999) return false;

    double ms = static_cast<double>(days_from_civil(static_cast<int>(y), mo, d)) * 86400000.0;
    if (s.size() > 10) {
        if (s[10] != L'T' && s[10] != L' ') return false;
        double tod = 0;
        if (!parse_clock_ms(s.substr(11), tod)) return false;
        ms += tod;
    }
    out = ms;
    return true;
}

double excel_serial_to_ms(double serial) {
    // 엑셀 1900 날짜 체계는 1900 을 윤년으로 잘못 센다. 일련값 60(존재하지 않는
    // 1900-02-29) 아래로는 하루가 어긋나므로 보정한다.
    double days = serial;
    if (days < 60.0) days += 1.0;
    return (days - 25569.0) * 86400000.0;
}

std::wstring unit_from_label(const std::wstring& label) {
    const std::wstring s = trim(label);
    if (s.empty() || s.size() > 128) return L"";
    for (size_t i = 0; i < s.size(); ++i) {
        wchar_t open = s[i];
        wchar_t close = (open == L'[') ? L']' : (open == L'(') ? L')' : 0;
        if (!close) continue;
        size_t j = s.find(close, i + 1);
        if (j == std::wstring::npos) break;
        std::wstring u = trim(s.substr(i + 1, j - i - 1));
        if (!u.empty() && u.size() <= 8) return u;
        break;
    }
    return L"";
}

}  // namespace lc
