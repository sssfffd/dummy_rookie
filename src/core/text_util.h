// text_util.h — 셀 문자열을 숫자·시간·디지털 값으로 바꾸는 순수 함수 모음.
// 여기 있는 함수는 신뢰할 수 없는 입력을 직접 받는다. 모든 함수는 길이를 먼저
// 검사하고, 실패를 예외가 아니라 false 로 알린다.
#pragma once

#include <string>

namespace lc {

// 앞뒤 공백(스페이스, 탭, CR, LF, NBSP)을 뗀 뷰를 만든다.
std::wstring trim(const std::wstring& s);

// 십진수 하나를 통째로 읽는다. 뒤에 남는 문자가 있으면 실패다.
// 천 단위 콤마는 허용한다. inf/nan 문자열은 거부한다.
bool parse_number(const std::wstring& s, double& out);

// 디지털로 읽히면 1 또는 0, 아니면 -1.
// 0/1, true/false, on/off, high/low, yes/no, t/f, h/l, y/n (대소문자 무시)
int parse_digital(const std::wstring& s);

// "HH:MM:SS.mmm", "HH:MM:SS", "MM:SS.mmm" → 자정 기준 밀리초.
bool parse_clock_ms(const std::wstring& s, double& out);

// "YYYY-MM-DD[ T]HH:MM:SS[.mmm]" → 1970-01-01 UTC 기준 밀리초.
bool parse_iso_ms(const std::wstring& s, double& out);

// 엑셀 일련값(1899-12-30 기준) → 1970-01-01 UTC 기준 밀리초.
double excel_serial_to_ms(double serial);

// 이름 비교용으로 접는다. 앞뒤 공백을 떼고, 안쪽 연속 공백을 하나로 줄이고,
// 대소문자를 없앤다. 같은 장비 로그라도 이런 차이는 흔하다.
std::wstring fold_name(const std::wstring& s);

// 라벨에서 단위를 뽑는다. "Time [s]" → "s", "t (ms)" → "ms". 없으면 빈 문자열.
std::wstring unit_from_label(const std::wstring& label);

}  // namespace lc
