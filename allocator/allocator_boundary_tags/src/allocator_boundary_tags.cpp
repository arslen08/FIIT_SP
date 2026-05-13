#include <new>
#include <stdexcept>
#include <utility>
#include <memory_resource>
#include "../include/allocator_boundary_tags.h"


namespace
{
    struct allocator_metadata
    {
        std::pmr::memory_resource *parent;
        allocator_with_fit_mode::fit_mode mode;
        size_t total_size;
        std::mutex sync;
        void *first_block;
    };

    struct block_metadata
    {
        size_t size_and_flag;     
        void  *prev_block;
        void  *next_block;
        void  *trusted_memory;    
    };

    inline bool   is_busy(size_t f) noexcept   { return (f & 1u) != 0; }
    inline size_t pure_size(size_t f) noexcept { return f & ~static_cast<size_t>(1); }
    inline size_t pack(size_t s, bool busy) noexcept { return s | (busy ? 1u : 0u); }

    inline size_t align_size(size_t s) noexcept
    {
        constexpr size_t A = alignof(void*);
        return (s + A - 1) & ~(A - 1);
    }

    inline allocator_metadata* meta_of(void* trusted) noexcept
    {
        return reinterpret_cast<allocator_metadata*>(trusted);
    }
}

allocator_boundary_tags::allocator_boundary_tags(
        size_t space_size,
        std::pmr::memory_resource *parent_allocator,
        allocator_with_fit_mode::fit_mode allocate_fit_mode)
    : _trusted_memory(nullptr)
{
    const size_t bm = align_size(sizeof(block_metadata));
    if (space_size < bm)
    {
        throw std::logic_error("allocator_boundary_tags: space_size too small");
    }

    if (parent_allocator == nullptr)
    {
        parent_allocator = std::pmr::get_default_resource();
    }

    const size_t am = align_size(sizeof(allocator_metadata));
    const size_t total = am + space_size;

    _trusted_memory = parent_allocator->allocate(total);

    auto *m = meta_of(_trusted_memory);
    m->parent     = parent_allocator;
    m->mode       = allocate_fit_mode;
    m->total_size = total;
    new (&m->sync) std::mutex();

    auto *first = reinterpret_cast<block_metadata*>(
        reinterpret_cast<std::byte*>(_trusted_memory) + am);
    first->size_and_flag  = pack(space_size, false);
    first->prev_block     = nullptr;
    first->next_block     = nullptr;
    first->trusted_memory = _trusted_memory;

    m->first_block = first;
}

allocator_boundary_tags::~allocator_boundary_tags()
{
    if (_trusted_memory == nullptr) return;
    auto *m = meta_of(_trusted_memory);
    auto *parent = m->parent;
    const size_t total = m->total_size;
    m->sync.~mutex();
    parent->deallocate(_trusted_memory, total);
    _trusted_memory = nullptr;
}

allocator_boundary_tags::allocator_boundary_tags(allocator_boundary_tags &&other) noexcept
    : _trusted_memory(other._trusted_memory)
{
    other._trusted_memory = nullptr;
}

allocator_boundary_tags &allocator_boundary_tags::operator=(allocator_boundary_tags &&other) noexcept
{
    if (this != &other)
    {
        if (_trusted_memory != nullptr)
        {
            auto *m = meta_of(_trusted_memory);
            auto *parent = m->parent;
            const size_t total = m->total_size;
            m->sync.~mutex();
            parent->deallocate(_trusted_memory, total);
        }
        _trusted_memory = other._trusted_memory;
        other._trusted_memory = nullptr;
    }
    return *this;
}

allocator_boundary_tags::allocator_boundary_tags(const allocator_boundary_tags &)
{
    throw std::logic_error("allocator_boundary_tags: copying is not supported");
}

allocator_boundary_tags &allocator_boundary_tags::operator=(const allocator_boundary_tags &)
{
    throw std::logic_error("allocator_boundary_tags: copying is not supported");
}


