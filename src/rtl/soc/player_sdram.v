`timescale 1ns / 1ps
`default_nettype none
/*
 player_sdram.v

 The SDRAM of the board (the player file buffer) (MT48LC16M16A2: 16 bits, 4 banks x 8192 rows x 512 columns, 32 MB) for the
 Doom SoC: one port in the CPU clock (clk, 75 MHz), a wishbone slave as the VexRiscv buses use it,
 after the scheme of archie_sdram.v (core-archie, Mini G at 80 MHz):

 - read: a line fill of the caches (CTI 010 = incrementing burst, from the first word of the 32-byte
   line, 8 words) is one ACTIVE and 16 READs in a row in the open row (the last with auto precharge);
   each word is acked as soon as its two halfwords are in (the master takes it, its address goes on).
   A single read (CTI 000 / 111 at the start) reads 2 halfwords.
 - write: one 32-bit word with the byte selects: ACTIVE, WRITE low, WRITE high (auto precharge), ack.
 Address (byte address of the port = SDRAM offset, 25 bits): bank a[24:23], row a[22:10],
 column { a[9:2], half }: a line never crosses a row.

 Timing (clk 13.3 ns at 75 MHz; also fine at 80): tRCD 2, tRP 2, tRC 5, tWR 2, tRFC 6 clocks; CAS
 latency 2, burst length 1. SDR_CLK comes from a PLL output 225 degrees after clk (the top, ODDR2):
 commands, address and write data leave IOB flip-flops on the rising edge of clk; read data are taken
 by an IOB flip-flop on the falling edge of clk and a second falling-edge stage (archie_sdram), used
 RD_DELAY clocks after their READ. Refresh: 8192 in 64 ms (one every REFRESH_CLOCKS), never inside a
 burst (owed ones first when idle).
*/
module player_sdram #(
	parameter INIT_CLOCKS = 15'd16000,    // 213 us at 75 MHz
	parameter REFRESH_CLOCKS = 10'd560,   // 7.5 us
	parameter RD_DELAY = 4
) (
	input wire clk,
	input wire reset,
	output wire ready,                    // init done

	// wishbone slave (byte address, 32 bit)
	input wire wb_cyc,
	input wire wb_stb,
	input wire wb_we,
	input wire [3:0] wb_sel,
	input wire [24:2] wb_adr,
	input wire [2:0] wb_cti,
	input wire [31:0] wb_dat_i,
	output reg [31:0] wb_dat_o = 32'd0,
	output reg wb_ack = 1'b0,

	// SDRAM
	output wire [12:0] sdr_a,
	output wire [1:0] sdr_ba,
	input wire [15:0] sdr_dq_i,           // the pins (the top makes the tristate: sim friendly)
	output wire [15:0] sdr_dq_o,
	output wire [15:0] sdr_dq_oe_n,
	output wire [1:0] sdr_dqm,
	output wire sdr_ras_n,
	output wire sdr_cas_n,
	output wire sdr_we_n
);

localparam [2:0] CMD_NOP = 3'b111, CMD_ACTIVE = 3'b011, CMD_READ = 3'b101, CMD_WRITE = 3'b100,
	CMD_PRECHARGE = 3'b010, CMD_REFRESH = 3'b001, CMD_MODE = 3'b000;

localparam [3:0] S_INIT = 0, S_INIT_PRE = 1, S_INIT_REF1 = 2, S_INIT_REF2 = 3, S_INIT_MODE = 4,
	S_IDLE = 5, S_WAIT = 6, S_RD = 7, S_RDEND = 8, S_WR = 9;

// ---------------------------------------------------------------- pins

(* IOB = "TRUE" *) reg [12:0] a_r = 13'd0;
(* IOB = "TRUE" *) reg [1:0] ba_r = 2'b00;
(* IOB = "TRUE" *) reg [1:0] dqm_r = 2'b11;
(* IOB = "TRUE" *) reg ras_r = 1'b1;
(* IOB = "TRUE" *) reg cas_r = 1'b1;
(* IOB = "TRUE" *) reg we_r = 1'b1;
(* IOB = "TRUE" *) reg [15:0] dq_out = 16'd0;
(* IOB = "TRUE", equivalent_register_removal = "no" *) reg [15:0] dq_oe_n = 16'hFFFF;
(* IOB = "TRUE" *) reg [15:0] dq_neg = 16'd0;

assign sdr_a = a_r;
assign sdr_ba = ba_r;
assign sdr_dqm = dqm_r;
assign sdr_ras_n = ras_r;
assign sdr_cas_n = cas_r;
assign sdr_we_n = we_r;
assign sdr_dq_o = dq_out;
assign sdr_dq_oe_n = dq_oe_n;

reg [15:0] dq_neg2 = 16'd0;
always @(negedge clk) begin
	dq_neg <= sdr_dq_i;
	dq_neg2 <= dq_neg;
end

// ---------------------------------------------------------------- controller

reg [3:0] state = S_INIT;
reg [3:0] next_state = S_IDLE;
reg [14:0] cnt = 15'd0;
reg [9:0] ref_cnt = 10'd0;
reg [2:0] ref_owed = 3'd0;
reg init_done = 1'b0;
assign ready = init_done;

