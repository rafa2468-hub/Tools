# Builds and runs the clock's rendering logic on the host. The sketch draws
# everything itself, so only "set one pixel" is stubbed - these tests run
# the real line, circle and glyph code. No hardware or toolchain needed:
#
#   make          build and run the tests
#   make preview  render the face to preview.png
#   make clean
#
# The -D switches redirect the sketch's clock reads to a time source the
# test drives, so a full 60-second sweep runs in a fraction of a second.

CXX      ?= g++
CXXFLAGS := -std=c++17 -O1 -Wall -Wno-unused-variable
INCLUDES := -I stubs -I ../analog_clock
PRELUDE  := -include stubs/panel_host.h \
            -include stubs/WiFi.h \
            -include testtime.h
TIMEHOOK := -DHOSTTEST=1 \
            -Dgettimeofday=test_gettimeofday \
            -Dsettimeofday=test_settimeofday

SKETCH := ../analog_clock/analog_clock.ino

.PHONY: all clean ino-check preview
all: ino-check test_clock
	./test_clock

# Renders the face to preview.png using the sketch's own drawing code, so
# you can see what the panel will show before flashing anything. Optional
# time argument, e.g.  make preview T="3 25 40"
preview: preview.cpp $(SKETCH) stubs/*.h testtime.h
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(PRELUDE) $(TIMEHOOK) preview.cpp -o preview
	./preview $(T)

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
	    -DHOSTTEST=1 -include stubs/panel_host.h -include stubs/WiFi.h \
	    .ino_generated.cpp && echo "PASS  sketch survives Arduino IDE prototype injection"

clean:
	rm -f test_clock .ino_generated.cpp