[[nodiscard]] void *allocator_boundary_tags::do_allocate_sm(size_t size)
{
    if (_trusted_memory == nullptr) throw std::bad_alloc();
    auto *m = meta_of(_trusted_memory);
    std::lock_guard<std::mutex> lock(m->sync);

    const size_t bm = align_size(sizeof(block_metadata));
    const size_t need = bm + size;     

    block_metadata *target = nullptr;
    block_metadata *cur = static_cast<block_metadata*>(m->first_block);

    while (cur != nullptr)
    {
        const size_t cs = pure_size(cur->size_and_flag);
        if (!is_busy(cur->size_and_flag) && cs >= need)
        {
            switch (m->mode)
            {
                case fit_mode::first_fit:
                    target = cur;
                    break;
                case fit_mode::the_best_fit:
                    if (target == nullptr || cs < pure_size(target->size_and_flag)) target = cur;
                    break;
                case fit_mode::the_worst_fit:
                    if (target == nullptr || cs > pure_size(target->size_and_flag)) target = cur;
                    break;
            }
            if (m->mode == fit_mode::first_fit) break;
        }
        cur = static_cast<block_metadata*>(cur->next_block);
    }

    if (target == nullptr) throw std::bad_alloc();

    const size_t target_sz = pure_size(target->size_and_flag);

    if (target_sz >= need + bm)
    {
        auto *tail = reinterpret_cast<block_metadata*>(
            reinterpret_cast<std::byte*>(target) + need);
        tail->size_and_flag  = pack(target_sz - need, false);
        tail->prev_block     = target;
        tail->next_block     = target->next_block;
        tail->trusted_memory = _trusted_memory;

        if (target->next_block)
        {
            static_cast<block_metadata*>(target->next_block)->prev_block = tail;
        }
        target->next_block = tail;
        target->size_and_flag = pack(need, true);
    }
    else
    {
        target->size_and_flag = pack(target_sz, true);
    }

    return reinterpret_cast<std::byte*>(target) + bm;
}

void allocator_boundary_tags::do_deallocate_sm(void *at)
{
    if (at == nullptr || _trusted_memory == nullptr) return;
    auto *m = meta_of(_trusted_memory);
    std::lock_guard<std::mutex> lock(m->sync);

    const size_t bm = align_size(sizeof(block_metadata));
    auto *block = reinterpret_cast<block_metadata*>(
        static_cast<std::byte*>(at) - bm);

    if (block->trusted_memory != _trusted_memory)
    {
        throw std::logic_error("allocator_boundary_tags: block does not belong to this allocator");
    }

    if (block->next_block && static_cast<block_metadata*>(block->next_block)->prev_block != block)
    {
        throw std::logic_error("allocator_boundary_tags: block list inconsistent");
    }
    if (block->prev_block && static_cast<block_metadata*>(block->prev_block)->next_block != block)
    {
        throw std::logic_error("allocator_boundary_tags: block list inconsistent");
    }

    if (!is_busy(block->size_and_flag))
    {
        throw std::logic_error("allocator_boundary_tags: double free");
    }

    size_t sz = pure_size(block->size_and_flag);
    block->size_and_flag = pack(sz, false);

    if (block->next_block)
    {
        auto *nx = static_cast<block_metadata*>(block->next_block);
        if (!is_busy(nx->size_and_flag))
        {
            sz += pure_size(nx->size_and_flag);
            block->size_and_flag = pack(sz, false);
            block->next_block = nx->next_block;
            if (nx->next_block)
            {
                static_cast<block_metadata*>(nx->next_block)->prev_block = block;
            }
        }
    }

    if (block->prev_block)
    {
        auto *pv = static_cast<block_metadata*>(block->prev_block);
        if (!is_busy(pv->size_and_flag))
        {
            const size_t ps = pure_size(pv->size_and_flag) + sz;
            pv->size_and_flag = pack(ps, false);
            pv->next_block = block->next_block;
            if (block->next_block)
            {
                static_cast<block_metadata*>(block->next_block)->prev_block = pv;
            }
        }
    }
}

bool allocator_boundary_tags::do_is_equal(const std::pmr::memory_resource &other) const noexcept
{
    return this == &other;
}

inline void allocator_boundary_tags::set_fit_mode(allocator_with_fit_mode::fit_mode mode)
{
    if (_trusted_memory == nullptr) return;
    auto *m = meta_of(_trusted_memory);
    std::lock_guard<std::mutex> lock(m->sync);
    m->mode = mode;
}

