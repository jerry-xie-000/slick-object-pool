# Object Pool 代码审查与修复报告

**目标平台**: Zynq 7020 (ARM Cortex-A9 双核, ARMv7-a, 32-bit)
**运行环境**: Linux (无 RT 补丁), 多线程, C++17
**审查文件**: `include/slick/object_pool.h`

---

## 第一轮修复（已完成）

| # | 严重度 | 类别 | 问题 | 状态 |
|---|--------|------|------|------|
| 1 | 🔴 Critical | 正确性 | `std::atomic<reserved_info>` 在 ARM32 上非 lock-free | ✅ 已修复 |
| 2 | 🔴 Critical | 兼容性 | `[[unlikely]]` 是 C++20 特性 | ✅ 已修复 |
| 3 | 🟠 High | 性能 | 缓存行大小假设 64 字节，Cortex-A9 为 32 字节 | ✅ 已修复 |
| 4 | 🟠 High | 实时性 | 自旋循环无退避策略，非 RT Linux 下调度延迟 | ✅ 已修复 |
| 5 | 🟠 High | 正确性 | `consume()` 中 `free_objects_` 读取在 CAS 之后，数据竞争 | ✅ 已修复 |
| 6 | 🟡 Medium | 健壮性 | `slot::size` 非原子访问 | ✅ 已修复 |
| 7 | 🟡 Medium | 健壮性 | `free()` 地址范围检查不精确 + 缺少 nullptr 保护 | ✅ 已修复 |
| 8 | 🟡 Medium | 正确性 | `consume()` 的 reset 检测逻辑有竞争条件 | ✅ 已修复 |
| 9 | 🔵 Low | 性能 | `virtual` 析构函数不必要 | ✅ 已修复 |
| 10 | 🔵 Low | 健壮性 | `reset()` 标记 `noexcept` 但内部 `new` 可抛异常 | ✅ 已修复 |
| 11 | 🔴 Critical | 兼容性 | CMakeLists.txt 使用 C++20 | ✅ 已修复 |
| 12 | 🟡 Medium | 功能 | 缺少无堆回退的 `try_allocate()` | ✅ 已修复 |

---

## 第二轮修复

| # | 严重度 | 类别 | 问题 | 状态 |
|---|--------|------|------|------|
| 13 | 🔴 Critical | 健壮性 | 构造函数用 `assert()` 校验 2 的幂，Release 构建下被禁用 | ✅ 已修复 |
| 14 | 🔴 Critical | 兼容性 | 缺少 `<new>` 头文件，`__cpp_lib_hardware_interference_size` 不可用 | ✅ 已修复 |
| 15 | 🟠 High | 健壮性 | `CacheLineSize` 无编译期校验，可传入非法值 | ✅ 已修复 |
| 16 | 🟠 High | 实时性 | `spin_yield` 退避策略过于简单，非 RT Linux 下需分级退避 + nanosleep | ✅ 已修复 |
| 17 | 🟠 High | 性能 | `consumed_` 后无填充，其他成员共享缓存行导致 false sharing | ✅ 已修复 |
| 18 | 🟠 High | 正确性 | `reserve()` 中 `buffer_wrapped` 分支为死代码（n=1 永不触发），含潜在 bug | ✅ 已修复 |
| 19 | 🟠 High | 正确性 | `consume()` 中 skip-ahead 逻辑为死代码，与 `buffer_wrapped` 耦合 | ✅ 已修复 |
| 20 | 🟠 High | 性能 | `slot::size` 始终为 1，浪费 ARM32 L2 缓存（仅 512KB） | ✅ 已修复 |
| 21 | 🟡 Medium | 健壮性 | `reset()` 先 `delete[]` 再 `new`，若 `new` 抛异常则 `control_` 悬空 | ✅ 已修复 |
| 22 | 🟡 Medium | 代码质量 | `operator[]` 和 `get_read_index()` 未使用，为死代码 | ✅ 已修复 |
| 23 | 🟡 Medium | 代码质量 | `consume()` 返回 `pair<T*, uint32_t>` 但 `sz` 始终为 1 | ✅ 已修复 |

