#include <bits/stdc++.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include "../include/json.hpp"
using json = nlohmann::json;
using namespace std;

int BUFFER_SIZE = 1024;
int opt = 1;
string tracker_log_file_path = "tracker.log";
int SYNC_THRESHOLD = 100;
int last_sequence_number = 0;
int global_log_count = 0;

vector<string> trackersList;
vector<string> trackersIPList;
vector<string> trackersPortList;
vector<int> tracker_broadcast_sockets;

pthread_mutex_t stop_execution_mutex = PTHREAD_MUTEX_INITIALIZER;
bool stopExecution = false;
int tracker_socket_fd = -1;
vector<int> client_sockets;
pthread_mutex_t client_sockets_mutex = PTHREAD_MUTEX_INITIALIZER;

struct ThreadArgs {
    int socketFd;
    sockaddr_in address;
    socklen_t addrlen;
};

struct User {
    string user_id;
    string passwd;
    // set of group ids the user is member of
    unordered_set<string> member_groups;
    // set of group ids the user owns
    unordered_set<string> owned_groups;
    // Group id : file names
    unordered_map<string, unordered_set<string>> downloaded_files;
    string ip_address;
    string port;
    int socket_fd;
    bool is_online;

    User() {
        user_id = "";
        passwd = "";
        ip_address = "";
        port = "";
        socket_fd = -1;
        is_online = false;
    }
};

struct Group {
    string group_id;
    string owner;
    // set of user_ids
    unordered_set<string> members;
    // file names
    unordered_set<string> files;
    // file name : Users
    unordered_map<string, unordered_set<User*>> file_owners;
    // file name : chunk sha : set of user who have this chunk
    unordered_map<string, unordered_map<string, unordered_set<User*>>> chunk_metadata;

    Group() {
        group_id = "";
        owner = "";
    }
};

// user_id : passwd
unordered_map<string, string> user_creds;
// user_id : User object
unordered_map<string, User*> user_details;
// group_id : Group object
unordered_map<string, Group*> group_details;
// group_id : Set of pending user ids
unordered_map<string, unordered_set<string>> pending_requests;
// client port : User object
unordered_map<string, User*> portToUser;
// file name : vector of sha
unordered_map<string, vector<string>> file_details;
// file name : file path
unordered_map<string, string> file_name_to_path;

pthread_mutex_t users_mutex;
pthread_mutex_t groups_mutex;
pthread_mutex_t files_mutex;

char* tracker_ip_address;
char* tracker_port;
int tracker_no;

bool sendLargeMessage(int sockfd, const string& data) {
    uint64_t len = data.size();
    uint64_t len_net = htobe64(len);
    if (send(sockfd, &len_net, sizeof(len_net), 0) != sizeof(len_net)) {
        return false;
    }

    size_t total_sent = 0;
    while (total_sent < len) {
        ssize_t sent = send(sockfd, data.data() + total_sent, len - total_sent, 0);
        if (sent <= 0) return false;
        total_sent += sent;
    }
    return true;
}

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
        if (r <= 0) return false;
        total_received += r;
    }

    return true;
}

void create_log_entry(json request_json, json response_json) {
    string request_content = request_json.dump();
    string response_content = response_json.dump();
    
    string command = request_json["command"];
    string log_entry = command + "##" + request_content + "##" + response_content + "\n";

    int log_file_fd = open(tracker_log_file_path.c_str(), O_RDWR | O_APPEND | O_CREAT, 0644);
    if (log_file_fd < 0) {
        return;
    }

    ssize_t bytes_written = write(log_file_fd, log_entry.c_str(), log_entry.size());
    close(log_file_fd);
}

void apply_log_entry(string entry_line);

void initializeFromLog(int start) {
    int log_file_fd = open(tracker_log_file_path.c_str(), O_RDONLY);
    if (log_file_fd < 0) {
        int create_fd = open(tracker_log_file_path.c_str(), O_WRONLY | O_CREAT, 0644);
        if (create_fd >= 0) {
            close(create_fd);
        }
        return;
    }

    char* buffer = new char[BUFFER_SIZE];
    string buffer_string = "";
    ssize_t bytes_read;
    while ((bytes_read = read(log_file_fd, buffer, BUFFER_SIZE)) > 0) {
        buffer_string.append(buffer, bytes_read);
    }
    delete[] buffer;

    istringstream iss(buffer_string);
    string line;
    while (getline(iss, line)) {
        line.erase(find_if(line.rbegin(), line.rend(),
            [](unsigned char ch) { return isprint(ch); }).base(), line.end());
        
        if (line.empty()) {
            continue;
        }

        if (last_sequence_number >= start) {
            apply_log_entry(line);
            last_sequence_number++;
        }
    }

    close(log_file_fd);
}

bool isLoggedIn(User* user) {
    return user->is_online;
}

json handleCreateUser(json request_json) {
    json response_json;
    string user_id = request_json["command_args"]["user_id"];

    pthread_mutex_lock(&users_mutex);

    if (user_creds.find(user_id) != user_creds.end()) {
        pthread_mutex_unlock(&users_mutex);
        response_json["status"] = "failure";
        response_json["message"] = "User with user id = " + user_id + " already exists";
        return response_json;
    }
    
    string passwd = request_json["command_args"]["passwd"];
    user_creds[user_id] = passwd;
    
    User* new_user = new User();
    new_user->user_id = user_id;
    new_user->passwd = passwd;
    new_user->is_online = false;
    user_details[user_id] = new_user;

    pthread_mutex_unlock(&users_mutex);
    
    response_json["status"] = "success";
    response_json["message"] = "User created successfully";

    if (request_json["apply_from_log"] == false) {
        create_log_entry(request_json, response_json);
    }
    return response_json;
}

