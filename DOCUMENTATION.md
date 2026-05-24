# slick-object-pool - Documentation Guide

## Overview

This document provides a comprehensive guide to the documentation in `slick/object_pool.h`.

## Documentation Style

The code uses **Doxygen-style** documentation comments for automatic API documentation generation.

### Doxygen Tags Used

| Tag | Purpose | Example |
|-----|---------|---------|
| `@file` | File description | File-level overview |
| `@brief` | Short description | One-line summary |
| `@details` | Detailed description | In-depth explanation |
| `@tparam` | Template parameter | Template type requirements |
| `@param` | Function parameter | Parameter description |
| `@return` | Return value | What the function returns |
| `@throws` | Exception specification | When exceptions are thrown |
| `@note` | Important note | Additional information |
| `@warning` | Warning | Critical warnings |
| `@post` | Postcondition | State after calling |
| `@par` | Paragraph heading | Subsection headers |
| `@code/@endcode` | Code example | Inline code examples |
| `@section` | Documentation section | Major sections |

## Documentation Coverage

### File-Level Documentation

Includes:
- Brief description of the library
- Detailed feature list
- Memory layout diagram
- Type requirements
- Thread safety guarantees
- ARM32-specific notes
- Usage examples
- Author and version information

### Class Documentation

**ObjectPool Template Class**

Comprehensive documentation covering:
- Purpose and design
- Template type requirements (`T` must be default constructible)
- Cache line size template parameter (`CacheLineSize`, default 64, use 32 for ARM Cortex-A9)
- Platform requirements (64-bit atomics must be lock-free)
- Thread safety model
- Example usage patterns

### Static Assertions

| Assertion | Purpose |
|-----------|---------|
| `std::is_default_constructible_v<T>` | T must have a default constructor |
| `std::atomic<uint64_t>::is_always_lock_free` | 64-bit atomics must be lock-free (ARMv7-a requires LDREXD/STREXD) |
| `CacheLineSize > 0 && power of 2` | Cache line size must be a valid power of 2 |
| `CacheLineSize >= alignof(std::max_align_t)` | Cache line size must respect max alignment |

### Public API Documentation

#### Constructors

1. **ObjectPool(uint32_t size)**
   - Full parameter documentation
   - Exception specifications (`std::invalid_argument` for non-power-of-2 size)
   - Postconditions
   - Usage examples
   - Exception-safe: validates size before allocating memory
   - If allocation fails (`bad_alloc`), previously allocated memory is properly freed

#### Destructor

- Cleanup behavior documentation (deletes buffer_, free_objects_, control_)

#### Public Methods

1. **uint32_t size() const noexcept**
   - Pool capacity query
   - Returns the power-of-2 size passed to constructor

2. **T\* allocate()**
   - Comprehensive documentation
   - Lock-free pool allocation with heap fallback
   - Never returns nullptr
   - Thread-safe
   - Usage examples

3. **T\* try_allocate() noexcept**
   - Lock-free pool allocation without heap fallback
   - Returns nullptr when pool is exhausted
   - Preferred for embedded/real-time systems
   - Thread-safe
   - Usage examples

4. **void free(T\* obj)**
   - Dual behavior (pool vs heap objects)
   - Automatic detection via address range check
   - nullptr-safe (no-op for nullptr)
   - Critical warnings (no double-free, no use-after-free)
   - Thread-safe

5. **void reset()**
   - Purpose and use cases (testing, application reset)
   - NOT THREAD-SAFE
   - Invalidates all outstanding object references
   - Exception-safe: allocates new control_ before deleting old

### Private Implementation Documentation

#### Internal Methods

1. **uint64_t reserve()**
   - Lock-free single-element reservation using CAS
   - 3-tier backoff for contention
   - Returns reserved index

2. **void publish(uint64_t index) noexcept**
   - Release-store to slot's data_index
   - Synchronizes with consumer's acquire-load

3. **T\* consume() noexcept**
   - Lock-free consumption using CAS
   - Reads free_objects_ BEFORE CAS to prevent data race
   - Returns nullptr when pool is empty
   - 3-tier backoff for contention

4. **static void spin_yield(unsigned& spin_count) noexcept**
   - 3-tier backoff: spin → yield → sleep
   - Documents non-RT Linux sleep_for(1us) behavior (actual 1-10ms)

#### Member Variables

