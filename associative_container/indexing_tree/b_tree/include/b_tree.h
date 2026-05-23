#ifndef SYS_PROG_B_TREE_H
#define SYS_PROG_B_TREE_H

#include "../../../include/associative_container.h"

#include <iterator>
#include <utility>
#include <stack>
#include <vector>
#include <stdexcept>
#include <initializer_list>
#include <boost/container/static_vector.hpp>
#include <pp_allocator.h>
#include <associative_container.h>
#include <not_implemented.h>

template <typename tkey, typename tvalue, comparator<tkey> compare = std::less<tkey>, std::size_t t = 5>
class B_tree final : private compare
{
public:
    using tree_data_type      = std::pair<tkey, tvalue>;
    using tree_data_type_const = std::pair<const tkey, tvalue>;
    using value_type          = tree_data_type_const;

private:
    static constexpr const size_t minimum_keys_in_node = t - 1;
    static constexpr const size_t maximum_keys_in_node = 2 * t - 1;

    inline bool compare_keys(const tkey& lhs, const tkey& rhs) const
    {
        return compare::operator()(lhs, rhs);
    }

    inline bool compare_pairs(const tree_data_type& lhs, const tree_data_type& rhs) const
    {
        return compare_keys(lhs.first, rhs.first);
    }

    struct btree_node
    {
        boost::container::static_vector<tree_data_type, maximum_keys_in_node + 1> _keys;
        boost::container::static_vector<btree_node*, maximum_keys_in_node + 2> _pointers;

        btree_node() noexcept = default;
    };

    pp_allocator<value_type> _allocator;
    btree_node* _root;
    size_t _size;

    pp_allocator<value_type> get_allocator() const noexcept
    {
        return _allocator;
    }

    btree_node* new_node()
    {
        return _allocator.template new_object<btree_node>();
    }

    void delete_node(btree_node* n)
    {
        if (n)
        {
            _allocator.template delete_object<btree_node>(n);
        }
    }

    static bool is_leaf(const btree_node* n) noexcept
    {
        return n == nullptr || n->_pointers.empty();
    }

    void destroy_subtree(btree_node* n)
    {
        if (!n) return;

        if (!is_leaf(n))
        {
            for (auto* c : n->_pointers)
            {
                destroy_subtree(c);
            }
        }

        delete_node(n);
    }

    btree_node* copy_subtree(const btree_node* src)
    {
        if (!src) return nullptr;

        btree_node* n = new_node();

        try
        {
            for (auto const& kv : src->_keys)
            {
                n->_keys.push_back(kv);
            }

            if (!is_leaf(src))
            {
                for (auto* c : src->_pointers)
                {
                    n->_pointers.push_back(copy_subtree(c));
                }
            }
        }
        catch (...)
        {
            if (!is_leaf(n))
            {
                for (auto* c : n->_pointers)
                {
                    destroy_subtree(c);
                }
            }

            delete_node(n);
            throw;
        }

        return n;
    }

    size_t lower_in_node(const btree_node* n, const tkey& key) const
    {
        size_t lo = 0, hi = n->_keys.size();

        while (lo < hi)
        {
            size_t mid = (lo + hi) / 2;

            if (compare_keys(n->_keys[mid].first, key))
            {
                lo = mid + 1;
            }
            else
            {
                hi = mid;
            }
        }

        return lo;
    }

    std::pair<btree_node*, tree_data_type> split_overfull(btree_node* y)
    {
        btree_node* z = new_node();
        const size_t med = t;

        for (size_t j = med + 1; j < y->_keys.size(); ++j)
        {
            z->_keys.push_back(std::move(y->_keys[j]));
        }

        if (!is_leaf(y))
        {
            for (size_t j = med + 1; j < y->_pointers.size(); ++j)
            {
                z->_pointers.push_back(y->_pointers[j]);
            }
        }

        tree_data_type median = std::move(y->_keys[med]);

        y->_keys.resize(med);

        if (!is_leaf(y))
        {
            y->_pointers.resize(med + 1);
        }

        return {z, std::move(median)};
    }