---

## 第二轮详细分析与修复说明

### 13. 🔴 构造函数用 `assert()` 校验 2 的幂

**位置**: `object_pool.h:139`

**问题**:
```cpp
assert((size && !(size & (size - 1))) && "size must be power of 2");
```

`assert()` 在 `NDEBUG` 定义时（Release 构建）被完全移除。在嵌入式系统中，Release 构建是生产部署的版本，此时 2 的幂校验消失。传入非法大小会导致：
- `mask_` 计算错误（`size - 1` 不是有效掩码）
- 环形缓冲区索引越界
- 静默的数据损坏，极难排查

**修复**:
替换为运行时异常检查：
```cpp
if (size == 0 || (size & (size - 1)) != 0) {
    throw std::invalid_argument("ObjectPool size must be a power of 2, got: " + std::to_string(size));
}
```

---

### 14. 🔴 缺少 `<new>` 头文件

**位置**: `object_pool.h:82-86`

**问题**:
```cpp
#ifdef __cpp_lib_hardware_interference_size
    static constexpr size_t CACHE_LINE_SIZE = std::hardware_destructive_interference_size;
#else
    static constexpr size_t CACHE_LINE_SIZE = CacheLineSize;
#endif
```

`__cpp_lib_hardware_interference_size` 宏和 `std::hardware_destructive_interference_size` 常量定义在 `<new>` 头文件中。当前代码未包含 `<new>`，因此：
- `__cpp_lib_hardware_interference_size` 永远不会被定义（即使编译器支持）
- 始终回退到手动指定的 `CacheLineSize`，无法利用编译器自动检测

**修复**:
添加 `#include <new>`。

---

### 15. 🟠 `CacheLineSize` 无编译期校验

**位置**: `object_pool.h:74`

**问题**:
`CacheLineSize` 模板参数可以为任意值，包括 0、非 2 的幂、或小于 `alignof(std::max_align_t)` 的值。非法值会导致：
- `alignas(0)` 或 `alignas(3)` 等非法对齐，编译错误或未定义行为
- `alignas` 值小于对象自然对齐，可能被编译器忽略

**修复**:
添加 `static_assert` 校验：
```cpp
static_assert(CacheLineSize > 0 && (CacheLineSize & (CacheLineSize - 1)) == 0,
    "CacheLineSize must be a power of 2");
static_assert(CacheLineSize >= alignof(std::max_align_t),
    "CacheLineSize must be at least alignof(std::max_align_t)");
```

---

### 16. 🟠 `spin_yield` 退避策略过于简单

**位置**: `object_pool.h:106-110`

**问题**:
当前实现仅在自旋 8 次后调用 `std::this_thread::yield()`。在非 RT Linux 上：
- `sched_yield()` 仅为提示，CFS 调度器可能忽略
- 高竞争时持续 yield 造成上下文切换风暴
- 无最终兜底机制，极端情况下线程可能无限自旋

在 Zynq 7020 双核 + 非 RT Linux 场景下，两个线程竞争同一原子变量时：
- 纯自旋：独占 CPU 核心，另一个线程无法调度
- 仅 yield：CFS 可能立即重新调度同一线程
- 需要 nanosleep 兜底：确保让出 CPU 足够长时间

**修复**:
三级退避策略：
```cpp
static void spin_yield(unsigned& spin_count) noexcept {
    ++spin_count;
    if (spin_count <= 4) {
        // 纯自旋：低竞争快速路径
    } else if (spin_count <= 16) {
        // 中等竞争：让出 CPU
        std::this_thread::yield();
    } else {
        // 高竞争：短暂睡眠，确保其他线程有机会运行
        std::this_thread::sleep_for(std::chrono::microseconds(1));
    }
}
```

---

### 17. 🟠 `consumed_` 后无填充，false sharing

**位置**: `object_pool.h:96`

