#include <iostream>
#include <unordered_map>
#include <list>
#include <string>
#include <windows.h>
#include <chrono>
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib") // Tells the compiler to link the Windows Network Library
#include <sstream>
#include <fstream> // For reading and writing files on disk 
#include <cstdlib> // For rand() and srand()
#include <ctime>   // For initializing random seed

using namespace std;

class CacheShard {
private:
    struct CacheNode {
        string key;
        string value;

        chrono::steady_clock::time_point expiry_time; 
        bool has_ttl; 
        
        // --- NEW APPROXIMATE LRU VARIABLE ---
        chrono::steady_clock::time_point last_accessed; 

        CacheNode(string k, string v, int ttl_seconds = 0) : key(k), value(v) {
            last_accessed = chrono::steady_clock::now(); // Set initial access time
            if (ttl_seconds > 0) {
                expiry_time = chrono::steady_clock::now() + chrono::seconds(ttl_seconds);
                has_ttl = true;
            } else {
                has_ttl = false;
            }
        }

        // Default constructor needed for map insertions
        CacheNode() : key(""), value(""), has_ttl(false) {
            last_accessed = chrono::steady_clock::now();
        }
    };

    size_t max_capacity;

    // --- CLEANED ENGINE MEMORY ---
    // Deleting list<CacheNode> entirely saves us 16 bytes per node in pointers!
    // Now mapping string keys directly to CacheNode structures
    unordered_map<string, CacheNode> cache_map;

    bool is_expired(const CacheNode& node) {
        if (!node.has_ttl) return false;
        return chrono::steady_clock::now() > node.expiry_time;
    }

    CRITICAL_SECTION shard_lock;

public:
    CacheShard(size_t capacity) {
        max_capacity = capacity;
        InitializeCriticalSection(&shard_lock);
    }

    CacheShard() {
        max_capacity = 10; 
        InitializeCriticalSection(&shard_lock);
    }

    void set_capacity(size_t capacity) {
        max_capacity = capacity;
    }

    ~CacheShard() {
        DeleteCriticalSection(&shard_lock);
    }

    string get(const string& key) {
        EnterCriticalSection(&shard_lock);

        auto map_iterator = cache_map.find(key);
        
        // Cache Miss
        if (map_iterator == cache_map.end()) {
            LeaveCriticalSection(&shard_lock); 
            return "";
        }

        // Passive Expiration Check
        if (is_expired(map_iterator->second)) {
            cache_map.erase(map_iterator);
            LeaveCriticalSection(&shard_lock); 
            return ""; 
        }
        
        // --- HIGH VELOCITY CLOCK UPDATE ---
        // Instead of heavy pointer re-linking via splice, we just overwrite a timestamp!
        map_iterator->second.last_accessed = chrono::steady_clock::now();

        string value = map_iterator->second.value;
        LeaveCriticalSection(&shard_lock); 
        return value;
    }

    void set(const string& key, const string& value, int ttl_seconds = 0) {
        EnterCriticalSection(&shard_lock);

        auto map_iterator = cache_map.find(key);

        // Key already exists -> Update data and update timestamp
        if (map_iterator != cache_map.end()) {
            map_iterator->second.value = value;
            map_iterator->second.last_accessed = chrono::steady_clock::now();

            if (ttl_seconds > 0) {
                map_iterator->second.expiry_time = chrono::steady_clock::now() + chrono::seconds(ttl_seconds);
                map_iterator->second.has_ttl = true;
            } else {
                map_iterator->second.has_ttl = false;
            }
            
            LeaveCriticalSection(&shard_lock); 
            return;
        }

        // --- NEW REDIS-STYLE APPROXIMATE LRU EVICTION ---
        if (cache_map.size() >= max_capacity) {
            // Redis default: sample N=5 keys at random and find the oldest one
            const int SAMPLE_SIZE = 5;
            string oldest_key = "";
            chrono::steady_clock::time_point oldest_time = chrono::steady_clock::time_point::max();

            auto it = cache_map.begin();
            for (int i = 0; i < SAMPLE_SIZE && it != cache_map.end(); ++i) {
                // Advance the map iterator by a pseudo-random index hop
                int hop = rand() % (cache_map.size() / SAMPLE_SIZE + 1);
                for (int h = 0; h < hop && it != cache_map.end(); ++h) {
                    ++it;
                }
                
                if (it == cache_map.end()) break;

                // Compare timestamps to track the least recently used candidate
                if (it->second.last_accessed < oldest_time) {
                    oldest_time = it->second.last_accessed;
                    oldest_key = it->first;
                }
                ++it;
            }

            // Evict the winner from memory
            if (!oldest_key.empty()) {
                cache_map.erase(oldest_key);
            }
        }

        // Insert new item directly into hash map
        cache_map[key] = CacheNode(key, value, ttl_seconds);

        LeaveCriticalSection(&shard_lock); 
    }

    // Background thread cleanup pass
    void clean_expired_keys() {
        EnterCriticalSection(&shard_lock);
        
        auto it = cache_map.begin();
        while (it != cache_map.end()) {
            if (is_expired(it->second)) {
                it = cache_map.erase(it); // Returns the next valid iterator
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

    // --- WAL SYSTEM VARIABLES ---
    ofstream wal_file;
    CRITICAL_SECTION wal_lock;
    
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
            shards[i].set_capacity(capacity_per_shard); // Keeps your existing call
        }

        // --- NEW WAL INITIALIZATION ---
        // Initialize WAL file lock
        InitializeCriticalSection(&wal_lock);
        
        // Open the log file in append mode
        wal_file.open("cache.log", ios::app);

        // Recover previous data on boot
        recover_from_wal();
        // ------------------------------
    }

    ~ShardedCache() {
        // --- NEW WAL CLEANUP ---
        if (wal_file.is_open()) wal_file.close();
        DeleteCriticalSection(&wal_lock);
        // -----------------------

        delete[] shards; // Free memory array when cache goes out of scope
    }

    // Intercepts the key, computes its shard route, and invokes that specific shard's get()
    string get(const string& key) {
        size_t index = get_shard_index(key);
        return shards[index].get(key);
    }

    // Intercepts the key, computes its shard route, and invokes that specific shard's set()
    void set(const string& key, const string& value, int ttl_seconds = 0) {
        // 1. Map to correct memory shard
        size_t index = get_shard_index(key);
        shards[index].set(key, value, ttl_seconds);

        // 2. Append to Disk Log (Thread-Safe)
        EnterCriticalSection(&wal_lock);
        if (wal_file.is_open()) {
            wal_file << "SET " << key << " " << value << " " << ttl_seconds << "\n";
            wal_file.flush(); // Forces Windows to write it to disk instantly
        }
        LeaveCriticalSection(&wal_lock);
    }

    void clean_all_shards() {
        for (size_t i = 0; i < num_shards; ++i) {
            shards[i].clean_expired_keys();
        }
    }

    void recover_from_wal() {
        ifstream read_file("cache.log");
        if (!read_file.is_open()) return; // No previous log file exists yet

        cout << "[WAL Recovery] Found existing cache.log file. Restoring state..." << endl;
        
        string action, key, value;
        int ttl;
        int commands_loaded = 0;

        // Read the file line by line
        while (read_file >> action >> key >> value >> ttl) {
            if (action == "SET") {
                size_t index = get_shard_index(key);
                shards[index].set(key, value, ttl);
                commands_loaded++;
            }
        }
        
        read_file.close();
        cout << "[WAL Recovery] Success. Restored " << commands_loaded << " keys back into memory." << endl;
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