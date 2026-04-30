#include "../include/allocator_sorted_list.h"

#include <cstddef>
#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>

namespace
{
    constexpr size_t off_parent      = 0;
    constexpr size_t off_fit_mode    = sizeof(std::pmr::memory_resource *);
    constexpr size_t off_space_size  = off_fit_mode + sizeof(allocator_with_fit_mode::fit_mode);
    constexpr size_t off_mutex       = off_space_size + sizeof(size_t);
    constexpr size_t off_first_free  = off_mutex + sizeof(std::mutex);
    constexpr size_t allocator_meta  = off_first_free + sizeof(void *);

    constexpr size_t off_block_link  = 0;
    constexpr size_t off_block_size  = sizeof(void *);
    constexpr size_t block_meta      = off_block_size + sizeof(size_t);

    inline char *as_bytes(void *p) noexcept { return static_cast<char *>(p); }
    inline const char *as_bytes(const void *p) noexcept { return static_cast<const char *>(p); }

    inline std::pmr::memory_resource *&parent_of(void *trusted) noexcept
    {
        return *reinterpret_cast<std::pmr::memory_resource **>(as_bytes(trusted) + off_parent);
    }
    inline allocator_with_fit_mode::fit_mode &fit_of(void *trusted) noexcept
    {
        return *reinterpret_cast<allocator_with_fit_mode::fit_mode *>(as_bytes(trusted) + off_fit_mode);
    }
    inline size_t &space_of(void *trusted) noexcept
    {
        return *reinterpret_cast<size_t *>(as_bytes(trusted) + off_space_size);
    }
    inline std::mutex &mutex_of(void *trusted) noexcept
    {
        return *reinterpret_cast<std::mutex *>(as_bytes(trusted) + off_mutex);
    }
    inline void *&first_free_of(void *trusted) noexcept
    {
        return *reinterpret_cast<void **>(as_bytes(trusted) + off_first_free);
    }

    inline void *&block_link(void *block) noexcept
    {
        return *reinterpret_cast<void **>(as_bytes(block) + off_block_link);
    }
    inline size_t &block_size(void *block) noexcept
    {
        return *reinterpret_cast<size_t *>(as_bytes(block) + off_block_size);
    }
    inline size_t block_size_c(const void *block) noexcept
    {
        return *reinterpret_cast<const size_t *>(as_bytes(block) + off_block_size);
    }
    inline void *block_link_c(const void *block) noexcept
    {
        return *reinterpret_cast<void *const *>(as_bytes(block) + off_block_link);
    }

    inline void *user_of(void *block) noexcept       { return as_bytes(block) + block_meta; }
    inline void *block_of(void *user) noexcept       { return as_bytes(user) - block_meta; }

    inline void *user_area_begin(void *trusted) noexcept
    {
        return as_bytes(trusted) + allocator_meta;
    }
    inline void *user_area_end(void *trusted) noexcept
    {
        return as_bytes(trusted) + allocator_meta + space_of(trusted);
    }
}

allocator_sorted_list::allocator_sorted_list(
    size_t space_size,
    std::pmr::memory_resource *parent_allocator,
    allocator_with_fit_mode::fit_mode allocate_fit_mode)
    : _trusted_memory(nullptr)
{
    if (space_size < block_meta + 1)
    {
        throw std::logic_error("allocator_sorted_list: space_size is too small");
    }

    if (space_size > std::numeric_limits<size_t>::max() - allocator_meta)
    {
        throw std::bad_alloc();
    }

    std::pmr::memory_resource *parent =
        (parent_allocator != nullptr) ? parent_allocator : std::pmr::get_default_resource();

    const size_t total = allocator_meta + space_size;
    _trusted_memory = parent->allocate(total, alignof(std::max_align_t));

    new (as_bytes(_trusted_memory) + off_mutex) std::mutex();

    parent_of(_trusted_memory) = parent;
    fit_of(_trusted_memory)    = allocate_fit_mode;
    space_of(_trusted_memory)  = space_size;

    void *initial = user_area_begin(_trusted_memory);
    block_link(initial) = nullptr;
    block_size(initial) = space_size - block_meta; 
    first_free_of(_trusted_memory) = initial;
}

allocator_sorted_list::~allocator_sorted_list()
{
    if (_trusted_memory == nullptr)
    {
        return;
    }
    std::pmr::memory_resource *parent = parent_of(_trusted_memory);
    const size_t total = allocator_meta + space_of(_trusted_memory);

    mutex_of(_trusted_memory).~mutex();

    parent->deallocate(_trusted_memory, total, alignof(std::max_align_t));
    _trusted_memory = nullptr;
}

allocator_sorted_list::allocator_sorted_list(allocator_sorted_list &&other) noexcept
    : _trusted_memory(other._trusted_memory)
{
    other._trusted_memory = nullptr;
}

