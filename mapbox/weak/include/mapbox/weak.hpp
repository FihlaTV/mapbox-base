#pragma once

#include <algorithm>
#include <atomic>
#include <cassert>
#include <functional>
#include <memory>
#include <shared_mutex>
#include <thread>
#include <type_traits>

namespace mapbox {
namespace base {

/// @cond internal
namespace internal {

class WeakPtrSharedData {
public:
    WeakPtrSharedData() = default;

    void sharedLock() { mutex_.lock_shared(); }

    void sharedUnlock() { mutex_.unlock_shared(); }

    void invalidate() {
        std::lock_guard<std::shared_timed_mutex> lock(mutex_);
        valid_ = false;
    }

    bool valid() const { return valid_; }

private:
    std::shared_timed_mutex mutex_; // Blocks on WeakPtrFactory destruction.
    std::atomic<bool> valid_{true};
};

using StrongRef = std::shared_ptr<WeakPtrSharedData>;
using WeakRef = std::weak_ptr<WeakPtrSharedData>;

template <typename Object>
class WeakPtrBase;

} // namespace internal
/// @endcond

/**
 * @brief Scope guard for the WeakPtr-wrapped object
 *
 * The WeakPtr-wrapped object is guaranteed not to be deleted while
 * a WeakPtrGuard instance is present in the scope.
 */
class WeakPtrGuard {
public:
    /**
     * @brief Default move constructor
     */
    WeakPtrGuard(WeakPtrGuard&&) noexcept = default;
    ~WeakPtrGuard() {
        if (strong_) {
            strong_->sharedUnlock();
        }
    }

private:
    explicit WeakPtrGuard(internal::StrongRef strong) : strong_(std::move(strong)) {
        assert(!strong_ || strong_->valid());
    }
    internal::StrongRef strong_;

    template <typename Object>
    friend class internal::WeakPtrBase;
};

namespace internal {

/**
 * @brief Base type for \c WeakPtr class.
 *
 * Contains the generic API for weak pointer classes.
 *
 * This class helps to create a \c WeakPtr template specialization for a
 * certain type, enabling the type-specific semantics.
 *
 * \sa WeakPtr
 *
 * @tparam Object the managed object type
 */
template <typename Object>
class WeakPtrBase {
public:
    /**
     * Gets a lock that protects the Object, giving
     * the guarantee that it will not be deleted (if not
     * deleted yet).
     *
     * Note that it won't make the Object thread-safe, but
     * rather make sure it exists (or not) when the lock
     * is being held.
     *
     * Note: there *MUST* be only one instance of the
     * guard referring to the same \a WeakPtrFactory
     * available in the scope at a time.
     */
    WeakPtrGuard lock() const {
        if (StrongRef strong = weak_.lock()) {
            strong->sharedLock();
            if (strong->valid()) {
                return WeakPtrGuard(std::move(strong));
            }
            strong->sharedUnlock();
        }
        return WeakPtrGuard(nullptr);
    }

    /**
     * @brief Quick nonblocking check that the wrapped Object still exists.
     *
     * Checks if the weak pointer is still pointing to a valid
     * object. Note that if the WeakPtrFactory lives in a different
     * thread, a `false` result cannot be guaranteed to be
     * correct since there is an implicit race condition,
     * but a `true` result is always correct.
     *
     * @return given the thread restrictions, true if expired,
     * false otherwise.
     */
    bool expired() const {
        if (StrongRef strong = weak_.lock()) {
            return !strong->valid();
        }
        return true;
    }

    /**
     * @brief Quick nonblocking check that the wrapped Object still exists.
     *
     * \sa expired()
     *
     * @return given the thread restrictions, true if the object exists,
     * false otherwise.
     */
    explicit operator bool() const { return !expired(); }

protected:
    /**
     * Get a pointer to the wrapped object.
     * The caller *MUST* call lock() and keep locker, then
     * check if it was nulled before using it.
     *
     * Usage should be as brief as possible, because it might
     * potentially block the thread where the Object lives.
     *
     * @return pointer to the object, nullptr if expired.
     */
    Object* object() const {
        if (StrongRef strong = weak_.lock()) {
            if (strong->valid()) {
                return ptr_;
            }
        }
        return nullptr;
    }
    /// @cond internal
    WeakPtrBase() = default;
    WeakPtrBase(WeakPtrBase&&) noexcept = default;
    WeakPtrBase(const WeakPtrBase&) noexcept = default;
    template <typename U> // NOLINTNEXTLINE
    WeakPtrBase(WeakPtrBase<U>&& other) noexcept
        : weak_(std::move(other.weak_)), ptr_(static_cast<Object*>(other.ptr_)) {}
    explicit WeakPtrBase(WeakRef weak, Object* ptr) : weak_(std::move(weak)), ptr_(ptr) { assert(ptr_); }
    WeakPtrBase& operator=(WeakPtrBase&& other) noexcept = default;
    WeakPtrBase& operator=(const WeakPtrBase& other) = default;

