# The program of the player core (rtl/soc/menu_soc.v): player.bin is the ROM of musicplayer.kg.
XPACK  ?= $(HOME)/opt/xpack-riscv-none-elf-gcc-15.2.0-1/bin
CROSS  := $(XPACK)/riscv-none-elf-
CFLAGS := -Ifatfs -march=rv32im -mabi=ilp32 -O2 -g -ffreestanding -fno-builtin -ffunction-sections -fdata-sections -fno-tree-loop-distribute-patterns -Wall
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
SRCS   := main.c player.c ui_skin.c skin_graphic.c ui.c fonts.c gfx.c fpga.c xm_fpga.c ay_fpga.c analysis.c sound.c rpc.c settings.c spi.c sd.c diskio.c fs.c syscalls.c \
          src.c fmt.c eng_esp32.c sd1.c audio.c mod_fpga.c formats/mod.c formats/xm.c formats/s3m.c formats/pt3.c formats/pt2.c formats/stc.c formats/ay_more.c \
          fatfs/ff.c fatfs/ffunicode.c
OBJS   := $(B)/crt0.o $(addprefix $(B)/,$(SRCS:.c=.o)) $(B)/ay_players.o $(B)/ay/ay_engine.o
$(B)/player.elf: $(OBJS) link.ld
	$(CROSS)gcc $(CFLAGS) -nostdlib -nostartfiles -T link.ld -Wl,--gc-sections -o $@ $(OBJS) -lc_nano -lgcc
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