json handleUserLogin(json request_json) {
    json response_json;
    string user_id = request_json["command_args"]["user_id"];
    string passwd = request_json["command_args"]["passwd"];
    int socket_fd = request_json["socket_fd"];

    pthread_mutex_lock(&users_mutex);
    pthread_mutex_lock(&groups_mutex);

    string port = request_json["sender_port"];
    
    if (user_creds.find(user_id) == user_creds.end()) {
        pthread_mutex_unlock(&users_mutex);
        pthread_mutex_unlock(&groups_mutex);
        response_json["status"] = "failure";
        response_json["message"] = "User with user id = " + user_id + " does not exist";
        return response_json;
    }
    
    if (user_creds[user_id] != passwd) {
        pthread_mutex_unlock(&users_mutex);
        pthread_mutex_unlock(&groups_mutex);
        response_json["status"] = "failure";
        response_json["message"] = "Password incorrect";
        return response_json;
    }

    User* current_user = user_details[user_id];

    if (user_details[user_id]->is_online == true && user_details[user_id]->port != request_json["sender_port"]) {
        pthread_mutex_unlock(&users_mutex);
        pthread_mutex_unlock(&groups_mutex);
        response_json["status"] = "failure";
        response_json["message"] = "User logged into another session";
        return response_json;
    }

    if (user_details[user_id]->is_online == true) {
        pthread_mutex_unlock(&users_mutex);
        pthread_mutex_unlock(&groups_mutex);
        response_json["status"] = "failure";
        response_json["message"] = "User already logged in";
        return response_json;
    }

    current_user->is_online = true;
    current_user->ip_address = request_json["sender_ip"];
    current_user->port = request_json["sender_port"];
    current_user->socket_fd = socket_fd;
    current_user->user_id = user_id;

    portToUser[current_user->port] = current_user;

    pthread_mutex_unlock(&users_mutex);
    pthread_mutex_unlock(&groups_mutex);

    response_json["status"] = "success";
    response_json["message"] = "User logged in successfully";

    if (request_json["apply_from_log"] == false) {
        create_log_entry(request_json, response_json);
    }
    return response_json;
}

json handleCreateGroup(json request_json) {
    json response_json;
    string group_id = request_json["command_args"]["group_id"];

    pthread_mutex_lock(&groups_mutex);
    pthread_mutex_lock(&users_mutex);

    string port = request_json["sender_port"];
    if (portToUser.find(port) == portToUser.end()) {
        pthread_mutex_unlock(&groups_mutex);
        pthread_mutex_unlock(&users_mutex);
        response_json["status"] = "failure";
        response_json["message"] = "User not logged in";
        return response_json;
    }
    User* current_user = portToUser[port];
    
    if (!isLoggedIn(current_user)) {
        pthread_mutex_lock(&groups_mutex);
        pthread_mutex_lock(&users_mutex);
        response_json["status"] = "failure";
        response_json["message"] = "User not logged in";
        return response_json;
    }

    if (group_details.find(group_id) != group_details.end()) {
        pthread_mutex_lock(&groups_mutex);
        pthread_mutex_lock(&users_mutex);
        response_json["status"] = "failure";
        response_json["message"] = "Group with group id = " + group_id + " already exists";
        return response_json;
    }
    
    Group* new_group = new Group();
    new_group->group_id = group_id;
    new_group->owner = current_user->user_id;
    group_details[group_id] = new_group;
    
    current_user->owned_groups.insert(group_id);
    portToUser[port] = current_user;

    pthread_mutex_unlock(&groups_mutex);
    pthread_mutex_unlock(&users_mutex);

    response_json["status"] = "success";
    response_json["message"] = "Group created successfully";

    if (request_json["apply_from_log"] == false) {
        create_log_entry(request_json, response_json);
    }
    return response_json;
}

json handleJoinGroup(json request_json) {
    json response_json;
    string group_id = request_json["command_args"]["group_id"];

    pthread_mutex_lock(&groups_mutex);
    pthread_mutex_lock(&users_mutex);

    string port = request_json["sender_port"];
    if (portToUser.find(port) == portToUser.end()) {
        pthread_mutex_unlock(&groups_mutex);
        pthread_mutex_unlock(&users_mutex);
        response_json["status"] = "failure";
        response_json["message"] = "User not logged in";
        return response_json;
    }
    User* current_user = portToUser[port];

    if (!isLoggedIn(current_user)) {
        pthread_mutex_unlock(&groups_mutex);
        pthread_mutex_unlock(&users_mutex);
        response_json["status"] = "failure";
        response_json["message"] = "User not logged in";
        return response_json;
    }

    if (group_details.find(group_id) == group_details.end()) {
        pthread_mutex_unlock(&groups_mutex);
        pthread_mutex_unlock(&users_mutex);
        response_json["status"] = "failure";
        response_json["message"] = "Group with group id = " + group_id + " does not exist";
        return response_json;
    }

    if (group_details[group_id]->owner == current_user->user_id || group_details[group_id]->members.find(current_user->user_id) != group_details[group_id]->members.end()) {
        pthread_mutex_unlock(&groups_mutex);
        pthread_mutex_unlock(&users_mutex);
        response_json["status"] = "failure";
        response_json["message"] = "User with user id = " + current_user->user_id + " is already part of group id = " + group_id;
        return response_json;
    }

    pending_requests[group_id].insert(current_user->user_id);

    pthread_mutex_unlock(&groups_mutex);
    pthread_mutex_unlock(&users_mutex);

    response_json["status"] = "success";
    response_json["message"] = "Join request created successfully";

    if (request_json["apply_from_log"] == false) {
        create_log_entry(request_json, response_json);
    }
    return response_json;
}

json handleLeaveGroup(json request_json) {
    json response_json;
    string group_id = request_json["command_args"]["group_id"];

    pthread_mutex_lock(&groups_mutex);
    pthread_mutex_lock(&users_mutex);

    string port = request_json["sender_port"];
    if (portToUser.find(port) == portToUser.end()) {
        pthread_mutex_unlock(&groups_mutex);
        pthread_mutex_unlock(&users_mutex);
        response_json["status"] = "failure";
        response_json["message"] = "User not logged in";
        return response_json;
    }
    User* current_user = portToUser[port];

    if (!isLoggedIn(current_user)) {
        pthread_mutex_unlock(&groups_mutex);
        pthread_mutex_unlock(&users_mutex);
        response_json["status"] = "failure";
        response_json["message"] = "User not logged in";
        return response_json;
    }

    if (group_details.find(group_id) == group_details.end()) {
        pthread_mutex_unlock(&groups_mutex);
        pthread_mutex_unlock(&users_mutex);
        response_json["status"] = "failure";
        response_json["message"] = "Group with group id = " + group_id + " does not exist";
        return response_json;
    }

    if (group_details[group_id]->owner != current_user->user_id && group_details[group_id]->members.find(current_user->user_id) == group_details[group_id]->members.end()) {
        pthread_mutex_unlock(&groups_mutex);
        pthread_mutex_unlock(&users_mutex);
        response_json["status"] = "failure";
        response_json["message"] = "User is not part of group = " + group_id;
        return response_json;
    }

    if (group_details[group_id]->owner == current_user->user_id) {
        if (group_details[group_id]->members.size() == 0) {
            current_user->owned_groups.erase(group_id);
            group_details.erase(group_id);
            
            pthread_mutex_unlock(&groups_mutex);
            pthread_mutex_unlock(&users_mutex);
            
            response_json["status"] = "success";
            response_json["message"] = "Owner left the group hence deleting the group = " + group_id;
            return response_json;
        }
        
        Group* group = group_details[group_id];
        string new_owner_id = *group->members.begin();
        group->members.erase(new_owner_id);
        group->owner = new_owner_id;
        
        for (auto file_it = group->chunk_metadata.begin(); file_it != group->chunk_metadata.end(); ) {
            auto& chunk_map = file_it->second;
            for (auto chunk_it = chunk_map.begin(); chunk_it != chunk_map.end(); ) {
                chunk_it->second.erase(current_user);
                if (chunk_it->second.empty()) {
                    chunk_it = chunk_map.erase(chunk_it);
                } 
                else {
                    ++chunk_it;
                }
            }

            if (chunk_map.empty()) {
                file_it = group->chunk_metadata.erase(file_it);
            } 
            else {
                ++file_it;
            }
        }

        group_details[group_id] = group;

        User* new_owner = user_details[new_owner_id];
        new_owner->member_groups.erase(group_id);
        new_owner->owned_groups.insert(group_id);
        user_details[new_owner_id] = new_owner;
    }

    pthread_mutex_unlock(&groups_mutex);
    pthread_mutex_unlock(&users_mutex);

    response_json["status"] = "success";
    response_json["message"] = "User left the group successfully";

    if (request_json["apply_from_log"] == false) {
        create_log_entry(request_json, response_json);
    }
    return response_json;
}

