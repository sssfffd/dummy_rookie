// logcore_test.cpp — 코어 로직 회귀 테스트.
//
// 일부러 Windows API 를 하나도 쓰지 않는다. 셀 해석과 데이터셋 구성은 버그가
// 실제로 숨는 곳이고, 그 부분은 플랫폼에 의존하지 않으므로 어느 컴파일러에서든
// 그대로 돌려 볼 수 있다. 파일 컨테이너(OPC/CSV 디코딩)는 Windows 전용이라
// 여기서 다루지 않는다.
//
//   Windows: cmake --build . --target logcore_test && ctest
//   그 밖:   g++ -std=c++17 tests/logcore_test.cpp src/core/dataset.cpp
//                src/core/text_util.cpp -Isrc/core -Isrc/core/include -o logcore_test

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "dataset.h"
#include "text_util.h"

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool ok, const char* what, int line) {
    ++g_checks;
    if (!ok) {
        std::printf("  FAIL (line %d): %s\n", line, what);
        ++g_failures;
    }
}
#define CHECK(expr) Check((expr), #expr, __LINE__)

lc::Cell Num(double v) {
    lc::Cell c;
    c.kind = lc::Cell::Kind::Number;
    c.num = v;
    return c;
}
lc::Cell Txt(const wchar_t* s) {
    lc::Cell c;
    c.kind = lc::Cell::Kind::Text;
    c.text = s;
    return c;
}
lc::Cell None() { return lc::Cell(); }

// ---------------------------------------------------------------------------

void TestNumberParsing() {
    std::printf("숫자 해석\n");
    double v = 0;
    CHECK(lc::parse_number(L"3.5", v) && v == 3.5);
    CHECK(lc::parse_number(L"  -12 ", v) && v == -12.0);
    CHECK(lc::parse_number(L"1,234.5", v) && v == 1234.5);
    CHECK(lc::parse_number(L"1e3", v) && v == 1000.0);
    // 뒤에 단위가 붙으면 숫자가 아니다. 이걸 허용하면 "12 V" 가 12 로 둔갑한다.
    CHECK(!lc::parse_number(L"12 V", v));
    CHECK(!lc::parse_number(L"", v));
    CHECK(!lc::parse_number(L"abc", v));
    // 무한대/NaN 문자열은 거부한다. 축 스케일이 통째로 망가진다.
    CHECK(!lc::parse_number(L"inf", v));
    CHECK(!lc::parse_number(L"nan", v));
}

void TestDigitalParsing() {
    std::printf("디지털 해석\n");
    CHECK(lc::parse_digital(L"1") == 1);
    CHECK(lc::parse_digital(L"ON") == 1);
    CHECK(lc::parse_digital(L"true") == 1);
    CHECK(lc::parse_digital(L"High") == 1);
    CHECK(lc::parse_digital(L"0") == 0);
    CHECK(lc::parse_digital(L"off") == 0);
    CHECK(lc::parse_digital(L"IDLE") == -1);
    CHECK(lc::parse_digital(L"") == -1);
}

void TestTimeParsing() {
    std::printf("시간 해석\n");
    double v = 0;
    CHECK(lc::parse_clock_ms(L"00:00:01.230", v) && std::fabs(v - 1230.0) < 1e-9);
    CHECK(lc::parse_clock_ms(L"1:02:03", v) && std::fabs(v - 3723000.0) < 1e-9);
    CHECK(lc::parse_clock_ms(L"02:30.5", v) && std::fabs(v - 150500.0) < 1e-9);
    CHECK(!lc::parse_clock_ms(L"1:2:3:4", v));
    CHECK(!lc::parse_clock_ms(L"12:99:00", v));
    CHECK(!lc::parse_clock_ms(L"hello", v));

    CHECK(lc::parse_iso_ms(L"1970-01-01T00:00:01.500", v) && std::fabs(v - 1500.0) < 1e-9);
    CHECK(lc::parse_iso_ms(L"2026-09-02 12:00:00", v) && v > 1.7e12);
    CHECK(!lc::parse_iso_ms(L"2026-13-02T00:00:00", v));

    // 엑셀 일련값 25569 = 1970-01-01
    CHECK(std::fabs(lc::excel_serial_to_ms(25569.0)) < 1e-6);
    CHECK(std::fabs(lc::excel_serial_to_ms(25570.0) - 86400000.0) < 1e-6);

    CHECK(lc::unit_from_label(L"Time [s]") == L"s");
    CHECK(lc::unit_from_label(L"t (ms)") == L"ms");
    CHECK(lc::unit_from_label(L"Time") == L"");
}

