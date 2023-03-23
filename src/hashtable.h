template<typename K, typename V = void>
struct HashTable {
    size_t      buckets;
    size_t      entries;
    size_t      load;
    char*       meta;
    Pair<K, V>* table;
};

template<typename K, typename V>
HashTable<K, V> table_init(size_t inital_cap) {

}

template<typename K, typename V>
size_t hash(K key) {

}

template<typename K, typename V>
bool table_insert(HashTable<K, V>* table, size_t hash, K key, V* prev = NULL) {

}

template<typename K, typename V>
V* table_get(HashTable<K, V>* table, K key) {
    size_t index = table_find(table, key);
    return &table->entries[index]._1;
}

template<typename K, typename V>
size_t table_find(HashTable<K, V>* table, K key) {
    size_t hash     = hash(elem);
    char   hash_low = (char) (hash & 0x7F);
    for (
}

template<typename K, typename V>
void table_rehash(HashTable<K, V>* table) {
    table_rehash(table, table->cap*2);
}

template<typename K, typename V>
void table_rehash(HashTable<K, V>* table, size_t new_cap) {

}

template<typename K, typename V>
void table_free(HashTable<K, V>* table) {
    free(table->meta);
    *table = {};
}