json handleListRequests(json request_json) {
    json response_json;
    string group_id = request_json["command_args"]["group_id"];

    pthread_mutex_lock(&groups_mutex);
    pthread_mutex_lock(&users_mutex);

    string port = request_json["sender_port"];
    if (portToUser.find(port) == portToUser.end()) {
        pthread_mutex_unlock(&groups_mutex);
        pthread_mutex_unlock(&users_mutex);
        response_json["status"] = "failure";
        response_json["message"] = "User not logged in";
        return response_json;
    }
    User* current_user = portToUser[port];

    if (!isLoggedIn(current_user)) {
        pthread_mutex_unlock(&groups_mutex);
        pthread_mutex_unlock(&users_mutex);
        response_json["status"] = "failure";
        response_json["message"] = "User not logged in";
        return response_json;
    }

    if (group_details.find(group_id) == group_details.end()) {
        pthread_mutex_unlock(&groups_mutex);
        pthread_mutex_unlock(&users_mutex);
        response_json["status"] = "failure";
        response_json["message"] = "Group with group id = " + group_id + " does not exist";
        return response_json;
    }

    if (group_details[group_id]->owner != current_user->user_id) {
        pthread_mutex_unlock(&groups_mutex);
        pthread_mutex_unlock(&users_mutex);
        response_json["status"] = "failure";
        response_json["message"] = "User is not the owner of group = " + group_id;
        return response_json;
    }

    response_json["pending_list"] = pending_requests[group_id];

    pthread_mutex_unlock(&groups_mutex);
    pthread_mutex_unlock(&users_mutex);

    response_json["status"] = "success";
    response_json["message"] = "List displayed successfully";

    if (request_json["apply_from_log"] == false) {
        create_log_entry(request_json, response_json);
    }
    return response_json;
}

json handleAcceptRequest(json request_json) {
    json response_json;
    string group_id = request_json["command_args"]["group_id"];
    string user_id = request_json["command_args"]["user_id"];

    pthread_mutex_lock(&groups_mutex);
    pthread_mutex_lock(&users_mutex);

    string port = request_json["sender_port"];
    if (portToUser.find(port) == portToUser.end()) {
        pthread_mutex_unlock(&groups_mutex);
        pthread_mutex_unlock(&users_mutex);
        response_json["status"] = "failure";
        response_json["message"] = "User not logged in";
        return response_json;
    }
    User* current_user = portToUser[port];

    if (!isLoggedIn(current_user)) {
        pthread_mutex_unlock(&groups_mutex);
        pthread_mutex_unlock(&users_mutex);
        response_json["status"] = "failure";
        response_json["message"] = "User not logged in";
        return response_json;
    }

    if (group_details.find(group_id) == group_details.end()) {
        pthread_mutex_unlock(&groups_mutex);
        pthread_mutex_unlock(&users_mutex);
        response_json["status"] = "failure";
        response_json["message"] = "Group with group id = " + group_id + " does not exist";
        return response_json;
    }

    if (user_details.find(user_id) == user_details.end()) {
        pthread_mutex_unlock(&groups_mutex);
        pthread_mutex_unlock(&users_mutex);
        response_json["status"] = "failure";
        response_json["message"] = "User with user id = " + user_id + " does not exist";
        return response_json;
    }

    if (group_details[group_id]->owner != current_user->user_id) {
        pthread_mutex_unlock(&groups_mutex);
        pthread_mutex_unlock(&users_mutex);
        response_json["status"] = "failure";
        response_json["message"] = "User " + current_user->user_id + " is not the owner of group = " + group_id;
        return response_json;
    }

    if (pending_requests[group_id].find(user_id) == pending_requests[group_id].end()) {
        pthread_mutex_unlock(&groups_mutex);
        pthread_mutex_unlock(&users_mutex);
        response_json["status"] = "failure";
        response_json["message"] = "No pending request from user id = " + user_id + " into group id = " + group_id;
        return response_json;
    }

    pending_requests[group_id].erase(user_id);
    Group* group = group_details[group_id];
    group->members.insert(user_id);
    group_details[group_id] = group;
    
    User* user = user_details[user_id];
    user->member_groups.insert(group_id);
    user_details[user_id] = user;

    pthread_mutex_unlock(&groups_mutex);
    pthread_mutex_unlock(&users_mutex);

    response_json["status"] = "success";
    response_json["message"] = "User with user id = " + user_id + " accepted into group id = " + group_id + " successfully";

    if (request_json["apply_from_log"] == false) {
        create_log_entry(request_json, response_json);
    }
    return response_json;
}

