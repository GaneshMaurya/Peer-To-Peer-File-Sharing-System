#include <bits/stdc++.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <openssl/sha.h>
#include <sys/stat.h>
#include <openssl/evp.h>
#include "../include/json.hpp"
#include "../include/encoder-decoder.hpp"
using json = nlohmann::json;
using namespace std;

int BUFFER_SIZE = 1024;
int CHUNK_SIZE = 512*1024;
int LISTEN_BACKLOG = 10;
int opt = 1;

struct ThreadArgs {
    int socketFd;
    sockaddr_in address;
    socklen_t addrlen;
    char * port;
};

vector<string> trackersList;
vector<string> trackersIPList;
vector<string> trackersPortList;

struct DownloadChunkArgs {
    int chunk_number;
    string file_path;
    string ip_address;
    string port;
};

// Shared set and mutex to track active (ip, port) pairs
map<pair<string, string>, pthread_t> client_threads;
mutex active_clients_mutex;

// Send a large message with length prefix
bool sendLargeMessage(int sockfd, const string& data) {
    uint64_t len = data.size();
    uint64_t len_net = htobe64(len);
    
    if (send(sockfd, &len_net, sizeof(len_net), 0) != sizeof(len_net)) {
        return false;
    } 
    
    size_t total_sent = 0;
    while (total_sent < len) {
        ssize_t sent = send(sockfd, data.data() + total_sent, len - total_sent, 0);
        if (sent <= 0) {
            return false;
        }

        total_sent += sent;
    }
    return true;
}

// Receive a large message with length prefix
bool recvLargeMessage(int sockfd, string& data) {
    uint64_t len_net;
    size_t received = 0;
    
    while (received < sizeof(len_net)) {
        ssize_t r = recv(sockfd, ((char*)&len_net) + received, sizeof(len_net) - received, 0);
        if (r <= 0) {
            return false;
        }

        received += r;
    }

    uint64_t len = be64toh(len_net);
    data.resize(len);
    size_t total_received = 0;
    while (total_received < len) {
        ssize_t r = recv(sockfd, &data[total_received], len - total_received, 0);
        if (r <= 0) {
            return false;
        }

        total_received += r;
    }
    return true;
}

string sha1ToHexString(const unsigned char sha1Hash[SHA_DIGEST_LENGTH])
{
    stringstream ss;
    ss << hex << setfill('0');
    for (int i = 0; i < SHA_DIGEST_LENGTH; i++)
    {
        ss << setw(2) << static_cast<unsigned int>(sha1Hash[i]);
    }
    return ss.str();
}

void* downloadChunkThread(void* arg) {
    DownloadChunkArgs* args = static_cast<DownloadChunkArgs*>(arg);

    string ip_address = args->ip_address;
    string port = args->port;
    string file_path = args->file_path;
    int chunk_number = args->chunk_number;

    json request_json;
    request_json["command"] = "download_file";
    request_json["chunk_number"] = chunk_number;
    request_json["ip_address"] = ip_address;
    request_json["port"] = port;
    request_json["file_path"] = file_path;

    // Connect to peer
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        string* result = new string("Failed to create socket");
        delete args;
        return result;
    }

    sockaddr_in peer_addr;
    peer_addr.sin_family = AF_INET;
    peer_addr.sin_port = htons(static_cast<uint16_t>(stoi(port)));
    inet_pton(AF_INET, ip_address.c_str(), &peer_addr.sin_addr);

    if (connect(sockfd, (sockaddr*)&peer_addr, sizeof(peer_addr)) < 0) {
        close(sockfd);
        string* result = new string("Failed to connect to peer " + ip_address + ":" + port);
        delete args;
        return result;
    }

    // Send request
    string req_str = request_json.dump();
    if (!sendLargeMessage(sockfd, req_str)) {
        close(sockfd);
        string* result = new string("Failed to send request to peer " + ip_address + ":" + port);
        delete args;
        return result;
    }

    // Receive response
    string response_str;
    if (!recvLargeMessage(sockfd, response_str)) {
        close(sockfd);
        string* result = new string("Failed to receive response from peer " + ip_address + ":" + port);
        delete args;
        return result;
    }
    close(sockfd);
    
    {
        lock_guard<mutex> lock(active_clients_mutex);
        client_threads.erase({args->ip_address, args->port});
    }

    string* result = new string(response_str);
    delete args;
    return result;
}

