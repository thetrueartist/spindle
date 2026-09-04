# Spindle - build and analysis. `make help` lists the targets.

# ARCH selects the Windows target: x86_64 (the default) or aarch64 for
# Windows on ARM. The aarch64 triple is only provided by llvm-mingw, which
# is not packaged anywhere -- CI fetches a pinned release. Nothing here can
# execute an ARM64 binary, so that build is compile-and-link only.
ARCH     ?= x86_64

CXX_WIN  := $(ARCH)-w64-mingw32-g++
CXX_HOST := g++
WINDRES  := $(ARCH)-w64-mingw32-windres

SRC      := src/core.cpp src/ntfs.cpp src/mfttree.cpp src/mft.cpp src/scan.cpp \
            src/update.cpp src/ui.cpp
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
            -lcomdlg32 -lwinhttp -lbcrypt -luuid -lmpr -lcomctl32 -lcrypt32

.PHONY: all help test test-core test-ntfs test-queue test-mft test-image test-win fuzz fuzz-lib bench stress analyze clean dirs icon hooks hygiene

all: dirs $(OUT)

help:
	@echo "make            cross-compile build/spindle.exe (MinGW-w64; ARCH=aarch64 for ARM64)"
	@echo "make test       host tests under ASan, UBSan and ThreadSanitizer"
	@echo "make test-image the MFT assembler against a real NTFS image (needs ntfs-3g, root)"
	@echo "make test-win   Windows-side tests -> build/test_win.exe (run: wine build/test_win.exe)"
	@echo "make fuzz       the parsers under random mutation (any compiler, ASan + UBSan)"
	@echo "make fuzz-lib   the same targets under libFuzzer (clang + libclang-rt-dev)"
	@echo "make bench      the portable hot paths on a synthetic million-file volume"
	@echo "make stress     the scanner's concurrency structure over a real tree"
	@echo "make analyze    cppcheck + clang-tidy over the portable core"
	@echo "make hygiene    refuse personal paths, secrets and typographic dashes"
	@echo "make hooks      run the hygiene check on every commit"
	@echo "make icon       regenerate spindle.ico"
	@echo "make clean"

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

$(OUT): $(SRC) src/spindle.h src/ntfs.h src/mfttree.h src/sync.h src/workqueue.h $(RES)
	$(CXX_WIN) $(CXXFLAGS) $(SRC) $(RES) -o $@ $(LDFLAGS) $(LIBS)
	@echo "built $@"

hooks:
	git config core.hooksPath .githooks
	@echo "hooks enabled: hygiene runs on every commit"

hygiene:
	tools/hygiene.sh

# Windows-side checks, built with the cross compiler. Run natively on the
# Windows CI job; on a developer box, `wine build/test_win.exe`.
test-win: dirs
	$(CXX_WIN) $(CXXFLAGS) -Isrc tests/test_win.cpp src/scan.cpp src/core.cpp \
	    src/mft.cpp src/mfttree.cpp src/ntfs.cpp -o build/test_win.exe \
	    -static -static-libgcc -static-libstdc++ $(HARDEN_L) $(LIBS)

# -- Host tests --------------------------------------------------------------
# Each binary depends on exactly what it compiles, so `make test` after a
# one-line change rebuilds one test, not four. ARGS passes flags through
# to the harness in tests/check.h:
#   make test-core ARGS='--filter cache'
#   make test-queue ARGS='--repeat 20'
# _GLIBCXX_ASSERTIONS turns the standard containers' bounds checks on, so
# an out-of-range index fails loudly instead of reading whatever is there.
TEST_FLAGS := -std=c++17 -O1 -g -I src -I tests $(WARN) \
              -fsanitize=address,undefined -fno-omit-frame-pointer \
              -D_GLIBCXX_ASSERTIONS
TEST_ENV   := ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1
ARGS       ?=

build/test_core: tests/test_core.cpp src/core.cpp src/spindle.h tests/check.h
	@mkdir -p build
	$(CXX_HOST) $(TEST_FLAGS) tests/test_core.cpp src/core.cpp -o $@

build/test_ntfs: tests/test_ntfs.cpp src/ntfs.cpp src/ntfs.h tests/check.h
	@mkdir -p build
	$(CXX_HOST) $(TEST_FLAGS) tests/test_ntfs.cpp src/ntfs.cpp -o $@

build/test_queue_asan: tests/test_queue.cpp src/workqueue.h src/sync.h tests/check.h
	@mkdir -p build
	$(CXX_HOST) $(TEST_FLAGS) tests/test_queue.cpp -o $@ -pthread

# The queue is the scanner's concurrency core, so it gets a ThreadSanitizer
# pass of its own on top of ASan.
build/test_queue_tsan: tests/test_queue.cpp src/workqueue.h src/sync.h tests/check.h
	@mkdir -p build
	$(CXX_HOST) -std=c++17 -O1 -g -I src -I tests $(WARN) \
	    -fsanitize=thread -fno-omit-frame-pointer \
	    tests/test_queue.cpp -o $@ -pthread

