# Peer-to-Peer File Sharing System

A distributed file sharing application that enables users to upload, download, and share files across a peer-to-peer network with a centralized tracker for metadata management.

## Overview

This system consists of:

- **Tracker**: Centralized server managing user/group metadata and chunk ownership
- **Clients**: Peer applications for file upload, download, and group management

Features include SHA-1 based file integrity verification, chunked file distribution, and parallel downloads from multiple peers.

## Features

- User authentication and session management
- Group creation and membership management
- File upload with automatic 512 KB chunking
- Parallel peer-to-peer chunk downloads
- SHA-1 piecewise and complete file verification
- File availability tracking and peer discovery
- Log-based state persistence for tracker
- Multi-threaded concurrent operations

## Data Flow

### Upload Flow:

```
Client A -> Upload file to Tracker -> Tracker records chunks & ownership
-> Peers can now download
```

### Download Flow:

```
Client B -> Request file from Tracker -> Tracker provides peer list
-> Client downloads chunks in parallel from peers
-> Verify SHA-1 hash -> Complete
```

### Peer Discovery:

```
Tracker maintains peer IP/port -> Returns available peers for download
```

## Configuration

### Tracker Configuration (tracker/tracker_info.txt)

```
127.0.0.1 8000
127.0.0.1 8001
127.0.0.1 8002
```

Format: IP PORT (one tracker per line)

## Installation & Build

```bash
cd Peer-To-Peer
make clean
make all
```

Generates:

- tracker/tracker
- client/client

## Usage

### Start Tracker

```bash
cd tracker
./tracker
```

### Start Client

```bash
cd client
./client
```

### Client Commands

```
User Management:
create_user <user_id> <password>
login <user_id> <password>
logout

Group Management:
create_group <group_id>
join_group <group_id>
leave_group <group_id>
list_groups

File Operations:
upload_file <group_id> <file_path>
download_file <group_id> <file_name> <destination_path>
list_files <group_id>
stop_share <group_id> <file_name>
show_downloads
```

## API Reference

### Login Request

```json
{
  "command": "login",
  "command_args": {
    "user_id": "alice",
    "passwd": "password123"
  },
  "sender_ip": "127.0.0.1",
  "sender_port": "5000"
}
```

### Upload File Request

```json
{
  "command": "upload_file",
  "command_args": {
    "group_id": "group1",
    "file_name": "document.pdf"
  },
  "piecewise_sha": ["sha1_chunk_1", "sha1_chunk_2"],
  "fileSha": "sha1_complete_file",
  "sender_ip": "127.0.0.1",
  "sender_port": "5000"
}
```

### Download File Request

```json
{
  "command": "download_file",
  "command_args": {
    "group_id": "group1",
    "file_name": "document.pdf"
  },
  "sender_ip": "127.0.0.1",
  "sender_port": "5000"
}
```

### Download File Response

```json
{
  "status": "success",
  "file_path": "/full/path/to/file",
  "file_chunks": ["chunk_sha_1", "chunk_sha_2"],
  "chunk_metadata": {
    "chunk_sha_1": [
      ["127.0.0.1", "5001"],
      ["127.0.0.2", "5002"]
    ],
    "chunk_sha_2": [["127.0.0.1", "5001"]]
  }
}
```
