#pragma once
#include "prelude.h"

inline size_t array_index(size_t len, ptrdiff_t index) {
    if (index < 0) {
        index = len + index;
        if (index < 0) abort();
    } else {
        if (index >= len) abort();
    }
    return index;
}

template<typename T>
struct ArrayView {
    T*     ptr;
    size_t len;

    inline T& operator[](ptrdiff_t index) {
        return this->ptr[array_index(this->len, index)];
    }

    inline operator ArrayView<void>() {
        return array_from((void*) this->ptr, array_len_in_bytes(this));
    }

    inline T* offset(size_t index) {
        return this->ptr + index;
    }

    inline T* get(ptrdiff_t index) {
        return this->offset(array_index(this->len, index));
    }

    inline T* begin() { return this->ptr; }
    inline T* end()   { return this->offset(this->len); }
};

template<>
struct ArrayView<void> {
    void*  ptr;
    size_t len;

    inline void* offset(size_t index) {
        return (void*) ((char*) this->ptr + index);
    }

    inline void* get(ptrdiff_t index) {
        return this->offset(array_index(this->len, index));
    }

    template<typename T>
    inline explicit operator ArrayView<T>() {
        if (this->len % sizeof(T) != 0 || (size_t) this->ptr & alignof(T) != 0) abort();
        return array_from((T*) this->ptr, this->len / sizeof(T));
    }

    inline void* begin() { return this->ptr; }
    inline void* end()   { return this->offset(this->len); }
};

template<typename T>
struct Array : public ArrayView<T> {
    size_t cap;
};

// helper macros

#define VLA_VIEW(array) (array_from(array, _countof(array)))

#define ARRAY_PTR_LEN(view)               ((view)->ptr),  ((view)->len)
#define ARRAY_VOID_PTR_LEN(view)  ((void*) (view)->ptr),  (array_len_in_bytes(view))
#define ARRAY_PTR_CAP(array)              ((array)->ptr), ((array)->cap)
#define ARRAY_VOID_PTR_CAP(array) ((void*) (array)->ptr), (array_cap_in_bytes(array))

#define ARRAY_LEN_PTR(view)       ((view)->len),               ((view)->ptr)
#define ARRAY_LEN_VOID_PTR(view)  (array_len_in_bytes(view)),  ((void*) (view)->ptr)
#define ARRAY_CAP_PTR(array)      ((array)->cap),              ((array)->ptr)
#define ARRAY_CAP_VOID_PTR(array) (array_cap_in_bytes(array)), ((void*) (array)->ptr)

// array constructors

template<typename T>
inline ArrayView<T> array_of(T* ptr) {
    return ArrayView<T> { ptr+0, 1 };
}

template<typename T>
inline ArrayView<T> array_from(T* ptr, size_t len) {
    return ArrayView<T> { ptr, len };
}

template<typename T>
inline Array<T> array_init(size_t inital_cap) {
    Array<T> array = {};
    array_reserve(&array, inital_cap);
    return array;
}

// array slicing

template<typename T>
inline ArrayView<T> array_slice(ArrayView<T>* view, ptrdiff_t from, ptrdiff_t to) {
    from = array_index(view, from);
    to   = array_index(view, to);

    if (from > to) abort();
    return ArrayView<T> { view->offset(from), to - from };
}

template<typename T>
inline ArrayView<T> array_slice_from(ArrayView<T>* view, ptrdiff_t from) {
    from = array_index(view, from);
    return ArrayView<T> { view->offset(from), view->len - from };
}

template<typename T>
inline ArrayView<T> array_slice_to(ArrayView<T>* view, ptrdiff_t to) {
    to   = array_index(view, to);
    return ArrayView<T> { view->ptr, to };
}

// array copying

template<typename T>
inline void array_copy(ArrayView<T>* dst, ArrayView<T>* src) {
    if (dst->len != src->len) abort();
    array_copy(dst, src->ptr);
}
template<typename T>
inline void array_copy(ArrayView<T>* dst, T* src) {
    memcpy((void*) dst->ptr, (void*) src, array_len_in_bytes(dst));
}
template<typename T>
inline void array_copy(T* dst, ArrayView<T>* src) {
    memcpy((void*) dst, (void*) src->ptr, array_len_in_bytes(src));
}

template<typename T>
inline void array_copy_nonoverlapping(ArrayView<T>* dst, ArrayView<T>* src) {
    if (dst->len != src->len) abort();
    array_copy_nonoverlapping(dst, src->ptr);
}
template<typename T>
inline void array_copy_nonoverlapping(ArrayView<T>* dst, T* src) {
    memmove((void*) dst->ptr, (void*) src, array_len_in_bytes(dst));
}
template<typename T>
inline void array_copy_nonoverlapping(T* dst, ArrayView<T>* src) {
    memmove((void*) dst, (void*) src->ptr, array_len_in_bytes(src));
}

template<typename T>
inline size_t array_len_in_bytes(ArrayView<T>* view) {
    return view->len * sizeof(*(view->ptr));
}
template<>
inline size_t array_len_in_bytes(ArrayView<void>* view) {
    return view->len;
}

template<typename T>
inline size_t array_cap_in_bytes(Array<T>* array) {
    return array->cap * sizeof(*(array->ptr));
}
template<>
inline size_t array_cap_in_bytes(Array<void>* array) {
    return array->cap;
}

// dynamic arrays

template<typename T>
inline void array_free(Array<T>* array) {
    free(array->ptr);
    memset(array, 0, sizeof(*array));
}

template<typename T>
inline T* array_push(Array<T>* array, T elem = {}) {
    T* ptr = array_push_uninitialized(array);
    *ptr = elem;
    return ptr;
}

template<typename T>
inline Array<T> array_clone(ArrayView<T>* view) {
    Array<T> clone = array_init(view->cap);
    memmove((void*) clone->ptr, ARRAY_VOID_PTR_LEN(view));
    clone->len = view->len;
    return clone;
}

template<typename T>
inline ArrayView<T> array_push_uninitialized(Array<T>* array, size_t count) {
    array_reserve(array, count);
    ArrayView<T> push = ArrayView<T> { array->offset(array->len), count };
    array->len += count;
    return push;
}

template<typename T>
inline T* array_push_uninitialized(Array<T>* array) {
    return array_push_uninitialized(array, 1).ptr;
}

template<typename T>
inline void array_concat(Array<T>* array, ArrayView<T>* other) {
    memmove((void*) array_push_uninitialized(array, other->len).ptr, other->ptr, array_len_in_bytes(other));
}

template<typename T>
inline void array_reserve(Array<T>* array, size_t additional_cap) {
    size_t new_len = array->len + additional_cap;
    if (new_len <= array->cap) return;

    // TODO: get exact allocation sizes capacity
    array->cap = max(array->cap*2, new_len);
    array->ptr = (T*) realloc((void*) array->ptr, array_cap_in_bytes(array));
}