**问题**:
```cpp
alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> reserved_index_{0};
alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> consumed_{0};
uint32_t size_;
uint32_t mask_;
T* buffer_ = nullptr;
// ...
```

`alignas` 确保 `consumed_` 起始于缓存行边界，但不阻止后续成员打包到同一缓存行。在 ARM32（`CacheLineSize=32`）上：

| 偏移 | 成员 | 缓存行 |
|------|------|--------|
| 0 | `reserved_index_` (8B) | Line 0 |
| 8-31 | *padding* | Line 0 |
| 32 | `consumed_` (8B) | Line 1 |
| 40 | `size_` (4B) | **Line 1** |
| 44 | `mask_` (4B) | **Line 1** |
| 48 | `buffer_` (4B) | **Line 1** |
| 52-63 | ... | **Line 1** |

`size_`、`mask_`、`buffer_` 等与 `consumed_` 共享 Line 1。虽然这些成员在构造后只读，但消费者线程每次加载 `consumed_` 时也会将它们拉入缓存，浪费宝贵的 L1 空间（Cortex-A9 L1D 仅 32KB）。

**修复**:
在 `consumed_` 后添加填充，确保后续成员在新缓存行：
```cpp
alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> reserved_index_{0};
char pad_reserved_[CACHE_LINE_SIZE - sizeof(std::atomic<uint64_t>)];
alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> consumed_{0};
char pad_consumed_[CACHE_LINE_SIZE - sizeof(std::atomic<uint64_t>)];
```

---

### 18. 🟠 `reserve()` 中 `buffer_wrapped` 死代码

**位置**: `object_pool.h:330-370`

**问题**:
`reserve(uint32_t n = 1)` 包含 `buffer_wrapped` 分支，用于处理 `n > 1` 时预留空间跨越环形缓冲区末尾的情况。但当前所有调用点均使用 `n = 1`，此分支永远不会执行。

更严重的是，`buffer_wrapped` 路径存在潜在 bug：
- 写入 `control_[current_reserved & mask_]` 在 CAS 成功之后，无内存屏障保护
- 如果未来启用 `n > 1`，多生产者并发 wrap-around 可能导致 `data_index` 不一致

**修复**:
- 移除 `n` 参数，`reserve()` 仅支持单元素预留
- 移除 `buffer_wrapped` 分支及相关代码
- 简化后 `reserve()` 从 40 行减至 15 行，逻辑更清晰

---

### 19. 🟠 `consume()` 中 skip-ahead 死代码

**位置**: `object_pool.h:427-433`

**问题**:
```cpp
if (stored_index > current_index && ((stored_index & mask_) != current)) {
    consumed_.compare_exchange_weak(
        current_index, stored_index,
        std::memory_order_release, std::memory_order_relaxed);
    spin_yield(spin_count);
    continue;
}
```

此逻辑仅用于处理 `reserve(n > 1)` 导致的 wrap-around。由于 `n` 始终为 1，此条件永远不会为真。保留此代码：
- 增加理解难度
- 与已移除的 `buffer_wrapped` 耦合
- 可能在边界条件下误触发

**修复**:
移除 skip-ahead 逻辑。

---

### 20. 🟠 `slot::size` 始终为 1，浪费缓存

**位置**: `object_pool.h:90-93`

**问题**:
```cpp
struct slot {
    std::atomic<uint64_t> data_index{INVALID_INDEX};
    std::atomic<uint32_t> size{1};
};
```

`size` 在当前设计中始终为 1（单元素预留）。每个 slot 多出 4 字节 `atomic<uint32_t>` + 4 字节对齐填充 = 8 字节。对于 2048 元素的池：
- 浪费 16KB（8B × 2048）
- Zynq 7020 L2 缓存仅 512KB，这占 3%
- 额外的缓存行占用降低 `control_` 数组的缓存命中率

**修复**:
- 从 `slot` 中移除 `size` 字段
- `consume()` 中 `next_index = stored_index + 1`（硬编码，因为 size 恒为 1）
- `publish()` 中移除 `size` 写入
- `slot` 从 16 字节缩减为 8 字节，缓存利用率翻倍