    ~WeakPtrBase() = default;

private:
    WeakRef weak_;
    Object* ptr_{};
    template <typename T>
    friend class WeakPtrBase;
    /// @endcond
};

} // namespace internal

/**
 * @brief Default implementation of a weak pointer to an object.
 *
 * Weak pointers are safe to access even if the
 * pointer outlives the Object this class wraps.
 *
 * This class will manage only object lifetime
 * but will not deal with thread-safeness of the
 * objects it is wrapping.
 */
template <typename Object>
class WeakPtr final : public internal::WeakPtrBase<Object> {
public:
    /**
     * @brief Default constructor.
     *
     * Constructs empty \c WeakPtr.
     */
    WeakPtr() = default;

    /**
     * @brief Converting move constructor
     *
     * \a other becomes empty after the call.
     *
     * @tparam U a type, which \c Object is convertible to
     * @param other \c WeakPtr<U> instance
     */
    template <typename U> // NOLINTNEXTLINE
    WeakPtr(WeakPtr<U>&& other) noexcept : internal::WeakPtrBase<Object>(std::move(other)) {}

    /**
     * @brief Default move constructor.
     */
    WeakPtr(WeakPtr&&) noexcept = default;

    /**
     * @brief Default copy constructor.
     */
    WeakPtr(const WeakPtr&) noexcept = default;

    /**
     * @brief Replaces the managed object with the one managed by \a other.
     *
     *  \a other becomes empty after the call.
     *
     * @param other
     * @return WeakPtr&  \c *this
     */
    WeakPtr& operator=(WeakPtr&& other) noexcept = default;

    /**
     * @brief Replaces the managed object with the one managed by \a other.
     *
     * @param other
     * @return WeakPtr&  \c *this
     */
    WeakPtr& operator=(const WeakPtr& other) = default;

    /**
     * @brief Dereferences the stored pointer.
     *
     * Must not be called on empty \c WeakPtr.
     *
     * @return Object*  the stored pointer.
     */
    Object* operator->() const {
        Object* ptr = this->object();
        assert(ptr);
        return ptr;
    }

private:
    explicit WeakPtr(internal::WeakRef weak, Object* object) : internal::WeakPtrBase<Object>(std::move(weak), object) {}

    template <typename T>
    friend class WeakPtrFactory;
};

/**
 * @brief Object wrapper that can create weak pointers.
 *
 * WARNING: the WeakPtrFactory should all be at the bottom of
 * the list of member of the class, making it the first to
 * be destroyed and the last to be initialized.
 */
template <typename Object>
class WeakPtrFactory final {
public:
    WeakPtrFactory(const WeakPtrFactory&) = delete;
    WeakPtrFactory& operator=(const WeakPtrFactory&) = delete;

    /**
     * @brief Construct a new \c WeakPtrFactory object.
     *
     * @param obj an \c Object instance to wrap.
     */
    explicit WeakPtrFactory(Object* obj) : strong_(std::make_shared<internal::WeakPtrSharedData>()), obj_(obj) {}

    /**
     * Destroys the factory, invalidating all the
     * weak pointers to this object, i.e. makes them empty.
     */
    ~WeakPtrFactory() { strong_->invalidate(); }

    /**
     * Make a weak pointer for this WeakPtrFactory. Weak pointer
     * can be used for safely accessing the Object and not worry
     * about lifetime.
     *
     * @return a weak pointer.
     */
    WeakPtr<Object> makeWeakPtr() { return WeakPtr<Object>{strong_, obj_}; }

    /**
     * @brief Makes a weak wrapper for calling a method on the wrapped
     * \c Object instance.
     *
     * While the wrapped \c Object instance exists, calling the returned wrapper is
     * equivalent to invoking \a method on the instance. Note that the instance deletion
     * is blocked during the wrapper call.
     *
     * If the wrapped \c Object instance does not exist, calling the returned wrapper
     * is ignored.
     *
     * The example below illustrates creating an \c std::function instance from the
     * returned wrapper.
     *
     * \code
     *  class Object {
     *      void foo(int);
     *      std::function<void(int)> makeWeakFoo() {
     *	        return weakFactory.makeWeakMethod(&Object::foo);
     *      }
     *      mbauto::WeakPtrFactory<Object> weakFactory{this};
     *  };
     * \endcode
     *
     * @param method Pointer to an \c Object class method.
     * @return auto Callable object
     */
    template <typename Method>
    auto makeWeakMethod(Method method) {
        return [weakPtr = makeWeakPtr(), method](auto&&... params) mutable {
            WeakPtrGuard guard = weakPtr.lock();
            if (Object* obj = weakPtr.object()) {
                (obj->*method)(std::forward<decltype(params)>(params)...);
            }
        };
    }

private:
    internal::StrongRef strong_;
    Object* obj_;
};

} // namespace base
} // namespace mapbox