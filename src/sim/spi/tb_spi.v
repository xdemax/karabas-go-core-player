`timescale 1ns / 1ps
// tb_spi: the SPI master of the player (rtl/spi/spi_master.v) against a slave model: mode 0, a byte
// out and a byte in, the CS register. Verilator --binary --timing.
module tb_spi;
	reg clk = 0;
	reg reset = 1;
	reg wr_dat = 0, wr_cs = 0, wr_div = 0;
	reg [7:0] dat = 0;
	reg [3:0] cs_in = 4'hF;
	reg [15:0] div_in = 2;
	wire [7:0] rdat;
	wire busy, sck, mosi, miso;
	wire [3:0] cs_n;
	integer errors = 0;

	spi_master dut (
		.clk(clk), .reset(reset),
		.wr_dat(wr_dat), .dat(dat), .wr_cs(wr_cs), .cs_in(cs_in), .wr_div(wr_div), .div_in(div_in),
		.miso(miso), .rdat(rdat), .busy(busy), .sck(sck), .mosi(mosi), .cs_n(cs_n)
	);

	// the slave: 0x3C back, the bits of MOSI collected
	reg [7:0] resp = 8'h3C;
	reg [7:0] got = 0;
	reg miso_r = 1'b0;
	assign miso = miso_r;
	wire active = !cs_n[0] | !cs_n[3];

	always #6.667 clk = ~clk;   // 75 MHz

	always @(negedge active) begin
		resp = 8'h3C;
		got = 0;
		miso_r = 8'h3C >> 7;
	end

	always @(posedge sck) if (active) got <= { got[6:0], mosi };
	always @(negedge sck) if (active) begin
		resp <= { resp[6:0], 1'b0 };
		miso_r <= resp[6];
	end

	task send(input [7:0] b);
		begin
			while (busy) @(posedge clk);
			dat = b;
			wr_dat = 1;
			@(posedge clk);
			wr_dat = 0;
			@(posedge clk);
			while (busy) @(posedge clk);
		end
	endtask

	task set_cs(input [3:0] c);
		begin
			cs_in = c;
			wr_cs = 1;
			@(posedge clk);
			wr_cs = 0;
			@(posedge clk);
		end
	endtask

	initial begin
		repeat (5) @(posedge clk);
		reset = 0;
		@(posedge clk);
		set_cs(4'b1110);          // SD2
		send(8'hA5);
		if (got !== 8'hA5) begin $display("FAIL: the slave saw %02x, not A5", got); errors = errors + 1; end
		if (rdat !== 8'h3C) begin $display("FAIL: the master read %02x, not 3C", rdat); errors = errors + 1; end
		set_cs(4'b1111);
		if (cs_n !== 4'hF) begin $display("FAIL: the CS lines %b", cs_n); errors = errors + 1; end

		// the ESP32 CS (bit 3) and a second byte
		set_cs(4'b0111);
		send(8'h00);
		if (got !== 8'h00) begin $display("FAIL: the second slave saw %02x", got); errors = errors + 1; end
		if (rdat !== 8'h3C) begin $display("FAIL: the second read %02x", rdat); errors = errors + 1; end
		set_cs(4'b1111);

		// 93: the clock is slower, the byte still right
		div_in = 93; wr_div = 1; @(posedge clk); wr_div = 0;
		set_cs(4'b1110);
		send(8'h5A);
		if (got !== 8'h5A) begin $display("FAIL: the slow slave saw %02x", got); errors = errors + 1; end
		set_cs(4'b1111);

		if (errors == 0) $display("tb_spi: 0 errors");
		else $display("tb_spi: %0d errors", errors);
		$finish;
	end
endmodule