std::vector<allocator_test_utils::block_info> allocator_boundary_tags::get_blocks_info() const
{
    if (_trusted_memory == nullptr) return {};
    auto *m = meta_of(_trusted_memory);
    std::lock_guard<std::mutex> lock(m->sync);
    return get_blocks_info_inner();
}

std::vector<allocator_test_utils::block_info> allocator_boundary_tags::get_blocks_info_inner() const
{
    std::vector<allocator_test_utils::block_info> out;
    if (_trusted_memory == nullptr) return out;

    auto *cur = static_cast<block_metadata*>(meta_of(_trusted_memory)->first_block);
    while (cur != nullptr)
    {
        out.push_back({ pure_size(cur->size_and_flag), is_busy(cur->size_and_flag) });
        cur = static_cast<block_metadata*>(cur->next_block);
    }
    return out;
}

allocator_boundary_tags::boundary_iterator allocator_boundary_tags::begin() const noexcept
{
    return boundary_iterator(_trusted_memory);
}

allocator_boundary_tags::boundary_iterator allocator_boundary_tags::end() const noexcept
{
    return boundary_iterator();
}

allocator_boundary_tags::boundary_iterator::boundary_iterator()
    : _occupied_ptr(nullptr), _occupied(false), _trusted_memory(nullptr) {}

allocator_boundary_tags::boundary_iterator::boundary_iterator(void *trusted)
    : _occupied_ptr(nullptr), _occupied(false), _trusted_memory(trusted)
{
    if (trusted == nullptr) return;
    _occupied_ptr = meta_of(trusted)->first_block;
    if (_occupied_ptr)
    {
        _occupied = is_busy(static_cast<block_metadata*>(_occupied_ptr)->size_and_flag);
    }
}

bool allocator_boundary_tags::boundary_iterator::operator==(const boundary_iterator &o) const noexcept
{ return _occupied_ptr == o._occupied_ptr; }

bool allocator_boundary_tags::boundary_iterator::operator!=(const boundary_iterator &o) const noexcept
{ return !(*this == o); }

allocator_boundary_tags::boundary_iterator&
allocator_boundary_tags::boundary_iterator::operator++() & noexcept
{
    if (_occupied_ptr == nullptr) return *this;
    auto *cur = static_cast<block_metadata*>(_occupied_ptr);
    _occupied_ptr = cur->next_block;
    if (_occupied_ptr)
    {
        _occupied = is_busy(static_cast<block_metadata*>(_occupied_ptr)->size_and_flag);
    }
    else
    {
        _occupied = false;
    }
    return *this;
}

allocator_boundary_tags::boundary_iterator&
allocator_boundary_tags::boundary_iterator::operator--() & noexcept
{
    if (_occupied_ptr == nullptr) return *this;
    auto *cur = static_cast<block_metadata*>(_occupied_ptr);
    _occupied_ptr = cur->prev_block;
    if (_occupied_ptr)
    {
        _occupied = is_busy(static_cast<block_metadata*>(_occupied_ptr)->size_and_flag);
    }
    else
    {
        _occupied = false;
    }
    return *this;
}

allocator_boundary_tags::boundary_iterator
allocator_boundary_tags::boundary_iterator::operator++(int)
{ boundary_iterator t(*this); ++(*this); return t; }

allocator_boundary_tags::boundary_iterator
allocator_boundary_tags::boundary_iterator::operator--(int)
{ boundary_iterator t(*this); --(*this); return t; }

size_t allocator_boundary_tags::boundary_iterator::size() const noexcept
{
    if (_occupied_ptr == nullptr) return 0;
    return pure_size(static_cast<block_metadata*>(_occupied_ptr)->size_and_flag);
}

bool allocator_boundary_tags::boundary_iterator::occupied() const noexcept
{
    return _occupied;
}

void* allocator_boundary_tags::boundary_iterator::operator*() const noexcept
{
    if (_occupied_ptr == nullptr) return nullptr;
    const size_t bm = align_size(sizeof(block_metadata));
    return static_cast<std::byte*>(_occupied_ptr) + bm;
}

void* allocator_boundary_tags::boundary_iterator::get_ptr() const noexcept
{
    return _occupied_ptr;
}