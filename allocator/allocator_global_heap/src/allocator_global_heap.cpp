#include "../include/allocator_global_heap.h"

#include <cstddef>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <utility>

allocator_global_heap::allocator_global_heap() = default;

[[nodiscard]] void *allocator_global_heap::do_allocate_sm(
    size_t size)
{
    if (size == 0) 
    {
        return nullptr;
    }

    if (size > std::numeric_limits<size_t>::max() - size_t_size) 
    {
        throw std::bad_alloc();
    }

    std::lock_guard<std::mutex> lock(_mutex);

    void *raw = ::operator new(size + size_t_size);

    std::memcpy(raw, &size, size_t_size);

    return static_cast<char *>(raw) + size_t_size;
}

void allocator_global_heap::do_deallocate_sm(
    void *at)
{
    if (at == nullptr) 
    {
        return;
    }

    std::lock_guard<std::mutex> lock(_mutex);

    void *raw = static_cast<char *>(at) - size_t_size;

    ::operator delete(raw);
}

allocator_global_heap::~allocator_global_heap() = default;

allocator_global_heap::allocator_global_heap(const allocator_global_heap &other) 
    : allocator_dbg_helper(other), smart_mem_resource(), _mutex()
{
    (void)other;
}

allocator_global_heap &allocator_global_heap::operator=(const allocator_global_heap &other)
{
    if (this != &other) 
    {
    }

    return *this;
}

bool allocator_global_heap::do_is_equal(const std::pmr::memory_resource &other) const noexcept
{
    return dynamic_cast<const allocator_global_heap *>(&other) != nullptr;
}

allocator_global_heap::allocator_global_heap(allocator_global_heap &&other) noexcept
    : allocator_dbg_helper(std::move(other)), smart_mem_resource(), _mutex()
{
    (void)other;
}

allocator_global_heap &allocator_global_heap::operator=(allocator_global_heap &&other) noexcept
{
    if (this != &other) 
    {
    }

    return *this;
}