    template <typename K, typename V>
    bool insert_post(K&& key, V&& val)
    {
        if (!_root)
        {
            _root = new_node();
        }

        std::vector<std::pair<btree_node*, size_t>> path;
        btree_node* cur = _root;

        while (true)
        {
            size_t i = lower_in_node(cur, key);

            if (i < cur->_keys.size()
                && !compare_keys(key, cur->_keys[i].first)
                && !compare_keys(cur->_keys[i].first, key))
            {
                return false;
            }

            if (is_leaf(cur))
            {
                cur->_keys.insert(
                    cur->_keys.begin() + i,
                    tree_data_type(std::forward<K>(key), std::forward<V>(val))
                );
                break;
            }

            path.push_back({cur, i});
            cur = cur->_pointers[i];
        }

        while (cur->_keys.size() > maximum_keys_in_node)
        {
            auto [z, median] = split_overfull(cur);

            if (path.empty())
            {
                btree_node* new_root = new_node();
                new_root->_keys.push_back(std::move(median));
                new_root->_pointers.push_back(cur);
                new_root->_pointers.push_back(z);
                _root = new_root;
                return true;
            }

            auto [parent, idx] = path.back();
            path.pop_back();

            parent->_keys.insert(parent->_keys.begin() + idx, std::move(median));
            parent->_pointers.insert(parent->_pointers.begin() + idx + 1, z);
            cur = parent;
        }

        return true;
    }

    void build_path(
        const tkey& key,
        std::stack<std::pair<btree_node**, size_t>>& path,
        size_t& idx,
        bool& found
    )
    {
        found = false;
        btree_node** cur = &_root;

        while (*cur)
        {
            size_t i = lower_in_node(*cur, key);

            if (i < (*cur)->_keys.size()
                && !compare_keys(key, (*cur)->_keys[i].first)
                && !compare_keys((*cur)->_keys[i].first, key))
            {
                path.push({cur, i});
                idx = i;
                found = true;
                return;
            }

            if (is_leaf(*cur))
            {
                return;
            }

            path.push({cur, i});
            cur = &((*cur)->_pointers[i]);
        }
    }

    void ensure_can_descend(btree_node* parent, size_t i)
    {
        btree_node* child = parent->_pointers[i];

        if (child->_keys.size() >= t)
        {
            return;
        }

        btree_node* left  = (i > 0) ? parent->_pointers[i - 1] : nullptr;
        btree_node* right = (i + 1 < parent->_pointers.size()) ? parent->_pointers[i + 1] : nullptr;

        if (left && left->_keys.size() >= t)
        {
            child->_keys.insert(child->_keys.begin(), std::move(parent->_keys[i - 1]));
            parent->_keys[i - 1] = std::move(left->_keys.back());
            left->_keys.pop_back();

            if (!is_leaf(left))
            {
                child->_pointers.insert(child->_pointers.begin(), left->_pointers.back());
                left->_pointers.pop_back();
            }

            return;
        }

        if (right && right->_keys.size() >= t)
        {
            child->_keys.push_back(std::move(parent->_keys[i]));
            parent->_keys[i] = std::move(right->_keys.front());
            right->_keys.erase(right->_keys.begin());

            if (!is_leaf(right))
            {
                child->_pointers.push_back(right->_pointers.front());
                right->_pointers.erase(right->_pointers.begin());
            }

            return;
        }

        if (left)
        {
            left->_keys.push_back(std::move(parent->_keys[i - 1]));

            for (auto& kv : child->_keys)
            {
                left->_keys.push_back(std::move(kv));
            }

            if (!is_leaf(child))
            {
                for (auto* p : child->_pointers)
                {
                    left->_pointers.push_back(p);
                }
            }

            parent->_keys.erase(parent->_keys.begin() + (i - 1));
            parent->_pointers.erase(parent->_pointers.begin() + i);
            delete_node(child);
        }
        else if (right)
        {
            child->_keys.push_back(std::move(parent->_keys[i]));

            for (auto& kv : right->_keys)
            {
                child->_keys.push_back(std::move(kv));
            }

            if (!is_leaf(right))
            {
                for (auto* p : right->_pointers)
                {
                    child->_pointers.push_back(p);
                }
            }

            parent->_keys.erase(parent->_keys.begin() + i);
            parent->_pointers.erase(parent->_pointers.begin() + i + 1);
            delete_node(right);
        }
    }

