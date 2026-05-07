CXX = g++
CXXFLAGS = -Wall -Wextra -std=c++17 -pthread

# Uncomment the one LIBS line that matches your GUI choice:
# LIBS = -lsfml-graphics -lsfml-window -lsfml-audio -lsfml-network -lsfml-system -lrt
# LIBS = $(shell sdl2-config --libs) -lrt
# LIBS = -lglfw -lGL -lrt
LIBS = -lncurses -lrt

BIN_DIR = bin
TARGETS = $(BIN_DIR)/arbiter $(BIN_DIR)/hip $(BIN_DIR)/asp

all: clean $(TARGETS)
	@echo Build complete.

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(BIN_DIR)/arbiter: arbiter/arbiter.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) arbiter/*.cpp shared.cpp -o $@ $(LIBS)

$(BIN_DIR)/hip: hip/hip.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) hip/*.cpp shared.cpp -o $@ $(LIBS)

$(BIN_DIR)/asp: asp/asp.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) asp/*.cpp shared.cpp -o $@ $(LIBS)

clean:
	rm -f $(TARGETS)

.PHONY: all clean
