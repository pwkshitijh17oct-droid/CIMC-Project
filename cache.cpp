#include <iostream>
#include <unordered_map>
#include <list>
#include <string>
#include <windows.h>
#include <chrono>
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib") // Tells the compiler to link the Windows Network Library
#include <sstream>

using namespace std;

class CacheShard {
private:
    struct CacheNode {
        string key;
        string value;

        chrono::steady_clock::time_point expiry_time; 
        bool has_ttl; // True if this item expires, false if it lives forever
        
        CacheNode(string k, string v, int ttl_seconds = 0) : key(k), value(v) {
            if (ttl_seconds > 0) {
                expiry_time = chrono::steady_clock::now() + chrono::seconds(ttl_seconds);
                has_ttl = true;
            } else {
                has_ttl = false;
            }
        }
    };

    size_t max_capacity;

    // used HashMap + Doubly Linked List to achieve O(1) get() and put() operations.

    // The Doubly Linked List: Front is MRU, Back is LRU.
    list<CacheNode> eviction_list;

    // The Hash Map: maps key to an iterator in the eviction_list.
    unordered_map<string, list<CacheNode>::iterator> cache_map;

    bool is_expired(const CacheNode& node) {
        if (!node.has_ttl) return false;
        return chrono::steady_clock::now() > node.expiry_time;
    }

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

        // --- PASSIVE EXPIRATION CHECK ---
        if (is_expired(*list_iterator)) {
            // Clean it out of memory immediately!
            cache_map.erase(key);
            eviction_list.erase(list_iterator);
            
            LeaveCriticalSection(&shard_lock); // Don't forget to unlock!
            return ""; // Treat it as a cache miss
        }
        // --------------------------------
        
        // Extracting the data before we move things around
        string value = list_iterator->value;

        // Moving the node to the front of the eviction_list (MRU position)
        // splice() is an O(1) operation that shifts an existing node to a new position
        eviction_list.splice(eviction_list.begin(), eviction_list, list_iterator);

        LeaveCriticalSection(&shard_lock); // Unlock before returning!
        return value;
    }

    void set(const string& key, const string& value, int ttl_seconds = 0) {

        EnterCriticalSection(&shard_lock);

        // Checking if the key already exists
        auto map_iterator = cache_map.find(key);

        if (map_iterator != cache_map.end()) {
            auto list_iterator = map_iterator->second;
            list_iterator->value = value; // Update the value

            if (ttl_seconds > 0) {
                list_iterator->expiry_time = chrono::steady_clock::now() + chrono::seconds(ttl_seconds);
                list_iterator->has_ttl = true;
            } else {
                list_iterator->has_ttl = false;
            }
            
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
        eviction_list.push_front(CacheNode(key, value, ttl_seconds));
        
        // Mapped the key to the newly inserted front node
        cache_map[key] = eviction_list.begin();

        LeaveCriticalSection(&shard_lock); // Unlock
    }

    void clean_expired_keys() {
        EnterCriticalSection(&shard_lock);
        
        auto it = eviction_list.begin();
        while (it != eviction_list.end()) {
            if (is_expired(*it)) {
                cache_map.erase(it->key);
                it = eviction_list.erase(it); // erase returns the next valid iterator
                cout << "[Background Shard Worker] Evicted expired key from memory." << endl;
            } else {
                ++it;
            }
        }
        
        LeaveCriticalSection(&shard_lock);
    }
};

class ShardedCache {
private:
    size_t num_shards;
    CacheShard* shards;    // Dynamic array of isolated buckets(shards)
    hash<string> hash_fn;  // Standard C++ string hashing function

    // Modulo maps the hash value uniformly to a valid shard index
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
    void set(const string& key, const string& value, int ttl_seconds = 0) {
        size_t index = get_shard_index(key);
        shards[index].set(key, value, ttl_seconds);
    }

    void clean_all_shards() {
        for (size_t i = 0; i < num_shards; ++i) {
            shards[i].clean_expired_keys();
        }
    }
};

DWORD WINAPI BackgroundCleanupWorker(LPVOID lpParam) {
    ShardedCache* cache = (ShardedCache*)lpParam;
    while (true) {
        Sleep(1000); 
        cache->clean_all_shards();
    }
    return 0;
}