lc::Grid MakeGrid() {
    lc::Grid g;
    g.push_back({Txt(L"Time [s]"), Num(0.0), Num(0.01), Num(0.02), Num(0.03)});
    g.push_back({Txt(L"DI_00_START"), Num(0), Num(1), Num(1), Num(0)});
    g.push_back({Txt(L"DO_01_VALVE"), Txt(L"OFF"), Txt(L"ON"), Txt(L"ON"), Txt(L"OFF")});
    g.push_back({Txt(L"AI_TEMP"), Num(181.5), Num(182.0), None(), Num(183.25)});
    g.push_back({Txt(L"SEQ_STATE"), Txt(L"IDLE"), Txt(L"RUN"), Txt(L"RUN"), Txt(L"FAULT")});
    return g;
}

void TestBuildDataset() {
    std::printf("데이터셋 구성\n");
    lc::Grid g = MakeGrid();
    lc::Dataset ds;
    lc::Limits lim;
    CHECK(lc::build_dataset(g, 0, lim, ds) == LC_OK);

    CHECK(ds.times.size() == 4);
    CHECK(ds.time_kind == LC_TIME_NUMBER);
    CHECK(ds.time_unit == L"s");
    CHECK(ds.channels.size() == 4);

    CHECK(ds.channels[0].name == L"DI_00_START");
    CHECK(ds.channels[0].type == LC_CH_DIGITAL);
    CHECK(ds.channels[0].min == 0.0 && ds.channels[0].max == 1.0);

    // ON/OFF 문자열도 디지털로 읽혀야 한다.
    CHECK(ds.channels[1].type == LC_CH_DIGITAL);
    CHECK(ds.channels[1].values[1] == 1.0);
    CHECK(ds.channels[1].values[0] == 0.0);

    CHECK(ds.channels[2].type == LC_CH_ANALOG);
    CHECK(std::fabs(ds.channels[2].min - 181.5) < 1e-9);
    CHECK(std::fabs(ds.channels[2].max - 183.25) < 1e-9);
    // 빈 셀은 0 이 아니라 끊김이어야 한다. 0 으로 채우면 없는 골짜기가 생긴다.
    CHECK(std::isnan(ds.channels[2].values[2]));

    CHECK(ds.channels[3].type == LC_CH_STATE);
    CHECK(ds.channels[3].states.size() == 3);
    CHECK(ds.channels[3].states[0] == L"IDLE");
    CHECK(ds.channels[3].values[1] == ds.channels[3].values[2]);  // RUN 이 이어진다
    CHECK(ds.channels[3].values[0] != ds.channels[3].values[3]);
}

void TestOrientation() {
    std::printf("배치 전환 (열 = IO)\n");
    // 위와 같은 내용을 전치해 둔 격자. orientation=1 이면 같은 결과가 나와야 한다.
    lc::Grid g;
    g.push_back({Txt(L"Time [s]"), Txt(L"DI_00_START"), Txt(L"AI_TEMP")});
    g.push_back({Num(0.0), Num(0), Num(181.5)});
    g.push_back({Num(0.01), Num(1), Num(182.0)});
    g.push_back({Num(0.02), Num(1), Num(183.0)});

    lc::Dataset ds;
    lc::Limits lim;
    CHECK(lc::build_dataset(g, 1, lim, ds) == LC_OK);
    CHECK(ds.channels.size() == 2);
    CHECK(ds.channels[0].name == L"DI_00_START");
    CHECK(ds.channels[0].type == LC_CH_DIGITAL);
    CHECK(ds.channels[1].name == L"AI_TEMP");
    CHECK(ds.times.size() == 3);
}

