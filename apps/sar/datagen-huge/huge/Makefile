# Synthetic Aperture Radar (SAR) point target simulator.
# USAGE: datagen <Data output file>
#                <Platform position outputfile>
#                <Pulse transmission time output file>
#                <Radar parameters>
# NOTES: The code expects Targets.txt and Parameters.txt to be
# located in the working directory.

INSTALL_DIR = ../../datasets/huge

RM=rm -f
CP=cp -f
MKDIR=mkdir -p

# Slurm settings (can be overridden)
MPI_NODES ?= 33
# Default to one rank per node; adjust MPI_TASKS if you want a different layout
MPI_TASKS ?= $(MPI_NODES)
# OpenMP threads per MPI task (adjust based on cores per node)
OMP_THREADS ?= 64

all:	Radar.txt

Radar.txt:	Parameters.txt Targets.txt
	@if [ -x "../mpidatagen" ]; then \
		if command -v srun >/dev/null 2>&1; then \
			echo "Running mpidatagen with Slurm (nodes=$(MPI_NODES), tasks=$(MPI_TASKS), threads=$(OMP_THREADS))"; \
			OMP_NUM_THREADS=$(OMP_THREADS) srun -N $(MPI_NODES) -n $(MPI_TASKS) --cpus-per-task=$(OMP_THREADS) --exclusive --mpi=pmi2 ../mpidatagen Data.bin PlatformPosition.bin PulseTransmissionTime.bin Radar.txt; \
		else \
			echo "Running mpidatagen with mpirun"; \
			mpirun -np $(MPI_NODES) ../mpidatagen Data.bin PlatformPosition.bin PulseTransmissionTime.bin Radar.txt; \
		fi; \
	else \
		echo "Running datagen (serial)"; \
		../datagen Data.bin PlatformPosition.bin PulseTransmissionTime.bin Radar.txt; \
	fi

run:	all
	../../utils/viewData 1 500.0 <Data.bin >ref.bmp
	../../utils/viewData 2 250.0 <Data.bin >cur.bmp

clean:
	$(RM)	Data.bin PlatformPosition.bin PulseTransmissionTime.bin Radar.txt \
		ref.bmp cur.bmp

install:	all
	$(MKDIR) $(INSTALL_DIR)
	$(CP)	Data.bin PlatformPosition.bin PulseTransmissionTime.bin \
		Parameters.txt Targets.txt Radar.txt \
		$(INSTALL_DIR)

uninstall:
	$(RM)	$(INSTALL_DIR)/Data.bin \
		$(INSTALL_DIR)/PlatformPosition.bin \
		$(INSTALL_DIR)/PulseTransmissionTime.bin \
		$(INSTALL_DIR)/Parameters.txt \
		$(INSTALL_DIR)/Targets.txt \
		$(INSTALL_DIR)/Radar.txt
