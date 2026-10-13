#pragma once

#include <atomic>
#include <cstddef>
#include <new>

#include "race_annotations.h"

// Bounded lock-free queue for exactly one producer thread and one consumer thread.
//
// Slots are preallocated and filled in place, so large elements (network packets)
// are written once by the producer and read once by the consumer, with no copy
// through the queue:
//
//	T *slot = ring.BeginPush();      // producer, nullptr when full
//	... fill *slot ...
//	ring.CommitPush();
//
//	T *slot = ring.Front();          // consumer, nullptr when empty
//	... read *slot ...
//	ring.Pop();
//
// The consumer can also read several queued elements before popping them all
// (Peek(i) then Pop(n)), to hand them to one system call.
template <typename T>
class SpscRing
{
public:
	SpscRing() : m_Slots(nullptr), m_Mask(0), m_Head(0), m_Tail(0), m_CachedHead(0), m_CachedTail(0)
	{
		// The indices are atomics: what they publish is annotated in the push and pop functions
		RACE_BENIGN(&m_Head, sizeof(m_Head), "atomic index");
		RACE_BENIGN(&m_Tail, sizeof(m_Tail), "atomic index");
	}

	~SpscRing()
	{
		Free();
	}

	// Capacity is rounded up to a power of two. Not thread-safe: call before the
	// producer and the consumer start, or after both have stopped.
	bool Allocate(size_t capacity)
	{
		Free();

		size_t size = 2;
		while (size < capacity)
			size <<= 1;

		m_Slots = new (std::nothrow) T[size];
		if (!m_Slots)
			return false;

		m_Mask = size - 1;
		Clear();
		return true;
	}

	void Free()
	{
		delete[] m_Slots;
		m_Slots = nullptr;
		m_Mask = 0;
		Clear();
	}

	// Not thread-safe, same rules as Allocate
	void Clear()
	{
		m_Head.store(0, std::memory_order_relaxed);
		m_Tail.store(0, std::memory_order_relaxed);
		m_CachedHead = 0;
		m_CachedTail = 0;
	}

	size_t Capacity() const { return m_Slots ? m_Mask + 1 : 0; }

	// Producer side

	T *BeginPush()
	{
		size_t tail = m_Tail.load(std::memory_order_relaxed);
		if (tail - m_CachedHead > m_Mask)
		{
			m_CachedHead = m_Head.load(std::memory_order_acquire);
			if (tail - m_CachedHead > m_Mask)
				return nullptr;

			// The consumer is done with the slots it popped
			RACE_HAPPENS_AFTER(&m_Head);
		}

		return &m_Slots[tail & m_Mask];
	}

	void CommitPush()
	{
		RACE_HAPPENS_BEFORE(&m_Tail);
		m_Tail.store(m_Tail.load(std::memory_order_relaxed) + 1, std::memory_order_release);
	}

	// Consumer side

	T *Front()
	{
		size_t head = m_Head.load(std::memory_order_relaxed);
		if (head == m_CachedTail)
		{
			m_CachedTail = m_Tail.load(std::memory_order_acquire);
			if (head == m_CachedTail)
				return nullptr;

			// The producer has filled the slots it pushed
			RACE_HAPPENS_AFTER(&m_Tail);
		}

		return &m_Slots[head & m_Mask];
	}

	void Pop()
	{
		Pop(1);
	}

	// Element i in queue order (0 is Front()), nullptr when fewer are queued
	T *Peek(size_t i)
	{
		size_t head = m_Head.load(std::memory_order_relaxed);
		if (m_CachedTail - head <= i)
		{
			m_CachedTail = m_Tail.load(std::memory_order_acquire);
			if (m_CachedTail - head <= i)
				return nullptr;

			RACE_HAPPENS_AFTER(&m_Tail);
		}

		return &m_Slots[(head + i) & m_Mask];
	}

	// Pops count elements, which the consumer has seen through Front or Peek
	void Pop(size_t count)
	{
		RACE_HAPPENS_BEFORE(&m_Head);
		m_Head.store(m_Head.load(std::memory_order_relaxed) + count, std::memory_order_release);
	}

	// Approximate number of queued elements, callable from either side
	size_t Size() const
	{
		// Head first: the tail read afterwards is never behind it
		size_t head = m_Head.load(std::memory_order_acquire);
		return m_Tail.load(std::memory_order_acquire) - head;
	}

private:
	SpscRing(const SpscRing &) = delete;
	SpscRing &operator=(const SpscRing &) = delete;

	T *m_Slots;
	size_t m_Mask;

	// Head is written by the consumer, tail by the producer: keep them, and each
	// side's cached copy of the other index, on separate cache lines.
	alignas(64) std::atomic<size_t> m_Head;
	alignas(64) std::atomic<size_t> m_Tail;
	alignas(64) size_t m_CachedHead;	// producer's view of m_Head
	alignas(64) size_t m_CachedTail;	// consumer's view of m_Tail
};