    bool erase_from(btree_node* n, const tkey& key)
    {
        size_t i = lower_in_node(n, key);
        const bool here = (i < n->_keys.size()
                           && !compare_keys(key, n->_keys[i].first)
                           && !compare_keys(n->_keys[i].first, key));

        if (here && is_leaf(n))
        {
            n->_keys.erase(n->_keys.begin() + i);
            return true;
        }

        if (here)
        {
            btree_node* lc = n->_pointers[i];
            btree_node* rc = n->_pointers[i + 1];

            if (lc->_keys.size() >= t)
            {
                btree_node* cur = lc;

                while (!is_leaf(cur))
                {
                    cur = cur->_pointers.back();
                }

                n->_keys[i] = cur->_keys.back();
                return erase_from(lc, n->_keys[i].first);
            }

            if (rc->_keys.size() >= t)
            {
                btree_node* cur = rc;

                while (!is_leaf(cur))
                {
                    cur = cur->_pointers.front();
                }

                n->_keys[i] = cur->_keys.front();
                return erase_from(rc, n->_keys[i].first);
            }

            lc->_keys.push_back(std::move(n->_keys[i]));

            for (auto& kv : rc->_keys)
            {
                lc->_keys.push_back(std::move(kv));
            }

            if (!is_leaf(rc))
            {
                for (auto* p : rc->_pointers)
                {
                    lc->_pointers.push_back(p);
                }
            }

            n->_keys.erase(n->_keys.begin() + i);
            n->_pointers.erase(n->_pointers.begin() + i + 1);
            delete_node(rc);

            return erase_from(lc, key);
        }

        if (is_leaf(n))
        {
            return false;
        }

        ensure_can_descend(n, i);

        size_t j = lower_in_node(n, key);

        if (j > n->_pointers.size() - 1)
        {
            j = n->_pointers.size() - 1;
        }

        return erase_from(n->_pointers[j], key);
    }

public:
    explicit B_tree(
        const compare& cmp = compare(),
        pp_allocator<value_type> alloc = pp_allocator<value_type>()
    )
        : compare(cmp)
        , _allocator(alloc)
        , _root(nullptr)
        , _size(0)
    {
    }

    explicit B_tree(
        pp_allocator<value_type> alloc,
        const compare& comp = compare()
    )
        : compare(comp)
        , _allocator(alloc)
        , _root(nullptr)
        , _size(0)
    {
    }

    template <input_iterator_for_pair<tkey, tvalue> iterator>
    explicit B_tree(
        iterator begin,
        iterator end,
        const compare& cmp = compare(),
        pp_allocator<value_type> alloc = pp_allocator<value_type>()
    )
        : compare(cmp)
        , _allocator(alloc)
        , _root(nullptr)
        , _size(0)
    {
        for (auto it = begin; it != end; ++it)
        {
            emplace(it->first, it->second);
        }
    }

    B_tree(
        std::initializer_list<std::pair<tkey, tvalue>> data,
        const compare& cmp = compare(),
        pp_allocator<value_type> alloc = pp_allocator<value_type>()
    )
        : compare(cmp)
        , _allocator(alloc)
        , _root(nullptr)
        , _size(0)
    {
        for (auto const& kv : data)
        {
            emplace(kv.first, kv.second);
        }
    }

    B_tree(const B_tree& other)
        : compare(static_cast<const compare&>(other))
        , _allocator(other._allocator)
        , _root(nullptr)
        , _size(other._size)
    {
        _root = copy_subtree(other._root);
    }

