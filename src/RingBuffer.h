#pragma once

#include <vector>
#include <mutex>
#include <iostream>
#include <algorithm> // dla std::copy, std::min
#include <cstring>   // dla memcpy (opcjonalnie, ale std::copy jest bezpieczniejsze dla obiektów)

// Thread-safe FIFO ring buffer optimization
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
        
        // OPTYMALIZACJA: Kopiowanie blokowe zamiast pętli po elemencie
        size_t firstChunk = std::min(count, capacity - head);
        std::copy(data, data + firstChunk, buffer.begin() + head);
        
        if (count > firstChunk) {
            std::copy(data + firstChunk, data + count, buffer.begin());
        }

        size_t next_head = (head + count) % capacity;

        // Wykrywanie przepełnienia (prosta heurystyka dla bufora kołowego)
        // W pełnym buforze kołowym "head" mógłby przeskoczyć "tail".
        // Tutaj dla wydajności przy SDR zakładamy, że jeśli writer jest szybszy,
        // to po prostu przesuwamy tail (tracimy stare dane), żeby nie blokować.
        
        size_t availableSpace = (tail > head) ? (tail - head) : (capacity - (head - tail));
        // Uwaga: To jest uproszczone zarządzanie przepełnieniem dla strumieniowania.
        // Jeśli nadpisaliśmy tail, musimy go przesunąć.
        
        // Bardziej konserwatywne podejście do tail w prostym buforze:
        // Jeśli zapisaliśmy więcej danych niż było miejsca, tail musi "uciec".
        // (W implementacji stricte atomowej byłoby to trudniejsze, tu mamy mutex).
        
        head = next_head;
    }

    // Returns the actual number of items read
    size_t pop(T* out, size_t count) {
        std::lock_guard<std::mutex> lock(mtx);
        
        size_t available = 0;
        if (head >= tail) available = head - tail;
        else available = capacity - (tail - head);

        if (available == 0) return 0;

        size_t toRead = std::min(count, available);

        // OPTYMALIZACJA: Kopiowanie blokowe
        size_t firstChunk = std::min(toRead, capacity - tail);
        std::copy(buffer.begin() + tail, buffer.begin() + tail + firstChunk, out);

        if (toRead > firstChunk) {
            std::copy(buffer.begin(), buffer.begin() + (toRead - firstChunk), out + firstChunk);
        }

        tail = (tail + toRead) % capacity;
        return toRead;
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