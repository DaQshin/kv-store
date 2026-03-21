CXX := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -Iinclude

BUILD := build

.PHONY: all clean run

all: $(BUILD)/client $(BUILD)/server

$(BUILD)/client: src/client.cpp logging/log.cpp
	mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(BUILD)/server: src/server.cpp src/storage/hashtable.cpp logging/log.cpp
	mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $^ -o $@

run_server: all 
			./$(BUILD)/server --port $(PORT)

run_client: all 
			./$(BUILD)/client $(CMD)

clean:
	rm -rf $(BUILD)