reg [24:2] r_adr = 23'd0;
reg [31:0] r_dat = 32'd0;
reg [3:0] r_sel = 4'd0;
reg [4:0] rd_n = 5'd0;                // READ commands issued (0..16)
reg [4:0] rd_last = 5'd0;             // the number of READs of this request (2 or 16)
reg [4:0] dat_n = 5'd0;               // halfwords received
reg [RD_DELAY:0] rd_pipe = 0;         // a READ RD_DELAY clocks ago
reg [15:0] lo_half = 16'd0;
reg wr_hi = 1'b0;

wire wb_req = wb_cyc & wb_stb & ~wb_ack;

task cmd(input [2:0] c);
	begin
		ras_r <= c[2];
		cas_r <= c[1];
		we_r <= c[0];
	end
endtask

always @(posedge clk) begin
	wb_ack <= 1'b0;

	if (ref_cnt == REFRESH_CLOCKS) begin
		ref_cnt <= 10'd0;
		if (ref_owed != 3'd7) ref_owed <= ref_owed + 3'd1;
	end else
		ref_cnt <= ref_cnt + 1'd1;

	// read data: halfword dat_n; a word is acked with its high half
	rd_pipe <= { rd_pipe[RD_DELAY-1:0], 1'b0 };
	if (rd_pipe[RD_DELAY-1]) begin
		dat_n <= dat_n + 5'd1;
		if (!dat_n[0])
			lo_half <= dq_neg2;
		else begin
			wb_dat_o <= { dq_neg2, lo_half };
			wb_ack <= 1'b1;
		end
	end

	cmd(CMD_NOP);
	dq_oe_n <= 16'hFFFF;
	dqm_r <= 2'b00;

	if (reset) begin
		state <= S_INIT;
		cnt <= 15'd0;
		init_done <= 1'b0;
		dqm_r <= 2'b11;
		rd_pipe <= 0;
	end else case (state)
		S_INIT: begin
			dqm_r <= 2'b11;
			cnt <= cnt + 1'd1;
			if (cnt == INIT_CLOCKS) state <= S_INIT_PRE;
		end
		S_INIT_PRE: begin
			cmd(CMD_PRECHARGE);
			a_r[10] <= 1'b1;
			cnt <= 15'd3;
			next_state <= S_INIT_REF1;
			state <= S_WAIT;
		end
		S_INIT_REF1: begin
			cmd(CMD_REFRESH);
			cnt <= 15'd8;
			next_state <= S_INIT_REF2;
			state <= S_WAIT;
		end
		S_INIT_REF2: begin
			cmd(CMD_REFRESH);
			cnt <= 15'd8;
			next_state <= S_INIT_MODE;
			state <= S_WAIT;
		end
		S_INIT_MODE: begin
			cmd(CMD_MODE);
			ba_r <= 2'b00;
			a_r <= 13'b000_0_00_010_0_000;   // burst writes as reads (length 1), CAS 2, sequential, burst 1
			cnt <= 15'd3;
			next_state <= S_IDLE;
			state <= S_WAIT;
		end

		S_WAIT: begin
			if (cnt == 15'd0) begin
				state <= next_state;
				if (next_state == S_IDLE) init_done <= 1'b1;
			end else
				cnt <= cnt - 1'd1;
		end

		S_IDLE: begin
			if (ref_owed != 3'd0) begin
				cmd(CMD_REFRESH);
				ref_owed <= ref_owed - 3'd1;
				cnt <= 15'd5;                 // tRFC
				next_state <= S_IDLE;
				state <= S_WAIT;
			end else if (wb_req) begin
				r_adr <= wb_adr;
				r_dat <= wb_dat_i;
				r_sel <= wb_sel;
				cmd(CMD_ACTIVE);
				ba_r <= wb_adr[24:23];
				a_r <= wb_adr[22:10];
				rd_n <= 5'd0;
				dat_n <= 5'd0;
				rd_last <= (wb_cti == 3'b010) ? 5'd16 : 5'd2;
				wr_hi <= 1'b0;
				cnt <= 15'd0;                 // tRCD: one clock between
				next_state <= wb_we ? S_WR : S_RD;
				state <= S_WAIT;
			end
		end

		// READs of the open row, the last with auto precharge (A10)
		S_RD: begin
			cmd(CMD_READ);
			a_r <= { 2'b00, rd_n == rd_last - 5'd1, 1'b0, r_adr[9:5], r_adr[4:2] + rd_n[3:1], rd_n[0] };
			rd_pipe[0] <= 1'b1;
			rd_n <= rd_n + 5'd1;
			if (rd_n == rd_last - 5'd1) state <= S_RDEND;
		end
		S_RDEND: begin
			if (rd_pipe == 0) begin
				cnt <= 15'd0;                 // tRP after the auto precharge (tRC holds too)
				next_state <= S_IDLE;
				state <= S_WAIT;
			end
		end

		// two WRITEs (low, high halfword), the second with auto precharge
		S_WR: begin
			cmd(CMD_WRITE);
			dq_oe_n <= 16'h0000;
			if (!wr_hi) begin
				a_r <= { 4'b0000, r_adr[9:2], 1'b0 };
				dq_out <= r_dat[15:0];
				dqm_r <= ~r_sel[1:0];
				wr_hi <= 1'b1;
			end else begin
				a_r <= { 4'b0010, r_adr[9:2], 1'b1 };
				dq_out <= r_dat[31:16];
				dqm_r <= ~r_sel[3:2];
				wb_ack <= 1'b1;
				cnt <= 15'd3;                 // tWR + tRP (and tRC from the ACTIVE)
				next_state <= S_IDLE;
				state <= S_WAIT;
			end
		end
		default: state <= S_IDLE;
	endcase
end

endmodule
