#include <iostream>
#include <unordered_map>
#include <list>
#include <string>
#include <windows.h>

using namespace std;

class CacheShard {
private:
    struct CacheNode {
        string key;
        string value;
        
        CacheNode(string k, string v) : key(k), value(v) {}
    };

    size_t max_capacity;

    // used HashMap + Doubly Linked List to achieve O(1) get() and put() operations.

    // The Doubly Linked List: Front is MRU, Back is LRU.
    list<CacheNode> eviction_list;

    // The Hash Map: maps key to an iterator in the eviction_list.
    unordered_map<string, list<CacheNode>::iterator> cache_map;

    // CRITICAL_SECTION is the native Windows equivalent of a mutex.
    // It prevents multiple threads from executing code inside a block simultaneously.
    CRITICAL_SECTION shard_lock;

public:
    CacheShard(size_t capacity) {
        max_capacity = capacity;
        InitializeCriticalSection(&shard_lock);
    }

    // Default constructor so arrays of shards can be allocated
    CacheShard() {
        max_capacity = 10; // Fallback default capacity
        InitializeCriticalSection(&shard_lock);
    }

    // A setter so the master class can configure each shard's size
    void set_capacity(size_t capacity) {
        max_capacity = capacity;
    }

    // Destructor: Clean up the lock from the OS memory when the cache is destroyed
    ~CacheShard() {
        DeleteCriticalSection(&shard_lock);
    }

    string get(const string& key) {

        // EnterCriticalSection "locks" the door. 
        // Other threads will pause here until this thread calls LeaveCriticalSection.
        EnterCriticalSection(&shard_lock);

        // Check if the key exists in our hash map
        auto map_iterator = cache_map.find(key);
        
        // (Cache Miss)
        if (map_iterator == cache_map.end()) {
            LeaveCriticalSection(&shard_lock); // Unlock before returning!
            return "";
        }

        // (Cache Hit)
        // map_iterator->second holds the list iterator (pointer to the node)
        auto list_iterator = map_iterator->second;
        
        // Extracting the data before we move things around
        string value = list_iterator->value;

        // Moving the node to the front of the eviction_list (MRU position)
        // splice() is an O(1) operation that shifts an existing node to a new position
        eviction_list.splice(eviction_list.begin(), eviction_list, list_iterator);

        LeaveCriticalSection(&shard_lock); // Unlock before returning!
        return value;
    }

    void set(const string& key, const string& value) {

        EnterCriticalSection(&shard_lock);

        // Checking if the key already exists
        auto map_iterator = cache_map.find(key);

        if (map_iterator != cache_map.end()) {
            auto list_iterator = map_iterator->second;
            list_iterator->value = value; // Update the value
            
            // Moved it to the front (MRU)
            eviction_list.splice(eviction_list.begin(), eviction_list, list_iterator);
            LeaveCriticalSection(&shard_lock); // Unlock
            return;
        }

        // Key is new. First, handle eviction if cache is full.
        if (cache_map.size() >= max_capacity) {
            // Get the least recently used element from the back of the list
            auto lru_node = eviction_list.back();

            cache_map.erase(lru_node.key);
            
            eviction_list.pop_back();
        }

        //Inserted the new item at the front of the list
        eviction_list.push_front(CacheNode(key, value));
        
        // Mapped the key to the newly inserted front node
        cache_map[key] = eviction_list.begin();

        LeaveCriticalSection(&shard_lock); // Unlock
    }
};

class ShardedCache {
private:
    size_t num_shards;
    CacheShard* shards;    // Dynamic array of isolated buckets
    hash<string> hash_fn;  // Standard C++ string hashing engine

    // Modulo arithmetic maps the hash value uniformly to a valid shard index
    size_t get_shard_index(const string& key) {
        return hash_fn(key) % num_shards;
    }

public:
    ShardedCache(size_t shard_count, size_t capacity_per_shard) {
        num_shards = shard_count;
        shards = new CacheShard[num_shards]; // Allocates using default constructor
        
        // Distribute the capacity sizing to each bucket segment
        for (size_t i = 0; i < num_shards; ++i) {
            shards[i].set_capacity(capacity_per_shard);
        }
    }

    ~ShardedCache() {
        delete[] shards; // Free memory array when cache goes out of scope
    }

    // Intercepts the key, computes its shard route, and invokes that specific shard's get()
    string get(const string& key) {
        size_t index = get_shard_index(key);
        return shards[index].get(key);
    }

    // Intercepts the key, computes its shard route, and invokes that specific shard's set()
    void set(const string& key, const string& value) {
        size_t index = get_shard_index(key);
        shards[index].set(key, value);
    }
};

int main() {
    // Instantiate 4 independent shards. Each individual shard holds up to 2 items.
    ShardedCache cache(4, 2);

    cout << "--- Testing Sharded Cache Architecture ---" << endl;
    cache.set("user_1", "Alice");
    cache.set("user_2", "Bob");

    cout << "Fetching user_1: " << cache.get("user_1") << " (Expected: Alice)" << endl;

    cout << "\n--- Adding user_3 ---" << endl;
    cache.set("user_3", "Charlie");

    cout << "Fetching user_2: " << cache.get("user_2") << endl;
    cout << "Fetching user_1: " << cache.get("user_1") << " (Expected: Alice)" << endl;
    cout << "Fetching user_3: " << cache.get("user_3") << " (Expected: Charlie)" << endl;

    return 0;
}