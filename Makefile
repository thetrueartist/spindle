# Spindle - build and analysis
#
#   make            cross-compile spindle.exe (MinGW-w64)
#   make test       build and run core tests natively with ASan + UBSan
#   make analyze    cppcheck + clang-tidy
#   make clean

# ARCH selects the Windows target: x86_64 (the default) or aarch64 for
# Windows on ARM. The aarch64 triple is only provided by llvm-mingw, which
# is not packaged anywhere -- CI fetches a pinned release. Nothing here can
# execute an ARM64 binary, so that build is compile-and-link only.
ARCH     ?= x86_64

CXX_WIN  := $(ARCH)-w64-mingw32-g++
CXX_HOST := g++
WINDRES  := $(ARCH)-w64-mingw32-windres

SRC      := src/core.cpp src/ntfs.cpp src/mft.cpp src/scan.cpp src/update.cpp src/ui.cpp
RC       := res/spindle.rc
RES      := build/spindle.res.o
ICON     := build/spindle.ico
OUT      := build/spindle.exe

# -- Warnings ----------------------------------------------------------------
WARN := -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion \
        -Wcast-qual -Wcast-align -Wformat=2 -Wnull-dereference \
        -Wdouble-promotion -Wimplicit-fallthrough -Wold-style-cast \
        -Wnon-virtual-dtor -Woverloaded-virtual -Wuseless-cast

# -- Exploit mitigations -----------------------------------------------------
# _FORTIFY_SOURCE needs an optimising build to do anything.
HARDEN_C := -fstack-protector-strong -D_FORTIFY_SOURCE=2 \
            -fno-common -ftrivial-auto-var-init=zero

# DEP, ASLR, 64-bit ASLR, SEH, and a non-relocatable-by-accident image.
HARDEN_L := -Wl,--nxcompat -Wl,--dynamicbase -Wl,--high-entropy-va \
            -Wl,--no-seh -Wl,--disable-auto-image-base

CXXFLAGS := -std=c++17 -municode -O2 -DNDEBUG -DUNICODE -D_UNICODE \
            $(WARN) $(HARDEN_C)

# -mwindows suppresses the console window; static linking avoids shipping the
# MinGW runtime DLLs alongside the exe.
LDFLAGS  := -mwindows -static -static-libgcc -static-libstdc++ \
            -Wl,--gc-sections $(HARDEN_L)
LIBS     := -ld2d1 -ldwrite -ldwmapi -lole32 -lshell32 -luser32 -lgdi32 \
            -ladvapi32 -lrstrtmgr \
            -lcomdlg32 -lwinhttp -lbcrypt -luuid

.PHONY: all test stress analyze clean dirs icon

all: dirs $(OUT)

dirs:
	@mkdir -p build

# Redrawn at every size rather than downscaled from one master; a 16px
# downscale of a 256px geometric mark loses its structure.
$(ICON): tools/make_icon.py
	@mkdir -p build
	python3 tools/make_icon.py

icon: $(ICON)

# -I res so the .rc can reference spindle.ico and spindle.manifest by name,
# and -I build so it finds the generated icon.
$(RES): $(RC) res/spindle.manifest $(ICON)
	$(WINDRES) -I res -I build --input $(RC) --output $@ --output-format=coff

$(OUT): $(SRC) src/spindle.h $(RES)
	$(CXX_WIN) $(CXXFLAGS) $(SRC) $(RES) -o $@ $(LDFLAGS) $(LIBS)
	@echo "built $@"

# Core logic and the work queue are platform-independent, so both run under
# the host sanitizers. The queue is where the scanner used to fall over, so it
# gets a ThreadSanitizer pass of its own on top of ASan.
test:
	@mkdir -p build
	$(CXX_HOST) -std=c++17 -O1 -g $(WARN) \
	    -fsanitize=address,undefined -fno-omit-frame-pointer \
	    tests/test_core.cpp src/core.cpp -o build/test_core
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 \
	    ./build/test_core
	$(CXX_HOST) -std=c++17 -O1 -g -I src \
	    -fsanitize=address,undefined -fno-omit-frame-pointer \
	    tests/test_ntfs.cpp src/ntfs.cpp -o build/test_ntfs
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 \
	    ./build/test_ntfs
	$(CXX_HOST) -std=c++17 -O1 -g -I src \
	    -fsanitize=address,undefined -fno-omit-frame-pointer \
	    tests/test_queue.cpp -o build/test_queue_asan -pthread
	ASAN_OPTIONS=detect_leaks=1 ./build/test_queue_asan
	$(CXX_HOST) -std=c++17 -O1 -g -I src \
	    -fsanitize=thread -fno-omit-frame-pointer \
	    tests/test_queue.cpp -o build/test_queue_tsan -pthread
	./build/test_queue_tsan

# Walks a real directory tree with the scanner's exact concurrency structure.
# Needs a POSIX host; the Windows scanner shares the queue and worker shape.
stress:
	@mkdir -p build
	$(CXX_HOST) -std=c++17 -O1 -g -I src -fsanitize=thread \
	    -fno-omit-frame-pointer tests/stress_scan.cpp src/core.cpp \
	    -o build/stress_scan -pthread
	./build/stress_scan /usr 16 3
	./build/stress_scan /usr 16 10 cancel

analyze:
	@echo "--- cppcheck ---"
	-cppcheck --enable=all --inconclusive --std=c++17 \
	    --suppress=missingIncludeSystem --suppress=unusedFunction \
	    --suppress=checkersReport \
	    --error-exitcode=0 -I src src/core.cpp 2>&1
	@echo "--- clang-tidy (portable core) ---"
	-clang-tidy src/core.cpp --quiet \
	    -checks='bugprone-*,cert-*,clang-analyzer-*,misc-*,performance-*,portability-*,-cert-err58-cpp,-misc-include-cleaner' \
	    -- -std=c++17 -I src 2>&1

clean:
	rm -rf build
