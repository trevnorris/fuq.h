# fuq test suite.
#
#   make check        build and run every test configuration
#   make check-basic  single-threaded functional tests (gcc/clang, both paths)
#   make check-pool   slab-pool recycling / leak tests (malloc wrapped)
#   make check-spsc   concurrent SPSC stress, incl. ThreadSanitizer
#   make check-cpp    verify the header includes and runs from C++
#   make example      build and run example/sporadic
#   make clean        remove build artifacts
#
# Override the iteration counts for the concurrent tests with e.g.
#   make check-spsc SPSC_BIG=20000000

CC        ?= gcc
CXX       ?= g++
CLANG     ?= clang

WARN       = -Wall -Wextra
OPT        = -O2
INC        = -I.
PTHREAD    = -pthread
BUILD      = tests/build

ASAN       = -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1
TSAN       = -fsanitize=thread -g -O1

# Sanitizers map shadow memory at fixed addresses, which the high ASLR entropy
# on recent kernels can collide with ("unexpected memory mapping"). Run the
# sanitized binaries with ASLR disabled when setarch is available.
ARCH      := $(shell uname -m)
NORAND    := $(shell command -v setarch >/dev/null 2>&1 && echo setarch $(ARCH) -R)

# LeakSanitizer scans for leaks at exit via ptrace, which fails fatally in
# containers / under a debugger. Leaks are covered by valgrind instead, so run
# the ASan binaries with leak detection off. (ASan still checks memory errors.)
ASAN_RUN   = ASAN_OPTIONS=detect_leaks=0 $(NORAND)

# Fewer iterations under the (much slower) sanitizers.
SPSC_BIG  ?= 2000000
SPSC_SMALL?= 200000

.PHONY: all check check-basic check-valgrind check-pool check-spsc check-cpp \
        example clean

all: check

$(BUILD):
	@mkdir -p $(BUILD)

check: check-basic check-valgrind check-pool check-spsc check-cpp example
	@echo
	@echo "===================================================="
	@echo "  all fuq test configurations passed"
	@echo "===================================================="


# ---- single-threaded functional tests ------------------------------------
check-basic: | $(BUILD)
	@echo "== test_basic: gcc C11, small slab =="
	@$(CC) $(WARN) -Werror -std=c11 $(OPT) $(INC) tests/test_basic.c -o $(BUILD)/basic_c11
	@$(BUILD)/basic_c11
	@echo "== test_basic: gcc C11, default slab (4095) =="
	@$(CC) $(WARN) -Werror -std=c11 $(OPT) $(INC) -DTEST_SLAB_SIZE=4095 tests/test_basic.c -o $(BUILD)/basic_c11_big
	@$(BUILD)/basic_c11_big
	@echo "== test_basic: gcc barrier fallback =="
	@$(CC) $(WARN) -Werror -std=c11 -DFUQ_NO_C11_ATOMICS $(OPT) $(INC) tests/test_basic.c -o $(BUILD)/basic_fb
	@$(BUILD)/basic_fb
	@echo "== test_basic: gcc gnu89 (fallback) =="
	@$(CC) $(WARN) -Werror -std=gnu89 $(OPT) $(INC) tests/test_basic.c -o $(BUILD)/basic_89
	@$(BUILD)/basic_89
	@echo "== test_basic: ASan+UBSan, C11 path =="
	@$(CC) $(WARN) -std=c11 $(ASAN) $(INC) tests/test_basic.c -o $(BUILD)/basic_asan
	@$(ASAN_RUN) $(BUILD)/basic_asan
	@echo "== test_basic: ASan+UBSan, fallback path =="
	@$(CC) $(WARN) -std=c11 -DFUQ_NO_C11_ATOMICS $(ASAN) $(INC) tests/test_basic.c -o $(BUILD)/basic_asan_fb
	@$(ASAN_RUN) $(BUILD)/basic_asan_fb
	@echo "== test_basic: clang C11 =="
	@$(CLANG) $(WARN) -Werror -std=c11 $(OPT) $(INC) tests/test_basic.c -o $(BUILD)/basic_clang
	@$(BUILD)/basic_clang


# ---- valgrind memcheck (leak / memory-error detection) -------------------
check-valgrind: | $(BUILD)
	@if command -v valgrind >/dev/null 2>&1; then \
	  echo "== valgrind: test_basic, small slab (C11) =="; \
	  $(CC) $(WARN) -std=c11 -O1 -g $(INC) tests/test_basic.c -o $(BUILD)/basic_vg && \
	  valgrind -q --error-exitcode=1 --leak-check=full $(BUILD)/basic_vg; \
	  echo "== valgrind: test_basic, default slab (C11) =="; \
	  $(CC) $(WARN) -std=c11 -O1 -g -DTEST_SLAB_SIZE=4095 $(INC) tests/test_basic.c -o $(BUILD)/basic_vg_big && \
	  valgrind -q --error-exitcode=1 --leak-check=full $(BUILD)/basic_vg_big; \
	  echo "== valgrind: test_basic, fallback path =="; \
	  $(CC) $(WARN) -std=c11 -DFUQ_NO_C11_ATOMICS -O1 -g $(INC) tests/test_basic.c -o $(BUILD)/basic_vg_fb && \
	  valgrind -q --error-exitcode=1 --leak-check=full $(BUILD)/basic_vg_fb; \
	else \
	  echo "== valgrind not installed, skipping memcheck =="; \
	fi