---

### 21. 🟡 `reset()` 异常安全问题

**位置**: `object_pool.h:295-308`

**问题**:
```cpp
void reset() {
    delete[] control_;           // control_ 变为悬空指针
    control_ = new slot[size_];  // 若 new 抛出 bad_alloc，control_ 悬空
    // ...
}
```

若 `new slot[size_]` 抛出 `std::bad_alloc`：
- `control_` 已被 delete，指向已释放内存
- 对象池进入不可恢复的损坏状态
- 后续任何操作均为未定义行为

**修复**:
先分配新内存，再释放旧内存：
```cpp
void reset() {
    auto* new_control = new slot[size_];
    delete[] control_;
    control_ = new_control;
    // ...
}
```

若 `new` 抛出异常，`control_` 仍指向有效旧内存，对象池保持可用状态。

---

### 22. 🟡 死代码清理

**位置**: `object_pool.h:311-313, 372-378`

**问题**:
- `get_read_index()`: 仅有声明，无任何调用点
- `operator[]` (非 const 和 const 版本): 无任何调用点

这些函数增加编译时间、二进制大小和维护负担。

**修复**:
移除所有未使用的私有方法。

---

### 23. 🟡 `consume()` 返回类型简化

**位置**: `object_pool.h:415-447`

**问题**:
`consume()` 返回 `std::pair<T*, uint32_t>`，但 `uint32_t` (slot_size) 始终为 1。所有调用点都需要解构 `pair` 并 `assert(sz == 1)`，增加无谓的复杂度。

**修复**:
- `consume()` 返回 `T*`（`nullptr` 表示池空）
- `allocate()` 和 `try_allocate()` 简化为直接使用指针

---

## Zynq 7020 推荐用法

```cpp
#include <slick/object_pool.h>

struct Packet {
    uint8_t data[1500];
    uint32_t len;
};

// 使用 32 字节缓存行对齐（Cortex-A9）
slick::ObjectPool<Packet, 32> packet_pool(2048);

// 线程安全分配（无堆回退）
void rx_thread() {
    while (running) {
        Packet* pkt = packet_pool.try_allocate();
        if (pkt) {
            pkt->len = receive(pkt->data);
            packet_pool.free(pkt);
        } else {
            // 池耗尽，丢弃或等待
        }
    }
}
```

---

## 第三轮修复

| # | 严重度 | 类别 | 问题 | 状态 |
|---|--------|------|------|------|
| 24 | 🔴 Critical | 内存泄漏 | 构造函数在 initializer list 中分配内存，若后续校验抛异常则泄漏 | ✅ 已修复 |
| 25 | 🟠 High | 文档过时 | README.md 仍标注 C++20，代码已改为 C++17 | ✅ 已修复 |
| 26 | 🟠 High | 文档过时 | DOCUMENTATION.md 引用已删除的方法/结构 | ✅ 已修复 |
| 27 | 🟡 Medium | 一致性 | 版本号不一致：CMakeLists.txt=0.1.3, object_pool.h=0.2.0 | ✅ 已修复 |
| 28 | 🟡 Medium | 测试 | `try_allocate()` 无测试覆盖 | ✅ 已修复 |
| 29 | 🟡 Medium | 文档 | `spin_yield` sleep_for(1us) 在非 RT Linux 下实际睡眠 1-10ms 未说明 | ✅ 已修复 |

---

## 第三轮详细分析与修复说明

### 24. 🔴 构造函数内存泄漏

**位置**: `object_pool.h:143-163`

**问题**:
```cpp
ObjectPool(uint32_t size)
    : size_(size)
    , mask_(size - 1)
    , buffer_(new T[size_])        // ← initializer list 中分配
    , free_objects_(new T*[size_]) // ← initializer list 中分配
    , control_(new slot[size_])    // ← initializer list 中分配
{
    if (size == 0 || (size & (size - 1)) != 0) {
        throw std::invalid_argument(...); // ← 抛异常，析构不被调用！
    }
    ...
}
```

