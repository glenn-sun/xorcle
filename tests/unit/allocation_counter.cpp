#include "allocation_counter.h"

#include <cstdlib>
#include <new>

namespace {

std::size_t allocations = 0;

void* counted_allocate(std::size_t bytes) {
    ++allocations;
    void* memory = std::malloc(bytes == 0 ? 1 : bytes);
    if (memory == nullptr) {
        throw std::bad_alloc();
    }
    return memory;
}

void* counted_allocate(std::size_t bytes, std::align_val_t alignment) {
    ++allocations;
    const std::size_t align = static_cast<std::size_t>(alignment);
    const std::size_t rounded = ((bytes + align - 1) / align) * align;
    void* memory = std::aligned_alloc(align, rounded == 0 ? align : rounded);
    if (memory == nullptr) {
        throw std::bad_alloc();
    }
    return memory;
}

} // namespace

namespace xorcle {

std::size_t allocation_count() noexcept { return allocations; }

} // namespace xorcle

void* operator new(std::size_t bytes) { return counted_allocate(bytes); }
void* operator new[](std::size_t bytes) { return counted_allocate(bytes); }
void* operator new(std::size_t bytes, std::align_val_t alignment) {
    return counted_allocate(bytes, alignment);
}
void* operator new[](std::size_t bytes, std::align_val_t alignment) {
    return counted_allocate(bytes, alignment);
}
void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept {
    try {
        return counted_allocate(bytes);
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
}
void* operator new[](std::size_t bytes, const std::nothrow_t&) noexcept {
    try {
        return counted_allocate(bytes);
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete(void* memory, std::align_val_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::align_val_t) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t, std::align_val_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept { std::free(memory); }
