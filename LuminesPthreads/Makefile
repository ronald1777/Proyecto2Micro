CXX ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -pedantic -O2
LDFLAGS ?= -pthread

TARGET = lumines
ifeq ($(OS),Windows_NT)
TARGET = lumines.exe
endif

.PHONY: all run self-test clean

all: $(TARGET)

$(TARGET): src/main.cpp
	$(CXX) $(CXXFLAGS) src/main.cpp -o $(TARGET) $(LDFLAGS)

run: $(TARGET)
	./$(TARGET)

self-test: $(TARGET)
	./$(TARGET) --self-test

clean:
ifeq ($(OS),Windows_NT)
	-del /Q lumines.exe high_scores.txt 2>NUL
else
	rm -f lumines high_scores.txt
endif
