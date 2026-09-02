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
    LC_ERR_INTERNAL    = 8
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

/* ---- 열기 옵션 ------------------------------------------------------------
 * 상한값은 신뢰할 수 없는 파일에 대한 방어선입니다. 0 을 넣으면 기본값을 씁니다.
 * 상한을 넘으면 파싱을 중단하고 LC_ERR_TOO_LARGE 를 돌려줍니다.
 */
typedef struct LcOpenOptions {
    uint32_t struct_size;              /* sizeof(LcOpenOptions). 필수 */
    uint32_t orientation;              /* 0 = 각 행이 채널(기본), 1 = 각 열이 채널 */
    uint32_t max_channels;             /* 기본 4096 */
    uint32_t max_samples;              /* 기본 1,048,576 */
    uint64_t max_cells;                /* 기본 32,000,000 */
    uint64_t max_uncompressed_bytes;   /* xlsx 압축 해제 총량 상한. 기본 512 MiB */
    uint32_t max_state_values;         /* 채널당 서로 다른 상태 문자열 수. 기본 4096 */
    uint32_t reserved;
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
