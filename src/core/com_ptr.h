// com_ptr.h — COM 인터페이스용 최소 스마트 포인터. 이것 때문에 라이브러리를
// 끌어오지 않으려고 직접 둔다.
#pragma once

#include <unknwn.h>

namespace lc {

template <class T>
class ComPtr {
public:
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ComPtr(ComPtr&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    ComPtr& operator=(ComPtr&& o) noexcept {
        if (this != &o) { reset(); p_ = o.p_; o.p_ = nullptr; }
        return *this;
    }
    ~ComPtr() { reset(); }

    void reset() {
        if (p_) { p_->Release(); p_ = nullptr; }
    }
    T** put() { reset(); return &p_; }
    void** put_void() { reset(); return reinterpret_cast<void**>(&p_); }
    T* get() const { return p_; }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }

private:
    T* p_ = nullptr;
};

}  // namespace lc