void* handleListen(void* args) {
    ThreadArgs *threadArgs = (ThreadArgs *)args;
    int client_socket_fd = threadArgs->socketFd;
    sockaddr_in client_adddress = threadArgs->address;
    socklen_t client_addrlen = threadArgs->addrlen;
    string client_port = threadArgs->port;

    fcntl(client_socket_fd, F_SETFL, O_NONBLOCK);

    while (true) {
        sockaddr_in peer_addr;
        socklen_t peer_addrlen = sizeof(peer_addr);
        int peer_fd = accept(client_socket_fd, (sockaddr*)&peer_addr, &peer_addrlen);
        if (peer_fd < 0) {
            // cout << "Failed to accept connection\n";
            continue;
        }

        // Receive message
        string request_str;
        if (!recvLargeMessage(peer_fd, request_str)) {
            cout << "Failed to receive message from peer\n";
            close(peer_fd);
            continue;
        }

        json request_json = json::parse(request_str);
        json response_json;
        response_json["ip_address"] = request_json["ip_address"];
        response_json["port"] = request_json["port"];

        if (request_json["command"] == "download_file") {
            int chunk_number = request_json["chunk_number"];
            string file_path = request_json["file_path"];

            int fd = open(file_path.c_str(), O_RDONLY);
            if (fd < 0) {
                response_json["status"] = "failure";
                response_json["message"] = "Could not open file";
            } 
            else {
                off_t offset = static_cast<off_t>(chunk_number) * CHUNK_SIZE;
                if (lseek(fd, offset, SEEK_SET) == (off_t)-1) {
                    response_json["status"] = "failure";
                    response_json["message"] = "Seek failed";
                    close(fd);
                } 
                else {
                    char buffer[CHUNK_SIZE];
                    ssize_t bytes_read = read(fd, buffer, CHUNK_SIZE);
                    close(fd);

                    if (bytes_read < 0) {
                        response_json["status"] = "failure";
                        response_json["message"] = "Read failed";
                    } 
                    else {
                        response_json["status"] = "success";
                        response_json["chunk_number"] = chunk_number;
                        string encoded = base64_encode(reinterpret_cast<const unsigned char*>(buffer), bytes_read);
                        response_json["data"] = encoded;
                    }
                }
            }
        } 
        else {
            response_json["status"] = "failure";
            response_json["message"] = "Unknown command";
        }

        string resp_str = response_json.dump();
        if (!sendLargeMessage(peer_fd, resp_str)) {
            cout << "Failed to send response to peer\n";
        }

        close(peer_fd);
    }

    return NULL;
}

int connectToAnyTracker(sockaddr_in& tracker_address) {
    for (int i = 0; i < trackersList.size(); i++)
    {
        char *trackerIpAddress = trackersIPList[i].data();
        char *trackerPort = trackersPortList[i].data();

        in_addr_t ipInBinary = inet_addr(trackerIpAddress);
        tracker_address.sin_addr.s_addr = ipInBinary;
        uint16_t PORT = static_cast<uint16_t>(strtoul(trackerPort, NULL, 10));
        tracker_address.sin_port = htons(PORT);

        int tracker_socket_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (tracker_socket_fd < 0)
        {
            continue;
        }

        int status = connect(tracker_socket_fd, (struct sockaddr *)&tracker_address, sizeof(tracker_address));
        if (status != -1) {
            cout << "Connected to server...\n\n";
            return tracker_socket_fd;
        }

        close(tracker_socket_fd);
    }

    return -1;
}

bool sendMessageToTracker(int& tracker_socket_fd, sockaddr_in& tracker_address, json& request_json, json& response_json) {
    string request_string = request_json.dump();

    if (tracker_socket_fd < 0 || !sendLargeMessage(tracker_socket_fd, request_string)) {
        if (tracker_socket_fd >= 0) {
            close(tracker_socket_fd);
        }

        tracker_socket_fd = connectToAnyTracker(tracker_address);
        if (tracker_socket_fd < 0) {
            return false;
        }
        
        if (!sendLargeMessage(tracker_socket_fd, request_string)) {
            return false;
        }
    }

    string response_str;
    if (!recvLargeMessage(tracker_socket_fd, response_str)) {
        close(tracker_socket_fd);
        tracker_socket_fd = -1;
        return false;
    }

    response_json = json::parse(response_str);
    return true;
}

