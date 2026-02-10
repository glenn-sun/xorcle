
.PHONY: all
all:
	mkdir -p bin
	/usr/bin/g++ -std=c++23 -pthread -g -fdiagnostics-show-template-tree -fno-elide-constructors -fdiagnostics-color=always -Wall -I . src/*.cpp -o bin/main

.PHONY: test
test: all
	./bin/main -d tests -p vsids_clause

.PHONY: clean
clean:
	rm -rf bin