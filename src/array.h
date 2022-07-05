#pragma once
#include "prelude.h"

inline size_t array_index_helper(size_t len, ptrdiff_t index) {
    if (index < 0) {
        index = len + index;
        if (index < 0) abort();
    } else {
        if (index >= len) abort();
    }
    return index;
}
inline size_t array_index_helper_inclusive(size_t len, ptrdiff_t index_to) {
    if (index_to < 0) {
        index_to = len + index_to;
        if (index_to < 0) abort();
    } else {
        if (index_to > len) abort();
    }
    return index_to;
}

template<typename T>
struct ArrayView {
    T*     ptr;
    size_t len;

    inline T& operator[](ptrdiff_t index) {
        return this->ptr[array_index_helper(this->len, index)];
    }

    inline bool operator !() {
        return !(this->ptr && this->len);
    }

    inline operator ArrayView<void>() {
        return array_from((void*) this->ptr, array_len_in_bytes(this));
    }

    inline T* offset(size_t index) {
        return this->ptr + index;
    }

    inline T* get(ptrdiff_t index) {
        return this->offset(array_index_helper(this->len, index));
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
        return this->offset(array_index_helper(this->len, index));
    }

    inline bool operator !() {
        return !(this->ptr && this->len);
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
    from = array_index_helper(view->len, from);
    to   = array_index_helper_inclusive(view->len, to);

    if (from > to) abort();
    return ArrayView<T> { view->offset(from), (size_t) (to - from) };
}

template<typename T>
inline ArrayView<T> array_slice_from(ArrayView<T>* view, ptrdiff_t from) {
    from = array_index_helper(view->len, from);
    return ArrayView<T> { view->offset(from), view->len - from };
}

template<typename T>
inline ArrayView<T> array_slice_to(ArrayView<T>* view, ptrdiff_t to) {
    to   = array_index_helper_inclusive(view->len, to);
    return ArrayView<T> { view->ptr, to };
}

// array copying

template<typename T>
inline void array_copy(ArrayView<T>* dst, const ArrayView<T>* src) {
    if (dst->len != src->len) abort();
    array_copy(dst, src->ptr);
}
template<typename T>
inline void array_copy(ArrayView<T>* dst, const T* src) {
    memmove((void*) dst->ptr, (void*) src, array_len_in_bytes(dst));
}
template<typename T>
inline void array_copy(T* dst, const ArrayView<T>* src) {
    memmove((void*) dst, (void*) src->ptr, array_len_in_bytes(src));
}

template<typename T>
inline void array_copy_nonoverlapping(ArrayView<T>* dst, const ArrayView<T>* src) {
    if (dst->len != src->len) abort();
    array_copy_nonoverlapping(dst, src->ptr);
}
template<typename T>
inline void array_copy_nonoverlapping(ArrayView<T>* dst, const T* src) {
    memcpy((void*) dst->ptr, (void*) src, array_len_in_bytes(dst));
}
template<typename T>
inline void array_copy_nonoverlapping(T* dst, const ArrayView<T>* src) {
    memcpy((void*) dst, (void*) src->ptr, array_len_in_bytes(src));
}

template<typename T>
inline size_t array_len_in_bytes(const ArrayView<T>* view) {
    return view->len * sizeof(*(view->ptr));
}
template<>
inline size_t array_len_in_bytes(const ArrayView<void>* view) {
    return view->len;
}

template<typename T>
inline size_t array_cap_in_bytes(const Array<T>* array) {
    return array->cap * sizeof(*(array->ptr));
}
template<>
inline size_t array_cap_in_bytes(const Array<void>* array) {
    return array->cap;
}

// dynamic arrays

template<typename T>
inline void array_free(Array<T>* array) {
    free(array->ptr);
    memset(array, 0, sizeof(*array));
}

template<typename T>
inline void array_push(Array<T>* array, T elem) {
    T* ptr = array_push_uninitialized(array);
    *ptr = elem;
}

template<typename T>
inline T* array_push_default(Array<T>* array) {
    T* ptr = array_push_uninitialized(array);
    *ptr = {};
    return ptr;
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
inline void array_insert(Array<T>* array, ptrdiff_t index, T elem) {
    *array_insert_uninitialized(array, index) = elem;
}

template<typename T>
inline T* array_insert_uninitialized(Array<T>* array, ptrdiff_t index) {
    index = array_index_helper_inclusive(array->len, index);
    array_reserve(array, 1);
    if (index < array->len) {
        memmove(array->ptr + index, array->ptr + index+1, (array->len - index)*sizeof(T));
    }
    array->len += 1;
    return array->ptr + index;
}

template<typename T>
inline ptrdiff_t array_index_of(Array<T>* array, T item) {
    for (int i = 0; i < array->len; i++) {
        if (memcmp(&array[i], &item, sizeof(T) == 0)) return i;
    }
    return PTRDIFF_MIN;
}

template<typename T>
inline bool array_contains(Array<T>* array, T item) {
    return array_index_of(array, item) >= 0;
}

template<typename T>
inline void array_remove_range(Array<T>* array, ptrdiff_t from, ptrdiff_t to) {
    from = array_index_helper(from);
    to   = array_index_helper_inclusive(to);
    memmove(array->ptr + from, array->ptr + to, (array->len - to)*sizeof(T));
    array->len -= to - from;
}

template<typename T>
inline T array_remove_at(Array<T>* array, ptrdiff_t index) {
    index = array_index_helper(index);
    T* ptr = array->ptr + index;
    T removed = *ptr;
    memmove(ptr, ptr + 1, , (array->len - index)*sizeof(T));
    array->len -= 1;
    return removed;
}

template<typename T>
inline bool array_remove(Array<T>* array, T item) {
    ptrdiff_t index = array_index_of(array, item);
    if (index >= 0) {
        array_remove_at(index);
        return true;
    } else {
        return false;
    }
}

template<typename T>
inline Array<T> array_clone(ArrayView<T>* view) {
    Array<T> clone = array_init(view->cap);
    memcpy((void*) clone->ptr, ARRAY_VOID_PTR_LEN(view));
    clone->len = view->len;
    return clone;
}

template<typename T>
inline void array_concat(Array<T>* array, const ArrayView<T>* other) {
    memcpy((void*) array_push_uninitialized(array, other->len).ptr, (void*)other->ptr, array_len_in_bytes(other));
}

template<typename T>
inline void array_reserve(Array<T>* array, size_t additional_len) {
    size_t new_len = array->len + additional_len;
    if (new_len <= array->cap) return;

    // TODO: use lzcnt
    array->cap = max(array->cap, 1);
    do { array->cap *= 2; } while (array->cap < new_len);

    // TODO: query true allocation size
    array->ptr = (T*) realloc((void*) array->ptr, array_cap_in_bytes(array));
}

// sorted arrays

template<typename T>
inline bool array_insert_sorted(Array<T>* array, T elem) {
    ptrdiff_t index = array_index_of_sorted(array, elem);
    if (index > 0) {
        return false;
    } else {
        array_insert(array, index, elem);
        return true;
    }
}

template<typename T>
inline bool array_remove_sorted(Array<T>* array, T elem) {
    ptrdiff_t index = array_index_of_sorted(array, elem);
    if (index < 0) {
        return false;
    } else {
        array_remove_at(array, index);
        return true;
    }
}

template<typename T>
inline bool array_contains_sorted(Array<T>* array, T elem) {
    return array_contains_key((ArrayView<Pair<T, void>>*) array, elem);
}

template<typename T>
inline ptrdiff_t array_get_insertion_index_sorted(ArrayView<T>* array, T elem) {
    return array_get_insertion_index_by_key((ArrayView<Pair<T, void>>*) array, elem);
}

template<typename T>
inline ptrdiff_t array_index_of_sorted(ArrayView<T>* array, T elem) {
    return array_index_of_key((ArrayView<Pair<T, void>>*) array, elem);
}

// associative arrays

template<typename K, typename V>
inline ptrdiff_t array_insert_by_key(Array<Pair<K, V>>* array, Pair<K, V> entry) {
    ptrdiff_t index = array_get_insertion_index_by_key(array, entry._0);
    if (index < 0) {
        (*array)[index] = entry;
        return index;
    } else {
        array_insert(array, index, entry);
        return array_index_helper(array->len, index);
    }
}

template<typename K, typename V>
inline ptrdiff_t array_get_insertion_index_by_key(ArrayView<Pair<K, V>>* array, K key) {
    ptrdiff_t min = 0;
    ptrdiff_t max = array->len;
    while (min != max) {
        ptrdiff_t index = (min + max) / 2;
        int cmp = memcmp(&key, &(*array)[index]._0, sizeof(K));
        if      (cmp > 0) min = index + 1;
        else if (cmp < 0) max = index;
        else {
            // already in array: return negative index
            index -= array->len;
            return index;
        }
    }
    return min; // not in array: positive index
}

template<typename K, typename V>
inline V* array_get_by_key(ArrayView<Pair<K, V>>* array, K key) {
    ptrdiff_t index = array_get_insertion_index_by_key(array, key);
    if (index < 0) {
        return &(*array)[index]._1;
    } else {
        return NULL;
    }
}

template<typename K, typename V>
inline bool array_remove_by_key(Array<Pair<K, V>>* array, K key, V* removed = NULL) {
    ptrdiff_t index = array_get_insertion_index_by_key(array, entry._0);
    if (index < 0) {
        if (removed) *removed = array_remove_at(array, index)._1;
        else                    array_remove_at(array, index);
        return true;
    } else {
        return false;
    }
}

template<typename K, typename V>
inline ptrdiff_t array_index_of_key(ArrayView<Pair<K, V>>* array, K key) {
    ptrdiff_t index = array_get_insertion_index_by_key(array, key);
    if (index < 0) return array->len + index;
    else           return PTRDIFF_MIN;
}

template<typename K, typename V>
inline bool array_contains_key(ArrayView<Pair<K, V>>* array, K key) {
    return array_index_of_key(array, key) >= 0;
}