C++ 标准规定：构造函数抛异常时，析构函数**不会被调用**。`buffer_`/`free_objects_`/`control_` 是原始指针，其析构是 no-op。因此：

- 传入 `size=1000`（非 2 的幂）：三块内存全部泄漏
- 传入 `size=0`：`new T[0]` 分配少量内存后泄漏
- `new T*[size_]` 抛 `bad_alloc`：`buffer_` 泄漏

在 Zynq 7020 256MB 内存环境下，这是不可接受的。

**修复**:
将内存分配移至构造函数体中，校验通过后再分配，并添加 `bad_alloc` 异常安全保护：
```cpp
ObjectPool(uint32_t size)
    : size_(size)
    , mask_(size - 1)
{
    if (size == 0 || (size & (size - 1)) != 0) {
        throw std::invalid_argument(...);
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
    ...
}
```

---

### 25. 🟠 README.md 仍标注 C++20

**位置**: `README.md:3, 11, 71, 377, 397, 477`

**问题**:
代码已在第一轮修复中从 C++20 降级为 C++17（CMakeLists.txt, object_pool.h），但 README.md 仍多处引用 C++20：
- Badge: `C++20`
- 描述: "C++20 compliant"
- 编译器要求: GCC 10+, Clang 11+, MSVC 2019 16.8+
- 命令示例: `g++ -std=c++20`
- 内存排序: "C++20 atomic memory ordering"

**修复**:
全部更新为 C++17，编译器要求调整为 GCC 8+, Clang 7+, MSVC 2017 15.7+。

---

### 26. 🟠 DOCUMENTATION.md 引用已删除的方法/结构

**位置**: `DOCUMENTATION.md:96-124`

**问题**:
DOCUMENTATION.md 引用了第二轮修复中已删除的多个方法/结构：
- `reserve(uint32_t n)` → 已简化为 `reserve()` 无参数
- `operator[]` → 已删除
- `get_read_index()` → 已删除
- `consume()` 返回 `pair<T*, uint32_t>` → 已简化为返回 `T*`
- `struct reserved_info` → 已删除
- `slot::size` → 已删除
- `@pre` 标签 → 已不再使用
- `///` 行内注释风格 → 已不再使用

**修复**:
完全重写 DOCUMENTATION.md，与当前代码一致：
- 更新所有方法签名和描述
- 添加 `try_allocate()` 文档
- 更新成员变量表（含 padding 字段）
- 更新内部结构（slot 仅含 data_index，8 字节）
- 更新 Doxyfile 版本号为 0.2.0

---

### 27. 🟡 版本号不一致

**位置**: 多文件

**问题**:
| 文件 | 旧版本 |
|------|--------|
| CMakeLists.txt | 0.1.3 |
| object_pool.h @version | 0.2.0 |
| DOCUMENTATION.md Doxyfile | 0.1.0 |

**修复**:
统一为 0.2.0。

---

### 28. 🟡 `try_allocate()` 无测试覆盖

**位置**: `tests/tests.cpp`

**问题**:
`try_allocate()` 在第一轮修复中添加（#12），但测试文件中没有任何测试用例覆盖此方法。缺少的测试场景：
- 基本分配和释放
- 池耗尽时返回 nullptr
- 与 `allocate()` 的行为差异（后者堆回退）
- 多线程并发调用

**修复**:
添加 4 个测试用例：
- `TryAllocateBasic`: 基本分配/释放/数据验证
- `TryAllocateReturnsNullOnExhaustion`: 池耗尽返回 nullptr
- `TryAllocateVsAllocate`: 对比 allocate() 的堆回退行为
- `TryAllocateMultiThreaded`: 多线程并发安全性

---

### 29. 🟡 `spin_yield` sleep_for 文档

**位置**: `object_pool.h:119`

