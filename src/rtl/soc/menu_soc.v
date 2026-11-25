`timescale 1ns / 1ps
`default_nettype none
/*
 menu_soc.v

 The computer of the boot menu (docs/PLAN.md): VexRiscv (rv32im, I$ 16K, D$ 8K, FPU, wishbone), the SRAM
 (menu_sram: the program, the data and the two frame buffers), the MMIO registers, the loader. One
 clock (clk, 75 MHz). After doom_soc.v of karabas-go-core-doom (no SDRAM: the Mini G rev G2 has it off,
 the menu must run on every board).

 Memory map of the CPU (sw/mmio.h):
   0x00000000 SRAM 4 MB: the program from 0 (the ROMs of boot.kg), the frame buffers FB0 0x300000 and
              FB1 0x380000 (800 x 600 bytes each, a colour index per pixel)
   0x80000000 MMIO (uncached: the VexRiscv IO range = bit 31), word registers:
     00 W  DBG: a byte of the debug console (dbg_uart, the testbench prints it)
     04 W  FB: bit 0 the frame buffer to show from the next frame; R: bit 0 the one shown now
     08 R  FRAMES: frames begun (the start of the vertical sync)
     0c R  CYCLES: clocks since the CPU started (low 32)
     10 W  SHOW: bit 0 the frame buffer on the screen (0: the background of the old menu)
     18 W  GRAD_TOP: 0x00RRGGBB, the top colour of the background gradient (menu_compose)
     20 W  GRAD_BOT: 0x00RRGGBB, its bottom colour (3/4 of the height and below)
     14 W  LAUNCH: the RPC call written (CORES LAUNCH) is made after the clear of the SRAM: the CPU
           stops for good, the SRAM is cleared (the next core starts with clean memory), then the call
           goes to the firmware, which replaces the core
     1c R  MS: milliseconds since the CPU started
     28 R  KBD0: kg_mcu2 KBD[31:0] = { key 3, key 2, key 1, modifiers } (the USB boot report)
     2c R  KBD1: { 0, key 6, key 5, key 4 }            30 R  KBDN: reports seen (a counter)
     34 R  MOUSE: bits 31..24 buttons (held now, or pressed since the last read: a click of press and
           release between two reads is not lost), 23..12 dy sum, 11..0 dx sum (signed, saturated;
           reading clears)
     3c R  WHEEL: bits 11..0 the wheel sum (signed, saturated; + = away from the user; reading clears)
     38 R  JOY: { joy_r, joy_l }
     40 R  US: microseconds since the CPU started
     80..9c      SOUND: the two channels of menu_sound (+0 address, +4 length, +8 step, +c control /
                 R playing), channel 1 at 0x90
     70 W  AUDIO: the next stereo sample { L[15:0], R[15:0] } of the FPGA mixer
     74 R  AUDIO: the free entries of the 8192-sample FIFO (48 kHz)
     c0 W  AY: { reset, 3'b0, chip, reg[3:0], data[7:0] } to the two YM2149s
     c4 W  AY: bit 0 on (the RTL pair drives the audio), bits 15..8 volume 0..64
     a0 W  MIDI: a byte to the SAM2695 (menu_midi_mux, whole messages with their status); R bit 0
           its fifo is full
     a4 W  ESP: a byte to the ESP8266 (the 256 byte fifo of esp8266.vhd: at most that much before an
           answer); R bit 8 a byte of the ESP is there, bits 7..0 the byte (reading takes it)
     a8 W/R  ESP_OWN: bit 0 the CPU has the uart of the ESP (else it goes to the MCU, PORT 1)
     50 W     SPI: the byte to send (starts a transfer); R: bit 8 busy, 7..0 the answer
     54 W/R  SPI: the chip selects, active low: bit 0 SD2, 1 FT812, 2 spare, 3 ESP32
     58 W/R  SPI: the half period of SCK in clocks (2 = 12.5 MHz, 93 = 399 kHz for the SD init)
     60 R    LOAD: bit 0 a file of the file loader / FL item is in (kind 1 / 2), 1 too big, 3..2 the kind
     64 R    LOAD: its size in bytes; the bytes are at 0x080000 + the offset of the DATA packets
     6c R    LOAD: the extension of an FL item (ASCII, lowercase, no dot: "mod", "xm", "s3m")
     68 W    LOAD: acknowledge (clear the ready / too big)
     400..bfc W  PAL: bank (a - 400) / 1024 (the palette of frame buffer 0 / 1), entry ((a - 400) / 4) % 256 = 0xARRGGBB: alpha 0..15 in bits 31..28 over the
                 background (0 transparent, 15 opaque)
     2000..3ffc  RPC: byte (a - 2000) / 4 of the mailbox of kg_l2_rpc (kg_mcu2 RPC_*), in the low byte
                 of a word (map in kg_l2_rpc.v: request 000.., response 200.., event 400.., registers 600..)
 Instruction fetch and data share the SRAM (the data first); an owner keeps it to the last ack of its
 transaction (CTI 010 = more beats).

 Loader (kg_mcu2 LD_*): kind 0 (the ROMs of boot.kg) to the SRAM from 0, a byte per write; ld_ready
 low while one is pending or the SRAM is being cleared (caps bit 4: the firmware asks LOAD STATUS before
 each packet). The CPU is held in reset while anything loads, while the SRAM is cleared, and until the
 first load has ended (no program before it).
*/
module menu_soc #(
	parameter MS_DIV = 17'd75000,         // clocks per millisecond
	parameter US_DIV = 7'd75,             // clocks per microsecond
	parameter [31:0] RESET_VECTOR = 32'h00000000,
	parameter [20:0] CLR_LAST = 21'h1FFFFF
) (
	input wire clk,
	input wire reset,                     // the PLL

	// loader (kg_mcu2)
	input wire [1:0] ld_kind,
	input wire ld_start,
	input wire [31:0] ld_size,
	input wire [31:0] ld_ext,
	input wire ld_active,
	input wire ld_end,
	input wire [31:0] ld_addr,
	input wire [7:0] ld_data,
	input wire ld_wr,
	output wire ld_ready,

	// HID (kg_mcu2)
	input wire [15:0] sw,                    // the OSD switches S0..S1 (kg_mcu2 SW)
	input wire [55:0] kbd,
	input wire kbd_wr,
	input wire [7:0] ms_x,
	input wire [7:0] ms_y,
	input wire [7:0] ms_z,
	input wire [7:0] ms_b,
	input wire ms_wr,
	input wire [15:0] joy_l,
	input wire [15:0] joy_r,

	// video (menu_video): the SRAM read port, the palette, the buffers
	input wire vid_req,
	input wire [20:0] vid_adr,
	input wire [8:0] vid_len,
	output wire vid_we,
	output wire [8:0] vid_wa,
	output wire [15:0] vid_wd,
	output reg pal_we = 1'b0,
	output reg [8:0] pal_wa = 9'd0,     // { bank, entry }
	output reg [27:0] pal_wd = 28'd0,
	output reg [23:0] grad_top = 24'h000000,  // black until the CPU writes the colours of karabas.ini
	output reg [23:0] grad_bot = 24'h000000,
	output reg fb_sel = 1'b0,             // the buffer to show
	input wire fb_shown,                  // the one shown (synchronised to clk)
	input wire frame_t,                   // toggles at each frame (synchronised to clk)
	output reg show = 1'b0,

	// sound: the PCM mix (48 kHz, snd_t toggles with each value) and the MIDI bytes of the CPU
	output wire signed [15:0] snd_out,
	output wire snd_t,
	// the stereo audio FIFO of the player (the FPGA mixer): 48 kHz, L/R
	output wire signed [15:0] aud_l,
	output wire signed [15:0] aud_r,
	output wire aud_t,
	output reg [7:0] midi_data = 8'd0,
	output reg midi_wr = 1'b0,
	input wire midi_full,

	// the ESP8266 (esp8266.vhd): its uart while esp_own, else the MCU has it (the top)
	input wire [7:0] esp_rx_data,
	input wire esp_rx_valid,
	output reg esp_rx_ack = 1'b0,
	output reg [7:0] esp_tx_data = 8'd0,
	output reg esp_tx_wr = 1'b0,
	output reg esp_own = 1'b0,

	// RPC (kg_mcu2 RPC_*)
	output reg [10:0] rpc_a = 11'd0,
	output reg rpc_we = 1'b0,
	output reg [7:0] rpc_wd = 8'd0,
	input wire [7:0] rpc_rd,

	// SPI (the SD2 card, the ESP32-S3 of the VDAC32 shield; see rtl/spi/spi_master.v)
	output wire spi_sck,
	output wire spi_mosi,
	input wire spi_miso,
	output wire [3:0] spi_cs_n,

	// SDRAM: the 32 MB file buffer of the player at 0x10000000
	output wire [12:0] sdr_a,
	output wire [1:0] sdr_ba,
	input wire [15:0] sdr_dq_i,
	output wire [15:0] sdr_dq_o,
	output wire [15:0] sdr_dq_oe_n,
	output wire [1:0] sdr_dqm,
	output wire sdr_ras_n,
	output wire sdr_cas_n,
	output wire sdr_we_n,

	// debug
	output reg [7:0] dbg_uart = 8'd0,
	output reg dbg_uart_wr = 1'b0,
	output wire cpu_run,

	// SRAM pins
	output wire [20:0] MA,
	input wire [15:0] MD_I,
	output wire [15:0] MD_O,
	output wire [15:0] MD_OE_N,
	output wire [1:0] MWR_N,
	output wire [1:0] MRD_N
);

