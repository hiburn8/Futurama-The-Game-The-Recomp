// Small COM owner for the native DXVK host; Windows keeps using WRL.
#pragma once
#include <memory>
#include <utility>

namespace fut::platform {
template<class T> class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { Reset(); }
    ComPtr(const ComPtr& other) : ptr_(other.ptr_) { if (ptr_) ptr_->AddRef(); }
    ComPtr(ComPtr&& other) noexcept : ptr_(std::exchange(other.ptr_, nullptr)) {}
    ComPtr& operator=(const ComPtr& other) {
        if (this != std::addressof(other)) {
            T* next = other.ptr_;
            if (next) next->AddRef();
            Reset();
            ptr_ = next;
        }
        return *this;
    }
    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != std::addressof(other)) {
            Reset();
            ptr_ = std::exchange(other.ptr_, nullptr);
        }
        return *this;
    }
    T* Get() const { return ptr_; }
    T* operator->() const { return ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }
    T** GetAddressOf() { return &ptr_; }
    T** operator&() { Reset(); return &ptr_; }
    void Reset() { if (auto* old = std::exchange(ptr_, nullptr)) old->Release(); }
    template<class U> auto As(U** out) const {
        return ptr_->QueryInterface(__uuidof(U), reinterpret_cast<void**>(out));
    }
private:
    T* ptr_ = nullptr;
};
} // namespace fut::platform
