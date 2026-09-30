# Laptop build of the tide display's drawing code. Needs a C++ compiler:
# on a Mac, `xcode-select --install` once if `make` complains.
SRC_DIR  := ../tide_standalone
CORE     := $(SRC_DIR)/tide_core.cpp $(SRC_DIR)/canvas.cpp $(SRC_DIR)/chart.cpp
HEADERS  := $(wildcard $(SRC_DIR)/*.h)
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -I$(SRC_DIR)

all: preview test

preview: preview.cpp $(CORE) $(HEADERS)
	$(CXX) $(CXXFLAGS) -o $@ preview.cpp $(CORE)

test: test.cpp $(CORE) $(HEADERS)
	$(CXX) $(CXXFLAGS) -o $@ test.cpp $(CORE)
	./test

clean:
	rm -f preview test *.bmp *.bin

.PHONY: all clean
