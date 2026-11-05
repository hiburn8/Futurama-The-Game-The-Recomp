// A COM smart pointer for the platforms without Microsoft's (wrl/client.h): what D3D11Device.cpp uses of
// Microsoft::WRL::ComPtr, over DXVK-native's COM-shaped objects (docs/analysis/linux_plan.md). Windows keeps the real
// one.
#pragma once

#ifndef _WIN32

#include <memory>
#include <utility>

namespace fut::render::xd3d {

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ComPtr(std::nullptr_t) {}
    ComPtr(T* p) : p_(p) { if (p_) p_->AddRef(); }
    ComPtr(const ComPtr& o) : p_(o.p_) { if (p_) p_->AddRef(); }
    ComPtr(ComPtr&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    ~ComPtr() { Reset(); }
    ComPtr& operator=(const ComPtr& o) {
        if (this != std::addressof(o)) { if (o.p_) o.p_->AddRef(); Reset(); p_ = o.p_; }
        return *this;
    }
    ComPtr& operator=(ComPtr&& o) noexcept {
        if (this != std::addressof(o)) { Reset(); p_ = o.p_; o.p_ = nullptr; }
        return *this;
    }
    ComPtr& operator=(std::nullptr_t) { Reset(); return *this; }
    T* Get() const { return p_; }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
    T* const* GetAddressOf() const { return &p_; }
    T** GetAddressOf() { return &p_; }
    T** ReleaseAndGetAddressOf() { Reset(); return &p_; }
    T** operator&() { return ReleaseAndGetAddressOf(); }   // (as WRL's: an out-parameter)
    void Reset() { if (p_) { p_->Release(); p_ = nullptr; } }
    T* Detach() { T* p = p_; p_ = nullptr; return p; }
    template <typename U>
    HRESULT As(ComPtr<U>* out) const {
        return p_->QueryInterface(__uuidof(U), reinterpret_cast<void**>(out->ReleaseAndGetAddressOf()));
    }
    template <typename U>
    HRESULT As(U** out) const {                       // (`&other` goes through operator& above)
        return p_->QueryInterface(__uuidof(U), reinterpret_cast<void**>(out));
    }

private:
    T* p_ = nullptr;
};

}  // namespace fut::render::xd3d

#endif
