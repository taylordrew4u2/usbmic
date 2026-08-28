#pragma once
#include <windows.h>

// A stand-in for Microsoft::WRL::ComPtr covering the operations the backend
// uses. Ownership semantics match: constructing from a raw pointer takes the
// reference, assignment adds one, destruction releases.
namespace Microsoft {
namespace WRL {

/// What operator& yields. A raw T** cannot convert implicitly to the void** an
/// Activate or CoCreateInstance call wants, so -- as real WRL does -- the
/// address is handed out through a proxy that converts to both.
template <typename T>
class ComPtrRef
{
public:
    explicit ComPtrRef (T** address) : ptr (address) {}

    operator T**() const { return ptr; }
    operator void**() const { return reinterpret_cast<void**> (ptr); }

    T** get() const { return ptr; }

private:
    T** ptr;
};

template <typename T>
class ComPtr
{
public:
    using InterfaceType = T;

    ComPtr() = default;
    ComPtr (const ComPtr& other) : ptr (other.ptr) { if (ptr) ptr->AddRef(); }

    ComPtr (ComPtr&& other) noexcept : ptr (other.ptr) { other.ptr = nullptr; }

    ~ComPtr() { if (ptr) ptr->Release(); }

    ComPtr& operator= (const ComPtr& other)
    {
        if (this != &other)
        {
            if (other.ptr) other.ptr->AddRef();
            if (ptr) ptr->Release();
            ptr = other.ptr;
        }
        return *this;
    }

    T* operator->() const { return ptr; }
    T* Get() const { return ptr; }

    /// Both spellings the backend uses to receive a freshly created interface.
    /// Neither releases: a call that fills them is expected to be given an
    /// empty ComPtr, exactly as on Windows.
    T** GetAddressOf() { return &ptr; }
    ComPtrRef<T> operator&() { return ComPtrRef<T> (&ptr); }

    void Reset()
    {
        if (ptr) ptr->Release();
        ptr = nullptr;
    }

    bool operator== (decltype (nullptr)) const { return ptr == nullptr; }
    bool operator!= (decltype (nullptr)) const { return ptr != nullptr; }
    explicit operator bool() const { return ptr != nullptr; }

private:
    T* ptr = nullptr;
};

} // namespace WRL
} // namespace Microsoft

namespace mmasim {

template <typename T>
const GUID& uuidOfPtr (const Microsoft::WRL::ComPtrRef<T>&) { return SimUuid<T>::value; }

template <typename T>
void** ppvHelper (const Microsoft::WRL::ComPtrRef<T>& ref)
{
    return reinterpret_cast<void**> (ref.get());
}

} // namespace mmasim
