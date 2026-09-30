# Simulated run of the full sketch on a laptop (see sim_main.cpp).
SK := ../../tide_standalone
SRC := $(SK)/tide_core.cpp $(SK)/canvas.cpp $(SK)/chart.cpp $(SK)/battery.cpp \
       $(SK)/net.cpp $(SK)/panel_waveshare.cpp
CXXFLAGS := -std=gnu++17 -O1 -g -Wall -Wextra -Istubs -I$(SK) -include Arduino.h

sim: sim_main.cpp sketch.o $(SRC) $(wildcard $(SK)/*.h) $(wildcard stubs/*.h)
	$(CXX) $(CXXFLAGS) -o $@ sim_main.cpp sketch.o $(SRC)

sketch.o: $(SK)/tide_standalone.ino $(wildcard $(SK)/*.h) $(wildcard stubs/*.h)
	$(CXX) $(CXXFLAGS) -x c++ -c -o $@ $(SK)/tide_standalone.ino

clean:
	rm -rf sim sketch.o out_* *.log
