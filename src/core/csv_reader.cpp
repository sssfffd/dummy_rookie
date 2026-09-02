#include <algorithm>
#include <string>
#include <vector>

#include "readers.h"
#include "text_util.h"

namespace lc {
namespace {

// 바이트 묶음을 UTF-16 문자열로. 인코딩은 BOM 우선, 없으면 UTF-8 → ANSI 순.
bool decode(const uint8_t* data, size_t size, std::wstring& out) {
    if (size >= 2 && data[0] == 0xFF && data[1] == 0xFE) {  // UTF-16 LE
        const size_t n = (size - 2) / 2;
        out.assign(reinterpret_cast<const wchar_t*>(data + 2), n);
        return true;
    }
    if (size >= 2 && data[0] == 0xFE && data[1] == 0xFF) {  // UTF-16 BE
        const size_t n = (size - 2) / 2;
        out.resize(n);
        for (size_t i = 0; i < n; ++i) {
            out[i] = static_cast<wchar_t>((data[2 + i * 2] << 8) | data[3 + i * 2]);
        }
        return true;
    }

    const uint8_t* p = data;
    size_t n = size;
    if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) { p += 3; n -= 3; }
    if (n == 0) { out.clear(); return true; }
    if (n > (size_t)INT_MAX) return false;

    for (UINT cp : {CP_UTF8, CP_ACP}) {
        const DWORD flags = (cp == CP_UTF8) ? MB_ERR_INVALID_CHARS : 0;
        const int need = MultiByteToWideChar(cp, flags, reinterpret_cast<const char*>(p),
                                             static_cast<int>(n), nullptr, 0);
        if (need <= 0) continue;
        out.resize(static_cast<size_t>(need));
        const int got = MultiByteToWideChar(cp, flags, reinterpret_cast<const char*>(p),
                                            static_cast<int>(n), &out[0], need);
        if (got == need) return true;
    }
    return false;
}

// 첫 줄에서 구분자를 고른다. 따옴표 밖에 가장 많이 나온 후보가 이긴다.
wchar_t detect_delimiter(const std::wstring& s) {
    const wchar_t candidates[] = {L',', L'\t', L';', L'|'};
    size_t counts[4] = {0, 0, 0, 0};
    bool in_quote = false;
    for (size_t i = 0; i < s.size() && s[i] != L'\n'; ++i) {
        if (s[i] == L'"') { in_quote = !in_quote; continue; }
        if (in_quote) continue;
        for (int k = 0; k < 4; ++k) {
            if (s[i] == candidates[k]) ++counts[k];
        }
    }
    int best = 0;
    for (int k = 1; k < 4; ++k) {
        if (counts[k] > counts[best]) best = k;
    }
    return counts[best] > 0 ? candidates[best] : L',';
}

Cell make_cell(const std::wstring& raw, bool was_quoted) {
    Cell c;
    const std::wstring t = trim(raw);
    if (t.empty()) return c;
    double v = 0;
    // 따옴표로 감싼 값은 사용자가 문자열로 의도한 것이므로 숫자로 바꾸지 않는다.
    if (!was_quoted && parse_number(t, v)) {
        c.kind = Cell::Kind::Number;
        c.num = v;
    } else {
        c.kind = Cell::Kind::Text;
        c.text = t;
    }
    return c;
}

}  // namespace

LcStatus read_delimited(const uint8_t* data, size_t size, const Limits& lim, Grid& out) {
    std::wstring text;
    if (!decode(data, size, text)) return LC_ERR_FORMAT;
    if (text.empty()) return LC_ERR_NO_DATA;

    const wchar_t delim = detect_delimiter(text);

    out.clear();
    Row row;
    std::wstring field;
    bool in_quote = false, quoted_field = false;
    uint64_t cells = 0;

    auto end_field = [&]() {
        row.push_back(make_cell(field, quoted_field));
        field.clear();
        quoted_field = false;
    };
    auto end_row = [&]() -> bool {
        end_field();
        cells += row.size();
        if (cells > lim.max_cells) return false;
        if (out.size() >= lim.max_channels + 1u) return false;
        out.push_back(std::move(row));
        row.clear();
        return true;
    };

    for (size_t i = 0; i < text.size(); ++i) {
        const wchar_t c = text[i];
        if (in_quote) {
            if (c == L'"') {
                if (i + 1 < text.size() && text[i + 1] == L'"') { field.push_back(L'"'); ++i; }
                else in_quote = false;
            } else {
                field.push_back(c);
            }
            continue;
        }
        if (c == L'"') { in_quote = true; quoted_field = true; }
        else if (c == delim) { end_field(); }
        else if (c == L'\n') { if (!end_row()) return LC_ERR_TOO_LARGE; }
        else if (c == L'\r') { /* CRLF 의 CR 은 버린다 */ }
        else { field.push_back(c); }

        if (field.size() > 1u << 20) return LC_ERR_TOO_LARGE;
    }
    if (!field.empty() || !row.empty() || quoted_field) {
        if (!end_row()) return LC_ERR_TOO_LARGE;
    }
    if (out.empty()) return LC_ERR_NO_DATA;
    return LC_OK;
}

}  // namespace lc
