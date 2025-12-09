
.PHONY: all
all:
	mkdir -p bin
	/usr/bin/g++ -std=c++23 -pthread -g -fdiagnostics-color=always -I . src/*.cpp -o bin/main

.PHONY: test
test: all
	./bin/main -d tests

.PHONY: clean
clean:
	rm -rf bin