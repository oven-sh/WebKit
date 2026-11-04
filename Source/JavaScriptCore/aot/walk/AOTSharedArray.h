/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <type_traits>
#include <utility>
#include <wtf/FastMalloc.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

// An array of plain records that is copied without looking at them: the copy and the original share every part that neither has written since.
// The records lie in leaves under a tree of branches. Whoever writes a record in a part that somebody else has too gets parts of his own on the way down to it, and no more.
// Up to inlineCapacity records lie in the array itself, without a tree, and are copied one by one.
// All the arrays that share parts belong to one thread.
template<typename T, size_t inlineCapacity, unsigned bitsOfLeaf = 5>
class SharedArray {
    static_assert(std::is_arithmetic_v<T> || (std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T> && std::is_aggregate_v<T>));
public:
    SharedArray() = default;

    SharedArray(const SharedArray& other) { takeCopyOf(other); }
    SharedArray(SharedArray&& other) { swap(other); }

    SharedArray& operator=(const SharedArray& other)
    {
        if (this == &other)
            return *this;
        // In this order: the two may share the root.
        Node* formerRoot = m_root;
        unsigned formerHeight = m_height;
        takeCopyOf(other);
        release(formerRoot, formerHeight);
        return *this;
    }

    // `other` is left with what this array held.
    SharedArray& operator=(SharedArray&& other)
    {
        swap(other);
        return *this;
    }

    ~SharedArray() { release(m_root, m_height); }

    void swap(SharedArray& other)
    {
        uint32_t numberInline = std::max(m_root ? 0 : m_size, other.m_root ? 0 : other.m_size);
        std::swap_ranges(inlineRecords(), inlineRecords() + numberInline, other.inlineRecords());
        std::swap(m_root, other.m_root);
        std::swap(m_size, other.m_size);
        std::swap(m_height, other.m_height);
    }

    uint32_t size() const { return m_size; }
    bool isEmpty() const { return !m_size; }

    const T& operator[](uint32_t index) const
    {
        ASSERT(index < m_size);
        if (!m_root)
            return inlineRecords()[index];
        const Node* node = m_root;
        for (unsigned level = m_height; level; --level)
            node = static_cast<const Branch*>(node)->children[childAt(index >> bitsOfLeaf, level)];
        return static_cast<const Leaf*>(node)->records()[index & (widthOfLeaf - 1)];
    }

    // It holds until the array is appended to, copied or assigned.
    T& mutableAt(uint32_t index)
    {
        ASSERT(index < m_size);
        if (!m_root)
            return inlineRecords()[index];
        return leafToWrite(index).records()[index & (widthOfLeaf - 1)];
    }

    void append(const T& record)
    {
        if (!m_root) {
            if (m_size < inlineCapacity) {
                inlineRecords()[m_size++] = record;
                return;
            }
            Leaf* leaf = make<Leaf>();
            std::copy_n(inlineRecords(), m_size, leaf->records());
            m_root = leaf;
        }
        if (m_size == capacity()) {
            Branch* root = make<Branch>();
            root->children[0] = m_root;
            m_root = root;
            ++m_height;
        }
        uint32_t index = m_size++;
        leafToWrite(index).records()[index & (widthOfLeaf - 1)] = record;
    }

    void clear()
    {
        release(std::exchange(m_root, nullptr), std::exchange(m_height, 0));
        m_size = 0;
    }

    // The records from there on are dropped. Their parts stay where they are until others are laid there.
    void shrink(uint32_t size)
    {
        ASSERT(size <= m_size);
        if (!size)
            return clear();
        m_size = size;
    }

    // As many records, all alike. They share one leaf until they are written.
    void fill(const T& record, uint32_t size)
    {
        clear();
        Leaf* leaf = make<Leaf>();
        std::fill_n(leaf->records(), widthOfLeaf, record);
        m_root = leaf;
        m_size = size;
        while (capacity() < size) {
            Branch* root = make<Branch>();
            std::fill_n(root->children, widthOfBranch, m_root);
            m_root->numberOfOwners = widthOfBranch;
            m_root = root;
            ++m_height;
        }
    }