**问题**:
`sleep_for(1us)` 在非 RT Linux 下的实际行为：
- HZ=100: 实际睡眠 ~10ms
- HZ=250: 实际睡眠 ~4ms
- HZ=1000: 实际睡眠 ~1ms

这是非 RT Linux 的 nanosleep 精度限制，由内核时钟中断频率决定。对于 Zynq 7020 场景，用户需要了解此行为以正确评估延迟。

**修复**:
在 `spin_yield` 的 Doxygen 注释中添加 `@note` 说明：
```
@note On non-RT Linux, sleep_for(1us) may actually sleep 1-10ms
      depending on kernel HZ (100/250/1000). This is acceptable
      as a last-resort backoff after 16 failed spins.
```

---

## 第四轮修复

| # | 严重度 | 类别 | 问题 | 状态 |
|---|--------|------|------|------|
| 30 | 🟠 High | ARM 性能 | `reserve()` CAS 成功序使用 `memory_order_release`，ARM 上多余 DMB (~20 cycles) | ✅ 已修复 |
| 31 | 🟠 High | ARM 性能 | `consume()` CAS 成功序使用 `memory_order_release`，ARM 上多余 DMB (~20 cycles) | ✅ 已修复 |
| 32 | 🟡 Medium | 文档错误 | README "Lock-Free MPMC Design" 中 producer/consumer 术语颠倒 | ✅ 已修复 |
| 33 | 🟡 Medium | 代码质量 | `allocate()`、`try_allocate()`、`size()` 缺少 `[[nodiscard]]` | ✅ 已修复 |
| 34 | 🟡 Medium | 代码质量 | `free()` 和 `reserve()` 缺少 `noexcept` | ✅ 已修复 |
| 35 | 🔵 Low | 测试覆盖 | `NullPointerHandling` 测试未实际测试 `free(nullptr)` | ✅ 已修复 |
| 36 | 🟡 Medium | 目标平台性能 | `allocate()` 池空时立即堆回退，无双核 ARM 优化 | ✅ 已修复 |

---

### 30. 🟠 `reserve()` CAS 成功序多余 DMB

**位置**: `object_pool.h:359-362`

**问题**:
```cpp
reserved_index_.compare_exchange_weak(
    current_reserved, next_reserved,
    std::memory_order_release,   // ← 多余
    std::memory_order_relaxed);
```

CAS 前无共享写入，`memory_order_release` 是空操作。真正的生产者-消费者同步通过 `data_index` 的 acquire/release 完成：

```
Producer: write free_objects_[idx] → publish(): data_index.store(release)
Consumer: data_index.load(acquire) → read free_objects_[idx]  ✅ 同步完成
```

在 ARM Cortex-A9 上，`memory_order_release` 编译为 `DMB ISH` 指令（~20 cycles @ 800MHz ≈ 25ns）。`free()` 热路径每次多一次无意义 DMB。

**修复**:
CAS 成功序改为 `memory_order_relaxed`。CAS 仍保证原子性和唯一索引分配，`data_index` 的 acquire/release 保证数据同步。

---

### 31. 🟠 `consume()` CAS 成功序多余 DMB

**位置**: `object_pool.h:417-419`

**问题**:
```cpp
consumed_.compare_exchange_weak(
    current_index, next_index,
    std::memory_order_release,   // ← 多余
    std::memory_order_relaxed);
```

CAS 前仅有读取操作（`consumed_.load(acquire)`, `data_index.load(acquire)`, `free_objects_[current]`），无共享写入。`memory_order_release` 对读取操作无意义。

**修复**:
CAS 成功序改为 `memory_order_relaxed`。`allocate()` 热路径每次减少一次 DMB。

---

### 32. 🟡 README producer/consumer 术语颠倒

**位置**: `README.md:251-252`

**问题**:
```
Producers (threads calling allocate()) atomically reserve slots from the pool
Consumers (threads calling free()) atomically return objects to the pool
```

