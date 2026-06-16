#include <iostream>
#include <unordered_map>
#include <list>
#include <string>
#include <windows.h>

using namespace std;

class LRUCache {
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
    CRITICAL_SECTION cache_lock;

public:
    LRUCache(size_t capacity) {
        max_capacity = capacity;
        InitializeCriticalSection(&cache_lock);
    }

    // Destructor: Clean up the lock from the OS memory when the cache is destroyed
    ~LRUCache() {
        DeleteCriticalSection(&cache_lock);
    }

    string get(const string& key) {

        // EnterCriticalSection "locks" the door. 
        // Other threads will pause here until this thread calls LeaveCriticalSection.
        EnterCriticalSection(&cache_lock);

        // Check if the key exists in our hash map
        auto map_iterator = cache_map.find(key);
        
        // (Cache Miss)
        if (map_iterator == cache_map.end()) {
            return "";
            LeaveCriticalSection(&cache_lock); // Unlock before returning!
        }

        // (Cache Hit)
        // map_iterator->second holds the list iterator (pointer to the node)
        auto list_iterator = map_iterator->second;
        
        // Extracting the data before we move things around
        string value = list_iterator->value;

        // Moving the node to the front of the eviction_list (MRU position)
        // splice() is an O(1) operation that shifts an existing node to a new position
        eviction_list.splice(eviction_list.begin(), eviction_list, list_iterator);

        LeaveCriticalSection(&cache_lock); // Unlock before returning!
        return value;
    }

    void set(const string& key, const string& value) {

        EnterCriticalSection(&cache_lock);

        // Checking if the key already exists
        auto map_iterator = cache_map.find(key);

        if (map_iterator != cache_map.end()) {
            auto list_iterator = map_iterator->second;
            list_iterator->value = value; // Update the value
            
            // Moved it to the front (MRU)
            eviction_list.splice(eviction_list.begin(), eviction_list, list_iterator);
            LeaveCriticalSection(&cache_lock); // Unlock
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

        LeaveCriticalSection(&cache_lock); // Unlock
    }
};

int main() {
    // Create a cache that can only hold 2 items
    LRUCache cache(2);

    cout << "--- Testing Cache Insertion ---" << endl;
    cache.set("user_1", "Alice");
    cache.set("user_2", "Bob");

    cout << "Fetching user_1: " << cache.get("user_1") << " (Expected: Alice)" << endl;

    // This insertion should trigger an eviction because capacity is 2!
    // Since we just fetched "user_1", "user_2" (Bob) is the Least Recently Used item.
    cout << "\n--- Adding user_3 (Should evict user_2) ---" << endl;
    cache.set("user_3", "Charlie");

    // "user_2" should be gone (Cache Miss -> returns empty string)
    cout << "Fetching user_2: " << cache.get("user_2") << " (Expected: [Empty String])" << endl;
    
    // "user_1" and "user_3" should still be there
    cout << "Fetching user_1: " << cache.get("user_1") << " (Expected: Alice)" << endl;
    cout << "Fetching user_3: " << cache.get("user_3") << " (Expected: Charlie)" << endl;

    return 0;
}