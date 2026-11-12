#pragma once

#include <vector>
#include <mutex>
#include <algorithm>

// Thread-safe FIFO ring buffer for streaming samples.
// When the writer is faster than the reader the OLDEST samples are dropped, so the
// reader always gets a continuous run of the newest data and latency stays bounded.
// The lock is held only for the block copies, which are short compared with the
// buffer length; clear() may be called from any thread.
template <typename T>
class RingBuffer {
private:
    std::vector<T> buffer;
    size_t head = 0;      // next write position
    size_t tail = 0;      // next read position
    size_t size = 0;      // number of stored items (head == tail is ambiguous without it)
    size_t capacity;
    size_t dropped = 0;   // items lost to overflow since the last clear()
    std::mutex mtx;

public:
    explicit RingBuffer(size_t cap) : buffer(cap > 0 ? cap : 1), capacity(cap > 0 ? cap : 1) {}

    void push(const T* data, size_t count) {
        if (count == 0) return;
        std::lock_guard<std::mutex> lock(mtx);

        // A push larger than the whole buffer: only the newest 'capacity' items can be kept
        if (count > capacity) {
            dropped += count - capacity;
            data += count - capacity;
            count = capacity;
        }

        // Not enough room: drop the oldest items by moving tail forward
        const size_t freeSpace = capacity - size;
        if (count > freeSpace) {
            const size_t overflow = count - freeSpace;
            tail = (tail + overflow) % capacity;
            size -= overflow;
            dropped += overflow;
        }

        const size_t firstChunk = std::min(count, capacity - head);
        std::copy(data, data + firstChunk, buffer.begin() + head);
        if (count > firstChunk) std::copy(data + firstChunk, data + count, buffer.begin());

        head = (head + count) % capacity;
        size += count;
    }

    // Returns the actual number of items read
    size_t pop(T* out, size_t count) {
        std::lock_guard<std::mutex> lock(mtx);
        const size_t toRead = std::min(count, size);
        if (toRead == 0) return 0;

        const size_t firstChunk = std::min(toRead, capacity - tail);
        std::copy(buffer.begin() + tail, buffer.begin() + tail + firstChunk, out);
        if (toRead > firstChunk)
            std::copy(buffer.begin(), buffer.begin() + (toRead - firstChunk), out + firstChunk);

        tail = (tail + toRead) % capacity;
        size -= toRead;
        return toRead;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mtx);
        head = tail = size = 0;
        dropped = 0;
    }

    size_t available() {
        std::lock_guard<std::mutex> lock(mtx);
        return size;
    }

    // Items dropped because the reader fell behind (for diagnostics)
    size_t droppedCount() {
        std::lock_guard<std::mutex> lock(mtx);
        return dropped;
    }
};
