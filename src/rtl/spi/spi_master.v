`timescale 1ns / 1ps
`default_nettype none
/*
 spi_master.v

 A byte-wide SPI master (mode 0, MSB first) for the SD2 card and the ESP32-S3 of the VDAC32 shield.
 One byte a write, CS held by a register, so a whole command / block goes without gaps that matter.
 The clock is (clk / (2 * (DIV + 1))): DIV 2 = 12.5 MHz at 75 MHz, DIV 93 = 399 kHz (the SD init).

 Registers are written by menu_soc (one-clock strobes):
   wr_dat  the byte to send; ignored while busy
   wr_cs   the four chip selects, active low (bit 0 SD2, 1 FT812, 2 spare, 3 ESP32)
   wr_div  the half period in clocks, 0..65535
 rdat holds the byte received with the last one written (valid when busy falls).
*/
module spi_master (
	input wire clk,
	input wire reset,

	input wire wr_dat,
	input wire [7:0] dat,
	input wire wr_cs,
	input wire [3:0] cs_in,
	input wire wr_div,
	input wire [15:0] div_in,

	input wire miso,
	output wire [7:0] rdat,
	output wire busy,
	output wire sck,
	output wire mosi,
	output wire [3:0] cs_n
);

reg [3:0] cs_r = 4'hF;
reg [15:0] div_r = 16'd2;
reg [7:0] tx = 8'd0;
reg [7:0] rx = 8'd0;
reg [15:0] cnt = 16'd0;
reg [3:0] bits = 4'd0;
reg sck_r = 1'b0;
reg mosi_r = 1'b1;
reg busy_r = 1'b0;

assign rdat = rx;
assign busy = busy_r;
assign sck = sck_r;
assign mosi = mosi_r;
assign cs_n = cs_r;

always @(posedge clk) begin
	if (reset) begin
		cs_r <= 4'hF;
		div_r <= 16'd2;
		tx <= 8'd0;
		rx <= 8'd0;
		cnt <= 16'd0;
		bits <= 4'd0;
		sck_r <= 1'b0;
		mosi_r <= 1'b1;
		busy_r <= 1'b0;
	end else if (wr_div) begin
		div_r <= div_in;
	end else if (wr_cs) begin
		cs_r <= cs_in;
	end else if (wr_dat && !busy_r) begin
		// the first bit goes out before the first rising edge
		busy_r <= 1'b1;
		sck_r <= 1'b0;
		mosi_r <= dat[7];
		tx <= { dat[6:0], 1'b0 };
		bits <= 4'd7;
		cnt <= 16'd0;
	end else if (busy_r) begin
		if (cnt >= div_r) begin
			cnt <= 16'd0;
			if (!sck_r) begin
				// rising edge: the slave has driven MISO since the falling one
				sck_r <= 1'b1;
				rx <= { rx[6:0], miso };
			end else begin
				// falling edge: the next bit, or the end after the 8th
				sck_r <= 1'b0;
				if (bits == 4'd0) begin
					busy_r <= 1'b0;
					mosi_r <= 1'b1;
				end else begin
					mosi_r <= tx[7];
					tx <= { tx[6:0], 1'b0 };
					bits <= bits - 4'd1;
				end
			end
		end else
			cnt <= cnt + 16'd1;
	end
end

endmodule