    B_tree(B_tree&& other) noexcept
        : compare(std::move(static_cast<compare&>(other)))
        , _allocator(std::move(other._allocator))
        , _root(other._root)
        , _size(other._size)
    {
        other._root = nullptr;
        other._size = 0;
    }

    B_tree& operator=(const B_tree& other)
    {
        if (this != &other)
        {
            clear();
            static_cast<compare&>(*this) = static_cast<const compare&>(other);
            _allocator = other._allocator;
            _root = copy_subtree(other._root);
            _size = other._size;
        }

        return *this;
    }

    B_tree& operator=(B_tree&& other) noexcept
    {
        if (this != &other)
        {
            clear();
            static_cast<compare&>(*this) = std::move(static_cast<compare&>(other));
            _allocator = std::move(other._allocator);
            _root = other._root;
            other._root = nullptr;
            _size = other._size;
            other._size = 0;
        }

        return *this;
    }

    ~B_tree() noexcept
    {
        clear();
    }

    class btree_iterator final
    {
        std::stack<std::pair<btree_node**, size_t>> _path;
        size_t _index;

    public:
        using value_type        = tree_data_type_const;
        using reference         = value_type&;
        using pointer           = value_type*;
        using iterator_category = std::bidirectional_iterator_tag;
        using difference_type   = ptrdiff_t;
        using self              = btree_iterator;

        friend class B_tree;
        friend class btree_const_iterator;
        friend class btree_reverse_iterator;
        friend class btree_const_reverse_iterator;

        explicit btree_iterator(
            const std::stack<std::pair<btree_node**, size_t>>& path = {},
            size_t index = 0
        )
            : _path(path)
            , _index(index)
        {
        }

        reference operator*() const noexcept
        {
            return *reinterpret_cast<value_type*>(&(*_path.top().first)->_keys[_index]);
        }

        pointer operator->() const noexcept
        {
            return reinterpret_cast<pointer>(&(*_path.top().first)->_keys[_index]);
        }

        bool operator==(const self& o) const noexcept
        {
            if (_path.empty() && o._path.empty()) return true;
            if (_path.empty() != o._path.empty()) return false;

            return _path.top().first == o._path.top().first && _index == o._index;
        }

        bool operator!=(const self& o) const noexcept
        {
            return !(*this == o);
        }

        size_t depth() const noexcept
        {
            return _path.empty() ? 0 : _path.size() - 1;
        }

        size_t index() const noexcept
        {
            return _index;
        }

        size_t current_node_keys_count() const noexcept
        {
            return _path.empty() ? 0 : (*_path.top().first)->_keys.size();
        }

        bool is_terminate_node() const noexcept
        {
            return _path.empty() ? true : is_leaf(*_path.top().first);
        }

        self& operator++()
        {
            if (_path.empty()) return *this;

            btree_node* node = *_path.top().first;

            if (!is_leaf(node))
            {
                _path.top().second = _index + 1;
                btree_node** child = &node->_pointers[_index + 1];

                while (true)
                {
                    _path.push({child, 0});

                    if (is_leaf(*child)) break;

                    child = &(*child)->_pointers[0];
                }

                _index = 0;
                return *this;
            }

            if (_index + 1 < node->_keys.size())
            {
                ++_index;
                return *this;
            }

            while (true)
            {
                _path.pop();

                if (_path.empty())
                {
                    _index = 0;
                    return *this;
                }

                size_t came_from = _path.top().second;

                if (came_from < (*_path.top().first)->_keys.size())
                {
                    _index = came_from;
                    return *this;
                }
            }
        }

        self operator++(int)
        {
            auto tmp = *this;
            ++(*this);
            return tmp;
        }

