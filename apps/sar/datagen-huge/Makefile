
SRCS =	src/fir_filter.c \
	src/gen_rand.c \
	src/main.c \
	src/inputs.c \
	src/fftw3.c

CFLAGS = -fopenmp -Wall -O3 -g -DRAG -DRAG_PARAM_ONLY_off
LDFLAGS = -lm

RM=rm -f

# Check if mpicc is available
HAS_MPICC := $(shell which mpicc 2>/dev/null)

# Build targets
ifeq ($(HAS_MPICC),)
all: datagen
else
all: datagen mpidatagen
endif

datagen: $(SRCS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)

# MPI datagen
mpidatagen: $(SRCS)
	mpicc $(CFLAGS) -DUSE_MPI -o $@ $(SRCS) $(LDFLAGS)

run:
	$(MAKE) -C ./huge run
# 	$(MAKE) -C ./test run

clean:
	$(RM) datagen mpidatagen
	$(MAKE) -C ./huge clean
# 	$(MAKE) -C ./test clean

install: all
	$(MAKE) -C ./huge install
# 	$(MAKE) -C ./test install

uninstall:
	$(MAKE) -C ./huge uninstall
# 	$(MAKE) -C ./test uninstall
