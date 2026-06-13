CXX ?= g++
CXXFLAGS ?= -Os -Wall -Wextra -std=c++17
LDFLAGS ?= -s

TARGET := ntp-server
SRC := src/ntp_server.cpp

.PHONY: all clean static install

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

# Fully static binary (no libc/libstdc++ dependency at runtime).
static: $(SRC)
	$(CXX) $(CXXFLAGS) -static -o $(TARGET) $< $(LDFLAGS)

clean:
	rm -f $(TARGET)

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/sbin/$(TARGET)
