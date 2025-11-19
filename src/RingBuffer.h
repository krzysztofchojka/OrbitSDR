#pragma once

#include <vector>
#include <mutex>
#include <iostream>

// Simple thread-safe FIFO ring buffer
template <typename T>
class RingBuffer {
private:
    std::vector<T> buffer;
    size_t head = 0;
    size_t tail = 0;
    size_t capacity;
    std::mutex mtx;
    bool overflowWarning = false;

public:
    RingBuffer(size_t size) : buffer(size), capacity(size) {}

    void push(const T* data, size_t count) {
        std::lock_guard<std::mutex> lock(mtx);
        for (size_t i = 0; i < count; ++i) {
            size_t next_head = (head + 1) % capacity;
            
            if (next_head == tail) {
                if (!overflowWarning) {
                    std::cerr << "[RingBuffer] Overflow! Dropping samples." << std::endl;
                    overflowWarning = true;
                }
                // Drop oldest data (advance tail) to make space
                tail = (tail + 1) % capacity;
            } else {
                overflowWarning = false;
            }
            
            buffer[head] = data[i];
            head = next_head;
        }
    }

    // Returns the actual number of items read
    size_t pop(T* out, size_t count) {
        std::lock_guard<std::mutex> lock(mtx);
        size_t readCount = 0;
        
        while (readCount < count && tail != head) {
            out[readCount++] = buffer[tail];
            tail = (tail + 1) % capacity;
        }
        return readCount;
    }
    
    void clear() {
        std::lock_guard<std::mutex> lock(mtx);
        head = 0;
        tail = 0;
    }
    
    size_t available() {
        std::lock_guard<std::mutex> lock(mtx);
        if (head >= tail) return head - tail;
        return capacity - (tail - head);
    }
};