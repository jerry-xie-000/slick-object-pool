/********************************************************************************
 * Copyright (c) 2025-2026 SlickQuant
 * All rights reserved
 *
 * This file is part of the slick-object-pool. Redistribution and use in source and
 * binary forms, with or without modification, are permitted exclusively under
 * the terms of the MIT license which is available at
 * https://github.com/SlickQuant/slick-object-pool/blob/main/LICENSE
 *
 ********************************************************************************/

#pragma once

#include <cstdint>
#include <atomic>
#include <stdexcept>
#include <string>
#include <cassert>
#include <limits>
#include <thread>
#include <utility>
#include <new>
#include <chrono>

namespace slick {

/**
 * @file object_pool.h
 * @brief Lock-free, cache-optimized object pool for high-performance allocation
 *
 * @details
 * This object pool implementation provides:
 * - Lock-free multi-producer multi-consumer (MPMC) architecture
 * - Cache-line alignment to eliminate false sharing
 * - O(1) allocation and deallocation
 * - Zero external dependencies (standard library only)
 * - Automatic heap allocation fallback when pool is exhausted
 *
 * @section memory_layout Memory Layout
 *
 * @code
 * [Cache Line 0: reserved_index_ Producer atomics (isolated cache line)]
 * [Cache Line 1: consumed_      Consumer atomics (isolated cache line)]
 * [Heap:         control_      Ring buffer metadata]
 * [Heap:         buffer_       Pooled objects]
 * [Heap:         free_objects_ Free object pointers]
 * @endcode
 *
 * @section thread_safety Thread Safety
 * - Multiple threads can call allocate() concurrently (lock-free)
 * - Multiple threads can call free() concurrently (lock-free)
 * - reset() is NOT thread-safe
 *
 * @section arm_notes ARM32 Notes
 * - On ARM Cortex-A9 (e.g. Zynq 7020), cache line size is 32 bytes.
 *   Use CacheLineSize=32 template parameter for optimal alignment.
 * - Requires ARMv7-a with LDREXD/STREXD for lock-free 64-bit atomics.
 * - Spin loops include 3-tier backoff (spin/yield/sleep) for non-RT Linux kernels.
 *
 * @section example Example Usage
 * @code
 * slick::ObjectPool<MyStruct, 32> pool(1024);
 * auto* obj = pool.allocate();
 * obj->field = value;
 * pool.free(obj);
 * @endcode
 *
 * @tparam T Object type to pool
 * @tparam CacheLineSize Cache line size in bytes (default 64, use 32 for ARM Cortex-A9)
 *
 * @author SlickQuant
 * @version 0.2.0
 * @date 2025
 * @copyright MIT License
 */
template<typename T, size_t CacheLineSize = 64>
class ObjectPool {
    static_assert(std::is_default_constructible_v<T>,
        "T must be default constructible");
    static_assert(std::atomic<uint64_t>::is_always_lock_free,
        "64-bit atomics must be lock-free on this platform; "
        "ARMv7-a requires LDREXD/STREXD support (-march=armv7-a)");
    static_assert(CacheLineSize > 0 && (CacheLineSize & (CacheLineSize - 1)) == 0,
        "CacheLineSize must be a power of 2");
    static_assert(CacheLineSize >= alignof(std::max_align_t),
        "CacheLineSize must be at least alignof(std::max_align_t)");

#ifdef __cpp_lib_hardware_interference_size
    static constexpr size_t CACHE_LINE_SIZE = std::hardware_destructive_interference_size;
#else
    static constexpr size_t CACHE_LINE_SIZE = CacheLineSize;
#endif

    static constexpr uint64_t INVALID_INDEX = std::numeric_limits<uint64_t>::max();

    struct slot {
        std::atomic<uint64_t> data_index{INVALID_INDEX};
    };

    alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> reserved_index_{0};
    char pad_reserved_[CACHE_LINE_SIZE - sizeof(std::atomic<uint64_t>)];
    alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> consumed_{0};
    char pad_consumed_[CACHE_LINE_SIZE - sizeof(std::atomic<uint64_t>)];

    uint32_t size_;
    uint32_t mask_;
    T* buffer_ = nullptr;
    uintptr_t lower_bound_ = 0;
    uintptr_t upper_bound_ = 0;
    T** free_objects_ = nullptr;
    slot* control_ = nullptr;