| Variable | Type | Description |
|----------|------|-------------|
| `reserved_index_` | `atomic<uint64_t>` | Producer counter (cache-line isolated) |
| `pad_reserved_` | `char[]` | Padding to prevent false sharing |
| `consumed_` | `atomic<uint64_t>` | Consumer counter (cache-line isolated) |
| `pad_consumed_` | `char[]` | Padding to prevent false sharing |
| `size_` | `uint32_t` | Pool capacity (power of 2) |
| `mask_` | `uint32_t` | Bitmask for index wrapping (size - 1) |
| `buffer_` | `T*` | Pooled objects array |
| `lower_bound_` | `uintptr_t` | Start address of buffer_ (for pool membership check) |
| `upper_bound_` | `uintptr_t` | End address of buffer_ (for pool membership check) |
| `free_objects_` | `T**` | Free object pointers ring buffer |
| `control_` | `slot*` | Ring buffer slot metadata |

#### Internal Structures

1. **struct slot**
   - `std::atomic<uint64_t> data_index` - Published index (INVALID_INDEX = unpublished)
   - 8 bytes per slot (cache-efficient)

## Generating API Documentation

### Using Doxygen

1. **Install Doxygen:**
   ```bash
   # Ubuntu/Debian
   sudo apt-get install doxygen graphviz

   # macOS
   brew install doxygen graphviz

   # Windows
   # Download from https://www.doxygen.nl/download.html
   ```

2. **Create Doxyfile:**
   ```bash
   cd /path/to/slick-object-pool
   doxygen -g
   ```

3. **Configure Doxyfile:**
   ```
   PROJECT_NAME           = "slick-object-pool"
   PROJECT_BRIEF          = "Lock-free object pool for C++17"
   PROJECT_VERSION        = 0.2.0
   OUTPUT_DIRECTORY       = docs
   INPUT                  = include/slick
   RECURSIVE              = YES
   EXTRACT_ALL            = YES
   EXTRACT_PRIVATE        = NO
   EXTRACT_STATIC         = YES
   GENERATE_HTML          = YES
   GENERATE_LATEX         = NO
   SOURCE_BROWSER         = YES
   INLINE_SOURCES         = YES
   HAVE_DOT               = YES
   CALL_GRAPH             = YES
   CALLER_GRAPH           = YES
   ```

4. **Generate Documentation:**
   ```bash
   doxygen Doxyfile
   ```

5. **View Documentation:**
   ```bash
   # Open in browser
   open docs/html/index.html  # macOS
   xdg-open docs/html/index.html  # Linux
   start docs/html/index.html  # Windows
   ```

## Documentation Best Practices

### What's Documented

- **All public APIs** - Complete parameter and return value docs
- **Examples** - Working code examples for common use cases
- **Thread safety** - Explicit guarantees and warnings
- **Performance** - Expected performance characteristics
- **Exceptions** - What exceptions are thrown and when
- **Warnings** - Critical usage warnings
- **Platform notes** - Platform-specific behavior (ARM32, non-RT Linux)

### Documentation Quality

| Aspect | Coverage |
|--------|----------|
| Public API | 100% |
| Private methods | 100% |
| Member variables | 100% |
| Code examples | Extensive |
| Thread safety | Explicit |
| Platform specifics | Complete |

## Reading the Documentation

### For Users (Public API)

Focus on:
- Constructor documentation
- `allocate()`, `try_allocate()`, and `free()` methods
- Thread safety guarantees
- Code examples
- Warnings and notes

### For Contributors (Implementation)

Additionally review:
- Private method documentation
- Internal structure documentation
- Memory layout details
- Platform-specific implementations

### For Library Maintainers

Full documentation including:
- Implementation algorithms (MPMC ring buffer with CAS)
- Memory ordering semantics (acquire-release on data_index)
- Platform-specific details (ARM32 LDREXD/STREXD, cache line sizes)
- Performance characteristics

## Updating Documentation

### When to Update

- Adding new public methods
- Changing method signatures
- Modifying behavior
- Adding new features
- Fixing bugs that change semantics
- Performance improvements

### Documentation Checklist

When adding/modifying code:

- [ ] Add/update `@brief` description
- [ ] Add/update `@details` if complex
- [ ] Document all parameters with `@param`
- [ ] Document return value with `@return`
- [ ] List exceptions with `@throws`
- [ ] Add thread safety notes
- [ ] Include performance characteristics
- [ ] Add code example if helpful
- [ ] Add warnings if needed
- [ ] Update file-level docs if major change

## Related Documentation

- **README.md** - User-facing documentation and examples
- **REVIEW.md** - Code review and fix history
- **TESTING.md** - Testing strategies and sanitizer usage
- **Comments in code** - Implementation notes (not in public API docs)

---

**Documentation Version:** 2.0
**Last Updated:** 2026
**Maintained By:** SlickQuant
