# 08 · 한 번 연 엑셀 파일을 다시 못 여는 문제

- **날짜** 2026-09-03
- **커밋** 이 커밋
- **바뀐 파일** `prototype/web/index.html`

## 요청

> 아니 이거 한번 엑셀 열면 다시 못열어?

## 답변

**웹 버전의 실제 버그였습니다. 고쳤고, 브라우저에서 재현·검증까지 했습니다.**

### 원인

```js
// 고치기 전
$('fileInput').addEventListener('change', function () {
  if (this.files && this.files[0]) readFile(this.files[0]);
});
```

`<input type="file">` 는 **값이 바뀔 때만** `change` 이벤트를 냅니다. 파일을 한 번
고르면 input 의 값이 그 파일 경로로 남아 있어서, **같은 파일을 다시 고르면 값이
그대로라 이벤트가 아예 발생하지 않습니다.** 파일 선택 창은 정상적으로 뜨고,
파일을 골라도 아무 일도 일어나지 않으니 "한 번 열면 다시 안 열린다" 로 보입니다.

### 고친 것

```js
$('fileInput').addEventListener('change', function () {
  if (this.files && this.files[0]) readFile(this.files[0]);
  // 값을 비워야 같은 파일을 다시 고를 수 있다.
  this.value = '';
});
```

`readFile` 이 `File` 객체를 이미 붙잡은 뒤라 값을 비워도 읽기에는 영향이 없습니다.

### 검증

수정 전/후 페이지를 각각 브라우저에 띄우고, **같은 파일을 두 번 선택**해서
핸들러가 실행되는지 셌습니다.

```
[before] 1회차 핸들러 실행=true  2회차(같은 파일) 실행=false   <- 버그 재현
[after]  1회차 핸들러 실행=true  2회차(같은 파일) 실행=true    <- 수정 확인
```

아티팩트도 같은 주소로 갱신했습니다:
https://claude.ai/code/artifact/f9c1a6ef-54cf-418a-a57b-0f17497fbe47

### C++ 앱이었다면

`logscope.exe` 쪽은 `IFileOpenDialog` 를 매번 새로 만들어 띄우기 때문에 이 종류의
상태가 남지 않습니다. 코드를 다시 읽어 봤지만 같은 성격의 버그는 찾지 못했습니다.
C++ 앱에서 겪으신 거라면 증상을 조금 더 알려 주세요 —

- 파일 선택 창 자체가 안 뜨는지
- 창은 뜨는데 고른 뒤 화면이 그대로인지
- 튕기는지

셋 중 어느 쪽인지에 따라 봐야 할 곳이 완전히 다릅니다.
