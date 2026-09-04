/* logcore.h — logcore.dll 공개 C ABI.
 *
 * 이 헤더는 C 링키지만 노출합니다. DLL 경계로 C++ 타입이나 힙 소유권을 넘기지
 * 않으므로, EXE 와 DLL 이 서로 다른 CRT 로 빌드돼도 힙이 깨지지 않습니다.
 * 반환되는 포인터는 모두 데이터셋이 소유하며, lc_close() 전까지만 유효합니다.
 * 호출자는 어떤 반환 포인터도 free() 하면 안 됩니다.
 */
#ifndef LOGCORE_H
#define LOGCORE_H

#include <stdint.h>
#include <wchar.h>

#if defined(_WIN32)
#  if defined(LOGCORE_BUILD)
#    define LC_API __declspec(dllexport)
#  else
#    define LC_API __declspec(dllimport)
#  endif
#else
#  define LC_API
#endif

/* 호출 규약을 고정한다. 그래야 EXE 와 DLL 을 서로 다른 기본 규약으로 빌드해도
 * 스택이 어긋나지 않는다. Windows 밖에서는 (로직 테스트 빌드) 의미가 없다. */
#if defined(_WIN32)
#  define LC_CALL __cdecl
#else
#  define LC_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 버전 --------------------------------------------------------------- */
#define LC_ABI_VERSION 1

/* ---- 상태 코드 ----------------------------------------------------------- */
typedef enum LcStatus {
    LC_OK              = 0,
    LC_ERR_ARG         = 1,  /* 잘못된 인자 */
    LC_ERR_OPEN        = 2,  /* 파일을 열 수 없음 */
    LC_ERR_FORMAT      = 3,  /* 형식을 해석할 수 없음 */
    LC_ERR_NO_DATA     = 4,  /* 채널이나 시간축을 찾지 못함 */
    LC_ERR_TOO_LARGE   = 5,  /* 자원 상한 초과 (아래 LcOpenOptions 참고) */
    LC_ERR_MEMORY      = 6,
    LC_ERR_UNSUPPORTED = 7,  /* 확장자를 지원하지 않음 */
    LC_ERR_INTERNAL    = 8,
    LC_ERR_CANCELLED   = 9   /* 진행 콜백이 취소를 요청함 */
} LcStatus;

/* ---- 채널 / 시간축 종류 --------------------------------------------------- */
typedef enum LcChannelType {
    LC_CH_DIGITAL = 0,  /* 0/1, TRUE/FALSE, ON/OFF, HIGH/LOW */
    LC_CH_ANALOG  = 1,  /* 실수 */
    LC_CH_STATE   = 2   /* 문자열 상태값. 값은 상태 테이블의 인덱스 */
} LcChannelType;

typedef enum LcTimeKind {
    LC_TIME_INDEX    = 0,  /* 시간을 해석하지 못해 샘플 번호를 씀 */
    LC_TIME_NUMBER   = 1,  /* 단위 없는 숫자 (lc_time_unit 참고) */
    LC_TIME_CLOCK_MS = 2,  /* 자정 기준 경과 밀리초 (HH:MM:SS.mmm) */
    LC_TIME_DATE_MS  = 3   /* 1970-01-01 UTC 기준 밀리초 */
} LcTimeKind;

/* ---- 표 배치 --------------------------------------------------------------
 * 로그 시트는 두 가지 배치로 옵니다. 어느 쪽이든, 실제 표가 시트 맨 위·맨 왼쪽에서
 * 시작한다는 보장은 없습니다. 장비 이름·측정 일시·설정값 같은 머리말이 앞에 붙어
 * 표가 시트 중간부터 시작하는 경우가 흔합니다.
 *
 *   LC_ORIENT_ROWS                     LC_ORIENT_COLS
 *   +--------+------+------+           +--------+--------+--------+
 *   | 이름   | 0.00 | 0.01 |           | Time   | DI_00  | AI_T   |
 *   | DI_00  |  0   |  1   |           | 0.00   |   0    | 182.4  |
 *   | AI_T   |182.4 |182.6 |           | 0.01   |   1    | 182.6  |
 *
 * LC_ORIENT_AUTO 는 시간축으로 읽히는 가장 긴 구간을 찾아 배치와 시작 위치를
 * 함께 정합니다. 찾은 결과는 lc_orientation() / lc_data_first_row() /
 * lc_data_first_column() 으로 확인할 수 있고, lc_notes() 에도 남습니다.
 */
