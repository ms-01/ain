// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#ifndef DEFI_SUPPORT_ALLOCATORS_SECURE_H
#define DEFI_SUPPORT_ALLOCATORS_SECURE_H

#include <support/lockedpool.h>
#include <support/cleanse.h>

#include <memory>
#include <new>
#include <string>
#include <utility>

//
// Allocator that locks its contents from being paged
// out of memory and clears its contents before deletion.
//
template <typename T>
struct secure_allocator {
    using value_type = T;

    secure_allocator() = default;
    template <typename U>
    secure_allocator(const secure_allocator<U>&) noexcept {}

    T* allocate(std::size_t n)
    {
        T* allocation = static_cast<T*>(LockedPoolManager::Instance().alloc(sizeof(T) * n));
        if (!allocation) {
            throw std::bad_alloc();
        }
        return allocation;
    }

    void deallocate(T* p, std::size_t n)
    {
        if (p != nullptr) {
            memory_cleanse(p, sizeof(T) * n);
        }
        LockedPoolManager::Instance().free(p);
    }

    template <typename U>
    friend bool operator==(const secure_allocator&, const secure_allocator<U>&) noexcept
    {
        return true;
    }
};

// This is exactly like std::string, but with a custom allocator.
typedef std::basic_string<char, std::char_traits<char>, secure_allocator<char> > SecureString;

/**
 * Deleter for objects that were allocated with secure_allocator.
 * The memory is cleansed by the allocator before it is released.
 */
template<typename T>
struct SecureUniqueDeleter {
    void operator()(T* t) noexcept {
        secure_allocator<T>().deallocate(t, 1);
    }
};

/** A unique_ptr for securely allocated (and cleansed) objects. */
template<typename T>
using secure_unique_ptr = std::unique_ptr<T, SecureUniqueDeleter<T>>;

/** Allocate and construct an object in secure memory. */
template<typename T, typename... Args>
secure_unique_ptr<T> make_secure_unique(Args&&... as)
{
    T* p = secure_allocator<T>().allocate(1);

    // initialize in place, and return as secure_unique_ptr
    try {
        return secure_unique_ptr<T>(new (p) T(std::forward<Args>(as)...));
    } catch (...) {
        secure_allocator<T>().deallocate(p, 1);
        throw;
    }
}

#endif // DEFI_SUPPORT_ALLOCATORS_SECURE_H
