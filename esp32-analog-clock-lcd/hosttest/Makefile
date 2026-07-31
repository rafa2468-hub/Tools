# Builds and runs the clock's rendering logic on the host, against stubbed
# Arduino/Arduino_GFX headers. No hardware or toolchain needed:
#
#   make        build and run
#   make clean
#
# The -D switches redirect the sketch's clock reads to a time source the
# test drives, so a full 60-second sweep runs in a fraction of a second.

CXX      ?= g++
CXXFLAGS := -std=c++17 -O1 -Wall -Wno-unused-variable
INCLUDES := -I stubs -I ../analog_clock
PRELUDE  := -include stubs/Arduino_GFX_Library.h \
            -include stubs/WiFi.h \
            -include testtime.h
TIMEHOOK := -Dgettimeofday=test_gettimeofday \
            -Dsettimeofday=test_settimeofday

.PHONY: all clean
all: test_clock
	./test_clock

test_clock: test_clock.cpp ../analog_clock/analog_clock.ino stubs/*.h testtime.h
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(PRELUDE) $(TIMEHOOK) $< -o $@

clean:
	rm -f test_clock
