.PHONY: all debug profile test clean rebuild
CXX ?= c++
SRC_FILES := $(shell find src -name '*.cpp')
COMMON_CPPFLAGS := -I . -I src
COMMON_CXXFLAGS := -std=c++23 -pthread -Wall -fdiagnostics-show-template-tree -fdiagnostics-color=auto
DEBUG_CXXFLAGS := -Og -g -fno-omit-frame-pointer
RELEASE_CXXFLAGS := -O3 -DNDEBUG
PROFILE_CXXFLAGS := -O2 -DNDEBUG -g3 -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer -fno-optimize-sibling-calls -fno-split-machine-functions -fdebug-info-for-profiling

OBJ_RELEASE := $(patsubst src/%.cpp, build/obj-release/%.o, $(SRC_FILES))
OBJ_DEBUG   := $(patsubst src/%.cpp, build/obj-debug/%.o,   $(SRC_FILES))
OBJ_PROFILE := $(patsubst src/%.cpp, build/obj-profile/%.o, $(SRC_FILES))

all: bin/xorcle
debug: bin/xorcle-debug
profile: bin/xorcle-profile

# --- Release ---
build/obj-release/%.o: src/%.cpp
	mkdir -p $(dir $@)
	$(CXX) $(COMMON_CXXFLAGS) $(RELEASE_CXXFLAGS) $(COMMON_CPPFLAGS) -c $< -o $@

bin/xorcle: $(OBJ_RELEASE)
	mkdir -p bin
	$(CXX) -pthread $^ -o $@

# --- Debug ---
build/obj-debug/%.o: src/%.cpp
	mkdir -p $(dir $@)
	$(CXX) $(COMMON_CXXFLAGS) $(DEBUG_CXXFLAGS) $(COMMON_CPPFLAGS) -c $< -o $@

bin/xorcle-debug: $(OBJ_DEBUG)
	mkdir -p bin
	$(CXX) -pthread $^ -o $@

# --- Profile ---
build/obj-profile/%.o: src/%.cpp
	mkdir -p $(dir $@)
	$(CXX) $(COMMON_CXXFLAGS) $(PROFILE_CXXFLAGS) $(COMMON_CPPFLAGS) -c $< -o $@

bin/xorcle-profile: $(OBJ_PROFILE)
	mkdir -p bin
	$(CXX) -pthread $^ -o $@
	if command -v dsymutil >/dev/null 2>&1; then dsymutil $@; fi

# --- Misc ---
test: all
	./bin/xorcle tests/generated/random_kxnf_n/random_kxnf_n_k2_n11.cnf

clean:
	rm -rf bin build

rebuild: clean all
