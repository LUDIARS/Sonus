// Minimal owning pointer for COM interfaces (Release on reset / destruction).
// No exceptions, no RTTI; used only on the thread that created the object.
#ifndef SND_PLATFORM_SRC_WINDOWS_COM_PTR_HPP
#define SND_PLATFORM_SRC_WINDOWS_COM_PTR_HPP

namespace snd::platform::detail::wasapi {

template <class T>
class ComPtr {
 public:
  ComPtr() noexcept = default;
  ~ComPtr() { Reset(); }
  ComPtr(const ComPtr&) = delete;
  ComPtr& operator=(const ComPtr&) = delete;

  T* Get() const noexcept { return p_; }
  T* operator->() const noexcept { return p_; }
  explicit operator bool() const noexcept { return p_ != nullptr; }
  // For out-parameters: releases the current object first.
  T** Put() noexcept {
    Reset();
    return &p_;
  }
  void** PutVoid() noexcept { return reinterpret_cast<void**>(Put()); }
  void Reset() noexcept {
    if (p_ != nullptr) p_->Release();
    p_ = nullptr;
  }

 private:
  T* p_ = nullptr;
};

}  // namespace snd::platform::detail::wasapi

#endif  // SND_PLATFORM_SRC_WINDOWS_COM_PTR_HPP
