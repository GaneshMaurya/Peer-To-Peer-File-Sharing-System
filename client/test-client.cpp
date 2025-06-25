#include <bits/stdc++.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <openssl/sha.h>
#include <sys/stat.h>
#include "../include/json.hpp"
using json = nlohmann::json;
using namespace std;

int BUFFER_SIZE = 1024;
int CHUNK_SIZE = 5;
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

// Send a large message with length prefix
bool sendLargeMessage(int sockfd, const std::string& data) {
    uint64_t len = data.size();
    uint64_t len_net = htobe64(len); // convert to network byte order
    // Send length
    if (send(sockfd, &len_net, sizeof(len_net), 0) != sizeof(len_net)) return false;
    // Send data
    size_t total_sent = 0;
    while (total_sent < len) {
        ssize_t sent = send(sockfd, data.data() + total_sent, len - total_sent, 0);
        if (sent <= 0) return false;
        total_sent += sent;
    }
    return true;
}

// Receive a large message with length prefix
bool recvLargeMessage(int sockfd, std::string& data) {
    uint64_t len_net;
    size_t received = 0;
    // Read the 8-byte length
    while (received < sizeof(len_net)) {
        ssize_t r = recv(sockfd, ((char*)&len_net) + received, sizeof(len_net) - received, 0);
        if (r <= 0) return false;
        received += r;
    }
    uint64_t len = be64toh(len_net);
    data.resize(len);
    size_t total_received = 0;
    while (total_received < len) {
        ssize_t r = recv(sockfd, &data[total_received], len - total_received, 0);
        if (r <= 0) return false;
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

void *handleListen(void *args) {
    ThreadArgs *threadArgs = (ThreadArgs *)args;
    int client_socket_fd = threadArgs->socketFd;
    sockaddr_in client_adddress = threadArgs->address;
    socklen_t client_addrlen = threadArgs->addrlen;
    string client_port = threadArgs->port;

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

        if (input.find("create_user") != string::npos) {
            string user_id = tokens[1];
            string passwd = tokens[2];
            request_json["command_args"]["user_id"] = user_id;
            request_json["command_args"]["passwd"] = passwd;
        }
        else if (input.find("login") != string::npos) {
            string user_id = tokens[1];
            string passwd = tokens[2];
            request_json["command_args"]["user_id"] = user_id;
            request_json["command_args"]["passwd"] = passwd;
        }
        else if (input.find("create_group") != string::npos) {
            string group_id = tokens[1];
            request_json["command_args"]["group_id"] = group_id;
        }
        else if (input.find("join_group") != string::npos) {
            string group_id = tokens[1];
            request_json["command_args"]["group_id"] = group_id;
        }
        else if (input.find("leave_group") != string::npos) {
            string group_id = tokens[1];
            request_json["command_args"]["group_id"] = group_id;
        }
        else if (input.find("list_requests") != string::npos) {
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
        else if (input.find("accept_request") != string::npos) {
            string group_id = tokens[1];
            string user_id = tokens[2];
            request_json["command_args"]["group_id"] = group_id;
            request_json["command_args"]["user_id"] = user_id;
        }
        else if (input.find("list_groups") != string::npos) {
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
        else if (input.find("list_files") != string::npos) {
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
        else if (input.find("upload_file") != string::npos) {
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
        else if (input.find("logout") != string::npos) {

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