CC=gcc
CFLAGS=-g -std=c99 -DRAG
LDFLAGS= -lm

all:	viewImage    viewCorr    dataImage    viewData \
	viewImageTxt viewCorrTxt dataImageAsm detects_ascii_to_txt

viewImage:	common.h      viewImage.c ../ocr/src/inputs.c Makefile
	$(CC) $(CFLAGS) -I../ocr/src -o  viewImage viewImage.c inputs.c $(LDFLAGS)

viewCorr:	common.h     viewCorr.c ../ocr/src/inputs.c Makefile
	$(CC) $(CFLAGS) -I../ocr/src -o viewCorr viewCorr.c inputs.c $(LDFLAGS)

viewImageTxt:	common.h     viewImageTxt.c ../ocr/src/inputs.c Makefile
	$(CC) $(CFLAGS) -I../ocr/src -o viewImageTxt viewImageTxt.c inputs.c $(LDFLAGS)

viewCorrTxt:	common.h     viewCorrTxt.c ../ocr/src/inputs.c Makefile
	$(CC) $(CFLAGS) -I../ocr/src -o viewCorrTxt viewCorrTxt.c inputs.c $(LDFLAGS)

dataImage:	common.h      dataImage.c ../ocr/src/inputs.c Makefile
	$(CC) $(CFLAGS) -I../ocr/src -o  dataImage dataImage.c inputs.c $(LDFLAGS)

dataImageAsm:	common.h     dataImageAsm.c ../ocr/src/inputs.c Makefile
	$(CC) $(CFLAGS) -I../ocr/src -o dataImageAsm dataImageAsm.c inputs.c $(LDFLAGS)

viewData:	../datagen-tsml/src/common.h      viewData.c ../datagen-tsml/src/inputs.c Makefile
	$(CC) $(CFLAGS) -I../datagen-tsml/src -o  viewData viewData.c ../datagen-tsml/src/inputs.c $(LDFLAGS)

detects_ascii_to_txt:	detects_ascii_to_txt.c Makefile
	$(CC) $(CFLAGS) -I../ocr/src -o detects_ascii_to_txt detects_ascii_to_txt.c $(LDFLAGS)

clean:
	$(RM) viewImage    viewCorr    dataImage    viewData \
	      viewImageTxt viewCorrTxt dataImageAsm detects_ascii_to_txt