# The tree assembly behind the MFT scan, on tables built to be hostile.
build/test_mft: tests/test_mft.cpp src/mfttree.cpp src/ntfs.cpp src/core.cpp \
                src/mfttree.h src/ntfs.h src/spindle.h tests/check.h \
                tests/ntfs_fixture.h
	@mkdir -p build
	$(CXX_HOST) $(TEST_FLAGS) tests/test_mft.cpp src/mfttree.cpp src/ntfs.cpp \
	    src/core.cpp -o $@

# A real NTFS image, written by mkntfs and filled through ntfs-3g. The
# mount needs root, so CI runs the script under sudo before `make
# test-image`; on a box without the tools, `make test` still covers the
# assembler with synthetic tables.
build/demo.ntfs: tools/make-ntfs-image.sh
	tools/make-ntfs-image.sh $@

test-core: build/test_core
	$(TEST_ENV) ./build/test_core $(ARGS)

test-mft: build/test_mft
	$(TEST_ENV) ./build/test_mft $(ARGS)

test-image: build/test_mft build/demo.ntfs
	$(TEST_ENV) ./build/test_mft --image build/demo.ntfs $(ARGS)

test-ntfs: build/test_ntfs
	$(TEST_ENV) ./build/test_ntfs $(ARGS)

test-queue: build/test_queue_asan build/test_queue_tsan
	ASAN_OPTIONS=detect_leaks=1 ./build/test_queue_asan $(ARGS)
	./build/test_queue_tsan $(ARGS)

test: test-core test-ntfs test-queue test-mft

# -- Fuzzing -----------------------------------------------------------------
# Each target in tests/fuzz_*.cpp is a libFuzzer entry point. `make fuzz`
# builds them with the host compiler and tests/fuzz_main.cpp, a driver that
# replays each target's seeds and mutates them at random under ASan and
# UBSan, so the targets run on any box. `make fuzz-lib` is the real thing:
# coverage-guided, under clang with its runtime (libclang-rt-dev), each
# target for FUZZ_SECONDS seeded from the driver's --seeds output.
FUZZERS         := ntfs cache settings text json
FUZZ_SRC        := src/core.cpp src/ntfs.cpp src/mfttree.cpp
FUZZ_DEPS       := tests/fuzz_common.h tests/ntfs_fixture.h src/spindle.h \
                   src/ntfs.h src/mfttree.h $(FUZZ_SRC)
FUZZ_ITERATIONS ?= 20000
FUZZ_SECONDS    ?= 20
CLANG           ?= clang++

build/fuzz_%: tests/fuzz_%.cpp tests/fuzz_main.cpp $(FUZZ_DEPS)
	@mkdir -p build
	$(CXX_HOST) $(TEST_FLAGS) tests/fuzz_main.cpp tests/fuzz_$*.cpp $(FUZZ_SRC) -o $@

build/libfuzz_%: tests/fuzz_%.cpp $(FUZZ_DEPS)
	@mkdir -p build
	$(CLANG) -std=c++17 -O1 -g -I src -I tests -Wall -Wextra \
	    -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer \
	    -D_GLIBCXX_ASSERTIONS tests/fuzz_$*.cpp $(FUZZ_SRC) -o $@

fuzz: $(FUZZERS:%=build/fuzz_%)
	@for t in $(FUZZERS); do \
	    echo "== fuzz_$$t"; \
	    $(TEST_ENV) ./build/fuzz_$$t --random $(FUZZ_ITERATIONS) || exit 1; \
	done

fuzz-lib: $(FUZZERS:%=build/libfuzz_%) $(FUZZERS:%=build/fuzz_%)
	@for t in $(FUZZERS); do \
	    echo "== libfuzz_$$t ($(FUZZ_SECONDS)s)"; \
	    mkdir -p build/corpus/$$t; \
	    ./build/fuzz_$$t --seeds build/corpus/$$t >/dev/null || exit 1; \
	    ./build/libfuzz_$$t -max_total_time=$(FUZZ_SECONDS) -max_len=65536 \
	        -artifact_prefix=build/corpus/$$t/ build/corpus/$$t \
	        > build/corpus/$$t.log 2>&1 || { tail -30 build/corpus/$$t.log; exit 1; }; \
	    grep -E '^Done|^#[0-9]+.*DONE' build/corpus/$$t.log | tail -1; \
	done

# -- Benchmarks --------------------------------------------------------------
# The portable hot paths on a synthetic million-file volume, at the
# program's own optimisation level. Run it before and after a change.
build/bench_core: tests/bench_core.cpp src/core.cpp src/ntfs.cpp src/mfttree.cpp \
                  tests/ntfs_fixture.h src/spindle.h src/ntfs.h src/mfttree.h
	@mkdir -p build
	$(CXX_HOST) -std=c++17 -O2 -DNDEBUG -g -I src -I tests $(WARN) \
	    tests/bench_core.cpp src/core.cpp src/ntfs.cpp src/mfttree.cpp -o $@

bench: build/bench_core
	./build/bench_core $(ARGS)

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
