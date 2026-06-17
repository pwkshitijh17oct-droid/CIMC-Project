#include <iostream>
#include <string>
#include <winsock2.h>

#pragma comment(lib, "ws2_32.lib")
using namespace std;

int main() {
    // Initializing Windows Networking
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        cout << "Winsock initialization failed." << endl;
        return 1;
    }

    cout << "========================================" << endl;
    cout << "   SHARDED CACHE INTERACTIVE CLIENT" << endl;
    cout << "========================================" << endl;
    cout << "Enter commands (e.g., SET user_1 Bob, GET user_1)" << endl;
    cout << "Type 'exit' to close the client.\n" << endl;

    while (true) {
        string command;
        cout << "cache> ";
        getline(cin, command);

        // Checking for exit conditions
        if (command == "exit" || command == "quit") break;
        if (command.empty()) continue;

        // Create a fresh TCP Socket for the request
        SOCKET client_socket = socket(AF_INET, SOCK_STREAM, 0);
        if (client_socket == INVALID_SOCKET) {
            cout << "Socket creation failed." << endl;
            continue;
        }

        // Configure target address (Localhost Port 8888)
        sockaddr_in server_addr{};
        server_addr.sin_family = AF_INET;
        server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        server_addr.sin_port = htons(8888);

        // Connect to the running Server
        if (connect(client_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
            cout << "Connection Error: Is your cache server running?" << endl;
            closesocket(client_socket);
            continue;
        }

        //Transmit the user text command
        send(client_socket, command.c_str(), command.length(), 0);

        //Receive the server's processed evaluation
        char buffer[1024] = {0};
        int bytes_received = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
        if (bytes_received > 0) {
            cout << buffer; // Server text format already handles trailing newlines
        } else {
            cout << "Server closed connection unexpectedly." << endl;
        }

        //Close connection pool cleanly
        closesocket(client_socket);
    }

    WSACleanup();
    return 0;
}