    /**
     * @brief 3-tier spin backoff for contention on non-RT systems
     *
     * @details
     * Tier 1 (1-4 spins):   Pure spin - low contention fast path
     * Tier 2 (5-16 spins):  sched_yield - moderate contention
     * Tier 3 (17+ spins):   sleep_for(1us) - high contention fallback
     *
     * @note On non-RT Linux, sleep_for(1us) may actually sleep 1-10ms
     *       depending on kernel HZ (100/250/1000). This is acceptable
     *       as a last-resort backoff after 16 failed spins.
     *
     * @param spin_count Reference to per-call-site spin counter
     */
    static void spin_yield(unsigned& spin_count) noexcept {
        ++spin_count;
        if (spin_count <= 4) {
        } else if (spin_count <= 16) {
            std::this_thread::yield();
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(1));
        }
    }

public:
    /**
     * @brief Construct a new object pool
     *
     * @details
     * Creates a new object pool with local memory allocation.
     * The pool size must be a power of 2 for efficient bit-masking operations.
     * All objects are pre-allocated and initialized.
     *
     * @param size Pool capacity (must be power of 2)
     *
     * @throws std::invalid_argument If size is not a power of 2
     *
     * @post Pool is ready for concurrent allocate/free operations
     *
     * @par Example
     * @code
     * slick::ObjectPool<MyStruct, 32> pool(256);
     * @endcode
     */
    ObjectPool(uint32_t size)
        : size_(size)
        , mask_(size - 1)
    {
        if (size == 0 || (size & (size - 1)) != 0) {
            throw std::invalid_argument(
                "ObjectPool size must be a power of 2, got: " + std::to_string(size));
        }

        buffer_ = new T[size_];
        try {
            free_objects_ = new T*[size_];
            try {
                control_ = new slot[size_];
            } catch (...) {
                delete[] free_objects_;
                free_objects_ = nullptr;
                throw;
            }
        } catch (...) {
            delete[] buffer_;
            buffer_ = nullptr;
            throw;
        }

        for (uint32_t i = 0; i < size_; ++i) {
            auto index = reserve();
            free_objects_[index & mask_] = &buffer_[i];
            publish(index);
        }

        lower_bound_ = reinterpret_cast<uintptr_t>(&buffer_[0]);
        upper_bound_ = reinterpret_cast<uintptr_t>(&buffer_[size_]);
    }

    /**
     * @brief Destructor - cleans up all resources
     */
    ~ObjectPool() noexcept {
        delete[] buffer_;
        buffer_ = nullptr;

        delete[] free_objects_;
        free_objects_ = nullptr;

        delete[] control_;
        control_ = nullptr;
    }

    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;
    ObjectPool(ObjectPool&&) = delete;
    ObjectPool& operator=(ObjectPool&&) = delete;

    /**
     * @brief Get pool capacity
     * @return Maximum number of objects the pool can hold
     */
    [[nodiscard]] uint32_t size() const noexcept {
        return size_;
    }

    /**
     * @brief Allocate an object from the pool
     *
     * @details
     * Returns a pre-allocated object from the pool if available.
     * If the pool appears empty, yields the CPU and retries once before
     * falling back to heap allocation. This retry helps on non-RT Linux
     * where a producer thread may be preempted between reserve() and
     * publish(), causing a transient false-empty condition.
     *
     * @return Pointer to allocated object (never nullptr)
     *
     * @note Objects allocated from heap (when pool exhausted) will be
     *       automatically deleted when returned via free()
     *
     * @par Thread Safety
     * Safe to call concurrently from multiple threads
     *
     * @par Example
     * @code
     * auto* obj = pool.allocate();
     * obj->value = 42;
     * @endcode
     */
    [[nodiscard]] T* allocate() {
        T* obj = consume();
        if (obj) {
            return obj;
        }
        std::this_thread::yield();
        obj = consume();
        return obj ? obj : new T();
    }

    /**
     * @brief Try to allocate an object from the pool without heap fallback
     *
     * @details
     * Returns a pre-allocated object from the pool if available.
     * Unlike allocate(), this method returns nullptr when the pool is
     * exhausted instead of falling back to heap allocation.
     * Preferred in embedded/real-time systems where heap allocation
     * is undesirable.
     *
     * @return Pointer to allocated object, or nullptr if pool is empty
     *
     * @par Thread Safety
     * Safe to call concurrently from multiple threads
     *
     * @par Example
     * @code
     * if (auto* obj = pool.try_allocate()) {
     *     obj->value = 42;
     *     pool.free(obj);
     * } else {
     *     // handle pool exhaustion
     * }
     * @endcode
     */
    [[nodiscard]] T* try_allocate() noexcept {
        return consume();
    }

