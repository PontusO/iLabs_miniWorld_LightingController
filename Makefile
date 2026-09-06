# Build wrapper for the miniWorld lighting controller.
#
# The arduino-pico core does not assemble .pio files that live in a sketch,
# so this Makefile does it: every <name>.pio in the sketch folder produces
# <name>.pio.h through pioasm, and only when the .pio is newer. The compile
# and upload targets depend on that step, so a changed PIO program can never
# be built against a stale header.
#
#   make            compile only
#   make upload     compile and flash, PORT=/dev/ttyACM0 by default
#   make pio        assemble the PIO headers and stop
#   make clean      remove generated PIO headers
#
# Override ARDUINO_CLI, PIOASM, FQBN or PORT on the command line if the
# defaults do not match the machine.
#
# Invector Embedded Systems AB

SKETCH      := miniWorld_LightingController
BOARD       := rp2040:rp2040:challenger_nb_2040_wifi
FLASH       := 8388608_1048576
FQBN        := $(BOARD):flash=$(FLASH)
PORT        ?= /dev/ttyACM0

ARDUINO_CLI ?= $(firstword $(wildcard $(HOME)/bin/arduino-cli) $(shell command -v arduino-cli 2>/dev/null))
PIOASM      ?= $(lastword $(sort $(wildcard $(HOME)/.arduino15/packages/rp2040/tools/pqt-pioasm/*/pioasm)))

PIO_SRC     := $(wildcard $(SKETCH)/*.pio)
PIO_HDR     := $(PIO_SRC:.pio=.pio.h)

.PHONY: all compile upload pio clean check-tools

all: compile

pio: check-tools $(PIO_HDR)

%.pio.h: %.pio
	$(PIOASM) -o c-sdk $< $@

compile: pio
	$(ARDUINO_CLI) compile --fqbn $(FQBN) --warnings default $(SKETCH)

upload: pio
	$(ARDUINO_CLI) upload --fqbn $(FQBN) -p $(PORT) $(SKETCH)

clean:
	rm -f $(PIO_HDR)

check-tools:
	@test -x "$(PIOASM)" || { echo "pioasm not found; set PIOASM=/path/to/pioasm"; exit 1; }
	@test -x "$(ARDUINO_CLI)" || { echo "arduino-cli not found; set ARDUINO_CLI=/path/to/arduino-cli"; exit 1; }
