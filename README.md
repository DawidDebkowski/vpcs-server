# VPCS with HTTP Server/Client Extension

This is an educational fork of VPCS (Virtual PC Simulator) extended with HTTP server and client capabilities as an extracurricular project for the Network Technologies course at Wrocław University of Science and Technology.

## About This Fork

 Added a simple virtual HTTP server and client functionality to demonstrate application-layer protocol implementation within the VPCS virtual TCP stack.

### Key Additions

- **HTTP Server**: Virtual HTTP servers that can be started/stopped on specific ports
- **HTTP Client**: GET request functionality using the VPCS virtual TCP stack
- **TCP Integration**: Seamless integration with existing VPCS networking architecture
- **Protocol Support**: HTTP/1.0 compliant server with request echoing functionality

### Usage Examples

```bash
# Start HTTP server on port 80
httpd start 80

# Check server status
httpd status

# Make HTTP request (from another PC)
httpd get 192.168.1.10 80 /index.html

# Stop HTTP server
httpd stop 80
```

### Installing

1. **Clone the repository**

2. **Compile the project**
   ```bash
   cd src
   ./mk.sh
   ```
   *Note: The build script automatically detects your OS (Linux, macOS, FreeBSD, etc.) and uses the appropriate Makefile.*

3. **Install for GNS3 usage**
   - Replace the existing VPCS binary in your GNS3 installation with the newly compiled `src/vpcs` binary
   - Or update your GNS3 VPCS path to point to the new binary location
   - Restart GNS3 to use the extended VPCS with HTTP capabilities


## Original VPCS

VPCS is a virtual PC simulator that can simulate multiple PCs on a single physical machine, commonly used for network simulation and testing in educational and laboratory environments.

This is **a fork of** a continuation of VPCS, based on the last development version and improved with patches wrote by various 
people from the community. The original VPCS code, which is unfortunately not maintained anymore, can be viewed on 
https://sourceforge.net/p/vpcs/code/