json handleListGroups(json request_json) {
    json response_json;

    pthread_mutex_lock(&groups_mutex);
    pthread_mutex_lock(&users_mutex);

    string port = request_json["sender_port"];
    if (portToUser.find(port) == portToUser.end()) {
        pthread_mutex_unlock(&groups_mutex);
        pthread_mutex_unlock(&users_mutex);
        response_json["status"] = "failure";
        response_json["message"] = "User not logged in";
        return response_json;
    }
    User* current_user = portToUser[port];

    if (!isLoggedIn(current_user)) {
        pthread_mutex_unlock(&groups_mutex);
        pthread_mutex_unlock(&users_mutex);
        response_json["status"] = "failure";
        response_json["message"] = "User not logged in";
        return response_json;
    }

    vector<string> groupsList;
    for (auto it: group_details) {
        groupsList.push_back(it.first);
    }
    response_json["groups_list"] = groupsList;

    pthread_mutex_unlock(&groups_mutex);
    pthread_mutex_unlock(&users_mutex);

    response_json["status"] = "success";
    response_json["message"] = "Groups listed successfully";

    if (request_json["apply_from_log"] == false) {
        create_log_entry(request_json, response_json);
    }
    return response_json;
}

string getFileName(const string& filePath) {
    size_t pos = filePath.find_last_of('/');
    if (pos == string::npos) {
        return filePath;
    }

    return filePath.substr(pos + 1);
}

json handleListFiles(json request_json) {
    json response_json;
    string group_id = request_json["command_args"]["group_id"];

    pthread_mutex_lock(&groups_mutex);
    pthread_mutex_lock(&users_mutex);
    pthread_mutex_lock(&files_mutex);

    string port = request_json["sender_port"];
    if (portToUser.find(port) == portToUser.end()) {
        pthread_mutex_unlock(&groups_mutex);
        pthread_mutex_unlock(&users_mutex);
        pthread_mutex_unlock(&files_mutex);
        response_json["status"] = "failure";
        response_json["message"] = "User not logged in";
        return response_json;
    }
    User* current_user = portToUser[port];

    if (!isLoggedIn(current_user)) {
        pthread_mutex_unlock(&groups_mutex);
        pthread_mutex_unlock(&users_mutex);
        pthread_mutex_unlock(&files_mutex);
        response_json["status"] = "failure";
        response_json["message"] = "User not logged in";
        return response_json;
    }
    
    if (group_details.find(group_id) == group_details.end()) {
        pthread_mutex_unlock(&groups_mutex);
        pthread_mutex_unlock(&users_mutex);
        pthread_mutex_unlock(&files_mutex);
        response_json["status"] = "failure";
        response_json["message"] = "Group with group id = " + group_id + " does not exist";
        return response_json;
    }
    
    if (group_details[group_id]->owner != current_user->user_id && group_details[group_id]->members.find(current_user->user_id) == group_details[group_id]->members.end()) {
        pthread_mutex_unlock(&groups_mutex);
        pthread_mutex_unlock(&users_mutex);
        pthread_mutex_unlock(&files_mutex);
        response_json["status"] = "failure";
        response_json["message"] = "User is not part of group = " + group_id;
        return response_json;
    }

    response_json["files_list"] = group_details[group_id]->files;

    pthread_mutex_unlock(&groups_mutex);
    pthread_mutex_unlock(&users_mutex);
    pthread_mutex_unlock(&files_mutex);

    response_json["status"] = "success";
    response_json["message"] = "Files listed successfully";

    if (request_json["apply_from_log"] == false && request_json["command"] != "broadcast_sync") {
        create_log_entry(request_json, response_json);
    }
    return response_json;
}

json handleUploadFile(json request_json) {
    json response_json;
    string file_path = request_json["command_args"]["file_path"];
    string group_id = request_json["command_args"]["group_id"];

    pthread_mutex_lock(&groups_mutex);
    pthread_mutex_lock(&users_mutex);
    pthread_mutex_lock(&files_mutex);

    string port = request_json["sender_port"];
    if (portToUser.find(port) == portToUser.end()) {
        pthread_mutex_unlock(&groups_mutex);
        pthread_mutex_unlock(&users_mutex);
        pthread_mutex_unlock(&files_mutex);
        response_json["status"] = "failure";
        response_json["message"] = "User not logged in";
        return response_json;
    }
    User* current_user = portToUser[port];

    if (!isLoggedIn(current_user)) {
        pthread_mutex_unlock(&groups_mutex);
        pthread_mutex_unlock(&users_mutex);
        pthread_mutex_unlock(&files_mutex);
        response_json["status"] = "failure";
        response_json["message"] = "User not logged in";
        return response_json;
    }
    
    if (group_details.find(group_id) == group_details.end()) {
        pthread_mutex_unlock(&groups_mutex);
        pthread_mutex_unlock(&users_mutex);
        pthread_mutex_unlock(&files_mutex);
        response_json["status"] = "failure";
        response_json["message"] = "Group with group id = " + group_id + " does not exist";
        return response_json;
    }
    
    if (group_details[group_id]->owner != current_user->user_id && group_details[group_id]->members.find(current_user->user_id) == group_details[group_id]->members.end()) {
        pthread_mutex_unlock(&groups_mutex);
        pthread_mutex_unlock(&users_mutex);
        pthread_mutex_unlock(&files_mutex);
        response_json["status"] = "failure";
        response_json["message"] = "User is not part of group = " + group_id;
        return response_json;
    }

    string file_name = getFileName(file_path);
    if (group_details[group_id]->files.find(file_name) != group_details[group_id]->files.end()) {
        pthread_mutex_unlock(&groups_mutex);
        pthread_mutex_unlock(&users_mutex);
        pthread_mutex_unlock(&files_mutex);
        response_json["status"] = "failure";
        response_json["message"] = "File " + file_name + " is already part of group = " + group_id;
        return response_json;
    }

    if (!request_json.contains("piecewise_sha") || !request_json["piecewise_sha"].is_array()) {
        pthread_mutex_unlock(&groups_mutex);
        pthread_mutex_unlock(&users_mutex);
        pthread_mutex_unlock(&files_mutex);
        response_json["status"] = "failure";
        response_json["message"] = "Piecewise Sha does not exist";
        return response_json;
    }
    
    file_name_to_path[file_name] = file_path;
    Group* group = group_details[group_id];
    group->files.insert(file_name);
    group->file_owners[file_name].insert(current_user);
    
    vector<string> chunkwise_sha = request_json["piecewise_sha"].get<vector<string>>();
    file_details[file_name] = chunkwise_sha;
    
    for (auto chunk_sha: chunkwise_sha) {
        group_details[group_id]->chunk_metadata[file_name][chunk_sha].insert(current_user);
    }
    group_details[group_id] = group;
    
    pthread_mutex_unlock(&groups_mutex);
    pthread_mutex_unlock(&users_mutex);
    pthread_mutex_unlock(&files_mutex);

    response_json["status"] = "success";
    response_json["message"] = "File uploaded successfully";

    if (request_json["apply_from_log"] == false && request_json["command"] != "broadcast_sync") {
        create_log_entry(request_json, response_json);
    }
    return response_json;
}

