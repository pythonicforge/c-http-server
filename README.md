# C HTTP Server

A lightweight, feature-rich HTTP server built with C. This server provides a simple file-sharing interface with upload, download, and delete capabilities.

## Table of Contents

- [Features](#features)
- [Requirements](#requirements)
- [Building](#building)
- [Running](#running)
- [Usage](#usage)
  - [Web Interface](#web-interface)
  - [API Endpoints](#api-endpoints)
- [Configuration](#configuration)
- [Project Structure](#project-structure)
- [Security Considerations](#security-considerations)
- [License](#license)

## Features

- **Simple Web Interface**: Browse and manage files through a clean, responsive web UI
- **File Upload**: Upload single or multiple files simultaneously
- **File Download**: Download files with proper HTTP headers and content disposition
- **File Management**: Delete files directly from the web interface
- **Directory Listing**: View all shared files with modification times and sizes
- **Responsive Design**: Works seamlessly on desktop and mobile devices
- **Secure File Handling**: Filename sanitization to prevent path traversal attacks
- **Large File Support**: Handles uploads up to 1EB (effectively unlimited for normal use)
- **Minimal Dependencies**: Built with standard POSIX C libraries only

## Requirements

- **GCC** (or compatible C compiler supporting C11)
- **POSIX-compliant system** (Linux, macOS, Unix, WSL on Windows)
- **Make** (for building)

## Building

### Quick Build

```bash
make
```

This command compiles the server with optimizations and safety flags enabled:
- `-Wall -Wextra`: Enable all compiler warnings
- `-std=c11`: Use C11 standard
- `-O2`: Level 2 optimization

### Clean Build

To remove compiled objects and the binary:

```bash
make clean
```

Then rebuild with `make`.

## Running

### Start the Server

```bash
./server
```

The server will output:
```
Listening on http://0.0.0.0:8080
```

### Access the Server

Open your web browser and navigate to:
```
http://localhost:8080
```

Or from another machine on your network:
```
http://<your-machine-ip>:8080
```

### Stop the Server

Press `Ctrl+C` to gracefully shut down the server.

## Usage

### Web Interface

The main page (`http://localhost:8080/`) displays:

1. **Server Information Panel**
   - Host name of the machine running the server
   - Port number (8080)
   - Total number of files shared
   - Total storage used

2. **File Upload Section**
   - File picker supporting multiple file selection
   - Upload button to start the transfer
   - Real-time status display showing upload progress
   - Selected file count and total size preview

3. **Shared Files List**
   - All files in the `./shared/` directory
   - File size in human-readable format (B, KB, MB, GB)
   - Last modification date and time
   - Download link for each file
   - Delete button (with confirmation) for each file

### API Endpoints

#### GET /
Returns the main HTML page with the web interface.

```
GET / HTTP/1.1
Host: localhost:8080
```

#### GET /download?file=filename
Downloads a specific file from the shared directory.

**Parameters:**
- `file`: (required) The name of the file to download (URL-encoded)

**Example:**
```
GET /download?file=my%20document.pdf HTTP/1.1
Host: localhost:8080
```

**Response:** File content with `Content-Disposition: attachment` header

#### POST/PUT /upload?name=filename
Uploads a file to the shared directory.

**Parameters:**
- `name`: (optional) Desired filename. If omitted, defaults to `uploaded_file.bin`

**Headers Required:**
- `Content-Length`: Size of the file being uploaded (required)
- `Content-Type`: `application/octet-stream` (recommended)

**Example:**
```
POST /upload?name=myfile.txt HTTP/1.1
Host: localhost:8080
Content-Length: 1024
Content-Type: application/octet-stream

[file binary data]
```

**Behavior:**
- If a file with the same name exists, the server automatically appends a number (e.g., `file_1.txt`)
- If all numbered variants are taken, uses a timestamp-based filename
- Automatically creates the `./shared/` directory if it doesn't exist

#### GET /delete?file=filename
Deletes a file from the shared directory.

**Parameters:**
- `file`: (required) The name of the file to delete (URL-encoded)

**Example:**
```
GET /delete?file=oldfile.txt HTTP/1.1
Host: localhost:8080
```

**Response:** Redirects to `/` (HTTP 303)

### Example Usage with curl

**List files** (get the home page):
```bash
curl http://localhost:8080/
```

**Upload a file:**
```bash
curl -X POST --data-binary @myfile.txt \
  "http://localhost:8080/upload?name=myfile.txt"
```

**Download a file:**
```bash
curl -O "http://localhost:8080/download?file=myfile.txt"
```

**Delete a file:**
```bash
curl "http://localhost:8080/delete?file=myfile.txt"
```

## Configuration

Edit `server.c` to customize:

| Constant | Default | Purpose |
|----------|---------|---------|
| `PORT` | 8080 | Server listening port |
| `SHARED_DIR` | `./shared` | Directory for shared files |
| `MAX_HEADER_SIZE` | 64 KB | Maximum HTTP header size |
| `MAX_HTML_SIZE` | 65 KB | Maximum page size |
| `MAX_UPLOAD_SIZE` | 1 EB | Maximum file upload size |
| `MAX_NAME_LEN` | 256 | Maximum filename length |
| `MAX_PATH_LEN` | 512 | Maximum path length |

After modifying, rebuild with `make`.

## Project Structure

```
c-http-server/
├── server.c          # Main server implementation
├── Makefile          # Build configuration
├── LICENSE           # Apache License 2.0
├── .gitignore        # Git ignore rules
└── README.md         # This file
```

### Source Code Overview

**server.c** contains:

- **HTTP Server Core** (`main`): Socket creation, binding, listening, and request handling
- **Request Processing**: HTTP header parsing, method routing, and response sending
- **File Operations**: Upload, download, delete, and directory listing
- **HTML Generation**: Dynamic page generation with file lists and upload UI
- **Utility Functions**: URL encoding/decoding, filename sanitization, size formatting
- **Security Functions**: HTML escaping, path traversal prevention, filename validation

## Security Considerations

### Implemented Protections

1. **Path Traversal Prevention**: Filenames are sanitized to prevent `../` attacks
2. **HTML Escaping**: File names are escaped in HTML to prevent XSS
3. **URL Encoding**: Proper encoding/decoding of filenames in URLs
4. **Upload Size Limits**: Configurable maximum file size to prevent DoS
5. **Content-Length Validation**: Validates `Content-Length` header for uploads
6. **Buffer Overflow Protection**: Bounded string operations throughout

### Limitations & Warnings

⚠️ **This server is designed for local network use only**

- **No Authentication**: Any user on the network can upload/download/delete files
- **No HTTPS/TLS**: Transfers are unencrypted; use only on trusted networks
- **No Access Control**: No per-user permissions or role-based access
- **No Logging**: Server activity is not logged
- **Single-threaded**: Processes one request at a time (acceptable for small deployments)
- **No Rate Limiting**: No protection against rapid-fire requests

### Usage Recommendations

- Use only on trusted, private networks
- Do not expose to the internet without proper authentication/TLS layer
- Run with appropriate file permissions on the shared directory
- Monitor disk space to prevent filling the partition
- Regularly clean up old files

## License

This project is licensed under the **Apache License 2.0**. See the [LICENSE](LICENSE) file for details.

## Contributing

Contributions are welcome! Feel free to:
- Report bugs and issues
- Suggest improvements and new features
- Submit pull requests with enhancements

## Support

For issues, questions, or suggestions, please open an issue on the GitHub repository.
