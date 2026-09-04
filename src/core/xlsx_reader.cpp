// xlsx_reader.cpp — .xlsx / .xlsm 파서.
//
// 서드파티 라이브러리를 쓰지 않는다. Windows 에 이미 들어 있는 두 가지만 쓴다.
//   * Packaging(OPC) API — msopc.h / opcservices.dll.
//     .xlsx 는 OPC 패키지(=ZIP + 관계 XML)이므로 이 API 가 컨테이너를 그대로 연다.
//     ZIP 해제 코드를 직접 만들지 않는다는 뜻이고, 그만큼 공격면이 줄어든다.
//   * XmlLite — xmllite.dll 의 IXmlReader.
//     스트리밍 pull 파서이고 DTD 를 아예 지원하지 않는다. 즉 XXE 와 엔티티 폭탄
//     (billion laughs)이 구조적으로 불가능하다. DOM 이 아니므로 큰 시트를 열어도
//     메모리가 파일 크기에 비례해 터지지 않는다.
//
// 그래도 파일 내용은 여전히 신뢰할 수 없다. 셀 수 · 행 수 · 열 수 · 공유 문자열
// 수를 Limits 로 막고, 인덱스는 모두 범위를 확인한 뒤에 쓴다.

#include <msopc.h>
#include <xmllite.h>

#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

#include "com_ptr.h"
#include "readers.h"
#include "text_util.h"