json handleDownload(json request_json) {
    json response_json;
    string group_id = request_json["command_args"]["group_id"];
    string file_name = request_json["command_args"]["file_name"];
    string destination_path = request_json["command_args"]["destination_path"];

    pthread_mutex_lock(&groups_mutex);
    pthread_mutex_lock(&users_mutex);
    pthread_mutex_lock(&files_mutex);

    string port = request_json["sender_port"];
    if (portToUser.find(port) == portToUser.end()) {
        pthread_mutex_unlock(&groups_mutex);
        pthread_mutex_unlock(&users_mutex);
        pthread_mutex_unlock(&files_mutex);
        response_json["status"] = "failure";
        response_json["message"] = "User not logged in";
        return response_json;
    }
    User* current_user = portToUser[port];

    if (!isLoggedIn(current_user)) {
        pthread_mutex_unlock(&groups_mutex);
        pthread_mutex_unlock(&users_mutex);
        pthread_mutex_unlock(&files_mutex);
        response_json["status"] = "failure";
        response_json["message"] = "User not logged in";
        return response_json;
    }
    
    if (group_details.find(group_id) == group_details.end()) {
        pthread_mutex_unlock(&groups_mutex);
        pthread_mutex_unlock(&users_mutex);
        pthread_mutex_unlock(&files_mutex);
        response_json["status"] = "failure";
        response_json["message"] = "Group with group id = " + group_id + " does not exist";
        return response_json;
    }
    
    if (group_details[group_id]->owner != current_user->user_id && group_details[group_id]->members.find(current_user->user_id) == group_details[group_id]->members.end()) {
        pthread_mutex_unlock(&groups_mutex);
        pthread_mutex_unlock(&users_mutex);
        pthread_mutex_unlock(&files_mutex);
        response_json["status"] = "failure";
        response_json["message"] = "User is not part of group = " + group_id;
        return response_json;
    }

    if (file_details.find(file_name) == file_details.end()) {
        pthread_mutex_unlock(&groups_mutex);
        pthread_mutex_unlock(&users_mutex);
        pthread_mutex_unlock(&files_mutex);
        response_json["status"] = "failure";
        response_json["message"] = "File " + file_name + " does not exist";
        return response_json;
    }

    if (group_details[group_id]->chunk_metadata.find(file_name) == group_details[group_id]->chunk_metadata.end()) {
        pthread_mutex_unlock(&groups_mutex);
        pthread_mutex_unlock(&users_mutex);
        pthread_mutex_unlock(&files_mutex);
        response_json["status"] = "failure";
        response_json["message"] = "No chunk metadata for file " + file_name;
        return response_json;
    }

    response_json["file_path"] = file_name_to_path[file_name];
    response_json["file_chunks"] = file_details[file_name];

    unordered_map<string, unordered_set<User*>> &chunk_map = group_details[group_id]->chunk_metadata[file_name];

    unordered_map<string, vector<pair<string, string>>> chunk_to_client;

    for (const auto& [chunk_sha, user_set]: chunk_map) {
        for (auto user_ptr: user_set) {
            if (user_ptr->is_online) {
                chunk_to_client[chunk_sha].push_back({user_ptr->port, user_ptr->ip_address});
            }
        }
    }
    response_json["chunk_metadata"] = chunk_to_client;

    pthread_mutex_unlock(&groups_mutex);
    pthread_mutex_unlock(&users_mutex);
    pthread_mutex_unlock(&files_mutex);

    response_json["status"] = "success";
    response_json["message"] = "File metadata delivered successfully";

    if (request_json["apply_from_log"] == false && request_json["command"] != "broadcast_sync") {
        create_log_entry(request_json, response_json);
    }
    return response_json;
}

json handleDownloadConfirm(json request_json) {
    json response_json;
    string file_name = request_json["command_args"]["file_name"];
    string group_id = request_json["command_args"]["group_id"];

    pthread_mutex_lock(&groups_mutex);
    pthread_mutex_lock(&users_mutex);
    pthread_mutex_lock(&files_mutex);

    string port = request_json["sender_port"];
    if (portToUser.find(port) == portToUser.end()) {
        pthread_mutex_unlock(&groups_mutex);
        pthread_mutex_unlock(&users_mutex);
        pthread_mutex_unlock(&files_mutex);
        response_json["status"] = "failure";
        response_json["message"] = "User not logged in";
        return response_json;
    }
    User* current_user = portToUser[port];

    if (!isLoggedIn(current_user)) {
        pthread_mutex_unlock(&groups_mutex);
        pthread_mutex_unlock(&users_mutex);
        pthread_mutex_unlock(&files_mutex);
        response_json["status"] = "failure";
        response_json["message"] = "User not logged in";
        return response_json;
    }
    
    if (group_details.find(group_id) == group_details.end()) {
        pthread_mutex_unlock(&groups_mutex);
        pthread_mutex_unlock(&users_mutex);
        pthread_mutex_unlock(&files_mutex);
        response_json["status"] = "failure";
        response_json["message"] = "Group with group id = " + group_id + " does not exist";
        return response_json;
    }
    
    if (group_details[group_id]->owner != current_user->user_id && group_details[group_id]->members.find(current_user->user_id) == group_details[group_id]->members.end()) {
        pthread_mutex_unlock(&groups_mutex);
        pthread_mutex_unlock(&users_mutex);
        pthread_mutex_unlock(&files_mutex);
        response_json["status"] = "failure";
        response_json["message"] = "User is not part of group = " + group_id;
        return response_json;
    }

    if (!request_json.contains("piecewise_sha") || !request_json["piecewise_sha"].is_array()) {
        pthread_mutex_unlock(&groups_mutex);
        pthread_mutex_unlock(&users_mutex);
        pthread_mutex_unlock(&files_mutex);
        response_json["status"] = "failure";
        response_json["message"] = "Piecewise Sha does not exist";
        return response_json;
    }

    current_user->downloaded_files[group_id].insert(file_name);

    Group* group = group_details[group_id];
    group->files.insert(file_name);
    group->file_owners[file_name].insert(current_user);
    
    vector<string> chunkwise_sha = request_json["piecewise_sha"].get<vector<string>>();
    int n = chunkwise_sha.size();

    vector<string> original_chunkwise_sha = file_details[file_name];
    
    for (int i=0; i<n; i++) {
        string chunk_sha = chunkwise_sha[i];
        string original_chunk_sha = original_chunkwise_sha[i];
        if (chunk_sha == original_chunk_sha) {
            group_details[group_id]->chunk_metadata[file_name][chunk_sha].insert(current_user);
        }
    }
    group_details[group_id] = group;
    
    pthread_mutex_unlock(&groups_mutex);
    pthread_mutex_unlock(&users_mutex);
    pthread_mutex_unlock(&files_mutex);

    response_json["status"] = "success";
    response_json["message"] = "File downloaded successfully";

    if (request_json["apply_from_log"] == false && request_json["command"] != "broadcast_sync") {
        create_log_entry(request_json, response_json);
    }
    return response_json;
}

