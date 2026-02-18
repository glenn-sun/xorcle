
.PHONY: all
all:
	mkdir -p bin
	/usr/bin/g++ -std=c++23 -pthread -g -fdiagnostics-show-template-tree -fno-elide-constructors -fdiagnostics-color=always -Wall -I . -I src $(shell find src -name '*.cpp') -o bin/main

.PHONY: test
test: all
	./bin/main -d tests -p cvsids

.PHONY: clean
clean:
	rm -rf bin
