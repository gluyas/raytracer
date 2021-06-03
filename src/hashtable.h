#include "prelude.h"

template<typename A, typename B>
struct Pair {
    A a;
    B b;
};

template<typename A>
struct Pair<A, void> {
    A a;
    inline operator A() {
        return this->a;
    }
};

template<typename B>
struct Pair<void, B> {
    B b;
    inline operator B() {
        return this->b;
    }
};

template<typename K, typename V, K EmptyKey = {}>
struct Hashtable {
    Array<Pair<T, U>> array;
};
