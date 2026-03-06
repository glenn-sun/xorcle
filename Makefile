.PHONY: all profile release test clean rebuild

all:
	mkdir -p bin
	/usr/bin/g++ -std=c++23 -pthread -Og -g -Wall -fdiagnostics-show-template-tree -fdiagnostics-color=auto -fno-omit-frame-pointer -I . -I src $(shell find src -name '*.cpp') -o bin/main

profile:
	mkdir -p bin build/obj-profile
	# Compile each .cpp into a real .o file (not a temp file), so dsymutil can find it.
	find src -name '*.cpp' | while read -r src_file; do \
		obj_file="build/obj-profile/$${src_file#src/}.o"; \
		mkdir -p "$$(dirname "$$obj_file")"; \
		/usr/bin/g++ -std=c++23 -pthread -O2 -g3 -Wall \
			-fdiagnostics-show-template-tree -fdiagnostics-color=auto \
			-fno-omit-frame-pointer -mno-omit-leaf-frame-pointer \
			-fno-optimize-sibling-calls \
			-fno-split-machine-functions \
			-fdebug-info-for-profiling \
			-I . -I src -c "$$src_file" -o "$$obj_file"; \
	done
	/usr/bin/g++ -pthread $$(find build/obj-profile -name '*.o') -o bin/main-profile
	dsymutil bin/main-profile

release:
	mkdir -p bin
	/usr/bin/g++ -std=c++23 -pthread -O3 -DNDEBUG -Wall -fdiagnostics-show-template-tree -fdiagnostics-color=auto -I . -I src $(shell find src -name '*.cpp') -o bin/main-release

test: all
	./bin/main tests/bijphp_n_n+1/bijphp_6.cnf

clean:
	rm -rf bin build

rebuild: clean all