typedef enum LcOrientation {
    LC_ORIENT_AUTO = 0,  /* 기본. 배치와 데이터 시작 위치를 자동으로 찾는다 */
    LC_ORIENT_ROWS = 1,  /* 각 행이 IO 채널, 첫 행이 시간 */
    LC_ORIENT_COLS = 2   /* 각 열이 IO 채널, 첫 열이 시간 */
} LcOrientation;

/* ---- 진행 상황 알림 --------------------------------------------------------
 * 큰 파일은 읽는 데 몇 초가 걸립니다. 그동안 아무 소식이 없으면 멈춘 것과
 * 구분되지 않으므로, 읽는 중간에 이 콜백을 불러 줍니다.
 *
 *   done  지금까지 읽은 줄/채널 수
 *   total 전체 개수. 미리 알 수 없으면 0.
 *
 * 파싱 스레드에서 불립니다. 0 을 돌려주면 읽기를 멈추고 LC_ERR_CANCELLED 로
 * 돌아옵니다. 0 이 아니면 계속합니다.
 */
typedef int (LC_CALL* LcProgressFn)(void* user, uint64_t done, uint64_t total);

/* ---- 열기 옵션 ------------------------------------------------------------
 * 상한값은 신뢰할 수 없는 파일에 대한 방어선입니다. 0 을 넣으면 기본값을 씁니다.
 * 상한을 넘으면 파싱을 중단하고 LC_ERR_TOO_LARGE 를 돌려줍니다.
 */
typedef struct LcOpenOptions {
    uint32_t struct_size;              /* sizeof(LcOpenOptions). 필수 */
    uint32_t orientation;              /* LcOrientation. 기본 LC_ORIENT_AUTO */
    uint32_t max_channels;             /* 기본 4096 */
    uint32_t max_samples;              /* 기본 1,048,576 */
    uint64_t max_cells;                /* 기본 32,000,000 */
    uint64_t max_uncompressed_bytes;   /* xlsx 압축 해제 총량 상한. 기본 512 MiB */
    uint32_t max_state_values;         /* 채널당 서로 다른 상태 문자열 수. 기본 4096 */
    uint32_t reserved;
    LcProgressFn progress;             /* 없으면 NULL */
    void* progress_user;
} LcOpenOptions;

/* opt 를 기본값으로 채운다. */
LC_API void LC_CALL lc_default_options(LcOpenOptions* opt);

/* ---- 데이터셋 ------------------------------------------------------------ */
typedef struct LcDataset LcDataset;

/* .xlsx / .xlsm / .csv / .tsv / .txt 를 연다. 확장자로 형식을 고른다.
 * 성공하면 *out 에 핸들이 들어가며, 호출자가 lc_close() 로 닫아야 한다. */
LC_API LcStatus LC_CALL lc_open_file(const wchar_t* path,
                                     const LcOpenOptions* opt,
                                     LcDataset** out);

/* 메모리 위의 바이트에서 연다. hint_name 은 형식 판별용 파일명(확장자)만 쓴다. */
LC_API LcStatus LC_CALL lc_open_memory(const void* bytes, size_t size,
                                       const wchar_t* hint_name,
                                       const LcOpenOptions* opt,
                                       LcDataset** out);

LC_API void LC_CALL lc_close(LcDataset* ds);

/* 상태 코드의 사람이 읽는 설명. 정적 문자열이며 해제하면 안 된다. */
LC_API const wchar_t* LC_CALL lc_status_text(LcStatus st);

/* 파싱 중 남긴 경고(시간축 대체, 건너뛴 행 등). 없으면 빈 문자열. */
LC_API const wchar_t* LC_CALL lc_notes(const LcDataset* ds);

/* ---- 실제로 사용한 배치 ----------------------------------------------------
 * LC_ORIENT_AUTO 로 열었을 때 무엇으로 판정했는지 돌려준다.
 * LC_ORIENT_AUTO 를 돌려주는 일은 없다 (항상 ROWS 아니면 COLS).
 */
LC_API LcOrientation LC_CALL lc_orientation(const LcDataset* ds);

/* 데이터 표가 시작한 위치. 엑셀과 같은 1 기반 번호이며, 이름이 있는 머리 행/열을
 * 가리킨다. 머리말 없이 A1 부터 시작하는 시트라면 둘 다 1 이다. */