    /**
     * @brief Return an object to the pool
     *
     * @details
     * Returns an object to the pool for reuse, or deletes it if it didn't
     * come from the pool. Uses lock-free operations for thread safety.
     *
     * This method automatically detects whether the object belongs to the
     * pool by checking its address range. Objects allocated from heap
     * (when pool was exhausted) are automatically deleted.
     *
     * @param obj Pointer to object to return
     *
     * @par Thread Safety
     * Safe to call concurrently from multiple threads
     *
     * @warning Do not free the same object twice
     * @warning Do not access object after calling free()
     */
    void free(T* obj) noexcept {
        if (!obj) {
            return;
        }

        auto o = reinterpret_cast<uintptr_t>(obj);
        if (o >= lower_bound_ && o < upper_bound_) {
            auto index = reserve();
            free_objects_[index & mask_] = obj;
            publish(index);
        } else {
            delete obj;
        }
    }

    /**
     * @brief Reset the pool to initial state
     *
     * @details
     * Reinitializes the pool, making all objects available again.
     * This is primarily intended for testing and should be used with caution.
     *
     * @warning NOT THREAD-SAFE
     * @warning Must be called when NO other threads are accessing the pool
     * @warning Invalidates all outstanding object references
     *
     * @par Use Cases
     * - Unit testing: Reset pool state between tests
     * - Application reset: Clear all allocations during shutdown
     *
     * @note Do not use in production with concurrent access
     */
    void reset() {
        auto* new_control = new slot[size_];
        delete[] control_;
        control_ = new_control;

        reserved_index_.store(0, std::memory_order_relaxed);
        consumed_.store(0, std::memory_order_relaxed);

        for (uint32_t i = 0; i < size_; ++i) {
            auto index = reserve();
            free_objects_[index & mask_] = &buffer_[i];
            publish(index);
        }
    }

private:
    /**
     * @brief Reserve space in the ring buffer for writing
     *
     * @details
     * Atomically reserves one slot in the ring buffer using compare-and-swap.
     * Before the CAS, verifies slot availability (ring buffer not full) to
     * prevent data corruption when producer threads are preempted between
     * reserve() and publish() on non-RT Linux kernels.
     * Includes 3-tier backoff (spin/yield/sleep) for contention.
     *
     * @return Index of the reserved slot
     *
     * @note Lock-free operation using CAS
     */
    uint64_t reserve() noexcept {
        unsigned spin_count = 0;

        while (true) {
            uint64_t current_reserved = reserved_index_.load(std::memory_order_relaxed);
            uint64_t c = consumed_.load(std::memory_order_relaxed);

            if (current_reserved - c >= size_) {
                spin_yield(spin_count);
                continue;
            }

            uint64_t next_reserved = current_reserved + 1;
            if (reserved_index_.compare_exchange_weak(
                    current_reserved, next_reserved,
                    std::memory_order_relaxed,
                    std::memory_order_relaxed)) {
                return current_reserved;
            }

            spin_yield(spin_count);
        }
    }

    /**
     * @brief Publish data written to reserved space
     *
     * @details
     * Makes previously reserved and written data visible to consumers.
     * Must be called after writing to reserved space.
     *
     * @param index Index returned by reserve()
     *
     * @note Uses release memory ordering on data_index for synchronization.
     */
    void publish(uint64_t index) noexcept {
        auto& s = control_[index & mask_];
        s.data_index.store(index, std::memory_order_release);
    }

    /**
     * @brief Consume data from the ring buffer
     *
     * @details
     * Atomically retrieves the next available object from the pool.
     * Returns nullptr if no objects are currently available.
     * Includes 3-tier backoff (spin/yield/sleep) for contention on non-RT systems.
     *
     * @return Pointer to allocated object, or nullptr if pool is empty
     *
     * @note Lock-free operation using CAS
     * @note Reads free_objects_ BEFORE CAS to prevent data race with
     *       producers that may reuse the same slot after CAS succeeds.
     */
    T* consume() noexcept {
        unsigned spin_count = 0;
        while (true) {
            uint64_t current_index = consumed_.load(std::memory_order_relaxed);
            auto current = current_index & mask_;
            slot* current_slot = &control_[current];
            uint64_t stored_index = current_slot->data_index.load(std::memory_order_acquire);

            if (stored_index == INVALID_INDEX || stored_index < current_index) {
                return nullptr;
            }

            T* obj = free_objects_[current];
            uint64_t next_index = stored_index + 1;

            if (consumed_.compare_exchange_weak(
                    current_index, next_index,
                    std::memory_order_relaxed, std::memory_order_relaxed)) {
                return obj;
            }

            spin_yield(spin_count);
        }
    }
};

}   // end namespace slick