int main(int argc, char *argv[]) {
    if (argc > 3)
    {
        cout << "Please enter the command in this format: ./a.out <ip>:<port> <tracker_info.txt>\n";
        return 1;
    }

    // Get the tracker details
    char *tracker_info_path = argv[2];
    int tracker_info_fd = open(tracker_info_path, O_RDWR);
    if (tracker_info_fd < 0)
    {
        cout << "Error: Could not open " << tracker_info_path << " file.\n";
        return 1;
    }

    char *tracker_info_content = new char[BUFFER_SIZE];
    ssize_t tracker_info_content_size = read(tracker_info_fd, tracker_info_content, BUFFER_SIZE);
    if (tracker_info_content_size < 0) {
        cout << "Error reading tracker info file\n";
        close(tracker_info_fd);
        delete[] tracker_info_content;
        return 1;
    }

    stringstream ss(tracker_info_content);string line;
    while (getline(ss, line)) {
        if (!line.empty()) {
            trackersList.push_back(line);
        }
    }

    for (const string& line : trackersList) {
        size_t pos = line.find(':');
        if (pos != string::npos) {
            string ip_address = line.substr(0, pos);
            string port = line.substr(pos + 1);

            trackersIPList.push_back(ip_address);
            trackersPortList.push_back(port);
        }
    }

    // Get the IP Address and Port of the client
    string arg(argv[1]);
    size_t pos = arg.find(':');
    string client_ip_address = arg.substr(0, pos);
    string client_port = (pos != string::npos) ? arg.substr(pos + 1) : "";

    // Socket creation
    int client_socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (client_socket_fd == -1)
    {
        cout << "An error occurred in creating socket\n";
        return 1;
    }

    if (setsockopt(client_socket_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt)))
    {
        cout << "Error: Sockopt\n";
        return 1;
    }

    // Binding
    sockaddr_in client_address;
    socklen_t client_addrlen = sizeof(client_address);
    client_address.sin_family = AF_INET;
    in_addr_t ipInBinary = inet_addr(client_ip_address.c_str());
    client_address.sin_addr.s_addr = ipInBinary;
    uint16_t PORT = static_cast<uint16_t>(strtoul(client_port.c_str(), NULL, 10));
    client_address.sin_port = htons(PORT);
    
    if (bind(client_socket_fd, (struct sockaddr *)&client_address, sizeof(client_address)) < 0)
    {
        cout << "Error: Could not bind to " << client_port << " port.\n";
        return 1;
    }

    // Listening
    if (listen(client_socket_fd, LISTEN_BACKLOG) < 0)
    {
        cout << "Error\n";
        return 1;
    }
    cout << "Client listening on port " << client_port << "...\n";
    

    ThreadArgs *threadArgs = new ThreadArgs{client_socket_fd, client_address, client_addrlen, client_port.data()};
    pthread_t client_listen_thread;
    if (pthread_create(&client_listen_thread, NULL, handleListen, (void *)threadArgs) != 0)
    {
        cout << "Failed to create listening thread...\n";
        return 1;
    }

    struct sockaddr_in tracker_address;
    tracker_address.sin_family = AF_INET;
    cout << "Initiating Connection with tracker...\n";
    int tracker_socket_fd = connectToAnyTracker(tracker_address);

    // Handle user input
    while (true) {
        string input;
        getline(cin, input);

        istringstream iss(input);
        vector<string> tokens;
        string token;
        while (iss >> token) {
            tokens.push_back(token);
        }

        if (tokens.empty()) {
            cout << "No command entered.\n";
            return 1;
        }

        string command = tokens[0];
        json request_json;
        request_json["command"] = command;

        if (tokens[0] == "create_user") {
            if (tokens.size() != 3) {
                cout << "Enter correct command with arguments\n\n";
                continue;
            }

            string user_id = tokens[1];
            string passwd = tokens[2];
            request_json["command_args"]["user_id"] = user_id;
            request_json["command_args"]["passwd"] = passwd;
        }
        else if (tokens[0] == "login") {
            if (tokens.size() != 3) {
                cout << "Enter correct command with arguments\n\n";
                continue;
            }

            string user_id = tokens[1];
            string passwd = tokens[2];
            request_json["command_args"]["user_id"] = user_id;
            request_json["command_args"]["passwd"] = passwd;
            request_json["socket_fd"] = client_socket_fd;
        }
        else if (tokens[0] == "create_group") {
            if (tokens.size() != 2) {
                cout << "Enter correct command with arguments\n\n";
                continue;
            }

            string group_id = tokens[1];
            request_json["command_args"]["group_id"] = group_id;
        }
        else if (tokens[0] == "join_group") {
            if (tokens.size() != 2) {
                cout << "Enter correct command with arguments\n\n";
                continue;
            }

            string group_id = tokens[1];
            request_json["command_args"]["group_id"] = group_id;
        }
        else if (tokens[0] == "leave_group") {
            if (tokens.size() != 2) {
                cout << "Enter correct command with arguments\n\n";
                continue;
            }

            string group_id = tokens[1];
            request_json["command_args"]["group_id"] = group_id;
        }
        else if (tokens[0] == "list_requests") {
            if (tokens.size() != 2) {
                cout << "Enter correct command with arguments\n\n";
                continue;
            }

            string group_id = tokens[1];
            request_json["command_args"]["group_id"] = group_id;
            request_json["sender_ip"] = client_ip_address;
            request_json["sender_port"] = client_port;

            json response_json;
            if (sendMessageToTracker(tracker_socket_fd, tracker_address, request_json, response_json) == false) {
                cout << "Operation Failed...\n";
                continue;
            }

            if (response_json["status"] == "success") {
                cout << "List of pending requests\n";
                for (auto user: response_json["pending_list"]) {
                    cout << user << "\n";
                }
                cout << "\n";
            }
            else {
                cout << response_json["message"] << "\n\n";
            }

            continue;
        }
        else if (tokens[0] == "accept_request") {
            if (tokens.size() != 3) {
                cout << "Enter correct command with arguments\n\n";
                continue;
            }

            string group_id = tokens[1];
            string user_id = tokens[2];
            request_json["command_args"]["group_id"] = group_id;
            request_json["command_args"]["user_id"] = user_id;
        }
        else if (tokens[0] == "list_groups") {
            if (tokens.size() != 1) {
                cout << "Enter correct command with arguments\n\n";
                continue;
            }

            request_json["sender_ip"] = client_ip_address;
            request_json["sender_port"] = client_port;

            json response_json;
            if (sendMessageToTracker(tracker_socket_fd, tracker_address, request_json, response_json) == false) {
                cout << "Operation Failed...\n";
                continue;
            }

            if (response_json["status"] == "success") {
                cout << "List of groups\n";
                for (auto group: response_json["groups_list"]) {
                    cout << group << "\n";
                }
                cout << "\n";
            }
            else {
                cout << response_json["message"] << "\n\n";
            }

            continue;
        }
        else if (tokens[0] == "list_files") {
            if (tokens.size() != 2) {
                cout << "Enter correct command with arguments\n\n";
                continue;
            }

            string group_id = tokens[1];
            request_json["command_args"]["group_id"] = group_id;
            request_json["sender_ip"] = client_ip_address;
            request_json["sender_port"] = client_port;

            json response_json;
            if (sendMessageToTracker(tracker_socket_fd, tracker_address, request_json, response_json) == false) {
                cout << "Operation Failed...\n";
                continue;
            }

            if (response_json["status"] == "success") {
                cout << "List of files\n";
                for (auto file: response_json["files_list"]) {
                    cout << file << "\n";
                }
                cout << "\n";
            }
            else {
                cout << response_json["message"] << "\n\n";
            }

            continue;
        }
        else if (tokens[0] == "upload_file") {
            if (tokens.size() != 3) {
                cout << "Enter correct command with arguments\n\n";
                continue;
            }

            string file_path = tokens[1];
            string group_id = tokens[2];
            request_json["command_args"]["file_path"] = file_path;
            request_json["command_args"]["group_id"] = group_id;

            vector<string> piecewise_sha;
            string fileSha = "";

            struct stat fileInfo;
            stat(file_path.c_str(), &fileInfo);
            int fileSize = fileInfo.st_size;
            int numChunks = (fileSize+CHUNK_SIZE-1)/CHUNK_SIZE;

            int inputFile = open(file_path.c_str(), O_RDONLY);
            if (inputFile < 0)
            {
                cout << "Error opening file: " << file_path << "...\n";
                continue;
            }

            for (int i=0; i<numChunks; i++)
            {
                int currentChunkSize = min(CHUNK_SIZE, fileSize);
                char *fileChunk = new char[currentChunkSize];

                ssize_t chunkRead = read(inputFile, fileChunk, currentChunkSize);
                if (chunkRead < 0)
                {
                    cout << "Error reading file: " << file_path << "...\n";
                    delete[] fileChunk;
                    break;
                }

                unsigned char chunkShaTemp[SHA_DIGEST_LENGTH];
                SHA1(reinterpret_cast<const unsigned char *>(fileChunk), chunkRead, chunkShaTemp);
                string chunkSha = sha1ToHexString(chunkShaTemp);
                
                piecewise_sha.push_back(chunkSha);
                fileSha += chunkSha;

                delete[] fileChunk;
                fileSize -= currentChunkSize;
            }
            close(inputFile);

            request_json["piecewise_sha"] = piecewise_sha;
            request_json["fileSha"] = fileSha;
        }
        else if (tokens[0] == "download_file") {
            if (tokens.size() != 4) {
                cout << "Enter correct command with arguments\n\n";
                continue;
            }

            string group_id = tokens[1];
            string file_name = tokens[2];
            string destination_path = tokens[3];
            request_json["command_args"]["group_id"] = group_id;
            request_json["command_args"]["file_name"] = file_name;
            request_json["command_args"]["destination_path"] = destination_path;
            request_json["sender_ip"] = client_ip_address;
            request_json["sender_port"] = client_port;

            json response_json;
            if (sendMessageToTracker(tracker_socket_fd, tracker_address, request_json, response_json) == false) {
                cout << "Operation Failed...\n";
                continue;
            }

            string response_string = response_json.dump();

            if (response_json["status"] == "success") {
                // communicate with other clients and get the file chunks
                vector<string> chunks_in_order = response_json["file_chunks"].get<vector<string>>();

                unordered_map<string, vector<pair<string, string>>> chunk_to_client = response_json["chunk_metadata"].get<unordered_map<string, vector<pair<string, string>>>>();

                string file_path = response_json["file_path"];
                int num_chunks = chunks_in_order.size();

                vector<pthread_t> tids;
                vector<void*> results;

                for (int i = 0; i < num_chunks; i++) {
                    string chunk = chunks_in_order[i];
                    int num_clients = chunk_to_client[chunk].size();
                    int client_id = i % num_clients;
                    pair<string, string> client_add = chunk_to_client[chunk][client_id];

                    DownloadChunkArgs* args = new DownloadChunkArgs{i, file_path, client_add.second, client_add.first};
                    pthread_t tid;
                    pthread_create(&tid, NULL, downloadChunkThread, args);
                    tids.push_back(tid);
                }

                string new_file_path = destination_path + "/" + file_name;
                int new_file_fd = open(new_file_path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
                if (new_file_fd == -1) {
                    cout << "Error creating the new file\n";
                    continue;
                }

                for (pthread_t tid : tids) {
                    void* ret_val;
                    pthread_join(tid, &ret_val);
                    string* result_str_ptr = static_cast<string*>(ret_val);
                    
                    json result_json = json::parse(*result_str_ptr);
                    int chunk_num = result_json["chunk_number"];
                    string encoded = result_json["data"];
                    string chunk_data = base64_decode(encoded);
                    string sender_port = result_json["port"];

                    cout << "Received chunk number " << chunk_num << " from client at port = " << sender_port << "\n";

                    off_t offset = chunk_num*CHUNK_SIZE;
                    if (lseek(new_file_fd, offset, SEEK_SET) == (off_t)-1) {
                        cout << "Lseek error\n";
                        close(new_file_fd);
                        return 1;
                    }

                    ssize_t bytes_written = write(new_file_fd, chunk_data.data(), chunk_data.size());
                    if (bytes_written < 0) {
                        cout << "Could not write to the new file\n";
                        close(new_file_fd);
                        return 1;
                    }

                    delete result_str_ptr;
                }

                close(new_file_fd);

                // inform tracker again (send file chunks sha and total file sha that you obtained after recreating the file)
                json new_request_json;
                new_request_json["command"] = "download_confirm";
                new_request_json["sender_ip"] = client_ip_address;
                new_request_json["sender_port"] = client_port;
                new_request_json["command_args"]["file_name"] = file_name;
                new_request_json["command_args"]["group_id"] = group_id;

                vector<string> piecewise_sha;
                string fileSha = "";
    
                struct stat fileInfo;
                stat(new_file_path.c_str(), &fileInfo);
                int fileSize = fileInfo.st_size;
                int numChunks = (fileSize+CHUNK_SIZE-1)/CHUNK_SIZE;
    
                new_file_fd = open(new_file_path.c_str(), O_RDONLY);
                if (new_file_fd < 0)
                {
                    cout << "Error opening file: " << new_file_path << "...\n";
                    continue;
                }
    
                for (int i=0; i<numChunks; i++)
                {
                    int currentChunkSize = min(CHUNK_SIZE, fileSize);
                    char *fileChunk = new char[currentChunkSize];
    
                    ssize_t chunkRead = read(new_file_fd, fileChunk, currentChunkSize);
                    if (chunkRead < 0)
                    {
                        cout << "Error reading file: " << new_file_path << "...\n";
                        delete[] fileChunk;
                        break;
                    }
    
                    unsigned char chunkShaTemp[SHA_DIGEST_LENGTH];
                    SHA1(reinterpret_cast<const unsigned char *>(fileChunk), chunkRead, chunkShaTemp);
                    string chunkSha = sha1ToHexString(chunkShaTemp);
                    
                    piecewise_sha.push_back(chunkSha);
                    fileSha += chunkSha;
    
                    delete[] fileChunk;
                    fileSize -= currentChunkSize;
                }
                close(new_file_fd);
    
                new_request_json["piecewise_sha"] = piecewise_sha;
                new_request_json["fileSha"] = fileSha;
    
                json new_response_json;
                if (sendMessageToTracker(tracker_socket_fd, tracker_address, new_request_json, new_response_json) == false) {
                    cout << "Operation Failed...\n";
                    continue;
                }

                cout << new_response_json["message"] << "\n\n";
            }

            continue;
        }
        else if (tokens[0] == "show_downloads") {
            if (tokens.size() != 1) {
                cout << "Enter correct command with arguments\n\n";
                continue;
            }

            json response_json;
            request_json["sender_ip"] = client_ip_address;
            request_json["sender_port"] = client_port;

            if (sendMessageToTracker(tracker_socket_fd, tracker_address, request_json, response_json) == false) {
                cout << "Operation Failed...\n";
                continue;
            }

            if (response_json["status"] == "success") {
                cout << "List of downloaded files\n";
                for (auto& [group_id, file_list] : response_json["downloaded_files"].items()) {
                    if (file_list.is_array()) {
                        for (auto& file_name : file_list) {
                            cout << "[D] " << group_id << " " << file_name.get<string>() << "\n";
                        }
                    }
                }
            }
            else {
                cout << response_json["message"] << "\n\n";
            }

            continue;
        }
        else if (tokens[0] == "stop_share") {
            if (tokens.size() != 3) {
                cout << "Enter correct command with arguments\n\n";
                continue;
            }

            string group_id = tokens[1];
            string file_name = tokens[2];
            request_json["command_args"]["file_name"] = file_name;
            request_json["command_args"]["group_id"] = group_id;
        }
        else if (tokens[0] == "logout") {
            if (tokens.size() != 1) {
                cout << "Enter correct command with arguments\n\n";
                continue;
            }
        }
        else {
            cout << "Error: Please enter correct command\n";
            continue;
        }

        request_json["sender_ip"] = client_ip_address;
        request_json["sender_port"] = client_port;

        json response_json;
        if (sendMessageToTracker(tracker_socket_fd, tracker_address, request_json, response_json) == false) {
            cout << "Operation Failed...\n";
            continue;
        }
        
        cout << response_json["message"] << "\n\n";
    }
    
    return 0;
}