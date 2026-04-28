# symboli – Symbol Interpeter

A fully vibe-coded programming lanugage.

## Why?

Because I can lol.

## Installation

### Prerequisites

- **C Compiler**: GCC, Clang, or MSVC
- **CMake**: Version 3.27 or later
- **Git**: For cloning the repository

#### macOS
Install Xcode Command Line Tools:
```bash
xcode-select --install
```

#### Linux (Ubuntu/Debian)
```bash
sudo apt update
sudo apt install build-essential cmake git
```

#### Windows
Install Visual Studio with C++ support or MinGW, then install CMake.

### Building from Source

1. Clone the repository:
```bash
git clone <repository-url>
cd symboli
```

2. Create build directory:
```bash
mkdir build
cd build
```

3. Configure with CMake:
```bash
cmake ..
```
For release build:
```bash
cmake -DCMAKE_BUILD_TYPE=Release ..
```

4. Build the project:
```bash
cmake --build .
```

5. The `symboli` executable will be created in the `../bin/` directory.

### Running

After building, you can run Symboli:
```bash
../bin/symboli --help
```

To run a Symbol script:
```bash
../bin/symboli script.sym
```

## Documentation

WIP.

## Contributing

Feel free to send me patches.

## License

TL;DR: It's MIT.