LC_API uint32_t LC_CALL lc_data_first_row(const LcDataset* ds);
LC_API uint32_t LC_CALL lc_data_first_column(const LcDataset* ds);

/* ---- 시간축 -------------------------------------------------------------- */
LC_API uint32_t     LC_CALL lc_sample_count(const LcDataset* ds);
LC_API const double* LC_CALL lc_times(const LcDataset* ds);   /* 오름차순, 길이 = lc_sample_count */
LC_API LcTimeKind   LC_CALL lc_time_kind(const LcDataset* ds);
LC_API const wchar_t* LC_CALL lc_time_unit(const LcDataset* ds); /* "s", "ms" 등. 없으면 "" */

/* t 에 가장 가까운 샘플의 인덱스. 데이터가 없으면 -1. */
LC_API int32_t LC_CALL lc_index_at(const LcDataset* ds, double t);

/* ---- 채널 ---------------------------------------------------------------- */
LC_API uint32_t      LC_CALL lc_channel_count(const LcDataset* ds);
LC_API const wchar_t* LC_CALL lc_channel_name(const LcDataset* ds, uint32_t ch);
LC_API LcChannelType LC_CALL lc_channel_type(const LcDataset* ds, uint32_t ch);
LC_API double        LC_CALL lc_channel_min(const LcDataset* ds, uint32_t ch);
LC_API double        LC_CALL lc_channel_max(const LcDataset* ds, uint32_t ch);

/* 값 배열. 길이 = lc_sample_count. 빈 셀은 NaN 이며 선을 잇지 않는 '끊김'을 뜻한다. */
LC_API const double* LC_CALL lc_channel_values(const LcDataset* ds, uint32_t ch);

/* LC_CH_STATE 채널의 상태 문자열 테이블. 값 v 는 이 테이블의 인덱스다. */
LC_API uint32_t      LC_CALL lc_state_count(const LcDataset* ds, uint32_t ch);
LC_API const wchar_t* LC_CALL lc_state_name(const LcDataset* ds, uint32_t ch, uint32_t state);

/* ---- 두 로그 비교 ----------------------------------------------------------
 * 같은 양식으로 뽑은 로그 두 개(이전/이후)를 견주려면 두 가지가 필요합니다.
 * 채널을 이름으로 맞추는 것과, 서로 다른 시간 격자를 맞추는 것입니다.
 */

/* 이름으로 채널을 찾는다. 정확히 같은 이름을 먼저 보고, 없으면 앞뒤 공백과
 * 대소문자를 무시하고 다시 찾는다. 못 찾으면 -1. */
LC_API int32_t LC_CALL lc_find_channel(const LcDataset* ds, const wchar_t* name);

/* 임의의 시각 t 에서의 값. 두 로그의 시간 격자가 달라도 한쪽 격자에 맞춰
 * 값을 읽어 올 수 있다.
 *   아날로그  : 앞뒤 샘플을 선형 보간
 *   디지털/상태: 직전 샘플 값을 유지 (계단). 없는 중간 값을 만들지 않는다.
 * t 가 기록 구간 밖이거나 근처 샘플이 비어 있으면 NaN. */
LC_API double LC_CALL lc_sample_at(const LcDataset* ds, uint32_t ch, double t);

/* ---- 그리기 보조 ---------------------------------------------------------
 * [t0,t1] 구간을 columns 개의 픽셀 열로 접어 열마다 최소/최대를 뽑는다.
 * 샘플이 픽셀보다 촘촘할 때 렌더러가 한 픽셀에 수천 번 선을 긋지 않게 한다.
 * out_min/out_max 는 호출자가 columns 개씩 잡아 넘긴다. 값이 없는 열은 NaN.
 * 실제로 채운 열 수를 돌려준다.
 */
LC_API uint32_t LC_CALL lc_decimate(const LcDataset* ds, uint32_t ch,
                                    double t0, double t1, uint32_t columns,
                                    double* out_min, double* out_max);

/* [t0,t1] 구간에서 값이 바뀐 횟수 (디지털 채널의 에지 수). */
LC_API uint32_t LC_CALL lc_edge_count(const LcDataset* ds, uint32_t ch,
                                      double t0, double t1);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* LOGCORE_H */
