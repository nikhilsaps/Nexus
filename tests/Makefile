# Host tests. No ESP-IDF needed: `make test` builds and runs every test_*.cpp.
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O1 -g -Wall -Wextra -Werror -I../main

SRCS := $(wildcard test_*.cpp)
BINS := $(SRCS:.cpp=)

test: $(BINS)
	@set -e; for b in $(BINS); do echo "== $$b"; ./$$b; done

%: %.cpp ../main/safe_replace.h
	$(CXX) $(CXXFLAGS) -o $@ $<

asan: CXXFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
asan: clean test

clean:
	rm -f $(BINS)

.PHONY: test asan clean
