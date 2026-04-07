#ifndef MEMORY_POOL_H
#define MEMORY_POOL_H

#include <stdlib.h>
#include <string.h>
#include <new>
#include <vector>

#include "../lock/locker.h"

class SmallBlockMemoryPool
{
public:
    static SmallBlockMemoryPool *get_instance()
    {
        static SmallBlockMemoryPool instance(256, 256);
        return &instance;
    }

    void *acquire()
    {
        m_lock.lock();
        if (m_free_list == NULL)
        {
            expand_pool();
        }

        FreeNode *node = m_free_list;
        m_free_list = m_free_list->next;
        m_lock.unlock();

        memset(node, 0, m_block_size);
        return node;
    }

    void release(void *ptr)
    {
        if (ptr == NULL)
        {
            return;
        }

        m_lock.lock();
        FreeNode *node = (FreeNode *)ptr;
        node->next = m_free_list;
        m_free_list = node;
        m_lock.unlock();
    }

private:
    struct FreeNode
    {
        FreeNode *next;
    };

    SmallBlockMemoryPool(size_t block_size, size_t blocks_per_chunk)
        : m_block_size(block_size < sizeof(FreeNode) ? sizeof(FreeNode) : block_size),
          m_blocks_per_chunk(blocks_per_chunk),
          m_free_list(NULL)
    {
    }

    ~SmallBlockMemoryPool()
    {
        for (size_t i = 0; i < m_chunks.size(); ++i)
        {
            free(m_chunks[i]);
        }
    }

    void expand_pool()
    {
        char *chunk = (char *)malloc(m_block_size * m_blocks_per_chunk);
        if (chunk == NULL)
        {
            throw std::bad_alloc();
        }

        m_chunks.push_back(chunk);
        for (size_t i = 0; i < m_blocks_per_chunk; ++i)
        {
            FreeNode *node = (FreeNode *)(chunk + i * m_block_size);
            node->next = m_free_list;
            m_free_list = node;
        }
    }

private:
    size_t m_block_size;
    size_t m_blocks_per_chunk;
    std::vector<char *> m_chunks;
    FreeNode *m_free_list;
    locker m_lock;
};

class MemoryPoolBuffer
{
public:
    MemoryPoolBuffer() : m_ptr(SmallBlockMemoryPool::get_instance()->acquire()) {}

    ~MemoryPoolBuffer()
    {
        SmallBlockMemoryPool::get_instance()->release(m_ptr);
    }

    char *get()
    {
        return (char *)m_ptr;
    }

private:
    void *m_ptr;
};

#endif
