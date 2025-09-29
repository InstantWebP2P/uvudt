# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is **UDT4 (UDP-based Data Transfer Library)**, version 4.10. UDT is a high-performance data transfer protocol built on top of UDP that provides reliable, ordered data transfer with congestion control.

## Build System

### Make-based Build

```bash
# Default build (Linux/x86_64)
make

# Platform-specific builds
make -e os=OSX arch=POWERPC
make -e os=BSD arch=IA32
make -e os=LINUX arch=AMD64

# Clean build
make clean
```

### Supported Platforms
- **os**: LINUX (default), BSD, OSX
- **arch**: x86_64 (default), IA32, POWERPC, IA64, AMD64, SPARC

### Build Output
- `libudt.so` - Shared library
- `libudt.a` - Static library
- `udt` - Header file directory

## Architecture

### Core Components

- **src/** - Core UDT implementation
  - `udt_core.cpp` - Main UDT core implementation
  - `api.cpp` - Public API functions
  - `packet.cpp` - Packet handling
  - `queue.cpp` - Data queues
  - `ccc.cpp` - Congestion control
  - `epoll.cpp` - Event polling
  - `buffer.cpp` - Buffer management
  - `cache.cpp` - Connection caching
  - `channel.cpp` - Communication channels
  - `window.cpp` - Flow control windows

### Key Headers
- `udt.h` - Main public API header
- `api.h` - Internal API definitions
- `core.h` - Core data structures
- `packet.h` - Packet structures
- `queue.h` - Queue implementations

### Protocol Features
- Reliable, ordered data transfer over UDP
- Congestion control algorithms
- Flow control mechanisms
- Epoll-based event handling
- Connection caching and multiplexing
- Cross-platform support (Linux, BSD, OSX, Windows)

## Development

### Example Applications

Located in `app/` directory:
- `appclient.cpp` - Basic client example
- `appserver.cpp` - Basic server example
- `sendfile.cpp` - File sending example
- `recvfile.cpp` - File receiving example
- `test.cpp` - Comprehensive test suite
- `app2p.cpp` - Peer-to-peer example

### Testing

Run the test suite:
```bash
cd app
./test
```

### Documentation

Comprehensive documentation available in `doc/` directory:
- `index.htm` - Main documentation entry point
- HTML documentation with JavaScript navigation
- API reference and usage examples

## Important Build Flags

- `-fPIC` - Position independent code
- `-Wall -Wextra` - Warning flags
- `-finline-functions -O3` - Optimization
- `-fno-strict-aliasing` - Required for UDT
- `-fvisibility=hidden` - Symbol visibility
- Platform-specific defines: `-DLINUX`, `-Dx86_64`, etc.

## Platform Notes

### Windows
- Use Visual C++ project files in `win/` directory
- Requires Windows Sockets 2 (Winsock2)

### Unix-like Systems
- Uses GNU Make (required for BSD systems)
- Supports both shared and static libraries
- Epoll support for efficient event handling

## Common Development Tasks

### Adding New Features
1. Add implementation to appropriate `.cpp` file in `src/`
2. Update relevant header files
3. Add tests to `app/test.cpp` or create new test files
4. Update documentation in `doc/` if API changes

### Debugging
- Build with debug symbols by modifying `CCFLAGS` in `src/Makefile`
- Use the test suite for validation
- Check platform-specific behavior using appropriate `os` and `arch` flags