// NetImmerse 4.2 reference counting (NiRefObject / NiPointer).
//   orig layout: vtable at +0, u32 m_uiRefCount at +4; DecRefCount deletes through the scalar deleting
//   destructor (vtbl+0) when the count reaches zero. NiPointer<T> is the engine's intrusive smart pointer.
#pragma once

#include "core/Types.h"

#include <utility>

namespace fut::ni {

class NiRefObject {
public:
    NiRefObject() = default;
    NiRefObject(const NiRefObject&) = delete;
    NiRefObject& operator=(const NiRefObject&) = delete;
    virtual ~NiRefObject() = default;

    void IncRefCount() { ++refCount_; }
    void DecRefCount() {
        if (--refCount_ == 0) delete this;
    }
    u32 RefCount() const { return refCount_; }

private:
    u32 refCount_ = 0;
};

template <class T>
class NiPointer {
public:
    NiPointer() = default;
    NiPointer(T* p) : p_(p) { if (p_) p_->IncRefCount(); }    // NOLINT: implicit like the engine's
    NiPointer(const NiPointer& o) : NiPointer(o.p_) {}
    NiPointer(NiPointer&& o) noexcept : p_(std::exchange(o.p_, nullptr)) {}
    ~NiPointer() { if (p_) p_->DecRefCount(); }
    NiPointer& operator=(T* p) {
        if (p != p_) {
            if (p) p->IncRefCount();
            if (p_) p_->DecRefCount();
            p_ = p;
        }
        return *this;
    }
    NiPointer& operator=(const NiPointer& o) { return *this = o.p_; }
    NiPointer& operator=(NiPointer&& o) noexcept {
        if (this != &o) {
            if (p_) p_->DecRefCount();
            p_ = std::exchange(o.p_, nullptr);
        }
        return *this;
    }
    T* Get() const { return p_; }
    T* operator->() const { return p_; }
    T& operator*() const { return *p_; }
    explicit operator bool() const { return p_ != nullptr; }

private:
    T* p_ = nullptr;
};

// A pointer the original keeps without a reference (a NiTList / NiTPointerList entry: the nodes' effect lists, the
// effect states' lists): the object's own destructor takes it out of the lists it knows of.
template <class T>
class NiRawPointer {
public:
    NiRawPointer() = default;
    NiRawPointer(T* p) : p_(p) {}                    // NOLINT: implicit like NiPointer
    T* Get() const { return p_; }
    T* operator->() const { return p_; }
    T& operator*() const { return *p_; }
    explicit operator bool() const { return p_ != nullptr; }

private:
    T* p_ = nullptr;
};

}  // namespace fut::ni