namespace lc {
namespace {

constexpr const wchar_t* kRelOfficeDocument =
    L"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument";
constexpr const wchar_t* kRelSharedStrings =
    L"http://schemas.openxmlformats.org/officeDocument/2006/relationships/sharedStrings";
constexpr const wchar_t* kRelStyles =
    L"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles";

// ---------------------------------------------------------------------------
// XmlLite 도우미
// ---------------------------------------------------------------------------

// 신뢰할 수 없는 XML 을 읽을 리더를 만든다. DTD 금지와 깊이 제한을 명시적으로 건다.
HRESULT make_reader(IStream* s, ComPtr<IXmlReader>& out) {
    // __uuidof 는 MSVC 확장이지만 MSVC(2017 포함)와 mingw/clang 모두 지원한다.
    // 이름 있는 GUID 상수(IID_IXmlReader 등)를 쓰면 uuid 라이브러리 링크가 필요한데,
    // 툴체인마다 어느 .lib 에 들어 있는지가 달라서 이쪽이 더 안전하다.
    HRESULT hr = CreateXmlReader(__uuidof(IXmlReader), out.put_void(), nullptr);
    if (FAILED(hr)) return hr;
    hr = out->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit);
    if (FAILED(hr)) return hr;
    (void)out->SetProperty(XmlReaderProperty_MaxElementDepth, 256);
    return out->SetInput(s);
}

std::wstring local_name(IXmlReader* r) {
    const wchar_t* p = nullptr;
    UINT len = 0;
    if (FAILED(r->GetLocalName(&p, &len)) || !p) return std::wstring();
    return std::wstring(p, len);
}

std::wstring node_value(IXmlReader* r) {
    const wchar_t* p = nullptr;
    UINT len = 0;
    if (FAILED(r->GetValue(&p, &len)) || !p) return std::wstring();
    return std::wstring(p, len);
}

// 현재 요소에서 이름이 name 인 속성 값을 읽는다. 없으면 false.
bool attr(IXmlReader* r, const wchar_t* name, std::wstring& out) {
    if (FAILED(r->MoveToFirstAttribute())) return false;
    bool found = false;
    for (;;) {
        if (local_name(r) == name) { out = node_value(r); found = true; break; }
        const HRESULT hr = r->MoveToNextAttribute();
        if (hr != S_OK) break;
    }
    (void)r->MoveToElement();
    return found;
}

// 현재 요소 안의 모든 텍스트를 이어 붙인다 (rich text run 대응). 요소 끝에서 멈춘다.
std::wstring read_element_text(IXmlReader* r, size_t max_len) {
    if (r->IsEmptyElement()) return std::wstring();
    std::wstring acc;
    UINT start_depth = 0;
    (void)r->GetDepth(&start_depth);
    XmlNodeType nt;
    while (r->Read(&nt) == S_OK) {
        if (nt == XmlNodeType_Text || nt == XmlNodeType_CDATA) {
            if (acc.size() < max_len) acc += node_value(r);
        } else if (nt == XmlNodeType_EndElement) {
            UINT d = 0;
            (void)r->GetDepth(&d);
            if (d <= start_depth) break;
        }
    }
    if (acc.size() > max_len) acc.resize(max_len);
    return acc;
}

// "BC12" → 0 기반 열 번호 54. 실패하면 false.
bool column_from_ref(const std::wstring& ref, uint32_t& out) {
    uint64_t col = 0;
    size_t i = 0;
    for (; i < ref.size(); ++i) {
        const wchar_t c = ref[i];
        wchar_t up = c;
        if (up >= L'a' && up <= L'z') up = static_cast<wchar_t>(up - L'a' + L'A');
        if (up < L'A' || up > L'Z') break;
        col = col * 26u + static_cast<uint64_t>(up - L'A' + 1);
        if (col > 16384u) return false;  // 엑셀 최대 열 수
    }
    if (i == 0 || col == 0) return false;
    out = static_cast<uint32_t>(col - 1);
    return true;
}

// ---------------------------------------------------------------------------
// OPC 도우미
// ---------------------------------------------------------------------------

struct Package {
    ComPtr<IOpcFactory> factory;
    ComPtr<IOpcPackage> package;
    ComPtr<IOpcPartSet> parts;
};

// 관계 집합에서 지정한 타입의 첫 관계를 따라가 파트를 얻는다.
HRESULT part_by_relationship(Package& pkg, IOpcRelationshipSet* rels, IOpcUri* base,
                             const wchar_t* type, ComPtr<IOpcPart>& out) {
    ComPtr<IOpcRelationshipEnumerator> it;
    HRESULT hr = rels->GetEnumeratorForType(type, it.put());
    if (FAILED(hr)) return hr;

    BOOL has = FALSE;
    while (SUCCEEDED(it->MoveNext(&has)) && has) {
        ComPtr<IOpcRelationship> rel;
        if (FAILED(it->GetCurrent(rel.put()))) continue;

        OPC_URI_TARGET_MODE mode = OPC_URI_TARGET_MODE_INTERNAL;
        // 패키지 밖을 가리키는 관계(External)는 따라가지 않는다. 따라갔다면
        // 파일이 임의의 URL 이나 로컬 경로를 열게 만들 수 있다.
        if (FAILED(rel->GetTargetMode(&mode)) || mode != OPC_URI_TARGET_MODE_INTERNAL) continue;

        ComPtr<IUri> target;
        if (FAILED(rel->GetTargetUri(target.put()))) continue;
        ComPtr<IOpcPartUri> part_uri;
        if (FAILED(base->CombinePartUri(target.get(), part_uri.put()))) continue;

        BOOL exists = FALSE;
        if (FAILED(pkg.parts->PartExists(part_uri.get(), &exists)) || !exists) continue;
        if (SUCCEEDED(pkg.parts->GetPart(part_uri.get(), out.put()))) return S_OK;
    }
    return E_FAIL;
}

// 관계 Id 로 파트를 얻는다 (<sheet r:id="rId1"/> 해석용).
HRESULT part_by_rel_id(Package& pkg, IOpcRelationshipSet* rels, IOpcUri* base,
                       const std::wstring& id, ComPtr<IOpcPart>& out) {
    ComPtr<IOpcRelationship> rel;
    HRESULT hr = rels->GetRelationship(id.c_str(), rel.put());
    if (FAILED(hr)) return hr;

    OPC_URI_TARGET_MODE mode = OPC_URI_TARGET_MODE_INTERNAL;
    if (FAILED(rel->GetTargetMode(&mode)) || mode != OPC_URI_TARGET_MODE_INTERNAL) return E_FAIL;

    ComPtr<IUri> target;
    hr = rel->GetTargetUri(target.put());
    if (FAILED(hr)) return hr;
    ComPtr<IOpcPartUri> part_uri;
    hr = base->CombinePartUri(target.get(), part_uri.put());
    if (FAILED(hr)) return hr;

    BOOL exists = FALSE;
    if (FAILED(pkg.parts->PartExists(part_uri.get(), &exists)) || !exists) return E_FAIL;
    return pkg.parts->GetPart(part_uri.get(), out.put());
}

// ---------------------------------------------------------------------------
// 각 파트 파싱
// ---------------------------------------------------------------------------

LcStatus parse_shared_strings(IStream* s, const Limits& lim,
                              std::vector<std::wstring>& out) {
    ComPtr<IXmlReader> r;
    if (FAILED(make_reader(s, r))) return LC_ERR_FORMAT;

    XmlNodeType nt;
    while (r->Read(&nt) == S_OK) {
        if (nt != XmlNodeType_Element) continue;
        if (local_name(r.get()) != L"si") continue;
        if (out.size() >= lim.max_cells) return LC_ERR_TOO_LARGE;
        if ((out.size() & 0xFFFu) == 0 && !lim.progress.report(out.size(), 0)) {
            return LC_ERR_CANCELLED;
        }
        out.push_back(read_element_text(r.get(), 1u << 16));
    }
    return LC_OK;
}

// cellXfs 의 각 항목이 날짜 서식인지 표시한다. 시간축이 엑셀 시간 셀로 들어오는
// 흔한 경우를 제대로 읽으려면 이 정보가 필요하다.
LcStatus parse_styles(IStream* s, std::vector<bool>& date_xf) {
    ComPtr<IXmlReader> r;
    if (FAILED(make_reader(s, r))) return LC_ERR_FORMAT;

    // 내장 날짜/시간 서식 id
    std::unordered_set<uint32_t> date_fmts = {14, 15, 16, 17, 18, 19, 20, 21, 22,
                                              45, 46, 47, 27, 30, 36, 50, 57};
    bool in_cell_xfs = false;
    XmlNodeType nt;
    while (r->Read(&nt) == S_OK) {
        if (nt == XmlNodeType_EndElement) {
            if (local_name(r.get()) == L"cellXfs") in_cell_xfs = false;
            continue;
        }
        if (nt != XmlNodeType_Element) continue;
        const std::wstring name = local_name(r.get());

        if (name == L"numFmt") {
            std::wstring id_s, code;
            if (attr(r.get(), L"numFmtId", id_s) && attr(r.get(), L"formatCode", code)) {
                double id = 0;
                if (parse_number(id_s, id) && id >= 0 && id < 65536) {
                    // 따옴표 밖에 날짜/시간 자리 문자가 있으면 날짜 서식으로 본다.
                    bool in_quote = false, is_date = false;
                    for (wchar_t c : code) {
                        if (c == L'"') { in_quote = !in_quote; continue; }
                        if (in_quote) continue;
                        if (c == L'y' || c == L'd' || c == L'h' || c == L's' ||
                            c == L'Y' || c == L'D' || c == L'H' || c == L'S') {
                            is_date = true;
                            break;
                        }
                    }
                    if (is_date) date_fmts.insert(static_cast<uint32_t>(id));
                }
            }
        } else if (name == L"cellXfs") {
            in_cell_xfs = true;
            date_xf.clear();
        } else if (name == L"xf" && in_cell_xfs) {
            std::wstring id_s;
            uint32_t id = 0;
            if (attr(r.get(), L"numFmtId", id_s)) {
                double v = 0;
                if (parse_number(id_s, v) && v >= 0 && v < 65536) id = static_cast<uint32_t>(v);
            }
            if (date_xf.size() >= 65536) return LC_ERR_TOO_LARGE;
            date_xf.push_back(date_fmts.count(id) != 0);
        }
    }
    return LC_OK;
}

struct SheetContext {
    const std::vector<std::wstring>* shared;
    const std::vector<bool>* date_xf;
    double date_epoch_offset;  // 1904 날짜 체계 보정
};

LcStatus parse_sheet(IStream* s, const Limits& lim, const SheetContext& cx, Grid& out) {
    ComPtr<IXmlReader> r;
    if (FAILED(make_reader(s, r))) return LC_ERR_FORMAT;

    bool in_sheet_data = false;
    uint32_t row_index = 0;   // 0 기반
    uint32_t next_col = 0;
    uint64_t cell_budget = lim.max_cells;
    XmlNodeType nt;

    while (r->Read(&nt) == S_OK) {
        if (nt == XmlNodeType_EndElement) {
            if (local_name(r.get()) == L"sheetData") break;
            continue;
        }
        if (nt != XmlNodeType_Element) continue;
        const std::wstring name = local_name(r.get());

        if (name == L"sheetData") { in_sheet_data = true; continue; }
        if (!in_sheet_data) continue;

        if (name == L"row") {
            std::wstring rs;
            if (attr(r.get(), L"r", rs)) {
                double v = 0;
                if (!parse_number(rs, v) || v < 1) return LC_ERR_FORMAT;
                if (v > static_cast<double>(lim.max_channels) + 1.0) return LC_ERR_TOO_LARGE;
                row_index = static_cast<uint32_t>(v) - 1u;
            } else {
                row_index = static_cast<uint32_t>(out.size());
            }
            if (row_index >= lim.max_channels + 1u) return LC_ERR_TOO_LARGE;
            if ((row_index & 0xFFu) == 0 && !lim.progress.report(row_index, 0)) {
                return LC_ERR_CANCELLED;
            }
            if (out.size() <= row_index) out.resize(row_index + 1u);
            next_col = 0;
            continue;
        }

        if (name != L"c") continue;
        if (cell_budget-- == 0) return LC_ERR_TOO_LARGE;

        std::wstring ref, type, style;
        const bool have_ref = attr(r.get(), L"r", ref);
        const bool have_type = attr(r.get(), L"t", type);
        const bool have_style = attr(r.get(), L"s", style);

        uint32_t col = next_col;
        if (have_ref && !column_from_ref(ref, col)) col = next_col;
        next_col = col + 1u;
        if (col >= lim.max_samples + 1u) return LC_ERR_TOO_LARGE;

        const bool empty_element = r->IsEmptyElement() != 0;

        // 값을 읽는다. 숫자 셀은 <v>, 인라인 문자열은 <is> 안에 있다.
        std::wstring raw;
        if (!empty_element) {
            UINT c_depth = 0;
            (void)r->GetDepth(&c_depth);
            XmlNodeType inner;
            while (r->Read(&inner) == S_OK) {
                if (inner == XmlNodeType_EndElement) {
                    UINT d = 0;
                    (void)r->GetDepth(&d);
                    if (d <= c_depth) break;
                    continue;
                }
                if (inner != XmlNodeType_Element) continue;
                const std::wstring inner_name = local_name(r.get());
                if (inner_name == L"v" || inner_name == L"t") {
                    raw = read_element_text(r.get(), 1u << 16);
                } else if (inner_name == L"is") {
                    raw = read_element_text(r.get(), 1u << 16);
                }
                // <f>(수식)은 무시한다. 계산된 값만 쓴다.
            }
        }

        Cell cell;
        if (!raw.empty()) {
            if (type == L"s") {
                double idx = 0;
                if (cx.shared && parse_number(raw, idx) && idx >= 0 &&
                    idx < static_cast<double>(cx.shared->size())) {
                    cell.kind = Cell::Kind::Text;
                    cell.text = (*cx.shared)[static_cast<size_t>(idx)];
                }
            } else if (type == L"b") {
                double v = 0;
                if (parse_number(raw, v)) { cell.kind = Cell::Kind::Bool; cell.num = (v != 0) ? 1 : 0; }
            } else if (type == L"inlineStr" || type == L"str" || type == L"e") {
                cell.kind = Cell::Kind::Text;
                cell.text = raw;
            } else if (type == L"d") {
                double ms = 0;
                if (parse_iso_ms(raw, ms)) { cell.kind = Cell::Kind::DateMs; cell.num = ms; }
            } else {
                double v = 0;
                if (parse_number(raw, v)) {
                    bool is_date = false;
                    if (have_style && cx.date_xf) {
                        double si = 0;
                        if (parse_number(style, si) && si >= 0 &&
                            si < static_cast<double>(cx.date_xf->size())) {
                            is_date = (*cx.date_xf)[static_cast<size_t>(si)];
                        }
                    }
                    if (is_date) {
                        cell.kind = Cell::Kind::DateMs;
                        cell.num = excel_serial_to_ms(v + cx.date_epoch_offset);
                    } else {
                        cell.kind = Cell::Kind::Number;
                        cell.num = v;
                    }
                } else {
                    cell.kind = Cell::Kind::Text;
                    cell.text = raw;
                }
            }
        }
        (void)have_type;

        if (cell.kind != Cell::Kind::Empty) {
            Row& row = out[row_index];
            if (row.size() <= col) row.resize(col + 1u);
            row[col] = std::move(cell);
        }
    }
    return LC_OK;
}

}  // namespace

