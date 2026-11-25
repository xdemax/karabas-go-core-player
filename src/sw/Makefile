# The program of the player core (rtl/soc/menu_soc.v): player.bin is the ROM of musicplayer.kg.
XPACK  ?= $(HOME)/opt/xpack-riscv-none-elf-gcc-15.2.0-1/bin
CROSS  := $(XPACK)/riscv-none-elf-
# Build id shown on screen: <A|N><core.json build>-<git rev>
#   A = custom ALU instructions compiled in, N = C fallback (no ALU).
# Override with e.g. `make ALU=1`. Never let two different binaries show the
# same id: the whole point is to tell flashed builds apart.
BUILD_BASE := $(shell sed -n 's/.*"build"[^"]*"\([^"]*\)".*/\1/p' ../core.json)
GITREV     := $(shell git -C .. rev-parse --short=6 HEAD 2>/dev/null || echo nogit)
ALU        ?= 0
ifeq ($(ALU),1)
FM_ALU_TAG := A
FM_ALU_CFLAGS := -DFM_CUSTOM_CPU=1
else
FM_ALU_TAG := N
FM_ALU_CFLAGS :=
endif
ifneq ($(IT_PROFILE),)
FM_PROF_TAG := p
else
FM_PROF_TAG :=
endif
BUILD_ID := $(FM_ALU_TAG)$(BUILD_BASE)-$(GITREV)$(FM_PROF_TAG)
CFLAGS := -DPLAYER_BUILD=\"$(BUILD_ID)\" -Ifatfs -Imikmod -Imikmod/include -Ihelix -DHELIX_STATIC -DNO_DEPACKERS $(FM_ALU_CFLAGS) -march=rv32im -mabi=ilp32 -O3 -flto -g -ffreestanding -fno-builtin -ffunction-sections -fdata-sections -fno-tree-loop-distribute-patterns -Wall
ifneq ($(IT_PROFILE),)
CFLAGS += -DIT_PROFILE
endif
CXXFLAGS := $(CFLAGS) -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit -fno-unwind-tables
B      := build

all: ../roms/player.bin

# the ROM of musicplayer.kg (core.json roms, loaded to address 0 of the SRAM)
../roms/player.bin: $(B)/player.bin
	mkdir -p ../roms
	cp $< $@

$(B)/%.o: %.c $(wildcard *.h) $(wildcard formats/*.h) $(wildcard fatfs/*.h) | $(B)
	mkdir -p $(dir $@)
	$(CROSS)gcc $(CFLAGS) -c $< -o $@
$(B)/ay_players.o: ay_players.cpp | $(B)
	$(CROSS)g++ $(CXXFLAGS) -c $< -o $@
$(B)/ay/ay_engine.o: ay/ay_engine.cpp $(wildcard ay/players/*.h) | $(B)
	mkdir -p $(dir $@)
	$(CROSS)g++ $(CXXFLAGS) -c $< -o $@

$(B)/%.o: %.S | $(B)
	$(CROSS)gcc $(CFLAGS) -c $< -o $@
SRCS   := main.c player.c ui_skin.c skin_graphic.c ui.c fonts.c gfx.c fpga.c xm_fpga.c ay_fpga.c analysis.c sound.c rpc.c settings.c spi.c sd.c diskio.c fs.c browser.c playlist.c syscalls.c \
          it/it_parser.c it/it_player.c src.c fmt.c audio.c mixer.c fastmix.c mod_fpga.c mikmod_fpga.c it_fpga.c s3m_fpga.c mp3_fpga.c resample.c formats/mod.c formats/xm.c formats/s3m.c formats/it.c formats/pt3.c formats/pt2.c formats/stc.c formats/ay_more.c formats/mp3.c \
          fatfs/ff.c fatfs/ffunicode.c
MIKMOD_SRCS := mikmod/mmio/mmalloc.c mikmod/mmio/mmerror.c mikmod/mmio/mmio.c \
               mikmod/playercode/mdriver.c mikmod/playercode/mdulaw.c mikmod/playercode/mloader.c \
               mikmod/playercode/mlutil.c mikmod/playercode/mplayer.c mikmod/playercode/munitrk.c \
               mikmod/playercode/mwav.c mikmod/playercode/npertab.c mikmod/playercode/sloader.c \
               mikmod/playercode/virtch.c mikmod/playercode/virtch2.c mikmod/playercode/virtch_common.c \
               mikmod/loaders/load_s3m.c mikmod/loaders/load_it.c mikmod/drivers/drv_nos.c
HELIX_SRCS := helix/mp3dec.c helix/mp3tabs.c helix/bitstream.c helix/buffers.c helix/dequant.c \
              helix/huffman.c helix/imdct.c helix/scalfact.c helix/stproc.c helix/subband.c \
              helix/dqchan.c helix/dct32.c helix/hufftabs.c helix/polyphase.c helix/trigtabs.c
OBJS   := $(B)/crt0.o $(addprefix $(B)/,$(SRCS:.c=.o)) $(addprefix $(B)/,$(MIKMOD_SRCS:.c=.o)) $(addprefix $(B)/,$(HELIX_SRCS:.c=.o)) $(B)/ay_players.o $(B)/ay/ay_engine.o
$(B)/player.o: ../core.json
$(B)/player.elf: $(OBJS) link.ld
	$(CROSS)gcc $(CFLAGS) -nostdlib -nostartfiles -T link.ld -Wl,--gc-sections -o $@ $(OBJS) -lc_nano -lm -lgcc
	$(CROSS)size $@
$(B)/player.bin: $(B)/player.elf
	$(CROSS)objcopy -O binary $< $@
$(B):
	mkdir -p $(B)
fonts:
	python3 tools/mkfonts.py fonts_gen.h
clean:
	rm -rf $(B)
.PHONY: all clean fonts
