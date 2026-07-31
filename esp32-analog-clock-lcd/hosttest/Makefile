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

SKETCH := ../analog_clock/analog_clock.ino

.PHONY: all clean ino-check
all: ino-check test_clock
	./test_clock

test_clock: test_clock.cpp $(SKETCH) stubs/*.h testtime.h
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(PRELUDE) $(TIMEHOOK) $< -o $@

# The Arduino IDE rewrites a .ino before compiling it: it generates
# prototypes for every function and injects them ahead of the first
# function definition. That reorders the file in a way plain C++ never
# does, and it breaks the build if a type used in a signature is declared
# further down. PlatformIO and this harness both compile the .ino as
# ordinary C++ and so sail straight past the problem - this target is what
# catches it.
ino-check:
	@command -v python3 >/dev/null || { echo "skipping ino-check (no python3)"; exit 0; }
	@python3 ino_preprocess.py $(SKETCH) .ino_generated.cpp
	@$(CXX) $(CXXFLAGS) -fsyntax-only $(INCLUDES) \
	    -include stubs/Arduino_GFX_Library.h -include stubs/WiFi.h \
	    .ino_generated.cpp && echo "PASS  sketch survives Arduino IDE prototype injection"

clean:
	rm -f test_clock .ino_generated.cpp