LcStatus read_xlsx(IStream* stream, const Limits& lim, Grid& out) {
    Package pkg;
    HRESULT hr = CoCreateInstance(__uuidof(OpcFactory), nullptr, CLSCTX_INPROC_SERVER,
                                  __uuidof(IOpcFactory), pkg.factory.put_void());
    if (FAILED(hr)) return LC_ERR_INTERNAL;

    // OPC_CACHE_ON_ACCESS: 파트를 미리 전부 펼치지 않고 접근할 때 읽는다.
    // OPC_VALIDATE_ON_LOAD 를 켜면 패키지 전체를 먼저 검증하므로 큰 파일에서
    // 메모리와 시간이 크게 늘어난다. 대신 아래에서 파트를 하나씩 검사한다.
    hr = pkg.factory->ReadPackageFromStream(stream, OPC_CACHE_ON_ACCESS, pkg.package.put());
    if (FAILED(hr)) return LC_ERR_FORMAT;
    if (FAILED(pkg.package->GetPartSet(pkg.parts.put()))) return LC_ERR_FORMAT;

    ComPtr<IOpcRelationshipSet> root_rels;
    if (FAILED(pkg.package->GetRelationshipSet(root_rels.put()))) return LC_ERR_FORMAT;
    ComPtr<IOpcUri> root_uri;
    if (FAILED(pkg.factory->CreatePackageRootUri(root_uri.put()))) return LC_ERR_INTERNAL;

    ComPtr<IOpcPart> workbook_part;
    if (FAILED(part_by_relationship(pkg, root_rels.get(), root_uri.get(),
                                    kRelOfficeDocument, workbook_part))) {
        return LC_ERR_FORMAT;
    }

    ComPtr<IOpcPartUri> workbook_uri;
    if (FAILED(workbook_part->GetName(workbook_uri.put()))) return LC_ERR_FORMAT;
    ComPtr<IOpcRelationshipSet> wb_rels;
    if (FAILED(workbook_part->GetRelationshipSet(wb_rels.put()))) return LC_ERR_FORMAT;

    // 공유 문자열 (있으면)
    std::vector<std::wstring> shared;
    {
        ComPtr<IOpcPart> part;
        if (SUCCEEDED(part_by_relationship(pkg, wb_rels.get(), workbook_uri.get(),
                                           kRelSharedStrings, part))) {
            ComPtr<IStream> s;
            if (SUCCEEDED(part->GetContentStream(s.put()))) {
                const LcStatus st = parse_shared_strings(s.get(), lim, shared);
                if (st != LC_OK) return st;
            }
        }
    }

    // 스타일 (날짜 서식 판별용, 있으면)
    std::vector<bool> date_xf;
    {
        ComPtr<IOpcPart> part;
        if (SUCCEEDED(part_by_relationship(pkg, wb_rels.get(), workbook_uri.get(),
                                           kRelStyles, part))) {
            ComPtr<IStream> s;
            if (SUCCEEDED(part->GetContentStream(s.put()))) {
                const LcStatus st = parse_styles(s.get(), date_xf);
                if (st != LC_OK) return st;
            }
        }
    }

    // workbook.xml 에서 첫 시트의 관계 Id 와 날짜 체계를 읽는다.
    std::wstring first_sheet_rid;
    double date_offset = 0.0;
    {
        ComPtr<IStream> s;
        if (FAILED(workbook_part->GetContentStream(s.put()))) return LC_ERR_FORMAT;
        ComPtr<IXmlReader> r;
        if (FAILED(make_reader(s.get(), r))) return LC_ERR_FORMAT;

        XmlNodeType nt;
        while (r->Read(&nt) == S_OK) {
            if (nt != XmlNodeType_Element) continue;
            const std::wstring name = local_name(r.get());
            if (name == L"workbookPr") {
                std::wstring v;
                if (attr(r.get(), L"date1904", v) && (v == L"1" || v == L"true")) {
                    date_offset = 1462.0;  // 1904 체계는 1900 체계보다 4년 뒤
                }
            } else if (name == L"sheet" && first_sheet_rid.empty()) {
                std::wstring rid;
                if (attr(r.get(), L"id", rid) && !rid.empty()) first_sheet_rid = rid;
            }
        }
    }
    if (first_sheet_rid.empty()) return LC_ERR_FORMAT;

    ComPtr<IOpcPart> sheet_part;
    if (FAILED(part_by_rel_id(pkg, wb_rels.get(), workbook_uri.get(),
                              first_sheet_rid, sheet_part))) {
        return LC_ERR_FORMAT;
    }
    ComPtr<IStream> sheet_stream;
    if (FAILED(sheet_part->GetContentStream(sheet_stream.put()))) return LC_ERR_FORMAT;

    SheetContext cx;
    cx.shared = &shared;
    cx.date_xf = &date_xf;
    cx.date_epoch_offset = date_offset;

    out.clear();
    return parse_sheet(sheet_stream.get(), lim, cx, out);
}

}  // namespace lc