void TestTimeFallbacks() {
    std::printf("시간축 대체 경로\n");
    {
        // 시간을 못 읽으면 샘플 번호로 물러선다.
        lc::Grid g;
        g.push_back({Txt(L"IO"), Txt(L"a"), Txt(L"b"), Txt(L"c")});
        g.push_back({Txt(L"CH"), Num(1), Num(2), Num(3)});
        lc::Dataset ds;
        lc::Limits lim;
        CHECK(lc::build_dataset(g, 0, lim, ds) == LC_OK);
        CHECK(ds.time_kind == LC_TIME_INDEX);
        CHECK(ds.times[2] == 2.0);
        CHECK(!ds.notes.empty());
    }
    {
        // 시간이 거꾸로 가면 그래프가 접히므로 샘플 번호로 물러선다.
        lc::Grid g;
        g.push_back({Txt(L"Time"), Num(0), Num(2), Num(1)});
        g.push_back({Txt(L"CH"), Num(1), Num(2), Num(3)});
        lc::Dataset ds;
        lc::Limits lim;
        CHECK(lc::build_dataset(g, 0, lim, ds) == LC_OK);
        CHECK(ds.time_kind == LC_TIME_INDEX);
    }
    {
        // HH:MM:SS 문자열 시간축
        lc::Grid g;
        g.push_back({Txt(L"Time"), Txt(L"00:00:00.000"), Txt(L"00:00:00.100")});
        g.push_back({Txt(L"CH"), Num(0), Num(1)});
        lc::Dataset ds;
        lc::Limits lim;
        CHECK(lc::build_dataset(g, 0, lim, ds) == LC_OK);
        CHECK(ds.time_kind == LC_TIME_CLOCK_MS);
        CHECK(std::fabs(ds.times[1] - 100.0) < 1e-9);
    }
}

void TestLimitsAndEdges() {
    std::printf("상한과 경계\n");
    {
        // 채널 상한을 넘으면 조용히 자르지 않고 명시적으로 거절한다.
        lc::Grid g;
        g.push_back({Txt(L"Time"), Num(0), Num(1)});
        for (int i = 0; i < 5; ++i) g.push_back({Txt(L"CH"), Num(0), Num(1)});
        lc::Dataset ds;
        lc::Limits lim;
        lim.max_channels = 3;
        CHECK(lc::build_dataset(g, 0, lim, ds) == LC_ERR_TOO_LARGE);
    }
    {
        // 데이터 행이 없으면 성공했다고 하지 않는다.
        lc::Grid g;
        g.push_back({Txt(L"Time"), Num(0), Num(1)});
        lc::Dataset ds;
        lc::Limits lim;
        CHECK(lc::build_dataset(g, 0, lim, ds) == LC_ERR_NO_DATA);
    }
    {
        // 이름 칸이 비면 자동으로 이름을 붙이고, 값이 전혀 없는 행은 건너뛴다.
        lc::Grid g;
        g.push_back({Txt(L"Time"), Num(0), Num(1)});
        g.push_back({None(), Num(3), Num(4)});
        g.push_back({Txt(L"EMPTY"), None(), None()});
        lc::Dataset ds;
        lc::Limits lim;
        CHECK(lc::build_dataset(g, 0, lim, ds) == LC_OK);
        CHECK(ds.channels.size() == 1);
        CHECK(ds.channels[0].name == L"IO_1");
    }
}

}  // namespace

int main() {
    TestNumberParsing();
    TestDigitalParsing();
    TestTimeParsing();
    TestBuildDataset();
    TestOrientation();
    TestTimeFallbacks();
    TestLimitsAndEdges();

    std::printf("\n%d개 검사 중 %d개 실패\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