json handleShowDownloads(json request_json) {
    json response_json;

    pthread_mutex_lock(&users_mutex);

    string port = request_json["sender_port"];
    if (portToUser.find(port) == portToUser.end()) {
        pthread_mutex_unlock(&groups_mutex);
        pthread_mutex_unlock(&users_mutex);
        pthread_mutex_unlock(&files_mutex);
        response_json["status"] = "failure";
        response_json["message"] = "User not logged in";
        return response_json;
    }
    User* current_user = portToUser[port];

    if (!isLoggedIn(current_user)) {
        pthread_mutex_unlock(&users_mutex);
        response_json["status"] = "failure";
        response_json["message"] = "User not logged in";
        return response_json;
    }

    unordered_map<string, vector<string>> filtered_downloaded_files;
    for (const auto& [group_id, file_name_set] : current_user->downloaded_files) {
        if (!file_name_set.empty()) {
            filtered_downloaded_files[group_id] = vector<string>(file_name_set.begin(), file_name_set.end());
        }
    }
    response_json["downloaded_files"] = filtered_downloaded_files;

    pthread_mutex_unlock(&users_mutex);

    response_json["status"] = "success";
    response_json["message"] = "Downloaded files listed successfully";

    if (request_json["apply_from_log"] == false && request_json["command"] != "broadcast_sync") {
        create_log_entry(request_json, response_json);
    }
    return response_json;
}

json handleStopShare(json request_json) {
    json response_json;
    string file_name = request_json["command_args"]["file_name"];
    string group_id = request_json["command_args"]["group_id"];

    pthread_mutex_lock(&groups_mutex);
    pthread_mutex_lock(&users_mutex);
    pthread_mutex_lock(&files_mutex);

    string port = request_json["sender_port"];
    if (portToUser.find(port) == portToUser.end()) {
        pthread_mutex_unlock(&groups_mutex);
        pthread_mutex_unlock(&users_mutex);
        pthread_mutex_unlock(&files_mutex);
        response_json["status"] = "failure";
        response_json["message"] = "User not logged in";
        return response_json;
    }
    User* current_user = portToUser[port];

    if (!isLoggedIn(current_user)) {
        pthread_mutex_unlock(&groups_mutex);
        pthread_mutex_unlock(&users_mutex);
        pthread_mutex_unlock(&files_mutex);
        response_json["status"] = "failure";
        response_json["message"] = "User not logged in";
        return response_json;
    }
    
    if (group_details.find(group_id) == group_details.end()) {
        pthread_mutex_unlock(&groups_mutex);
        pthread_mutex_unlock(&users_mutex);
        pthread_mutex_unlock(&files_mutex);
        response_json["status"] = "failure";
        response_json["message"] = "Group with group id = " + group_id + " does not exist";
        return response_json;
    }
    
    if (group_details[group_id]->owner != current_user->user_id && group_details[group_id]->members.find(current_user->user_id) == group_details[group_id]->members.end()) {
        pthread_mutex_unlock(&groups_mutex);
        pthread_mutex_unlock(&users_mutex);
        pthread_mutex_unlock(&files_mutex);
        response_json["status"] = "failure";
        response_json["message"] = "User is not part of group = " + group_id;
        return response_json;
    }

    if (group_details[group_id]->files.find(file_name) == group_details[group_id]->files.end()) {
        pthread_mutex_unlock(&groups_mutex);
        pthread_mutex_unlock(&users_mutex);
        pthread_mutex_unlock(&files_mutex);
        response_json["status"] = "failure";
        response_json["message"] = "File " + file_name + " is not part of group = " + group_id;
        return response_json;
    }

    Group* group = group_details[group_id];
    group->files.erase(file_name);
    group->file_owners[file_name].erase(current_user);
    
    auto& chunk_map = group->chunk_metadata[file_name];
    for (auto chunk_it = chunk_map.begin(); chunk_it != chunk_map.end(); ) {
        chunk_it->second.erase(current_user);
        if (chunk_it->second.empty()) {
            chunk_it = chunk_map.erase(chunk_it);
        } 
        else {
            ++chunk_it;
        }
    }

    if (chunk_map.empty()) {
        group->chunk_metadata.erase(file_name);
        group->files.erase(file_name);
        file_details.erase(file_name);
    }

    pthread_mutex_unlock(&groups_mutex);
    pthread_mutex_unlock(&users_mutex);
    pthread_mutex_unlock(&files_mutex);

    response_json["status"] = "success";
    response_json["message"] = "File removed successfully";

    if (request_json["apply_from_log"] == false && request_json["command"] != "broadcast_sync") {
        create_log_entry(request_json, response_json);
    }
    return response_json;
}

json handleLogout(json request_json) {
    json response_json;

    pthread_mutex_lock(&groups_mutex);
    pthread_mutex_lock(&users_mutex);

    string port = request_json["sender_port"];
    if (portToUser.find(port) == portToUser.end()) {
        pthread_mutex_unlock(&groups_mutex);
        pthread_mutex_unlock(&users_mutex);
        response_json["status"] = "failure";
        response_json["message"] = "User not logged in";
        return response_json;
    }
    User* current_user = portToUser[port];

    if (!isLoggedIn(current_user)) {
        pthread_mutex_unlock(&groups_mutex);
        pthread_mutex_unlock(&users_mutex);
        response_json["status"] = "failure";
        response_json["message"] = "User not logged in";
        return response_json;
    }

    current_user->ip_address = "";
    current_user->port = "";
    current_user->user_id = "";
    current_user->passwd = "";
    current_user->socket_fd = -1;
    current_user->is_online = false;

    portToUser.erase(port);

    pthread_mutex_unlock(&groups_mutex);
    pthread_mutex_unlock(&users_mutex);

    response_json["status"] = "success";
    response_json["message"] = "User logged out successfully";

    if (request_json["apply_from_log"] == false && request_json["command"] != "broadcast_sync") {
        create_log_entry(request_json, response_json);
    }
    return response_json;
}