allocator_sorted_list &allocator_sorted_list::operator=(allocator_sorted_list &&other) noexcept
{
    if (this != &other)
    {
        if (_trusted_memory != nullptr)
        {
            std::pmr::memory_resource *parent = parent_of(_trusted_memory);
            const size_t total = allocator_meta + space_of(_trusted_memory);
            mutex_of(_trusted_memory).~mutex();
            parent->deallocate(_trusted_memory, total, alignof(std::max_align_t));
        }
        _trusted_memory = other._trusted_memory;
        other._trusted_memory = nullptr;
    }
    return *this;
}

allocator_sorted_list::allocator_sorted_list(const allocator_sorted_list &other)
    : _trusted_memory(nullptr)
{
    (void)other;
    throw std::logic_error("allocator_sorted_list: copy construction is not supported");
}

allocator_sorted_list &allocator_sorted_list::operator=(const allocator_sorted_list &other)
{
    if (this != &other)
    {
        throw std::logic_error("allocator_sorted_list: copy assignment is not supported");
    }
    return *this;
}

bool allocator_sorted_list::do_is_equal(const std::pmr::memory_resource &other) const noexcept
{
    return this == &other;
}

[[nodiscard]] void *allocator_sorted_list::do_allocate_sm(size_t size)
{
    if (size == 0)
    {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(mutex_of(_trusted_memory));

    const fit_mode mode = fit_of(_trusted_memory);

    void *chosen = nullptr;
    void *chosen_prev = nullptr;     
    size_t chosen_size = 0;

    void *prev = nullptr;
    void *cur = first_free_of(_trusted_memory);

    while (cur != nullptr)
    {
        const size_t s = block_size(cur);

        if (s >= size)
        {
            bool take = false;
            switch (mode)
            {
                case fit_mode::first_fit:    take = (chosen == nullptr); break;
                case fit_mode::the_best_fit: take = (chosen == nullptr) || (s < chosen_size); break;
                case fit_mode::the_worst_fit: take = (chosen == nullptr) || (s > chosen_size); break;
            }

            if (take)
            {
                chosen = cur;
                chosen_prev = prev;
                chosen_size = s;
                if (mode == fit_mode::first_fit)
                {
                    break;
                }
            }
        }

        prev = cur;
        cur = block_link(cur);
    }

    if (chosen == nullptr)
    {
        throw std::bad_alloc();
    }

    void *next_after_chosen = block_link(chosen);
    const size_t leftover = chosen_size - size;

    void *new_link_target;
    if (leftover >= block_meta + 1)
    {
        void *split = as_bytes(chosen) + block_meta + size;
        block_size(split) = leftover - block_meta;
        block_link(split) = next_after_chosen;
        block_size(chosen) = size;
        new_link_target = split;
    }
    else
    {
        new_link_target = next_after_chosen;
    }

    if (chosen_prev == nullptr)
    {
        first_free_of(_trusted_memory) = new_link_target;
    }
    else
    {
        block_link(chosen_prev) = new_link_target;
    }

    block_link(chosen) = _trusted_memory;

    return user_of(chosen);
}

void allocator_sorted_list::do_deallocate_sm(void *at)
{
    if (at == nullptr)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_of(_trusted_memory));

    void *block = block_of(at);

    char *uarea_b = static_cast<char *>(user_area_begin(_trusted_memory));
    char *uarea_e = static_cast<char *>(user_area_end(_trusted_memory));
    char *block_b = static_cast<char *>(block);

    if (block_b < uarea_b || block_b >= uarea_e)
    {
        throw std::logic_error("allocator_sorted_list::deallocate: pointer does not belong to this allocator");
    }
    if (block_link(block) != _trusted_memory)
    {
        throw std::logic_error("allocator_sorted_list::deallocate: block ownership mismatch (double free or foreign pointer)");
    }

    const size_t this_size = block_size(block);

    void *prev = nullptr;
    void *next = first_free_of(_trusted_memory);
    while (next != nullptr && next < block)
    {
        prev = next;
        next = block_link(next);
    }

    block_link(block) = next;
    if (prev == nullptr)
    {
        first_free_of(_trusted_memory) = block;
    }
    else
    {
        block_link(prev) = block;
    }

    if (next != nullptr)
    {
        char *block_end = as_bytes(block) + block_meta + this_size;
        if (block_end == as_bytes(next))
        {
            block_size(block) = this_size + block_meta + block_size(next);
            block_link(block) = block_link(next);
        }
    }

    if (prev != nullptr)
    {
        char *prev_end = as_bytes(prev) + block_meta + block_size(prev);
        if (prev_end == as_bytes(block))
        {
            block_size(prev) = block_size(prev) + block_meta + block_size(block);
            block_link(prev) = block_link(block);
        }
    }
}

void allocator_sorted_list::set_fit_mode(allocator_with_fit_mode::fit_mode mode)
{
    std::lock_guard<std::mutex> lock(mutex_of(_trusted_memory));
    fit_of(_trusted_memory) = mode;
}

std::vector<allocator_test_utils::block_info> allocator_sorted_list::get_blocks_info() const noexcept
{
    try
    {
        std::lock_guard<std::mutex> lock(mutex_of(_trusted_memory));
        return get_blocks_info_inner();
    }
    catch (...)
    {
        return {};
    }
}

