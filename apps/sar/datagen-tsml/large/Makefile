# Synthetic Aperture Radar (SAR) point target simulator.
# USAGE: datagen <Data output file>
#                <Platform position outputfile>
#                <Pulse transmission time output file>
#                <Radar parameters>
# NOTES: The code expects Targets.txt and Parameters.txt to be
# located in the working directory.

INSTALL_DIR = ../../datasets/large

RM=rm -f
CP=cp -f
MKDIR=mkdir -p

TG_AS=../../../../../tg/tg/install/bin/xstg-linux-elf-as

all:	Radar.txt

Radar.txt:	Parameters.txt Targets.txt
	../datagen Data.bin PlatformPosition.bin PulseTransmissionTime.bin Radar.txt
	../../utils/viewData 1 500.0 <Data.bin >ref.bmp
	../../utils/viewData 2 250.0 <Data.bin >cur.bmp

run:	all

clean:
	$(RM)	Data.bin PlatformPosition.bin PulseTransmissionTime.bin Radar.txt \
		ref.bmp cur.bmp

install: all
	$(MKDIR) $(INSTALL_DIR) $(INSTALL_DIR)/x86
	$(CP)	Data.bin PlatformPosition.bin PulseTransmissionTime.bin \
		Parameters.txt Targets.txt Radar.txt \
		$(INSTALL_DIR)
	@if [ ! -f $(INSTALL_DIR)/x86/Data.c ]; then \
		(cd ../../ocr/large;../../utils/dataImage 1 <$(INSTALL_DIR)/Data.bin >$(INSTALL_DIR)/x86/Data.c); \
	fi
	@if [ ! -f $(INSTALL_DIR)/x86/PlatformPosition.c ]; then \
		(cd ../../ocr/large;../../utils/dataImage 2 <$(INSTALL_DIR)/PlatformPosition.bin >$(INSTALL_DIR)/x86/PlatformPosition.c); \
	fi
	@if [ ! -f $(INSTALL_DIR)/x86/PulseTransmissionTime.c ]; then \
		(cd ../../ocr/large;../../utils/dataImage 3 <$(INSTALL_DIR)/PulseTransmissionTime.bin >$(INSTALL_DIR)/x86/PulseTransmissionTime.c); \
	fi
	@if [ -x "$(TG_AS)" ]; then \
		$(MKDIR) $(INSTALL_DIR)/tg; \
		if [ ! -f $(INSTALL_DIR)/tg/Data.s ]; then \
			(cd ../../ocr/large;../../utils/dataImageAsm 1 <$(INSTALL_DIR)/Data.bin >$(INSTALL_DIR)/tg/Data.s); \
		fi; \
		if [ ! -f $(INSTALL_DIR)/tg/PlatformPosition.s ]; then \
			(cd ../../ocr/large;../../utils/dataImageAsm 2 <$(INSTALL_DIR)/PlatformPosition.bin >$(INSTALL_DIR)/tg/PlatformPosition.s); \
		fi; \
		if [ ! -f $(INSTALL_DIR)/tg/PulseTransmissionTime.s ]; then \
			(cd ../../ocr/large;../../utils/dataImageAsm 3 <$(INSTALL_DIR)/PulseTransmissionTime.bin >$(INSTALL_DIR)/tg/PulseTransmissionTime.s); \
		fi; \
	fi
	@if [ ! -f $(INSTALL_DIR)/x86/Data.o ]; then \
		$(CC) -c -o $(INSTALL_DIR)/x86/Data.o $(INSTALL_DIR)/x86/Data.c; \
	fi
	@if [ ! -f $(INSTALL_DIR)/x86/PlatformPosition.o ]; then \
		$(CC) -c -o $(INSTALL_DIR)/x86/PlatformPosition.o $(INSTALL_DIR)/x86/PlatformPosition.c; \
	fi
	@if [ ! -f $(INSTALL_DIR)/x86/PulseTransmissionTime.o ]; then \
		$(CC) -c -o $(INSTALL_DIR)/x86/PulseTransmissionTime.o $(INSTALL_DIR)/x86/PulseTransmissionTime.c; \
	fi
	@if [ -x "$(TG_AS)" ]; then \
		if [ ! -f $(INSTALL_DIR)/tg/Data.o ]; then \
			$(TG_AS) -o $(INSTALL_DIR)/tg/Data.o $(INSTALL_DIR)/tg/Data.s; \
		fi; \
		if [ ! -f $(INSTALL_DIR)/tg/PlatformPosition.o ]; then \
			$(TG_AS) -o $(INSTALL_DIR)/tg/PlatformPosition.o $(INSTALL_DIR)/tg/PlatformPosition.s; \
		fi; \
		if [ ! -f $(INSTALL_DIR)/tg/PulseTransmissionTime.o ]; then \
			$(TG_AS) -o $(INSTALL_DIR)/tg/PulseTransmissionTime.o $(INSTALL_DIR)/tg/PulseTransmissionTime.s; \
		fi; \
	fi

uninstall:
	$(RM)	$(INSTALL_DIR)/Data.bin \
		$(INSTALL_DIR)/PlatformPosition.bin \
		$(INSTALL_DIR)/PulseTransmissionTime.bin \
		$(INSTALL_DIR)/Parameters.txt \
		$(INSTALL_DIR)/Targets.txt \
		$(INSTALL_DIR)/Radar.txt \
		$(INSTALL_DIR)/x86/Data.c \
		$(INSTALL_DIR)/x86/PlatformPosition.c \
		$(INSTALL_DIR)/x86/PulseTransmissionTime.c \
		$(INSTALL_DIR)/x86/Data.o \
		$(INSTALL_DIR)/x86/PlatformPosition.o \
		$(INSTALL_DIR)/x86/PulseTransmissionTime.o
	@if [ -d "$(INSTALL_DIR)/tg" ]; then \
		$(RM) $(INSTALL_DIR)/tg/Data.s \
		      $(INSTALL_DIR)/tg/PlatformPosition.s \
		      $(INSTALL_DIR)/tg/PulseTransmissionTime.s \
		      $(INSTALL_DIR)/tg/Data.o \
		      $(INSTALL_DIR)/tg/PlatformPosition.o \
		      $(INSTALL_DIR)/tg/PulseTransmissionTime.o; \
	fi