void apply_log_entry(string entry_line) {
    size_t first = entry_line.find("##");
    size_t second = entry_line.find("##", first + 2);

    if (first == string::npos || second == string::npos) {
        return;
    }

    string command = entry_line.substr(0, first);
    string request_string = entry_line.substr(first + 2, second - (first + 2));
    string response_string = entry_line.substr(second + 2);

    json request_json = json::parse(request_string);
    json response_json = json::parse(response_string);
    request_json["apply_from_log"] = true;

    json temp_response_json;
    if (response_json["status"] == "success") {
        if (command == "create_user") {
            temp_response_json = handleCreateUser(request_json);
        }
        else if (command == "login") {
            temp_response_json = handleUserLogin(request_json);
        }
        else if (command == "create_group") {
            temp_response_json = handleCreateGroup(request_json);
        }
        else if (command == "join_group") {
            temp_response_json = handleJoinGroup(request_json);
        }
        else if (command == "leave_group") {
            temp_response_json = handleLeaveGroup(request_json);
        }
        else if (command == "list_requests") {
            temp_response_json = handleListRequests(request_json);
        }
        else if (command == "accept_request") {
            temp_response_json = handleAcceptRequest(request_json);
        }
        else if (command == "list_groups") {
            temp_response_json = handleListGroups(request_json);
        }
        else if (command == "list_files") {
            temp_response_json = handleListFiles(request_json);
        }
        else if (command == "upload_file") {
            temp_response_json = handleUploadFile(request_json);
        }
        else if (command == "download_file") {
            temp_response_json = handleDownload(request_json);
        }
        else if (command == "download_confirm") {
            temp_response_json = handleDownloadConfirm(request_json);
        }
        else if (command == "logout") {
            temp_response_json = handleLogout(request_json);
        }
        else if (command == "show_downloads") {
            temp_response_json = handleShowDownloads(request_json);
        }
        else if (command == "stop_share") {
            temp_response_json = handleStopShare(request_json);
        }
    }
}

void *handleClientRequest(void* args) {
    ThreadArgs *threadArgs = (ThreadArgs *)args;
    int client_socket_fd = threadArgs->socketFd;
    sockaddr_in client_address = threadArgs->address;
    socklen_t client_addrlen = threadArgs->addrlen;

    pthread_mutex_lock(&client_sockets_mutex);
    client_sockets.push_back(client_socket_fd);
    pthread_mutex_unlock(&client_sockets_mutex);

    while (true)
    {
        pthread_mutex_lock(&stop_execution_mutex);
        bool should_stop = stopExecution;
        pthread_mutex_unlock(&stop_execution_mutex);
        if (should_stop) {
            break;
        }

        string client_request_str;
        if (!recvLargeMessage(client_socket_fd, client_request_str)) {
            cout << "Client disconnected or error occurred.\n";
            close(client_socket_fd);
            break;
        }
        
        json request_json = json::parse(client_request_str);
        json response_json;
        request_json["apply_from_log"] = false;
        if (request_json["command"] == "create_user") {
            response_json = handleCreateUser(request_json);
        }
        else if (request_json["command"] == "login") {
            response_json = handleUserLogin(request_json);
            
            if (response_json["status"] == "success") {
                portToUser[request_json["sender_port"]]->socket_fd = client_socket_fd;
            }
        }
        else if (request_json["command"] == "create_group") {
            response_json = handleCreateGroup(request_json);
        }
        else if (request_json["command"] == "join_group") {
            response_json = handleJoinGroup(request_json);
        }
        else if (request_json["command"] == "leave_group") {
            response_json = handleLeaveGroup(request_json);
        }
        else if (request_json["command"] == "list_requests") {
            response_json = handleListRequests(request_json);
        }
        else if (request_json["command"] == "accept_request") {
            response_json = handleAcceptRequest(request_json);
        }
        else if (request_json["command"] == "list_groups") {
            response_json = handleListGroups(request_json);
        }
        else if (request_json["command"] == "list_files") {
            response_json = handleListFiles(request_json);
        }
        else if (request_json["command"] == "upload_file") {
            response_json = handleUploadFile(request_json);
        }
        else if (request_json["command"] == "download_file") {
            response_json = handleDownload(request_json);
        }
        else if (request_json["command"] == "download_confirm") {
            response_json = handleDownloadConfirm(request_json);
        }
        else if (request_json["command"] == "logout") {
            response_json = handleLogout(request_json);
        }
        else if (request_json["command"] == "show_downloads") {
            response_json = handleShowDownloads(request_json);
        }
        else if (request_json["command"] == "stop_share") {
            response_json = handleStopShare(request_json);
        }

        string response = response_json.dump();
        cout << response_json["message"] << "\n\n";
        if (!sendLargeMessage(client_socket_fd, response)) {
            cout << "Failed to send request\n";
        }
    }

    pthread_mutex_lock(&client_sockets_mutex);
    auto it = find(client_sockets.begin(), client_sockets.end(), client_socket_fd);
    if (it != client_sockets.end()) {
        client_sockets.erase(it);
    }
    pthread_mutex_unlock(&client_sockets_mutex);

    shutdown(client_socket_fd, SHUT_RDWR);
    close(client_socket_fd);
    delete threadArgs;
    pthread_exit(NULL);
}

void *handleTrackerInput(void *args) {
    string input;
    while (stopExecution == false)
    {
        getline(cin, input);
        if (input == "quit")
        {
            pthread_mutex_lock(&stop_execution_mutex);
            stopExecution = true;
            pthread_mutex_unlock(&stop_execution_mutex);

            cout << "Closing...\n";

            if (tracker_socket_fd != -1) {
                shutdown(tracker_socket_fd, SHUT_RDWR);
                close(tracker_socket_fd);
                tracker_socket_fd = -1;
            }

            for (int sock_fd : tracker_broadcast_sockets) {
                if (sock_fd != -1) {
                    shutdown(sock_fd, SHUT_RDWR);
                    close(sock_fd);
                }
            }
            tracker_broadcast_sockets.clear();

            pthread_mutex_lock(&client_sockets_mutex);
            for (int sock_fd : client_sockets) {
                if (sock_fd != -1) {
                    shutdown(sock_fd, SHUT_RDWR);
                    close(sock_fd);
                }
            }
            client_sockets.clear();
            pthread_mutex_unlock(&client_sockets_mutex);
            break;
        }
    }
    
    pthread_exit(NULL);
}

