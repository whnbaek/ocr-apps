OCR_INPUT_HOME=../../../apps
PROG=ocr_mkl_cholesky
BUILD_DIR=build/x86
INSTALL_DIR=install/x86
CFLAGS= -O3 -g -I${OCR_INSTALL}/include -DOCR_TYPE_H=x86.h
CLIBS= -L${OCR_INSTALL}/lib -locr_$(OCR_TYPE) -lmkl_intel_lp64 -lmkl_gnu_thread -lmkl_core -lm -fopenmp
OCR_INPUT=${OCR_INPUT_HOME}/cholesky/datasets

ifndef OCR_TYPE
$(error OCR_TYPE not set)
endif

ifndef OCR_INSTALL
$(error OCR_INSTALL not set)
endif

ifndef OCR_CONFIG
OCR_CONFIG=${OCR_INSTALL}/share/ocr/config/$(OCR_TYPE)/default.cfg
$(warning OCR_CONFIG not set, defaulting to ${OCR_CONFIG})
endif

ifndef OCR_INPUT_HOME
$(error OCR_INPUT_HOME not set)
endif

OCR_RUN_FLAGS=-ocr:cfg ${OCR_CONFIG}

all: compile
all-test: compile run verify

MATRIX_SIZE=50
TILE_SIZE=5

INPUT_FILE=m_${MATRIX_SIZE}.in
OUTPUT_FILE=cholesky_out_${MATRIX_SIZE}.txt


compile: $(BUILD_DIR)
	$(CC) -g $(CFLAGS) -I. $(PROG).c -o $(BUILD_DIR)/$(PROG) $(CLIBS)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)


compile.omp.task: $(BUILD_DIR)
	$(CC) -fopenmp $(CFLAGS) $(PROG).omp.task.c -o $(BUILD_DIR)/$(PROG).omp.task


compile_pp: $(BUILD_DIR)
	$(CPP) $(CFLAGS) -I. $(PROG).cpp -o $(BUILD_DIR)/$(PROG) $(CLIBS)


run:
	$(BUILD_DIR)/$(PROG) $(OCR_RUN_FLAGS) ${MATRIX_SIZE} ${TILE_SIZE} ${OCR_INPUT}/${INPUT_FILE}


run.omp.task:
	$(BUILD_DIR)/$(PROG).omp.task ${MATRIX_SIZE} ${TILE_SIZE} ${OCR_INPUT}/${INPUT_FILE}


clean:
	-rm -Rf *.o $(BUILD_DIR) $(INSTALL_DIR) $(PROG).out *.csv

install: compile
	mkdir -p $(INSTALL_DIR)
	cp $(BUILD_DIR)/$(PROG) $(INSTALL_DIR)/

verify:
	diff -b ${OCR_INPUT}/$(OUTPUT_FILE) $(PROG).out
