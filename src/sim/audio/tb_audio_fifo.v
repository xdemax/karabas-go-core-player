// tb_audio_fifo: the player's FIFO count/full logic (menu_soc.v) against the old 12-bit bug.
// build: verilator --binary --timing --top-module tb_audio_fifo tb_audio_fifo.v
`timescale 1ns/1ps
`default_nettype none

module fifo_new #(parameter AW = 11) (
	input wire clk,
	input wire reset,
	input wire [31:0] wdat,
	input wire we,
	input wire tick,
	output wire [31:0] free_o,
	output wire [15:0] drop_o,
	output wire [15:0] under_o,
	output reg signed [15:0] l = 0,
	output reg signed [15:0] r = 0
);
	reg [AW-1:0] wr = 0, rd = 0;
	reg [31:0] mem [0:(1 << AW) - 1];
	reg [15:0] drop = 0, under = 0;

	wire [AW-1:0] cnt = wr - rd;                    // 11 bit: mod 2048
	wire full  = (cnt == { AW{1'b1} });             // 2047
	wire empty = (wr == rd);
	assign free_o  = 32'd2047 - { 21'd0, cnt };
	assign drop_o  = drop;
	assign under_o = under;

	always @(posedge clk) begin
		if (reset) begin
			wr <= 0; rd <= 0; drop <= 0; under <= 0; l <= 0; r <= 0;
		end else begin
			if (we && full) drop <= drop + 1'b1;
			if (tick && empty) under <= under + 1'b1;
			if (we && !full) begin mem[wr] <= wdat; wr <= wr + 1'b1; end
			if (tick) begin
				if (!empty) begin { l, r } <= mem[rd]; rd <= rd + 1'b1; end
				else begin l <= 0; r <= 0; end
			end
		end
	end
endmodule

// The old code, for the comparison: 12-bit subtraction = mod 4096.
module fifo_old #(parameter AW = 11) (
	input wire clk,
	input wire reset,
	input wire [31:0] wdat,
	input wire we,
	input wire tick,
	output wire [31:0] free_o,
	output wire [15:0] drop_o,
	output wire [15:0] under_o,
	output reg signed [15:0] l = 0,
	output reg signed [15:0] r = 0
);
	reg [AW-1:0] wr = 0, rd = 0;
	reg [31:0] mem [0:(1 << AW) - 1];
	reg [15:0] drop = 0, under = 0;

	wire [AW:0] cnt = { 1'b0, wr } - { 1'b0, rd };  // 12 bit: mod 4096 (the bug)
	wire full  = (cnt == (1 << AW) - 1);
	wire empty = (wr == rd);
	assign free_o  = 32'd2047 - cnt;
	assign drop_o  = drop;
	assign under_o = under;

	always @(posedge clk) begin
		if (reset) begin
			wr <= 0; rd <= 0; drop <= 0; under <= 0; l <= 0; r <= 0;
		end else begin
			if (we && full) drop <= drop + 1'b1;
			if (tick && empty) under <= under + 1'b1;
			if (we && !full) begin mem[wr] <= wdat; wr <= wr + 1'b1; end
			if (tick) begin
				if (!empty) begin { l, r } <= mem[rd]; rd <= rd + 1'b1; end
				else begin l <= 0; r <= 0; end
			end
		end
	end
endmodule

module tb_audio_fifo;
	reg clk = 0, reset = 1, we = 0, tick = 0;
	reg [31:0] wdat = 0;
	wire [31:0] fn, fo;
	wire [15:0] dn, un, dold, uold;
	wire signed [15:0] ln, rn, lo, ro;

	fifo_new n(.clk(clk), .reset(reset), .wdat(wdat), .we(we), .tick(tick),
	           .free_o(fn), .drop_o(dn), .under_o(un), .l(ln), .r(rn));
	fifo_old o(.clk(clk), .reset(reset), .wdat(wdat), .we(we), .tick(tick),
	           .free_o(fo), .drop_o(dold), .under_o(uold), .l(lo), .r(ro));

	always #5 clk = ~clk;

	integer i, errors = 0;
	task chk(input cond, input [255:0] what);
		begin
			if (!cond) begin
				$display("FAIL: %0s (new free=%0d old free=%0d)", what, fn, fo);
				errors = errors + 1;
			end
		end
	endtask

	initial begin
		#23 reset = 0;
		#10;
		chk(fn == 2047 && fo == 2047, "empty free");

		// fill 2047
		for (i = 0; i < 2047; i = i + 1) begin
			@(negedge clk); we = 1; wdat = i;
		end
		@(negedge clk); we = 0;
		chk(fn == 0 && fo == 0, "full free");
		chk(dn == 0 && dold == 0, "no drop on exact fill");

		// drain all 2047
		for (i = 0; i < 2047; i = i + 1) begin
			@(negedge clk); tick = 1;
		end
		@(negedge clk); tick = 0;
		chk(fn == 2047 && fo == 2047, "empty after drain");
		// one more audio tick on the empty FIFO: it must be silence, not the last sample
		@(negedge clk); tick = 1;
		@(negedge clk); tick = 0;
		#10;
		chk(ln == 0 && rn == 0, "empty output is silence");

		// one write after wr==rd==2047: the old code gives 65534, the new 2046
		@(negedge clk); we = 1; wdat = 32'h1234;
		@(negedge clk); we = 0;
		$display("after wrap: new free=%0d old free=%0d (old is the bug)", fn, fo);
		chk(fn == 2046, "new free after wrap");
		chk(fo == 32'hFFFFFFFE, "old free is the known 65534 bug");

		// fill the new FIFO to full: 2046 more writes
		for (i = 0; i < 2046; i = i + 1) begin
			@(negedge clk); we = 1; wdat = 0;
		end
		@(negedge clk); we = 0;
		chk(fn == 0, "new full after wrap");
		chk(dn == 0, "still no drop");

		// one write while full: drop increments, the FIFO content is not overwritten
		@(negedge clk); we = 1;
		@(negedge clk); we = 0;
		chk(dn == 1, "drop on full");
		chk(fn == 0, "free stays 0");

		// one tick frees one slot, one write is accepted
		@(negedge clk); tick = 1;
		@(negedge clk); tick = 0;
		chk(fn == 1, "free after one tick");
		@(negedge clk); we = 1; wdat = 0;
		@(negedge clk); we = 0;
		chk(fn == 0, "free 0 after the write");

		// a tick while empty increments under
		// drain all, then one more tick
		for (i = 0; i < 2047; i = i + 1) begin
			@(negedge clk); tick = 1;
		end
		@(negedge clk); tick = 0;
		@(negedge clk); tick = 1;
		@(negedge clk); tick = 0;
		chk(un >= 1, "under on empty");

		#20;
		if (errors) $display("tb_audio_fifo: %0d errors", errors);
		else $display("tb_audio_fifo: 0 errors");
		$finish;
	end
endmodule
`default_nettype wire