        self& operator--()
        {
            if (_path.empty()) return *this;

            btree_node* node = *_path.top().first;

            if (!is_leaf(node))
            {
                _path.top().second = _index;
                btree_node** child = &node->_pointers[_index];

                while (true)
                {
                    size_t k = (*child)->_keys.size();

                    if (is_leaf(*child))
                    {
                        _path.push({child, 0});
                        _index = k - 1;
                        return *this;
                    }

                    _path.push({child, k});
                    child = &(*child)->_pointers[k];
                }
            }

            if (_index > 0)
            {
                --_index;
                return *this;
            }

            while (true)
            {
                _path.pop();

                if (_path.empty())
                {
                    _index = 0;
                    return *this;
                }

                size_t came_from = _path.top().second;

                if (came_from > 0)
                {
                    _index = came_from - 1;
                    return *this;
                }
            }
        }

        self operator--(int)
        {
            auto tmp = *this;
            --(*this);
            return tmp;
        }
    };

    class btree_const_iterator final
    {
        std::stack<std::pair<btree_node* const*, size_t>> _path;
        size_t _index;

    public:
        using value_type        = tree_data_type_const;
        using reference         = const value_type&;
        using pointer           = const value_type*;
        using iterator_category = std::bidirectional_iterator_tag;
        using difference_type   = ptrdiff_t;
        using self              = btree_const_iterator;

        friend class B_tree;

        explicit btree_const_iterator(
            const std::stack<std::pair<btree_node* const*, size_t>>& path = {},
            size_t index = 0
        )
            : _path(path)
            , _index(index)
        {
        }

        btree_const_iterator(const btree_iterator& it) noexcept
            : _index(it._index)
        {
            std::stack<std::pair<btree_node**, size_t>> tmp = it._path;
            std::vector<std::pair<btree_node**, size_t>> buf;

            while (!tmp.empty())
            {
                buf.push_back(tmp.top());
                tmp.pop();
            }

            for (auto rit = buf.rbegin(); rit != buf.rend(); ++rit)
            {
                _path.push({rit->first, rit->second});
            }
        }

        reference operator*() const noexcept
        {
            return *reinterpret_cast<const value_type*>(&(*_path.top().first)->_keys[_index]);
        }

        pointer operator->() const noexcept
        {
            return reinterpret_cast<pointer>(&(*_path.top().first)->_keys[_index]);
        }

        bool operator==(const self& o) const noexcept
        {
            if (_path.empty() && o._path.empty()) return true;
            if (_path.empty() != o._path.empty()) return false;

            return _path.top().first == o._path.top().first && _index == o._index;
        }

        bool operator!=(const self& o) const noexcept
        {
            return !(*this == o);
        }

        size_t depth() const noexcept
        {
            return _path.empty() ? 0 : _path.size() - 1;
        }

        size_t index() const noexcept
        {
            return _index;
        }

        size_t current_node_keys_count() const noexcept
        {
            return _path.empty() ? 0 : (*_path.top().first)->_keys.size();
        }

        bool is_terminate_node() const noexcept
        {
            return _path.empty() ? true : is_leaf(*_path.top().first);
        }

        self& operator++()
        {
            if (_path.empty()) return *this;

            btree_node const* node = *_path.top().first;

            if (!is_leaf(node))
            {
                _path.top().second = _index + 1;
                btree_node* const* child = &node->_pointers[_index + 1];

                while (true)
                {
                    _path.push({child, 0});

                    if (is_leaf(*child)) break;

                    child = &(*child)->_pointers[0];
                }

                _index = 0;
                return *this;
            }

            if (_index + 1 < node->_keys.size())
            {
                ++_index;
                return *this;
            }

            while (true)
            {
                _path.pop();

                if (_path.empty())
                {
                    _index = 0;
                    return *this;
                }

                size_t came_from = _path.top().second;

                if (came_from < (*_path.top().first)->_keys.size())
                {
                    _index = came_from;
                    return *this;
                }
            }
        }

        self operator++(int)
        {
            auto tmp = *this;
            ++(*this);
            return tmp;
        }

