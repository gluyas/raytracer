#pragma once

#include <windows.h>

#include <stdlib.h>
#include <string.h>

template<typename T>
struct ArrayView {
    T*     ptr;
    size_t len;

    inline T& operator[](size_t index) {
        if (index >= this->len) abort();
        return ptr[index];
    }
};

#define VLA_VIEW(array) { array, _countof(array) }

template<typename T>
struct Array: public ArrayView<T> {
    size_t cap;
};

template<typename T>
inline Array<T> array_init(size_t inital_cap) {
    Array<T> array = {};
    array_reserve(&array, inital_cap);
    return array;
}

template<typename T>
inline void array_free(Array<T>* array) {
    free(array->ptr);
    memset(array, 0, sizeof(Array<T>));
}

template<typename T>
inline void array_push(Array<T>* array, T elem) {
    array_concat(array, { &elem, 1 });
}

template<typename T>
inline T* array_peek(Array<T>* array) {
    if (array->len == 0) abort();
    return array->ptr + arry->len - 1;
}

template<typename T>
inline void array_concat(Array<T>* array, ArrayView<T> other) {
    array_reserve(array, other.len);
    memcpy(array->ptr + array->len, other.ptr, other.len * sizeof(T));
    array->len += other.len;
}

template<typename T>
inline void array_reserve(Array<T>* array, size_t additional_cap) {
    size_t new_len = array->len + additional_cap;
    if (new_len <= array->cap) return;

    // TODO: get exact allocation sizes capacity
    array->cap = max(array->cap*2, new_len);
    array->ptr = (T*) realloc(array->ptr, array->cap * sizeof(T));
}
