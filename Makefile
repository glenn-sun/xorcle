.PHONY: all debug profile test clean rebuild

CXX ?= c++
SRC_FILES := $(shell find src -name '*.cpp')
COMMON_CPPFLAGS := -I . -I src
COMMON_CXXFLAGS := -std=c++23 -pthread -Wall -fdiagnostics-show-template-tree -fdiagnostics-color=auto
DEBUG_CXXFLAGS := -Og -g -fno-omit-frame-pointer
RELEASE_CXXFLAGS := -O3 -DNDEBUG
PROFILE_CXXFLAGS := -O2 -DNDEBUG -g3 -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer -fno-optimize-sibling-calls -fno-split-machine-functions -fdebug-info-for-profiling

all: bin/xorcle

bin/xorcle:
	mkdir -p bin
	$(CXX) $(COMMON_CXXFLAGS) $(RELEASE_CXXFLAGS) $(COMMON_CPPFLAGS) $(SRC_FILES) -o $@

debug: bin/xorcle-debug

bin/xorcle-debug:
	mkdir -p bin
	$(CXX) $(COMMON_CXXFLAGS) $(DEBUG_CXXFLAGS) $(COMMON_CPPFLAGS) $(SRC_FILES) -o $@

profile:
	mkdir -p bin build/obj-profile
	find src -name '*.cpp' | while read -r src_file; do \
		obj_file="build/obj-profile/$${src_file#src/}.o"; \
		mkdir -p "$$(dirname "$$obj_file")"; \
		$(CXX) $(COMMON_CXXFLAGS) $(PROFILE_CXXFLAGS) $(COMMON_CPPFLAGS) -c "$$src_file" -o "$$obj_file"; \
	done
	$(CXX) -pthread $$(find build/obj-profile -name '*.o') -o bin/xorcle-profile
	if command -v dsymutil >/dev/null 2>&1; then dsymutil bin/xorcle-profile; fi

test: all
	./bin/xorcle tests/generated/random_kxnf_n/random_kxnf_n_k2_n11.cnf

clean:
	rm -rf bin build

rebuild: clean all