        self& operator--()
        {
            if (_path.empty()) return *this;

            btree_node const* node = *_path.top().first;

            if (!is_leaf(node))
            {
                _path.top().second = _index;
                btree_node* const* child = &node->_pointers[_index];

                while (true)
                {
                    size_t k = (*child)->_keys.size();

                    if (is_leaf(*child))
                    {
                        _path.push({child, 0});
                        _index = k - 1;
                        return *this;
                    }

                    _path.push({child, k});
                    child = &(*child)->_pointers[k];
                }
            }

            if (_index > 0)
            {
                --_index;
                return *this;
            }

            while (true)
            {
                _path.pop();

                if (_path.empty())
                {
                    _index = 0;
                    return *this;
                }

                size_t came_from = _path.top().second;

                if (came_from > 0)
                {
                    _index = came_from - 1;
                    return *this;
                }
            }
        }

        self operator--(int)
        {
            auto tmp = *this;
            --(*this);
            return tmp;
        }
    };

    class btree_reverse_iterator final
    {
        btree_iterator _it;

    public:
        using value_type        = tree_data_type_const;
        using reference         = value_type&;
        using pointer           = value_type*;
        using iterator_category = std::bidirectional_iterator_tag;
        using difference_type   = ptrdiff_t;
        using self              = btree_reverse_iterator;

        friend class B_tree;

        explicit btree_reverse_iterator(
            const std::stack<std::pair<btree_node**, size_t>>& path = {},
            size_t index = 0
        )
            : _it(path, index)
        {
        }

        btree_reverse_iterator(const btree_iterator& it) noexcept
            : _it(it)
        {
        }

        operator btree_iterator() const noexcept
        {
            auto tmp = _it;
            --tmp;
            return tmp;
        }

        reference operator*() const noexcept
        {
            auto tmp = _it;
            --tmp;
            return *tmp;
        }

        pointer operator->() const noexcept
        {
            auto tmp = _it;
            --tmp;
            return tmp.operator->();
        }

        self& operator++()
        {
            --_it;
            return *this;
        }

        self operator++(int)
        {
            auto tmp = *this;
            --_it;
            return tmp;
        }

        self& operator--()
        {
            ++_it;
            return *this;
        }

        self operator--(int)
        {
            auto tmp = *this;
            ++_it;
            return tmp;
        }

        bool operator==(const self& o) const noexcept
        {
            return _it == o._it;
        }

        bool operator!=(const self& o) const noexcept
        {
            return _it != o._it;
        }

        size_t depth() const noexcept
        {
            return _it.depth();
        }

        size_t index() const noexcept
        {
            return _it.index();
        }

        size_t current_node_keys_count() const noexcept
        {
            return _it.current_node_keys_count();
        }

        bool is_terminate_node() const noexcept
        {
            return _it.is_terminate_node();
        }
    };

    class btree_const_reverse_iterator final
    {
        btree_const_iterator _it;

    public:
        using value_type        = tree_data_type_const;
        using reference         = const value_type&;
        using pointer           = const value_type*;
        using iterator_category = std::bidirectional_iterator_tag;
        using difference_type   = ptrdiff_t;
        using self              = btree_const_reverse_iterator;

        friend class B_tree;

        explicit btree_const_reverse_iterator(
            const std::stack<std::pair<btree_node* const*, size_t>>& path = {},
            size_t index = 0
        )
            : _it(path, index)
        {
        }

        btree_const_reverse_iterator(const btree_reverse_iterator& it) noexcept
            : _it(static_cast<btree_iterator>(it))
        {
        }

        operator btree_const_iterator() const noexcept
        {
            auto tmp = _it;
            --tmp;
            return tmp;
        }

        reference operator*() const noexcept
        {
            auto tmp = _it;
            --tmp;
            return *tmp;
        }

        pointer operator->() const noexcept
        {
            auto tmp = _it;
            --tmp;
            return tmp.operator->();
        }

        self& operator++()
        {
            --_it;
            return *this;
        }

        self operator++(int)
        {
            auto tmp = *this;
            --_it;
            return tmp;
        }

        self& operator--()
        {
            ++_it;
            return *this;
        }

        self operator--(int)
        {
            auto tmp = *this;
            ++_it;
            return tmp;
        }

        bool operator==(const self& o) const noexcept
        {
            return _it == o._it;
        }

        bool operator!=(const self& o) const noexcept
        {
            return _it != o._it;
        }