string process_client_command(ShardedCache& cache, const string& raw_command) {
    stringstream ss(raw_command);
    string action, key, value;
    int ttl = 0;

    ss >> action; // Extracts the first word (GET or SET)

    if (action == "SET") {
        ss >> key >> value;
        if (ss >> ttl) {
            // If a TTL value was provided, pass it along
            cache.set(key, value, ttl);
        } else {
            cache.set(key, value, 0);
        }
        return "OK\r\n";
    } 
    else if (action == "GET") {
        ss >> key;
        string result = cache.get(key);
        if (result == "") {
            return "(nil)\r\n"; // Standard Redis-style response for a cache miss
        }
        return result + "\r\n";
    }

    return "ERROR: Unknown Command\r\n";
}

// This structure wraps the data a worker thread needs to communicate with a client
struct ThreadParam {
    SOCKET client_socket;
    ShardedCache* cache;
};

// The worker function executed by each independent client thread
DWORD WINAPI ClientHandlerThread(LPVOID lpParam) {
    ThreadParam* params = (ThreadParam*)lpParam;
    SOCKET client_socket = params->client_socket;
    ShardedCache* cache = params->cache;

    char buffer[1024];
    int bytes_received = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
    
    if (bytes_received > 0) {
        buffer[bytes_received] = '\0';
        string client_msg(buffer);

        // Clean up trailing whitespace and newlines
        while (!client_msg.empty() && (client_msg.back() == '\n' || client_msg.back() == '\r' || client_msg.back() == ' ')) {
            client_msg.pop_back();
        }

        cout << "[Thread " << GetCurrentThreadId() << "] Received: " << client_msg << endl;

        // Process and respond
        string response = process_client_command(*cache, client_msg);
        send(client_socket, response.c_str(), response.length(), 0);
    }

    closesocket(client_socket);
    
    // Clean up the dynamically allocated parameters memory to avoid leaks
    delete params; 
    return 0;
}

int main() {
    // 1. Initialize our 4-shard Cache
    ShardedCache cache(4, 2);

    // 2. Start the Background TTL Cleanup Thread
    CreateThread(NULL, 0, BackgroundCleanupWorker, &cache, 0, NULL);

    // 3. Initialize Winsock Layer
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        cout << "Failed to initialize Winsock." << endl;
        return 1;
    }

    // 4. Create the Listening Socket
    SOCKET server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == INVALID_SOCKET) {
        cout << "Socket creation failed." << endl;
        WSACleanup();
        return 1;
    }

    // 5. Bind the socket to Port 8888
    sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY; // Listen on any network interface (localhost)
    server_addr.sin_port = htons(8888);       // Port number 8888

    if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        cout << "Bind failed with error code: " << WSAGetLastError() << endl;
        closesocket(server_socket);
        WSACleanup();
        return 1;
    }

    // 6. Start Listening for connections (Max queue length of 5)
    listen(server_socket, 5);
    cout << "🚀 Sharded Cache Server is live and listening on port 8888..." << endl;

    // 7. The Multi-Threaded Server Loop
    while (true) {
        // Accept incoming client connection
        SOCKET client_socket = accept(server_socket, NULL, NULL);
        if (client_socket == INVALID_SOCKET) continue;

        cout << "[Master Server] Connection accepted. Dispatching to worker thread..." << endl;

        // Package up the socket and cache reference dynamically for the new thread
        ThreadParam* params = new ThreadParam();
        params->client_socket = client_socket;
        params->cache = &cache;

        // Fire and forget: Spin up an independent worker thread for this client
        HANDLE hThread = CreateThread(NULL, 0, ClientHandlerThread, params, 0, NULL);
        if (hThread != NULL) {
            // Close the handle token right away so the OS cleans up thread resources 
            // automatically when it finishes executing. The thread keeps running!
            CloseHandle(hThread); 
        } else {
            // Safe fallback if the OS runs out of resources to spawn a thread
            cout << "[Master Server] Error: Failed to create worker thread." << endl;
            closesocket(client_socket);
            delete params;
        }
    }

    // Cleanup (Unreachable code in infinite loop, but good practice to write)
    closesocket(server_socket);
    WSACleanup();
    return 0;
}