# ---- slab-pool recycling / leak tests ------------------------------------
check-pool: | $(BUILD)
	@echo "== test_pool: gcc C11 (malloc/free wrapped) =="
	@$(CC) $(WARN) -Werror -std=c11 $(OPT) $(INC) -Wl,--wrap=malloc,--wrap=free tests/test_pool.c -o $(BUILD)/pool_c11
	@$(BUILD)/pool_c11
	@echo "== test_pool: gcc barrier fallback =="
	@$(CC) $(WARN) -Werror -std=c11 -DFUQ_NO_C11_ATOMICS $(OPT) $(INC) -Wl,--wrap=malloc,--wrap=free tests/test_pool.c -o $(BUILD)/pool_fb
	@$(BUILD)/pool_fb


# ---- concurrent SPSC stress ----------------------------------------------
check-spsc: | $(BUILD)
	@echo "== test_spsc: gcc C11, $(SPSC_BIG) iters =="
	@$(CC) $(WARN) -Werror -std=c11 $(OPT) $(PTHREAD) $(INC) tests/test_spsc.c -o $(BUILD)/spsc_c11
	@$(BUILD)/spsc_c11 $(SPSC_BIG)
	@echo "== test_spsc: gcc barrier fallback, $(SPSC_BIG) iters =="
	@$(CC) $(WARN) -Werror -std=c11 -DFUQ_NO_C11_ATOMICS $(OPT) $(PTHREAD) $(INC) tests/test_spsc.c -o $(BUILD)/spsc_fb
	@$(BUILD)/spsc_fb $(SPSC_BIG)
	@echo "== test_spsc: ThreadSanitizer (gcc C11), $(SPSC_SMALL) iters =="
	@$(CC) $(WARN) -std=c11 $(TSAN) $(PTHREAD) $(INC) tests/test_spsc.c -o $(BUILD)/spsc_tsan
	@$(NORAND) $(BUILD)/spsc_tsan $(SPSC_SMALL)
	@echo "== test_spsc: ASan+UBSan (gcc C11), $(SPSC_SMALL) iters =="
	@$(CC) $(WARN) -std=c11 $(ASAN) $(PTHREAD) $(INC) tests/test_spsc.c -o $(BUILD)/spsc_asan
	@$(ASAN_RUN) $(BUILD)/spsc_asan $(SPSC_SMALL)


# ---- C++ (std::atomic back end, plus forced barrier fallback) -------------
check-cpp: | $(BUILD)
	@echo "== C++ std::atomic: g++ test_basic =="
	@$(CXX) $(WARN) -Werror -std=c++11 $(OPT) $(INC) -x c++ tests/test_basic.c -o $(BUILD)/basic_cpp
	@$(BUILD)/basic_cpp
	@echo "== C++ std::atomic: g++ test_spsc, $(SPSC_BIG) iters =="
	@$(CXX) $(WARN) -Werror -std=c++11 $(OPT) $(PTHREAD) $(INC) -x c++ tests/test_spsc.c -o $(BUILD)/spsc_cpp
	@$(BUILD)/spsc_cpp $(SPSC_BIG)
	@echo "== C++ std::atomic: ThreadSanitizer (g++), $(SPSC_SMALL) iters =="
	@$(CXX) $(WARN) -std=c++11 $(TSAN) $(PTHREAD) $(INC) -x c++ tests/test_spsc.c -o $(BUILD)/spsc_cpp_tsan
	@$(NORAND) $(BUILD)/spsc_cpp_tsan $(SPSC_SMALL)
	@echo "== C++ barrier fallback (forced): compile + functional smoke only =="
	@echo "   (barrier path is functional-only, not a race-verified concurrent backend)"
	@$(CXX) $(WARN) -Werror -std=c++11 -DFUQ_NO_C11_ATOMICS $(OPT) $(INC) -x c++ tests/test_basic.c -o $(BUILD)/basic_cpp_fb
	@$(BUILD)/basic_cpp_fb


# ---- example --------------------------------------------------------------
example: | $(BUILD)
	@echo "== example/sporadic =="
	@$(CC) $(WARN) -std=gnu89 $(OPT) $(PTHREAD) $(INC) example/sporadic.c -o $(BUILD)/sporadic
	@$(BUILD)/sporadic


clean:
	@rm -rf $(BUILD)
