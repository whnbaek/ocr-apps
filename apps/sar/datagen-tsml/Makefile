OCR_INSTALL ?=
OCR_TYPE ?= x86

SOURCES = \
	src/fir_filter.c \
	src/gen_rand.c \
	src/main.c \
	src/inputs.c \
	src/fftw3.c

CC=gcc
CFLAGS= -g -std=gnu99  -DRAG_ADD_ERROR=1 -fopenmp
LDFLAGS=-lm
RM=rm -f

all: datagen ../utils/viewData

../utils/viewData: ../utils/viewData.c
	$(MAKE) -C ../utils viewData

../utils/dataImage: ../utils/dataImage.c
	$(MAKE) -C ../utils dataImage

../utils/dataImageAsm: ../utils/dataImageAsm.c
	$(MAKE) -C ../utils dataImageAsm

datagen: $(SOURCES)
	$(CC) $(CFLAGS) -o $@ -O $(SOURCES) $(LDFLAGS)

run:	all
	$(MAKE) -C ./tiny   run
	$(MAKE) -C ./small  run
	$(MAKE) -C ./medium run
	$(MAKE) -C ./large  run

clean:
	$(RM) datagen
	$(MAKE) -C ./tiny   clean
	$(MAKE) -C ./small  clean
	$(MAKE) -C ./medium clean
	$(MAKE) -C ./large  clean

install: all ../utils/dataImage ../utils/dataImageAsm
	$(MAKE) -C ./tiny   install
	$(MAKE) -C ./small  install
	$(MAKE) -C ./medium install
	$(MAKE) -C ./large  install

uninstall:
	$(MAKE) -C ./tiny   uninstall
	$(MAKE) -C ./small  uninstall
	$(MAKE) -C ./medium uninstall
	$(MAKE) -C ./large  uninstall

tiny:	all
	$(MAKE) -C ./tiny run

small:	all
	$(MAKE) -C ./small run

medium:	all
	$(MAKE) -C ./medium run

large:	all
	$(MAKE) -C ./large run