        size_t depth() const noexcept
        {
            return _it.depth();
        }

        size_t index() const noexcept
        {
            return _it.index();
        }

        size_t current_node_keys_count() const noexcept
        {
            return _it.current_node_keys_count();
        }

        bool is_terminate_node() const noexcept
        {
            return _it.is_terminate_node();
        }
    };

    friend class btree_iterator;
    friend class btree_const_iterator;
    friend class btree_reverse_iterator;
    friend class btree_const_reverse_iterator;

    btree_iterator begin()
    {
        std::stack<std::pair<btree_node**, size_t>> path;

        if (!_root) return btree_iterator{};

        btree_node** cur = &_root;

        while (*cur)
        {
            path.push({cur, 0});

            if (is_leaf(*cur)) break;

            cur = &(*cur)->_pointers[0];
        }

        return btree_iterator(path, 0);
    }

    btree_iterator end()
    {
        return btree_iterator{};
    }

    btree_const_iterator begin() const
    {
        return cbegin();
    }

    btree_const_iterator end() const
    {
        return cend();
    }

    btree_const_iterator cbegin() const
    {
        std::stack<std::pair<btree_node* const*, size_t>> path;

        if (!_root) return btree_const_iterator{};

        btree_node* const* cur = &_root;

        while (*cur)
        {
            path.push({cur, 0});

            if (is_leaf(*cur)) break;

            cur = &(*cur)->_pointers[0];
        }

        return btree_const_iterator(path, 0);
    }

    btree_const_iterator cend() const
    {
        return btree_const_iterator{};
    }

    btree_reverse_iterator rbegin()
    {
        auto e = end();
        return btree_reverse_iterator(e);
    }

    btree_reverse_iterator rend()
    {
        if (!_root) return btree_reverse_iterator{};

        auto b = begin();
        return btree_reverse_iterator(b);
    }

    btree_const_reverse_iterator rbegin() const
    {
        return crbegin();
    }

    btree_const_reverse_iterator rend() const
    {
        return crend();
    }

    btree_const_reverse_iterator crbegin() const
    {
        auto e = cend();
        return btree_const_reverse_iterator(static_cast<btree_const_iterator>(e));
    }

    btree_const_reverse_iterator crend() const
    {
        if (!_root) return btree_const_reverse_iterator{};

        auto b = cbegin();
        return btree_const_reverse_iterator(b);
    }

    size_t size() const noexcept
    {
        return _size;
    }

    bool empty() const noexcept
    {
        return _size == 0;
    }

    btree_iterator find(const tkey& key)
    {
        std::stack<std::pair<btree_node**, size_t>> path;
        size_t idx = 0;
        bool found = false;

        build_path(key, path, idx, found);

        if (!found) return end();

        return btree_iterator(path, idx);
    }

    btree_const_iterator find(const tkey& key) const
    {
        return const_cast<B_tree*>(this)->find(key);
    }

    btree_iterator lower_bound(const tkey& key)
    {
        std::stack<std::pair<btree_node**, size_t>> path;
        btree_node** cur = &_root;

        if (!_root) return end();

        while (*cur)
        {
            size_t i = lower_in_node(*cur, key);

            if (is_leaf(*cur))
            {
                if (i < (*cur)->_keys.size())
                {
                    path.push({cur, i});
                    return btree_iterator(path, i);
                }

                while (!path.empty())
                {
                    size_t came_from = path.top().second;

                    if (came_from < (*path.top().first)->_keys.size())
                    {
                        return btree_iterator(path, came_from);
                    }

                    path.pop();
                }

                return end();
            }

            path.push({cur, i});

            if (i < (*cur)->_keys.size()
                && !compare_keys(key, (*cur)->_keys[i].first)
                && !compare_keys((*cur)->_keys[i].first, key))
            {
                return btree_iterator(path, i);
            }

            cur = &(*cur)->_pointers[i];
        }

        return end();
    }