    // Records, all alike, are appended until there are `size`. Whole parts of them are one part until they are written, so this costs what the tree is high and not what is appended.
    // Whole parts of zeros are the same in every array of the thread: two copies of one array that have each grown by zeros share them, and forEachStretchNotSharedWith() passes them over.
    void grow(const T& record, uint32_t size)
    {
        while (m_size < size && (!m_root || m_size & (widthOfLeaf - 1)))
            append(record);

        bool areZeros = isZero(record);
        // By level: a part that holds nothing else.
        Node* partsAlike[maxHeight] { };
        unsigned numberOfPartsAlike = 0;
        while (m_size < size) {
            if (m_size == capacity()) {
                Branch* root = make<Branch>();
                root->children[0] = m_root;
                m_root = root;
                ++m_height;
            }
            // The largest part under the root that begins here. It may reach past `size`, as what fill() makes does.
            uint32_t indexOfLeaf = m_size >> bitsOfLeaf;
            unsigned level = 0;
            while (level + 1 < m_height && !childAt(indexOfLeaf, level + 1))
                ++level;
            for (; !areZeros && numberOfPartsAlike <= level; ++numberOfPartsAlike) {
                if (!numberOfPartsAlike) {
                    Leaf* leaf = make<Leaf>();
                    std::fill_n(leaf->records(), widthOfLeaf, record);
                    partsAlike[0] = leaf;
                    continue;
                }
                Branch* branch = make<Branch>();
                std::fill_n(branch->children, widthOfBranch, partsAlike[numberOfPartsAlike - 1]);
                partsAlike[numberOfPartsAlike - 1]->numberOfOwners += widthOfBranch;
                partsAlike[numberOfPartsAlike] = branch;
            }

            Node** slot = &m_root;
            for (unsigned above = m_height; above > level; --above) {
                makeOwn(*slot, above);
                slot = &static_cast<Branch*>(*slot)->children[childAt(indexOfLeaf, above)];
            }
            release(*slot, level);
            *slot = areZeros ? partOfZeros(level) : partsAlike[level];
            ++(*slot)->numberOfOwners;
            m_size = static_cast<uint32_t>(std::min<uint64_t>(size, m_size + (static_cast<uint64_t>(widthOfLeaf) << (bitsOfBranch * level))));
        }
        while (numberOfPartsAlike--)
            release(partsAlike[numberOfPartsAlike], numberOfPartsAlike);
    }

    // How many records are left without the zeros at the end. A whole part of zeros that grow() has laid is passed at once.
    uint32_t sizeWithoutZerosAtEnd() const
    {
        uint32_t size = m_size;
        while (size) {
            uint32_t last = size - 1;
            const T* records = inlineRecords();
            uint32_t firstOfPart = 0;
            if (m_root) {
                const Node* node = m_root;
                unsigned level = m_height;
                for (; level && node != s_partsOfZeros[level]; --level)
                    node = static_cast<const Branch*>(node)->children[childAt(last >> bitsOfLeaf, level)];
                firstOfPart = static_cast<uint32_t>(last & ~((static_cast<uint64_t>(widthOfLeaf) << (bitsOfBranch * level)) - 1));
                if (node == s_partsOfZeros[level]) {
                    size = firstOfPart;
                    continue;
                }
                records = static_cast<const Leaf*>(node)->records();
            }
            while (size > firstOfPart && isZero(records[size - 1 - firstOfPart]))
                --size;
            if (size > firstOfPart)
                break;
        }
        return size;
    }

    // For each stretch [begin, end) of positions in a leaf that `other` does not have at those positions. What is passed over holds the same in both.
    // The functor writes neither array.
    void forEachStretchNotSharedWith(const SharedArray& other, NOESCAPE const Invocable<void(uint32_t begin, uint32_t end)> auto& functor) const
    {
        if (!m_size)
            return;
        if (!m_root || !other.m_root)
            functor(0, m_size);
        else
            visitPartsNotShared(m_root, m_height, 0, other, functor);
    }

private:
    static constexpr unsigned bitsOfBranch = 5;
    static constexpr uint32_t widthOfBranch = 1u << bitsOfBranch;
    static constexpr uint32_t widthOfLeaf = 1u << bitsOfLeaf;
    // No array of 2^32 records is higher.
    static constexpr unsigned maxHeight = (32 - bitsOfLeaf + bitsOfBranch - 1) / bitsOfBranch + 1;
    static_assert(inlineCapacity <= widthOfLeaf);

    struct Node {
        uint32_t numberOfOwners { 1 };
    };
    struct Leaf : Node {
        T* records() { return reinterpret_cast<T*>(storage); }
        const T* records() const { return reinterpret_cast<const T*>(storage); }

        alignas(T) std::byte storage[sizeof(T) * widthOfLeaf];
    };
    // A child is null where no record lies under it yet.
    struct Branch : Node {
        Node* children[widthOfBranch] { };
    };

    template<typename NodeType>
    static NodeType* make()
    {
        return new (NotNull, fastMalloc(sizeof(NodeType))) NodeType;
    }

    static bool isZero(const T& record)
    {
        static constexpr std::byte zeros[sizeof(T)] { };
        return !std::memcmp(&record, zeros, sizeof(T));
    }

