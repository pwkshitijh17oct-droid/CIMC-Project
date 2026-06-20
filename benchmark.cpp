#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <winsock2.h>

#pragma comment(lib, "ws2_32.lib")
using namespace std;
using namespace std::chrono;

const string HOST = "127.0.0.1";
const int PORT = 8888;
const int OPS_PER_THREAD = 1000; // Total requests = THREAD_COUNT * OPS_PER_THREAD

// Structure to pass configuration data to each benchmark thread
struct WorkerConfig {
    int thread_id;
};

// Helper to send a single fast command over TCP
void send_command(const string& cmd) {
    SOCKET client_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (client_socket == INVALID_SOCKET) return;

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr(HOST.c_str());
    server_addr.sin_port = htons(PORT);

    if (connect(client_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) != SOCKET_ERROR) {
        send(client_socket, cmd.c_str(), cmd.length(), 0);
        
        char buffer[256];
        recv(client_socket, buffer, sizeof(buffer) - 1, 0); // Wait for server confirmation
    }
    closesocket(client_socket);
}

// Worker thread runner
DWORD WINAPI BenchmarkWorker(LPVOID lpParam) {
    WorkerConfig* config = (WorkerConfig*)lpParam;
    int id = config->thread_id;

    for (int i = 0; i < OPS_PER_THREAD; ++i) {
        // Alternating mix of 50% writes and 50% reads
        if (i % 2 == 0) {
            send_command("SET bench_key_" + to_string(id) + "_" + to_string(i) + " val");
        } else {
            send_command("GET bench_key_" + to_string(id) + "_" + to_string(i - 1));
        }
    }

    delete config;
    return 0;
}

int main(int argc, char* argv[]) {
    int thread_count = 10; // Default to 10 parallel threads
    if (argc > 1) {
        thread_count = atoi(argv[1]);
    }

    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    int total_operations = thread_count * OPS_PER_THREAD;
    cout << "==================================================" << endl;
    cout << "        LAUNCHING CACHE BENCHMARK SUITE          " << endl;
    cout << "==================================================" << endl;
    cout << "Concurrently Spawning : " << thread_count << " worker threads" << endl;
    cout << "Operations per thread : " << OPS_PER_THREAD << endl;
    cout << "Total Network Workload: " << total_operations << " requests\n" << endl;
    cout << "Running... please wait..." << endl;

    // Start tracking high-resolution wall clock execution time
    auto start_time = high_resolution_clock::now();

    vector<HANDLE> threads(thread_count);
    for (int i = 0; i < thread_count; ++i) {
        WorkerConfig* config = new WorkerConfig{i};
        threads[i] = CreateThread(NULL, 0, BenchmarkWorker, config, 0, NULL);
    }

    // Waiting for all parallel client threads to cross the finish line
    WaitForMultipleObjects(thread_count, threads.data(), TRUE, INFINITE);

    auto end_time = high_resolution_clock::now();
    
    // Cleaning up thread handles
    for (int i = 0; i < thread_count; ++i) {
        CloseHandle(threads[i]);
    }
    WSACleanup();

    // metric calculations
    auto total_duration_ms = duration_cast<milliseconds>(end_time - start_time).count();
    double total_duration_seconds = total_duration_ms / 1000.0;
    double throughput = total_operations / total_duration_seconds;
    double avg_latency = (double)total_duration_ms / total_operations;

    cout << "\n================ BENCHMARK RESULTS ================" << endl;
    cout << "Total Time Taken : " << total_duration_seconds << " seconds" << endl;
    cout << "Throughput       : " << (int)throughput << " operations/sec" << endl;
    cout << "Average Latency  : " << avg_latency << " ms per request" << endl;
    cout << "===================================================" << endl;

    return 0;
}