从环形缓冲区视角（代码实现视角）：
- `free()` 调用 `reserve()` + `publish()` → **生产者**（向环形缓冲区写入条目）
- `allocate()` 调用 `consume()` → **消费者**（从环形缓冲区读取条目）

README 的描述与代码语义相反，且与同一文档中缓存行布局描述矛盾（`reserved_index_` 标注为 "Producer owned"，由 `free()` 使用）。

**修复**:
```
Producers (threads calling free()) atomically return objects to the pool
Consumers (threads calling allocate()) atomically reserve slots from the pool
```

---

### 33. 🟡 缺少 `[[nodiscard]]`

**位置**: `object_pool.h:241, 271, 215`

**问题**:
`allocate()`、`try_allocate()`、`size()` 的返回值丢弃几乎一定是 bug（内存泄漏或无意义调用）。C++17 的 `[[nodiscard]]` 可在编译期检测此类错误。

**修复**:
为三个函数添加 `[[nodiscard]]` 属性。

---

### 34. 🟡 `free()` 和 `reserve()` 缺少 `noexcept`

**位置**: `object_pool.h:294, 353`

**问题**:
`free()` 和 `reserve()` 内部全部操作均为 `noexcept`：
- 原子 CAS：`noexcept`
- `spin_yield()`：已标记 `noexcept`（内部调用 `yield()` 和 `sleep_for()`，均为 `noexcept`）
- `delete obj`：C++17 析构函数默认 `noexcept`
- 指针比较和写入：`noexcept`

未标记 `noexcept` 阻止编译器优化（异常处理开销、内联决策），且与 `consume()` 和 `publish()` 的 `noexcept` 标注不一致。

**修复**:
为 `free()` 和 `reserve()` 添加 `noexcept`。

---

### 35. 🔵 `free(nullptr)` 测试缺失

**位置**: `tests/tests.cpp:437-449`

**问题**:
`NullPointerHandling` 测试名暗示覆盖了 `nullptr` 路径，但实际只测试了外部堆对象释放。`free(nullptr)` 的早期返回路径（`object_pool.h:295-297`）完全未被测试。

**修复**:
在测试中添加 `pool.free(nullptr)` 调用。

---

### 36. 🟡 `allocate()` 池空时立即堆回退

**位置**: `object_pool.h:241-244`

**问题**:
```cpp
T* allocate() {
    T* obj = consume();
    return obj ? obj : new T();  // ← 立即堆回退
}
```

在 Zynq 7020 双核 + 非 RT Linux 场景下，生产者线程可能在 `reserve()` 和 `publish()` 之间被 CFS 调度器抢占。此时：
- `reserved_index_` 已递增，但 `data_index` 未更新
- 所有消费者看到池为空，立即堆分配
- 堆分配代价（~200ns-1us）远高于 yield + 重试（~1-2us）

**修复**:
添加一次 `yield()` + `consume()` 重试：
```cpp
T* allocate() {
    T* obj = consume();
    if (obj) return obj;
    std::this_thread::yield();  // 给被抢占的生产者机会完成 publish
    obj = consume();
    return obj ? obj : new T();
}
```

- 池非空时：零额外开销（第一次 `consume()` 成功直接返回）
- 池暂时空时：一次 yield（~1-2us），可能避免堆分配
- 池真正空时：一次 yield + 一次 `consume()` 失败，额外开销可忽略

---

## 修改文件清单

| 文件 | 修改内容 |
|------|----------|
| `include/slick/object_pool.h` | 第一轮 12 项 + 第二轮 11 项 + 第三轮 2 项 + 第四轮 7 项修复 |
| `CMakeLists.txt` | C++ 标准从 20 改为 17 + 版本号 0.1.3→0.2.0 |
| `README.md` | C++20→17 全面更新 + try_allocate 文档 + 缓存行/内存布局更新 + producer/consumer 术语纠正 |
| `DOCUMENTATION.md` | 完全重写，与当前代码一致 |
| `tests/tests.cpp` | 添加 4 个 try_allocate() 测试用例 + free(nullptr) 测试 |
