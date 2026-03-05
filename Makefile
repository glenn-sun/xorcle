.PHONY: all profile release test clean rebuild

all:
	mkdir -p bin
	/usr/bin/g++ -std=c++23 -pthread -Og -g -Wall -fdiagnostics-show-template-tree -fdiagnostics-color=auto -fno-omit-frame-pointer -I . -I src $(shell find src -name '*.cpp') -o bin/main

profile:
	mkdir -p bin
	/usr/bin/g++ -std=c++23 -pthread -O2 -g -Wall -fdiagnostics-show-template-tree -fdiagnostics-color=auto -fno-omit-frame-pointer -I . -I src $(shell find src -name '*.cpp') -o bin/main-profile

release:
	mkdir -p bin
	/usr/bin/g++ -std=c++23 -pthread -O3 -DNDEBUG -Wall -fdiagnostics-show-template-tree -fdiagnostics-color=auto -I . -I src $(shell find src -name '*.cpp') -o bin/main-release

test: all
	./bin/main tests/bphp/bphp-6-5.cnf

clean:
	rm -rf bin build

rebuild: clean all