std::vector<allocator_test_utils::block_info> allocator_sorted_list::get_blocks_info_inner() const
{
    std::vector<allocator_test_utils::block_info> result;

    void *trusted = _trusted_memory;
    char *uarea_b = static_cast<char *>(user_area_begin(trusted));
    char *uarea_e = static_cast<char *>(user_area_end(trusted));

    char *cur = uarea_b;
    void *first_free = first_free_of(trusted);

    while (cur < uarea_e)
    {
        const size_t s = block_size_c(cur);
        bool is_free = false;
        for (void *f = first_free; f != nullptr; f = block_link_c(f))
        {
            if (f == cur) { is_free = true; break; }
        }

        allocator_test_utils::block_info info;
        info.block_size = s;
        info.is_block_occupied = !is_free;
        result.push_back(info);

        cur += block_meta + s;
    }
    return result;
}

allocator_sorted_list::sorted_free_iterator allocator_sorted_list::free_begin() const noexcept
{
    return sorted_free_iterator(first_free_of(_trusted_memory));
}

allocator_sorted_list::sorted_free_iterator allocator_sorted_list::free_end() const noexcept
{
    return sorted_free_iterator(nullptr);
}

allocator_sorted_list::sorted_iterator allocator_sorted_list::begin() const noexcept
{
    return sorted_iterator(_trusted_memory);
}

allocator_sorted_list::sorted_iterator allocator_sorted_list::end() const noexcept
{
    return sorted_iterator{};
}

allocator_sorted_list::sorted_free_iterator::sorted_free_iterator()
    : _free_ptr(nullptr) {}

allocator_sorted_list::sorted_free_iterator::sorted_free_iterator(void *trusted_or_first)
    : _free_ptr(trusted_or_first) {}

bool allocator_sorted_list::sorted_free_iterator::operator==(const sorted_free_iterator &other) const noexcept
{
    return _free_ptr == other._free_ptr;
}

bool allocator_sorted_list::sorted_free_iterator::operator!=(const sorted_free_iterator &other) const noexcept
{
    return !(*this == other);
}

allocator_sorted_list::sorted_free_iterator &allocator_sorted_list::sorted_free_iterator::operator++() & noexcept
{
    if (_free_ptr != nullptr)
    {
        _free_ptr = block_link(_free_ptr);
    }
    return *this;
}

allocator_sorted_list::sorted_free_iterator allocator_sorted_list::sorted_free_iterator::operator++(int)
{
    sorted_free_iterator tmp = *this;
    ++(*this);
    return tmp;
}

size_t allocator_sorted_list::sorted_free_iterator::size() const noexcept
{
    if (_free_ptr == nullptr) return 0;
    return block_size(_free_ptr);
}

void *allocator_sorted_list::sorted_free_iterator::operator*() const noexcept
{
    return _free_ptr;
}

allocator_sorted_list::sorted_iterator::sorted_iterator()
    : _free_ptr(nullptr), _current_ptr(nullptr), _trusted_memory(nullptr) {}

allocator_sorted_list::sorted_iterator::sorted_iterator(void *trusted)
    : _free_ptr(trusted ? first_free_of(trusted) : nullptr),
      _current_ptr(trusted ? user_area_begin(trusted) : nullptr),
      _trusted_memory(trusted)
{
    if (trusted != nullptr)
    {
        char *uarea_e = static_cast<char *>(user_area_end(trusted));
        if (static_cast<char *>(_current_ptr) >= uarea_e)
        {
            _current_ptr = nullptr;
        }
    }
}

bool allocator_sorted_list::sorted_iterator::operator==(const sorted_iterator &other) const noexcept
{
    return _current_ptr == other._current_ptr;
}

bool allocator_sorted_list::sorted_iterator::operator!=(const sorted_iterator &other) const noexcept
{
    return !(*this == other);
}

allocator_sorted_list::sorted_iterator &allocator_sorted_list::sorted_iterator::operator++() & noexcept
{
    if (_current_ptr == nullptr || _trusted_memory == nullptr) return *this;

    const size_t s = block_size(_current_ptr);
    char *next_block = as_bytes(_current_ptr) + block_meta + s;
    char *uarea_e = static_cast<char *>(user_area_end(_trusted_memory));

    if (next_block >= uarea_e)
    {
        _current_ptr = nullptr;
    }
    else
    {
        _current_ptr = next_block;
    }
    return *this;
}

allocator_sorted_list::sorted_iterator allocator_sorted_list::sorted_iterator::operator++(int)
{
    sorted_iterator tmp = *this;
    ++(*this);
    return tmp;
}

size_t allocator_sorted_list::sorted_iterator::size() const noexcept
{
    if (_current_ptr == nullptr) return 0;
    return block_size(_current_ptr);
}

void *allocator_sorted_list::sorted_iterator::operator*() const noexcept
{
    return _current_ptr;
}

bool allocator_sorted_list::sorted_iterator::occupied() const noexcept
{
    if (_current_ptr == nullptr || _trusted_memory == nullptr) return false;
    for (void *f = first_free_of(_trusted_memory); f != nullptr; f = block_link(f))
    {
        if (f == _current_ptr) return false;
    }
    return true;
}