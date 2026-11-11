# Build

Requirements: C++17 compiler, CMake 3.21+, Ninja or another generator, Qt 6.8+ Widgets/Multimedia/Test development modules.
Windows build tested with MSVC 19.44 (x64) and Qt 6.8.3 msvc2022_64. CUDA tested with toolkit 13.3.

In an x64 Visual Studio developer command prompt:

```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:\Qt\6.8.3\msvc2022_64
cmake --build build
C:\Qt\6.8.3\msvc2022_64\bin\windeployqt.exe --release "build\Wavy-Flo.exe"
```

Keep `build/effects/WavyInvert.dll` in `effects` next to the executable. Copy `tools` next to the executable too.

Optional CUDA exposure DLL (requires NVIDIA Toolkit; in same developer prompt):

```bat
nvcc --shared -std=c++17 -O2 -I sdk plugins\cuda.cu -o build\WavyCuda.dll -Xcompiler /MD -gencode arch=compute_75,code=sm_75 -gencode arch=compute_86,code=sm_86 -gencode arch=compute_89,code=sm_89 -gencode arch=compute_120,code=sm_120 -gencode arch=compute_120,code=compute_120
```

For other GPU architectures, add appropriate nvcc code targets supported by the installed toolkit. If a GPU dispatch fails, the host uses CPU exposure.

macOS (source target, not yet tested): install Xcode command-line tools and matching Qt, then configure with CMake and CMAKE_PREFIX_PATH pointing to Qt. CMake builds WavyMetal using Foundation/Metal frameworks; place the resulting dylib alongside the executable. Deployment into a proper macOS .app is not implemented in this Windows preview. Never describe this as a verified macOS release until it has been built/tested there.

Self-test:

```bat
set WAVY_FLO_PYTHON=C:\Path\To\Python3\python.exe
"build\Wavy-Flo.exe" --self-test
```

Qt Test is linked for deterministic interaction regression tests. `--screenshot --graph` captures the native widget layout to native-preview.png and exits.