    btree_const_iterator lower_bound(const tkey& key) const
    {
        return const_cast<B_tree*>(this)->lower_bound(key);
    }

    btree_iterator upper_bound(const tkey& key)
    {
        return lower_bound(key);
    }

    btree_const_iterator upper_bound(const tkey& key) const
    {
        return const_cast<B_tree*>(this)->upper_bound(key);
    }

    bool contains(const tkey& key) const
    {
        return find(key) != cend();
    }

    tvalue& at(const tkey& key)
    {
        auto it = find(key);

        if (it == end())
        {
            throw std::out_of_range("B_tree::at: key not found");
        }

        return it->second;
    }

    const tvalue& at(const tkey& key) const
    {
        auto it = find(key);

        if (it == cend())
        {
            throw std::out_of_range("B_tree::at: key not found");
        }

        return it->second;
    }

    tvalue& operator[](const tkey& key)
    {
        auto it = find(key);

        if (it != end()) return it->second;

        auto r = emplace(key, tvalue{});
        return r.first->second;
    }

    tvalue& operator[](tkey&& key)
    {
        auto it = find(key);

        if (it != end()) return it->second;

        auto r = emplace(std::move(key), tvalue{});
        return r.first->second;
    }

    void clear() noexcept
    {
        destroy_subtree(_root);
        _root = nullptr;
        _size = 0;
    }

    template <typename... Args>
    std::pair<btree_iterator, bool> emplace(Args&&... args)
    {
        tree_data_type tmp(std::forward<Args>(args)...);
        return insert(std::move(tmp));
    }

    std::pair<btree_iterator, bool> insert(const tree_data_type& data)
    {
        tree_data_type tmp = data;
        return insert(std::move(tmp));
    }

    std::pair<btree_iterator, bool> insert(tree_data_type&& data)
    {
        tkey key_copy = data.first;
        bool inserted = insert_post(std::move(data.first), std::move(data.second));

        if (inserted) ++_size;

        return {find(key_copy), inserted};
    }

    btree_iterator insert_or_assign(const tree_data_type& data)
    {
        auto it = find(data.first);

        if (it != end())
        {
            it->second = data.second;
            return it;
        }

        return insert(data).first;
    }

    btree_iterator insert_or_assign(tree_data_type&& data)
    {
        auto it = find(data.first);

        if (it != end())
        {
            it->second = std::move(data.second);
            return it;
        }

        return insert(std::move(data)).first;
    }

    template <typename... Args>
    btree_iterator emplace_or_assign(Args&&... args)
    {
        tree_data_type tmp(std::forward<Args>(args)...);
        return insert_or_assign(std::move(tmp));
    }

    btree_iterator erase(const tkey& key)
    {
        auto it = find(key);
        if (it == end()) return end();

        auto next_it = it;
        ++next_it;

        bool has_next = (next_it != end());
        tkey next_key;

        if (has_next)
        {
            next_key = next_it->first;
        }

        if (!erase_from(_root, key)) return end();

        --_size;

        if (_root && _root->_keys.empty())
        {
            btree_node* old = _root;
            _root = is_leaf(old) ? nullptr : old->_pointers.front();
            delete_node(old);
        }

        if (has_next) return find(next_key);

        return end();
    }

    btree_iterator erase(btree_iterator pos)
    {
        if (pos == end()) return end();

        return erase(pos->first);
    }

    btree_iterator erase(btree_const_iterator pos)
    {
        if (pos == cend()) return end();

        return erase(pos->first);
    }

    btree_iterator erase(btree_iterator beg, btree_iterator en)
    {
        while (beg != en && beg != end())
        {
            auto k = beg->first;
            ++beg;
            erase(k);
        }

        return beg;
    }

    btree_iterator erase(btree_const_iterator beg, btree_const_iterator en)
    {
        while (beg != en && beg != cend())
        {
            auto k = beg->first;
            ++beg;
            erase(k);
        }

        return end();
    }

private:
    btree_iterator find_iter_for(const tkey& key)
    {
        return find(key);
    }
};

#endif // SYS_PROG_B_TREE_H