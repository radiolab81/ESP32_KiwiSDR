# PC-Build des portablen Kerns (gleiche Quellen wie im ESP-IDF-Projekt).
#   make          -> kiwi_pc (Client)  und  test_core
#   make test     -> Kern-Tests + End-to-End-Test gegen lokalen Mock-Kiwi
CORE := ../components/kiwi_core
CC   ?= gcc
CFLAGS ?= -std=c11 -O2 -g -Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion
CFLAGS += -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -I$(CORE)/include
SRCS := $(CORE)/src/kiwi_ws.c $(CORE)/src/kiwi_adpcm.c $(CORE)/src/kiwi_client.c $(CORE)/src/port_sockets.c

# Verzeichnisdienst (Serverliste, Datenbank, Auswahl) - eigene Komponente neben kiwi_core
DIR  := ../components/kiwi_dir
CFLAGS += -I$(DIR)/include
DSRCS := $(wildcard $(DIR)/src/*.c)

# Optionale Spektrumanzeige (SSD1306/ST7735) mit simulierten Panels
SPEC  := ../components/kiwi_disp
CFLAGS += -I$(SPEC)/include
SPSRCS := $(wildcard $(SPEC)/src/kiwi_wf.c $(SPEC)/src/kiwi_spec.c $(SPEC)/src/kiwi_spec_draw.c $(SPEC)/src/kiwi_disp.c $(SPEC)/src/disp_ssd1306.c $(SPEC)/src/disp_st7735.c) sim_panel.c

all: kiwi_pc test_core kiwi_dirtool test_dir test_spec

kiwi_pc: main_pc.c sink_wav.c spec_pc.c $(SPSRCS) $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^

kiwi_pc_debug: main_pc.c sink_wav.c spec_pc.c $(SPSRCS) $(SRCS)
	$(CC) $(CFLAGS) -DKIWI_DEBUG -o $@ $^

test_core: test_core.c $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^

kiwi_dirtool: kiwi_dirtool.c store_file.c $(DSRCS) $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^

test_spec: test_spec.c $(SPSRCS) $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^

test_dir: test_dir.c $(DSRCS) $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^

test: all
	./test_core
	python3 e2e_test.py
	./test_dir
	python3 e2e_dir_test.py
	./test_spec
	python3 e2e_wf_test.py

clean:
	rm -f kiwi_pc kiwi_pc_debug test_core kiwi_dirtool test_dir test_spec *.wav *.ppm e2e_dir_*.bin

.PHONY: all test clean
