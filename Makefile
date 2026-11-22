CXX ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic
PYTHON ?= python3
ARGS ?=

.PHONY: all console web test clean
all: build/voltos

build/voltos: src/main.cpp src/station.cpp src/station.hpp
	mkdir -p build
	$(CXX) $(CXXFLAGS) -pthread src/main.cpp src/station.cpp -o $@

console: all
	./build/voltos $(ARGS)

web: all
	$(PYTHON) web/server.py $(ARGS)

test: all
	$(PYTHON) tests/integration.py

clean:
	rm -rf build
