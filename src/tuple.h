#pragma once

template<typename T0, typename T1>
struct Pair {
    T0 _0;
    T1 _1;
};
template<typename T0>
struct Pair<T0, void> {
    T0 _0;
};
template<typename T1>
struct Pair<void, T1> {
    T1 _1;
};

template<typename T0, typename T1, typename T2>
struct Triplet {
    T0 _0;
    T1 _1;
    T2 _2;
};

template<typename T, size_t N>
struct Tuple {
    T _[N];
};