    // It is never given back: this list is one of its owners, so nobody writes it.
    static Node* partOfZeros(unsigned level)
    {
        if (Node* part = s_partsOfZeros[level])
            return part;
        if (!level) {
            Leaf* leaf = make<Leaf>();
            std::memset(leaf->storage, 0, sizeof(leaf->storage));
            return s_partsOfZeros[0] = leaf;
        }
        Node* below = partOfZeros(level - 1);
        Branch* branch = make<Branch>();
        std::fill_n(branch->children, widthOfBranch, below);
        below->numberOfOwners += widthOfBranch;
        return s_partsOfZeros[level] = branch;
    }

    T* inlineRecords() { return reinterpret_cast<T*>(m_inline); }
    const T* inlineRecords() const { return reinterpret_cast<const T*>(m_inline); }
    static uint32_t childAt(uint32_t indexOfLeaf, unsigned level) { return indexOfLeaf >> (bitsOfBranch * (level - 1)) & (widthOfBranch - 1); }
    uint64_t capacity() const { return static_cast<uint64_t>(widthOfLeaf) << (bitsOfBranch * m_height); }

    void takeCopyOf(const SharedArray& other)
    {
        m_root = other.m_root;
        m_size = other.m_size;
        m_height = other.m_height;
        if (m_root)
            ++m_root->numberOfOwners;
        else
            std::copy_n(other.inlineRecords(), m_size, inlineRecords());
    }

    static void release(Node* node, unsigned level)
    {
        if (!node || --node->numberOfOwners)
            return;
        if (level) {
            for (Node* child : static_cast<Branch*>(node)->children)
                release(child, level - 1);
        }
        fastFree(node);
    }

    // Behind it the node in the slot is there, and nobody else has it.
    static void makeOwn(Node*& slot, unsigned level)
    {
        if (!slot) {
            slot = level ? static_cast<Node*>(make<Branch>()) : make<Leaf>();
            return;
        }
        if (slot->numberOfOwners == 1)
            return;
        --slot->numberOfOwners;
        if (!level) {
            Leaf* copy = make<Leaf>();
            std::copy_n(static_cast<const Leaf*>(slot)->records(), widthOfLeaf, copy->records());
            slot = copy;
            return;
        }
        Branch* copy = make<Branch>();
        for (uint32_t index = 0; index < widthOfBranch; ++index) {
            Node* child = static_cast<const Branch*>(slot)->children[index];
            copy->children[index] = child;
            if (child)
                ++child->numberOfOwners;
        }
        slot = copy;
    }

    Leaf& leafToWrite(uint32_t index)
    {
        Node** slot = &m_root;
        for (unsigned level = m_height; level; --level) {
            makeOwn(*slot, level);
            slot = &static_cast<Branch*>(*slot)->children[childAt(index >> bitsOfLeaf, level)];
        }
        makeOwn(*slot, 0);
        return *static_cast<Leaf*>(*slot);
    }

    // The node at that level over the leaf. Null: there is none.
    const Node* nodeOver(uint32_t indexOfLeaf, unsigned level) const
    {
        if (level > m_height || static_cast<uint64_t>(indexOfLeaf) << bitsOfLeaf >= capacity())
            return nullptr;
        const Node* node = m_root;
        for (unsigned above = m_height; node && above > level; --above)
            node = static_cast<const Branch*>(node)->children[childAt(indexOfLeaf, above)];
        return node;
    }

    void visitPartsNotShared(const Node* node, unsigned level, uint32_t firstLeaf, const SharedArray& other, const auto& functor) const
    {
        if (node == other.nodeOver(firstLeaf, level))
            return;
        if (!level) {
            functor(firstLeaf << bitsOfLeaf, static_cast<uint32_t>(std::min<uint64_t>(m_size, static_cast<uint64_t>(firstLeaf + 1) << bitsOfLeaf)));
            return;
        }
        for (uint32_t index = 0; index < widthOfBranch; ++index) {
            uint32_t firstLeafOfChild = firstLeaf + (index << (bitsOfBranch * (level - 1)));
            const Node* child = static_cast<const Branch*>(node)->children[index];
            if (!child || static_cast<uint64_t>(firstLeafOfChild) << bitsOfLeaf >= m_size)
                return;
            visitPartsNotShared(child, level - 1, firstLeafOfChild, other, functor);
        }
    }

    // By level. Null: none was asked for yet.
    static inline thread_local Node* s_partsOfZeros[maxHeight] { };

    Node* m_root { nullptr };
    uint32_t m_size { 0 };
    // Branches between the root and a leaf.
    unsigned m_height { 0 };
    alignas(T) std::byte m_inline[sizeof(T) * std::max<size_t>(inlineCapacity, 1)];
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