void *handleTrackerBroadcast(void *args) {
    // Socket creation
    int tracker_broadcast_socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (tracker_broadcast_socket_fd == -1) {
        // cout << "Socket failed\n";
        return NULL;
    }

    fcntl(tracker_broadcast_socket_fd, F_SETFL, O_NONBLOCK);

    int opt = 1;
    if (setsockopt(tracker_broadcast_socket_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt)))
    {
        return NULL;
    }

    // Define server address
    sockaddr_in tracker_address{};
    socklen_t addrlen = sizeof(tracker_address);
    tracker_address.sin_family = AF_INET;
    in_addr_t ipInBinary = inet_addr(tracker_ip_address);
    tracker_address.sin_addr.s_addr = ipInBinary;
    
    int PORT = *((int*)args);
    delete (int*)args;
    tracker_address.sin_port = htons(PORT);

    // Bind socket to address
    if (bind(tracker_broadcast_socket_fd, (struct sockaddr*)&tracker_address, addrlen) < 0) {
        close(tracker_broadcast_socket_fd);
        return NULL;
    }

    // Start listening
    if (listen(tracker_broadcast_socket_fd, 5) < 0) {
        close(tracker_broadcast_socket_fd);
        return NULL;
    }

    // cout << "Tracker listening for broadcast messages on port " << tracker_port << "...\n";

    tracker_broadcast_sockets.push_back(tracker_broadcast_socket_fd);

    while (stopExecution == false) {
        sockaddr_in client_address;
        socklen_t client_addrlen = sizeof(client_address);

        int client_socket_fd = accept(tracker_broadcast_socket_fd, (struct sockaddr*)&client_address, (socklen_t*)&client_addrlen);
        if (client_socket_fd < 0) {
            if (stopExecution == true) {
                break;
            }
            usleep(100000);
            continue;
        }

        string request;
        if (!recvLargeMessage(client_socket_fd, request)) {
            cout << "Error receiving data from tracker\n";
            close(client_socket_fd);
            continue;
        }

        json request_json = json::parse(request);
        if (request_json["command"] == "broadcast_sync") {
            int start = last_sequence_number;
            initializeFromLog(start);
        }

        close(client_socket_fd);
    }
    
    close(tracker_broadcast_socket_fd);
    pthread_exit(NULL);
}

int main(int argc, char *argv[]) {
    initializeFromLog(0);

    if (argc < 3) {
        cout << "Please enter the command in this format: ./a.out <ip>:<port> <tracker_info.txt>\n";
        return 1;
    }

    tracker_no = atoi(argv[2])-1;
    if (tracker_no < 0) {
        cout << "Tracker number must be a positive integer.\n";
        return 1;
    }

    char* tracker_info_file = argv[1];
    int tracker_info_fd = open(tracker_info_file, O_RDWR);
    if (tracker_info_fd < 0) {
        cout << "Error opening tracker info file\n";
        return 1;
    }

    char* tracker_info_content = new char[BUFFER_SIZE];
    ssize_t tracker_info_content_size = read(tracker_info_fd, tracker_info_content, BUFFER_SIZE);
    if (tracker_info_content_size <= 0) {
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

            ip_address.erase(ip_address.find_last_not_of(" \n\r\t")+1);
            port.erase(0, port.find_first_not_of(" \n\r\t"));
            port.erase(port.find_last_not_of(" \n\r\t")+1);

            if (!port.empty() && all_of(port.begin(), port.end(), ::isdigit)) {
                trackersIPList.push_back(ip_address);
                trackersPortList.push_back(port);
            }
        }
    }

    tracker_ip_address = trackersIPList[tracker_no].data();
    tracker_port = trackersPortList[tracker_no].data();

    // Socket creation
    tracker_socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (tracker_socket_fd == -1) {
        cout << "Socket failed\n";
        return 1;
    }

    fcntl(tracker_socket_fd, F_SETFL, O_NONBLOCK);

    if (setsockopt(tracker_socket_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt)))
    {
        cout << "Error: Sockopt.\n\n";
        return 1;
    }

    // Define server address
    sockaddr_in tracker_address{};
    socklen_t addrlen = sizeof(tracker_address);
    tracker_address.sin_family = AF_INET;
    in_addr_t ipInBinary = inet_addr(tracker_ip_address);
    tracker_address.sin_addr.s_addr = ipInBinary;
    uint16_t PORT = static_cast<uint16_t>(strtoul(tracker_port, NULL, 10));
    tracker_address.sin_port = htons(PORT);

    // Bind socket to address
    if (bind(tracker_socket_fd, (struct sockaddr*)&tracker_address, addrlen) < 0) {
        cout << "Bind Failed\n";
        close(tracker_socket_fd);
        return 1;
    }

    // Start listening
    if (listen(tracker_socket_fd, 5) < 0) {
        cout << "Listen Failed\n";
        close(tracker_socket_fd);
        return 1;
    }

    cout << "Tracker started successfully...\n";
    cout << "Tracker listening on port " << tracker_port << "...\n";

    // For getting inputs from user (quit case)
    pthread_t tracker_input_thread;
    if (pthread_create(&tracker_input_thread, NULL, handleTrackerInput, NULL) != 0)
    {
        cout << "Failed to create thread.\n";
        return 1;
    }

    vector<pthread_t> clientThreads;
    while (true) {
        pthread_mutex_lock(&stop_execution_mutex);
        bool should_stop = stopExecution;
        pthread_mutex_unlock(&stop_execution_mutex);
        if (should_stop) {
            break;
        }

        sockaddr_in client_address;
        socklen_t client_addrlen = sizeof(client_address);

        int client_socket_fd = accept(tracker_socket_fd, (struct sockaddr*)&client_address, (socklen_t*)&client_addrlen);
        if (client_socket_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(100000);
                continue;
            }
            break;
        }

        ThreadArgs *threadArgs = new ThreadArgs;
        threadArgs->socketFd = client_socket_fd;
        threadArgs->address = client_address;
        threadArgs->addrlen = client_addrlen;

        pthread_t client_handler_thread;
        if (pthread_create(&client_handler_thread, NULL, handleClientRequest, (void *)threadArgs) != 0) {
            cout << "Failed to create thread\n";
            delete threadArgs;
            close(client_socket_fd);
        } else {
            clientThreads.push_back(client_handler_thread);
        }
    }

    // Join all threads
    for (pthread_t thread : clientThreads) {
        pthread_join(thread, NULL);
    }
    
    pthread_join(tracker_input_thread, NULL);
    if (tracker_socket_fd != -1) {
        close(tracker_socket_fd);
    }

    return 0;
}