// ------------------------------------------------------------------ reset / clear / loader

// the clear of the SRAM after the PLL lock: every core starts with clean memory
reg reset_d = 1'b1;
reg clr_req = 1'b0;
wire clr_busy;
always @(posedge clk) begin
	reset_d <= reset;
	clr_req <= (reset_d & ~reset) | launch_clr;
end

wire sdram_ready;                     // the SDRAM controller finished its init
reg loaded = 1'b0;                    // a program came (the end of a kind 0 load)
reg halted = 1'b0;                    // LAUNCH: the CPU stops for good (its program is cleared)
reg cpu_reset = 1'b1;
assign cpu_run = ~cpu_reset;
always @(posedge clk) begin
	if (reset) loaded <= 1'b0;
	else if (ld_end && ld_kind == 2'd0) loaded <= 1'b1;
	// only a ROM (kind 0) holds the CPU: an FL file (kind 1 / 2) is loaded while the CPU runs, the
	// SRAM arbiter keeps the loader's bytes and the CPU apart
	cpu_reset <= reset | (ld_active && ld_kind == 2'd0) | clr_busy | clr_req | ~loaded | halted | ~sdram_ready;
end

// LAUNCH: the clear, then the call (the write of 605 to kg_l2_rpc)
reg launch_clr = 1'b0;                // the clear is asked
reg launch_wait = 1'b0;               // waiting for its end
reg launch_call = 1'b0;               // one clock: the call

reg ld_pend = 1'b0;
reg [21:0] ld_a = 22'd0;
reg [7:0] ld_d = 8'd0;
assign ld_ready = ~ld_pend & ~clr_busy & ~clr_req;

// ------------------------------------------------------------------ CPU

wire i_cyc, i_stb, d_cyc, d_stb, d_we;
wire [29:0] i_adr, d_adr;
wire [31:0] d_dat_o;
wire [3:0] d_sel;
wire [2:0] i_cti, d_cti;
reg [31:0] i_dat_i, d_dat_i;
reg i_ack, d_ack;

VexRiscv cpu (
	.timerInterrupt(1'b0),
	.softwareInterrupt(1'b0),
	.externalInterrupt(1'b0),
	.iBusWishbone_CYC(i_cyc),
	.iBusWishbone_STB(i_stb),
	.iBusWishbone_ACK(i_ack),
	.iBusWishbone_WE(),
	.iBusWishbone_ADR(i_adr),
	.iBusWishbone_DAT_MISO(i_dat_i),
	.iBusWishbone_DAT_MOSI(),
	.iBusWishbone_SEL(),
	.iBusWishbone_ERR(1'b0),
	.iBusWishbone_CTI(i_cti),
	.iBusWishbone_BTE(),
	.dBusWishbone_CYC(d_cyc),
	.dBusWishbone_STB(d_stb),
	.dBusWishbone_ACK(d_ack),
	.dBusWishbone_WE(d_we),
	.dBusWishbone_ADR(d_adr),
	.dBusWishbone_DAT_MISO(d_dat_i),
	.dBusWishbone_DAT_MOSI(d_dat_o),
	.dBusWishbone_SEL(d_sel),
	.dBusWishbone_ERR(1'b0),
	.dBusWishbone_CTI(d_cti),
	.dBusWishbone_BTE(),
	.clk(clk),
	.reset(cpu_reset)
);

wire d_req = d_cyc & d_stb;
wire i_req = i_cyc & i_stb;
wire d_io = d_adr[29];                      // 0x80000000
wire d_sd = ~d_io & d_adr[26];              // 0x10000000 (d_adr is a word address): the SDRAM file buffer
wire d_sr = ~d_io & ~d_sd;

// ------------------------------------------------------------------ SRAM and its owner

localparam [1:0] O_NONE = 2'd0, O_D = 2'd1, O_I = 2'd2, O_LD = 2'd3;
reg [1:0] own = O_NONE;
wire sr_ack;
wire [31:0] sr_dat;
reg sr_cyc, sr_we;
reg [3:0] sr_sel;
reg [21:2] sr_adr;
reg [2:0] sr_cti;
reg [31:0] sr_wdat;

always @(*) begin
	sr_cyc = 1'b0; sr_we = 1'b0; sr_sel = 4'hF; sr_adr = 20'd0; sr_cti = 3'd0; sr_wdat = 32'd0;
	case (own)
	O_D: begin sr_cyc = d_req; sr_we = d_we; sr_sel = d_sel; sr_adr = d_adr[19:0]; sr_cti = d_cti; sr_wdat = d_dat_o; end
	O_I: begin sr_cyc = i_req; sr_we = 1'b0; sr_sel = 4'hF; sr_adr = i_adr[19:0]; sr_cti = i_cti; end
	O_LD: begin sr_cyc = ld_pend; sr_we = 1'b1; sr_sel = 4'b0001 << ld_a[1:0]; sr_adr = ld_a[21:2]; sr_cti = 3'd0; sr_wdat = {4{ld_d}}; end
	default: ;
	endcase
end

// the owner: kept to the last ack of a transaction (an ack with CTI not 010)
always @(posedge clk) begin
	if (cpu_reset) begin
		if (own == O_NONE && ld_pend) own <= O_LD;
		else if (own != O_LD || sr_ack) own <= O_NONE;
	end else if (own == O_NONE) begin
		if (d_req && d_sr) own <= O_D;
		else if (i_req) own <= O_I;
	end else if (sr_ack && sr_cti != 3'b010)
		own <= O_NONE;
end

// ------------------------------------------------------------------ SDRAM

wire sd_ack;
wire [31:0] sd_dat;

player_sdram sdram (
	.clk(clk),
	.reset(reset),
	.ready(sdram_ready),
	.wb_cyc(d_req & d_sd), .wb_stb(d_req & d_sd), .wb_we(d_we), .wb_sel(d_sel),
	.wb_adr(d_adr[22:0]), .wb_cti(d_cti), .wb_dat_i(d_dat_o), .wb_dat_o(sd_dat), .wb_ack(sd_ack),
	.sdr_a(sdr_a), .sdr_ba(sdr_ba), .sdr_dq_i(sdr_dq_i), .sdr_dq_o(sdr_dq_o), .sdr_dq_oe_n(sdr_dq_oe_n),
	.sdr_dqm(sdr_dqm), .sdr_ras_n(sdr_ras_n), .sdr_cas_n(sdr_cas_n), .sdr_we_n(sdr_we_n)
);

wire aud_req, aud_ack;
wire [20:0] aud_adr;
wire [15:0] aud_dat;
menu_sram #(.CLR_LAST(CLR_LAST)) sram (
	.clk(clk),
	.clr_req(clr_req), .clr_busy(clr_busy),
	.aud_req(aud_req), .aud_adr(aud_adr), .aud_ack(aud_ack), .aud_dat(aud_dat),
	.wb_cyc(sr_cyc), .wb_stb(sr_cyc), .wb_we(sr_we), .wb_sel(sr_sel), .wb_adr(sr_adr), .wb_cti(sr_cti),
	.wb_dat_i(sr_wdat), .wb_dat_o(sr_dat), .wb_ack(sr_ack),
	// no fetches while the frame buffer is off (a loader byte must not wait behind a video chunk)
	.vid_req(vid_req & show & ~cpu_reset), .vid_adr(vid_adr), .vid_len(vid_len), .vid_we(vid_we), .vid_wa(vid_wa),
	.vid_wd(vid_wd), .vid_busy(),
	.MA(MA), .MD_I(MD_I), .MD_O(MD_O), .MD_OE_N(MD_OE_N), .MWR_N(MWR_N), .MRD_N(MRD_N)
);

// ------------------------------------------------------------------ sound

reg snd_wr = 1'b0;
reg [1:0] snd_ch = 2'd0, snd_reg = 2'd0;
reg [31:0] snd_dat = 32'd0;
wire [1:0] snd_playing;
menu_sound #(.CLK_HZ(MS_DIV * 1000)) sound (
	.clk(clk), .reset(cpu_reset),
	.wr(snd_wr), .wr_ch(snd_ch), .wr_reg(snd_reg), .wr_dat(snd_dat), .playing(snd_playing),
	.aud_req(aud_req), .aud_adr(aud_adr), .aud_ack(aud_ack), .aud_dat(aud_dat),
	.out(snd_out), .out_t(snd_t)
);

// ------------------------------------------------------------------ Turbo Sound
// Two YM2149s from the tsconf core (rtl/sound/ym2149.v, the same chip model that tsconf
// uses for its Turbosound). The tracker players only write the AY registers at 50 Hz; the
// chips generate the tones continuously here, so no software AY sampler is in the audio path.
//
// 0xc0 W: bits 7..0 data, 11..8 register, 12 chip, 31 reset both chips
// 0xc4 W: bit 0 active, bits 15..8 volume (0..64); the other bits are ignored
reg [3:0] ay_addr = 4'd0;
reg [7:0] ay_data = 8'd0;
reg       ay_chip = 1'b0;
reg       ay_cmd = 1'b0;
reg       ay_rst_req = 1'b0;
reg [3:0] ay_rst_cnt = 4'd0;
reg       ay_active = 1'b0;
reg [7:0] ay_gain = 8'd64;

reg [1:0] ay_state = 2'd0;
reg [3:0] ay_addr_l = 4'd0;
reg [7:0] ay_data_l = 8'd0;
reg       ay_chip_l = 1'b0;
reg       ay_bdir = 1'b0;
reg       ay_bc = 1'b0;
reg [7:0] ay_di = 8'd0;
wire      ay_reset = (ay_rst_cnt != 4'd0);

// 75 MHz / 3.5 MHz: the same CE rate that tsconf gives its YM2149 pair (the PSG clock
// is the chip's own /2 divider). The fractional accumulator keeps the average exact.
reg [31:0] ay_ce_acc = 32'd0;
wire ay_ce = (ay_ce_acc + 32'd3500000 >= 32'd75000000);
always @(posedge clk) begin
	if (cpu_reset) ay_ce_acc <= 32'd0;
	else if (ay_ce) ay_ce_acc <= ay_ce_acc + 32'd3500000 - 32'd75000000;
	else ay_ce_acc <= ay_ce_acc + 32'd3500000;
end

wire [7:0] ay0_a, ay0_b, ay0_c, ay1_a, ay1_b, ay1_c;
ym2149 ay0 (
	.CLK(clk), .CE(ay_ce), .RESET(ay_reset | cpu_reset),
	.BDIR(ay_bdir & ~ay_chip_l), .BC(ay_bc), .DI(ay_di), .DO(),
	.CHANNEL_A(ay0_a), .CHANNEL_B(ay0_b), .CHANNEL_C(ay0_c),
	.SEL(1'b1), .MODE(1'b0), .ACTIVE(),
	.IOA_in(8'hFF), .IOA_out(), .IOB_in(8'hFF), .IOB_out()
);
ym2149 ay1 (
	.CLK(clk), .CE(ay_ce), .RESET(ay_reset | cpu_reset),
	.BDIR(ay_bdir & ay_chip_l), .BC(ay_bc), .DI(ay_di), .DO(),
	.CHANNEL_A(ay1_a), .CHANNEL_B(ay1_b), .CHANNEL_C(ay1_c),
	.SEL(1'b1), .MODE(1'b0), .ACTIVE(),
	.IOA_in(8'hFF), .IOA_out(), .IOB_in(8'hFF), .IOB_out()
);

// A / C are placed left / right and B in the centre, as the tsconf mixer does for TS.
wire [10:0] ay_lvl_l = ({1'b0, ay0_a} << 1) + ay0_b + ({1'b0, ay1_a} << 1) + ay1_b;
wire [10:0] ay_lvl_r = ({1'b0, ay0_c} << 1) + ay0_b + ({1'b0, ay1_c} << 1) + ay1_b;
wire signed [17:0] ay_raw_l = $signed({7'b0, ay_lvl_l});
wire signed [17:0] ay_raw_r = $signed({7'b0, ay_lvl_r});

// The AY output is a stepped/square signal. tsconf filters the OPL4 and GS with a two-stage
// one-pole low-pass (audio_lpf.v) before its mixer; MegaBuzz uses a 64-tap FIR. The AY here is
// filtered at its own 3.5 MHz CE, before the 48 kHz sample, so the edges do not fold back into
// the audio band. Two 1/16 stages: about 22 kHz at -3 dB.
reg signed [17:0] ay_lp1_l = 18'sd0, ay_lp2_l = 18'sd0;
reg signed [17:0] ay_lp1_r = 18'sd0, ay_lp2_r = 18'sd0;
always @(posedge clk) begin
	if (cpu_reset) begin
		ay_lp1_l <= 18'sd0; ay_lp2_l <= 18'sd0;
		ay_lp1_r <= 18'sd0; ay_lp2_r <= 18'sd0;
	end else if (ay_ce) begin
		ay_lp1_l <= ay_lp1_l + ((ay_raw_l - ay_lp1_l) >>> 4);
		ay_lp2_l <= ay_lp2_l + ((ay_lp1_l - ay_lp2_l) >>> 4);
		ay_lp1_r <= ay_lp1_r + ((ay_raw_r - ay_lp1_r) >>> 4);
		ay_lp2_r <= ay_lp2_r + ((ay_lp1_r - ay_lp2_r) >>> 4);
	end
end

wire [7:0] ay_gain_c = (ay_gain > 8'd64) ? 8'd64 : ay_gain;
wire signed [26:0] ay_scaled_l = (ay_lp2_l * $signed({1'b0, ay_gain_c})) >>> 2;
wire signed [26:0] ay_scaled_r = (ay_lp2_r * $signed({1'b0, ay_gain_c})) >>> 2;
function signed [15:0] ay_sat16(input signed [26:0] v);
	begin
		ay_sat16 = (v > 27'sd32767) ? 16'sh7FFF : (v < -27'sd32768) ? 16'sh8000 : v[15:0];
	end
endfunction
wire signed [15:0] ay_l = ay_sat16(ay_scaled_l);
wire signed [15:0] ay_r = ay_sat16(ay_scaled_r);

// The register write: address (BC=1), then data (BC=0), exactly as the YM2149 bus needs.
always @(posedge clk) begin
	if (cpu_reset) begin
		ay_state <= 2'd0; ay_bdir <= 1'b0; ay_bc <= 1'b0; ay_di <= 8'd0;
		ay_rst_cnt <= 4'd0;
	end else begin
		ay_bdir <= 1'b0;
		ay_bc <= 1'b0;
		if (ay_rst_req) begin
			ay_rst_cnt <= 4'hF;
			ay_state <= 2'd0;
		end else begin
			if (ay_rst_cnt != 4'd0) ay_rst_cnt <= ay_rst_cnt - 1'b1;
			if (ay_cmd) begin
				ay_addr_l <= ay_addr; ay_data_l <= ay_data; ay_chip_l <= ay_chip;
				ay_state <= 2'd1;
			end else case (ay_state)
				2'd0: ;
				2'd1: begin ay_di <= {4'd0, ay_addr_l}; ay_bdir <= 1'b1; ay_bc <= 1'b1; ay_state <= 2'd2; end
				2'd2: begin ay_di <= ay_data_l; ay_bdir <= 1'b1; ay_bc <= 1'b0; ay_state <= 2'd3; end
				2'd3: begin ay_bdir <= 1'b0; ay_bc <= 1'b0; ay_state <= 2'd0; end
			endcase
		end
	end
end

// ------------------------------------------------------------------ SPI

// 0x50 W the byte to send (a transfer starts), R { busy, the byte received }
// 0x54 W/R the chip selects, active low: bit 0 SD2, 1 FT812, 2 spare, 3 ESP32
// 0x58 W/R the half period of SCK in clocks (2 = 12.5 MHz, 93 = 399 kHz for the SD init)
reg spi_wr = 1'b0, spi_cs_wr = 1'b0, spi_div_wr = 1'b0;
reg [7:0] spi_dat = 8'd0;
reg [3:0] spi_cs = 4'hF;
reg [15:0] spi_div = 16'd2;
wire spi_busy;
wire [7:0] spi_rx;

spi_master spi (
	.clk(clk), .reset(cpu_reset),
	.wr_dat(spi_wr), .dat(spi_dat),
	.wr_cs(spi_cs_wr), .cs_in(spi_cs),
	.wr_div(spi_div_wr), .div_in(spi_div),
	.miso(spi_miso), .rdat(spi_rx), .busy(spi_busy),
	.sck(spi_sck), .mosi(spi_mosi), .cs_n(spi_cs_n)
);

// ------------------------------------------------------------------ the FPGA audio FIFO

// The player's mixer writes stereo samples here; the audio tick pops one at the configured rate;
// the top takes L / R to the DAC and HDMI. 8192 samples (~170 ms at 48 kHz) hide the SD reads.
localparam AUD_W = 13;                       // 8192 entries (~170 ms at 48 kHz)
localparam [26:0] AUD_RATE = 27'd48000;     // default rate
localparam [31:0] AUD_CLK = MS_DIV * 1000;
reg [26:0] aud_rate = 27'd48000;

// The FIFO is an explicit block RAM (kg_sdpram), not an array that XST may turn into
// distributed LUTRAM when the AY output is muxed into the same always block.
wire [31:0] aud_dout;
reg aud_valid = 1'b0;
reg [AUD_W-1:0] aud_wr = 0, aud_rd = 0;
reg [15:0] aud_drop = 16'd0, aud_under = 16'd0;
reg [31:0] aud_wdat = 32'd0;
reg aud_we = 1'b0;
reg aud_stats_clr = 1'b0;
reg signed [15:0] aud_l_r = 16'sd0, aud_r_r = 16'sd0;
reg aud_t_r = 1'b0;

// Hardware AY tap: the CPU reads the real mixed YM2149 samples for the common
// output mixer/analyzer. The ring is written at the 48 kHz audio tick.
reg [9:0] ay_tap_wr = 10'd0, ay_tap_rd = 10'd0;
reg [31:0] ay_tap [0:1023];

wire aud_read = aud_tick && !ay_active && !aud_empty;
kg_sdpram #(.DW(32), .AW(AUD_W)) aud_ram (
	.clka(clk), .wea(aud_we && !aud_full), .addra(aud_wr), .dina(aud_wdat),
	.clkb(clk), .enb(aud_read), .addrb(aud_rd), .doutb(aud_dout)
);
reg [26:0] aud_acc = 27'd0;
reg aud_tick = 1'b0;

// The subtraction must stay 11 bit: 12 bit would count modulo 4096 and break when wr<rd
wire [AUD_W-1:0] aud_cnt = aud_wr - aud_rd;
// 13-bit pointers: 8192 and 0 look the same, so the FIFO holds 8191 samples (one slot empty).
wire aud_full = (aud_cnt == { AUD_W{1'b1} });
wire aud_empty = (aud_wr == aud_rd);

always @(posedge clk) begin
	aud_tick <= 1'b0;
	if (aud_acc + aud_rate >= AUD_CLK) begin
		aud_acc <= aud_acc + aud_rate - AUD_CLK;
		aud_tick <= 1'b1;
	end else
		aud_acc <= aud_acc + aud_rate;
end

always @(posedge clk) begin
	if (cpu_reset) begin
		aud_wr <= 0;
		aud_rd <= 0;
		aud_valid <= 1'b0;
		aud_l_r <= 16'sd0;
		aud_r_r <= 16'sd0;
		aud_t_r <= 1'b0;
		aud_drop <= 16'd0;
		aud_under <= 16'd0;
		ay_tap_wr <= 10'd0;
		aud_rate <= AUD_RATE;
	end else begin
		if (aud_stats_clr) begin
			aud_drop <= 16'd0;
			aud_under <= 16'd0;
		end else begin
			if (aud_we && aud_full) aud_drop <= aud_drop + 1'b1;
			if (aud_tick && aud_empty && !ay_active) aud_under <= aud_under + 1'b1;
		end
		if (aud_we && !aud_full) aud_wr <= aud_wr + 1'b1;
		if (aud_tick) begin
			ay_tap[ay_tap_wr] <= {ay_l, ay_r};
			ay_tap_wr <= ay_tap_wr + 10'd1;
			if (ay_active) begin
				aud_l_r <= ay_l;
				aud_r_r <= ay_r;
				aud_valid <= 1'b0;
				aud_rd <= aud_wr;          // discard the software samples while the chips play
			end else begin
				// kg_sdpram has one clock of read latency: aud_dout holds the sample requested
				// on the previous tick. The next sample is fetched now.
				if (aud_valid) begin
					aud_l_r <= aud_dout[31:16];
					aud_r_r <= aud_dout[15:0];
				end else begin
					aud_l_r <= 16'sd0;
					aud_r_r <= 16'sd0;
				end
				if (!aud_empty) begin
					aud_rd <= aud_rd + 1'b1;
					aud_valid <= 1'b1;
				end else aud_valid <= 1'b0;
			end
			aud_t_r <= ~aud_t_r;
		end
	end
end

assign aud_l = aud_l_r;
assign aud_r = aud_r_r;
assign aud_t = aud_t_r;

// ------------------------------------------------------------------ MMIO

reg [31:0] cycles = 32'd0;
reg [31:0] ms = 32'd0;
reg [16:0] ms_cnt = 17'd0;
reg [31:0] us = 32'd0;
reg [6:0] us_div = 7'd0;
reg [31:0] kbd_n = 32'd0;
reg [31:0] frames = 32'd0;
reg frame_p = 1'b0;
reg signed [11:0] ms_dx = 12'sd0, ms_dy = 12'sd0, ms_dz = 12'sd0;
reg [7:0] ms_btn = 8'd0;
reg [7:0] ms_press = 8'd0;   // the buttons pressed since the last read
reg io_ack = 1'b0;
reg [31:0] io_dat = 32'd0;
wire [11:0] io_off = { d_adr[9:0], 2'b00 };
wire io_rpc = d_adr[11:11] == 1'b1;          // 0x2000..0x3ffc (word address bit 11)
// an RPC read: kg_l2_rpc registers the address, then its RAM / register output: rpc_rd is valid two
// clocks after rpc_a
reg rpc_rd_wait = 1'b0, rpc_rd_wait2 = 1'b0;

function signed [11:0] sat_add(input signed [11:0] a, input [7:0] d);
	reg signed [12:0] s;
	begin
		s = { a[11], a } + { {5{d[7]}}, d };
		sat_add = (s > 13'sd2047) ? 12'sd2047 : (s < -13'sd2048) ? -12'sd2048 : s[11:0];
	end
endfunction

always @(posedge clk) begin
	if (cpu_reset) ay_tap_rd <= 10'd0;
	io_ack <= 1'b0;
	pal_we <= 1'b0;
	dbg_uart_wr <= 1'b0;
	rpc_we <= 1'b0;
	snd_wr <= 1'b0;
	midi_wr <= 1'b0;
	esp_rx_ack <= 1'b0;
	esp_tx_wr <= 1'b0;
	spi_wr <= 1'b0;
	spi_cs_wr <= 1'b0;
	spi_div_wr <= 1'b0;
	file_ack <= 1'b0;
	aud_we <= 1'b0;
	aud_stats_clr <= 1'b0;
	ay_cmd <= 1'b0;
	ay_rst_req <= 1'b0;
	launch_clr <= 1'b0;
	launch_call <= 1'b0;
	if (launch_wait && !clr_req && !clr_busy && !launch_clr) begin
		launch_wait <= 1'b0;
		rpc_a <= 11'h605;
		rpc_wd <= 8'h01;
		rpc_we <= 1'b1;
	end
	if (reset) begin
		halted <= 1'b0;
		launch_wait <= 1'b0;
	end
	// an RPC read: the byte a clock after its address
	rpc_rd_wait2 <= rpc_rd_wait;
	rpc_rd_wait <= 1'b0;
	if (rpc_rd_wait2) begin
		io_ack <= 1'b1;
		io_dat <= { 24'd0, rpc_rd };
	end
	if (cpu_reset) begin
		ay_active <= 1'b0;
		ay_gain <= 8'd64;
		cycles <= 32'd0;
		ms <= 32'd0;
		ms_cnt <= 17'd0;
		us <= 32'd0;
		us_div <= 7'd0;
	end else begin
		cycles <= cycles + 32'd1;
		if (ms_cnt == MS_DIV - 17'd1) begin ms_cnt <= 17'd0; ms <= ms + 32'd1; end
		else ms_cnt <= ms_cnt + 17'd1;
		if (us_div == US_DIV - 7'd1) begin us_div <= 7'd0; us <= us + 32'd1; end
		else us_div <= us_div + 7'd1;
	end
	frame_p <= frame_t;
	if (frame_t != frame_p) frames <= frames + 32'd1;
	if (kbd_wr) kbd_n <= kbd_n + 32'd1;
	if (ms_wr) begin
		ms_dx <= sat_add(ms_dx, ms_x);
		ms_dy <= sat_add(ms_dy, ms_y);
		ms_dz <= sat_add(ms_dz, ms_z);
		ms_btn <= ms_b;
		ms_press <= ms_press | ms_b;
	end
	if (reset) begin
		show <= 1'b0;
		fb_sel <= 1'b0;
	end

	if (d_req && d_io && !io_ack && !rpc_rd_wait && !rpc_rd_wait2 && !cpu_reset && io_rpc) begin
		rpc_a <= d_adr[10:0];
		if (d_we) begin
			rpc_wd <= d_dat_o[7:0];
			rpc_we <= 1'b1;
			io_ack <= 1'b1;
		end else
			rpc_rd_wait <= 1'b1;
	end else if (d_req && d_io && !io_ack && !rpc_rd_wait && !rpc_rd_wait2 && !cpu_reset && !io_rpc) begin
		io_ack <= 1'b1;
		if (d_we) begin
			if (io_off[11:10] == 2'b01 || io_off[11:10] == 2'b10) begin   // 0x400..0xbfc
				pal_we <= 1'b1;
				pal_wa <= { io_off[11], io_off[9:2] };
				pal_wd <= { d_dat_o[31:28], d_dat_o[23:0] };
			end else if (io_off[7:5] == 3'b100) begin   // 0x80..0x9c: the sound
				snd_wr <= 1'b1;
				snd_ch <= { 1'b0, io_off[4] };
				snd_reg <= io_off[3:2];
				snd_dat <= d_dat_o;
			end else case (io_off[7:0])
				8'h00: begin dbg_uart <= d_dat_o[7:0]; dbg_uart_wr <= 1'b1; end
				8'ha0: begin midi_data <= d_dat_o[7:0]; midi_wr <= 1'b1; end
				8'ha4: begin esp_tx_data <= d_dat_o[7:0]; esp_tx_wr <= 1'b1; end
				8'ha8: esp_own <= d_dat_o[0];
				8'h04: fb_sel <= d_dat_o[0];
				8'h10: show <= d_dat_o[0];
				8'h18: grad_top <= d_dat_o[23:0];
				8'h20: grad_bot <= d_dat_o[23:0];
				8'h50: begin spi_dat <= d_dat_o[7:0]; spi_wr <= 1'b1; end
				8'h54: begin spi_cs <= d_dat_o[3:0]; spi_cs_wr <= 1'b1; end
				8'h58: begin spi_div <= d_dat_o[15:0]; spi_div_wr <= 1'b1; end
				8'h68: file_ack <= 1'b1;
				8'h70: begin aud_wdat <= d_dat_o; aud_we <= 1'b1; end
				8'h80: aud_rate <= d_dat_o[26:0];
				8'hc0: begin
					ay_addr <= d_dat_o[11:8];
					ay_data <= d_dat_o[7:0];
					ay_chip <= d_dat_o[12];
					ay_cmd <= 1'b1;
					if (d_dat_o[31]) ay_rst_req <= 1'b1;
				end
				8'hc4: begin ay_active <= d_dat_o[0]; ay_gain <= d_dat_o[15:8]; end
				8'h78: aud_stats_clr <= 1'b1;
				8'h14: begin halted <= 1'b1; show <= 1'b0; launch_clr <= 1'b1; launch_wait <= 1'b1; end
				default: ;
			endcase
		end else begin
			case (io_off[7:0])
				8'h04: io_dat <= { 31'd0, fb_shown };
				8'h08: io_dat <= frames;
				8'h0c: io_dat <= cycles;
				8'h1c: io_dat <= ms;
				8'h28: io_dat <= kbd[31:0];
				8'h2c: io_dat <= { 8'd0, kbd[55:32] };
				8'h30: io_dat <= kbd_n;
				8'h34: begin
					io_dat <= { ms_btn | ms_press, ms_dy, ms_dx };
					ms_press <= ms_wr ? ms_b : 8'd0;
					if (ms_wr) begin
						ms_dx <= sat_add(12'sd0, ms_x);
						ms_dy <= sat_add(12'sd0, ms_y);
					end else begin
						ms_dx <= 12'sd0;
						ms_dy <= 12'sd0;
					end
				end
				8'h38: io_dat <= { joy_r, joy_l };
				8'h3c: begin
					io_dat <= { {20{ms_dz[11]}}, ms_dz };
					ms_dz <= ms_wr ? sat_add(12'sd0, ms_z) : 12'sd0;
				end
				8'h40: io_dat <= us;
				8'hb0: io_dat <= { 16'd0, sw };
				8'hd0: begin
					if (ay_tap_rd != ay_tap_wr) begin
						io_dat <= ay_tap[ay_tap_rd];
						ay_tap_rd <= ay_tap_rd + 10'd1;
					end else io_dat <= 32'd0;
				end
				8'hd4: io_dat <= { 31'd0, (ay_tap_rd != ay_tap_wr) };
				8'h8c: io_dat <= { 23'd0, snd_playing[0], 8'd0 };
				8'h9c: io_dat <= { 23'd0, snd_playing[1], 8'd0 };
				8'ha0: io_dat <= { 31'd0, midi_full };
				8'ha4: begin io_dat <= { 23'd0, esp_rx_valid, esp_rx_data }; esp_rx_ack <= esp_rx_valid; end
				8'ha8: io_dat <= { 31'd0, esp_own };
				8'h50: io_dat <= { 23'd0, spi_busy, spi_rx };
				8'h54: io_dat <= { 28'd0, spi_cs };
				8'h58: io_dat <= { 16'd0, spi_div };
				8'h60: io_dat <= { 28'd0, file_kind, file_bad, file_ready };
				8'h64: io_dat <= file_size;
				8'h6c: io_dat <= ld_ext;
				8'h74: io_dat <= 32'd8191 - { 19'd0, aud_cnt };
				8'hbc: io_dat <= { 31'd0, sdram_ready };
				8'h78: io_dat <= { 16'd0, aud_drop };
				8'h7c: io_dat <= { 16'd0, aud_under };
				8'h80: io_dat <= { 5'd0, aud_rate };
				default: io_dat <= 32'd0;
			endcase
		end
	end

	// the program (kind 0) and the files of the file loader / FL items (kind 1 / 2) go to the SRAM
	// by their own always below (ld_pend / ld_a are shared with the loader owner)
end

// ------------------------------------------------------------------ loader: the program and the files

// The file loader and the FL items (LOAD kind 1 / 2) come with the byte offset of the file from 0:
// the bytes are put at FILE_BASE + offset, the size / extension from START (kind 2) are available to
// the CPU at 0x60 / 0x64. A file over FILE_MAX (2.5 MB) sets file_bad; the program (kind 0) goes to
// its address 0 as before.
localparam [21:0] FILE_BASE = 22'h080000;
localparam [21:0] FILE_MAX  = 22'h280000;   // 2.5 MB: the program / heap / stack below 0x80000
reg file_ready = 1'b0, file_bad = 1'b0, file_ack = 1'b0;
reg [1:0] file_kind = 2'd0;
reg [21:0] file_len = 22'd0;
reg [31:0] file_size = 32'd0;

always @(posedge clk) begin
	if (reset) begin
		ld_pend <= 1'b0;
		file_ready <= 1'b0;
		file_bad <= 1'b0;
		file_kind <= 2'd0;
		file_len <= 22'd0;
		file_size <= 32'd0;
	end else begin
		if (file_ack) begin
			file_ready <= 1'b0;
			file_bad <= 1'b0;
		end
		if (ld_start && ld_kind != 2'd0) begin
			file_ready <= 1'b0;
			file_bad <= 1'b0;
			file_kind <= ld_kind;
			file_len <= 22'd0;
			file_size <= (ld_kind == 2'd2) ? ld_size : 32'd0;
		end
		if (ld_wr && !ld_pend) begin
			ld_pend <= 1'b1;
			ld_d <= ld_data;
			if (ld_kind == 2'd0) begin
				ld_a <= ld_addr[21:0];
			end else begin
				if (ld_addr[21:0] < FILE_MAX) ld_a <= FILE_BASE + ld_addr[21:0];
				else begin ld_a <= FILE_BASE; file_bad <= 1'b1; end
				if (file_len < FILE_MAX) file_len <= file_len + 22'd1;
				else file_bad <= 1'b1;
			end
		end
		if (ld_end && ld_kind != 2'd0) begin
			if (file_bad || (ld_kind == 2'd2 && file_size != 32'd0 && file_len != file_size[21:0]))
				file_bad <= 1'b1;
			else begin
				if (!(ld_kind == 2'd2 && file_size != 32'd0))
					file_size <= { 10'd0, file_len };
				file_ready <= 1'b1;
			end
		end
		if (own == O_LD && sr_ack) ld_pend <= 1'b0;
	end
end

// ------------------------------------------------------------------ CPU bus answers

always @(*) begin
	i_ack = (own == O_I) & sr_ack;
	i_dat_i = sr_dat;
	d_ack = ((own == O_D) & sr_ack) | io_ack | sd_ack;
	d_dat_i = d_io ? io_dat : (d_sd ? sd_dat : sr_dat);
end

endmodule
