package vexriscv.demo

import spinal.core._
import vexriscv.plugin.Plugin
import vexriscv.{Stageable, DecoderService, VexRiscv}

// Tracker mixer custom ALU plugin for music player
// opcode custom-0 = 0x0b, funct3 = 0, funct7 selects operation
// R-type: funct7|rs2|rs1|funct3|rd|opcode

class CustomMixAluPlugin extends Plugin[VexRiscv] {
  object IS_CUSTOM_MIX extends Stageable(Bool)
  object FUNCT7 extends Stageable(Bits(7 bits))

  override def setup(pipeline: VexRiscv): Unit = {
    import pipeline.config._

    val decoderService = pipeline.service(classOf[DecoderService])
    decoderService.addDefault(IS_CUSTOM_MIX, False)
    decoderService.addDefault(FUNCT7, B(0, 7 bits))

    // Pattern: funct7|rs2|rs1|000|rd|01011
    decoderService.add(
      key = M"-----------------000-----0001011",
      List(
        IS_CUSTOM_MIX            -> True,
        REGFILE_WRITE_VALID      -> True,
        BYPASSABLE_EXECUTE_STAGE -> True,
        BYPASSABLE_MEMORY_STAGE  -> True,
        RS1_USE                  -> True,
        RS2_USE                  -> True
      )
    )

    // SETF (funct7=0): write=false, RS2_USE=false
    decoderService.add(
      key = M"0000000----------000-----0001011",
      List(
        IS_CUSTOM_MIX            -> True,
        REGFILE_WRITE_VALID      -> False,
        RS1_USE                  -> True,
        RS2_USE                  -> False
      )
    )

    // GETF (funct7=3): RS1_USE=false, RS2_USE=false
    decoderService.add(
      key = M"0000011----------000-----0001011",
      List(
        IS_CUSTOM_MIX            -> True,
        REGFILE_WRITE_VALID      -> True,
        BYPASSABLE_EXECUTE_STAGE -> True,
        BYPASSABLE_MEMORY_STAGE  -> True,
        RS1_USE                  -> False,
        RS2_USE                  -> False
      )
    )
  }

  override def build(pipeline: VexRiscv): Unit = {
    import pipeline._
    import pipeline.config._

    execute plug new Area {
      val rs1 = execute.input(RS1).asUInt
      val rs2 = execute.input(RS2).asUInt
      val funct7 = execute.input(INSTRUCTION)(31 downto 25).asUInt
      val rd = UInt(32 bits)
      rd := 0

      // Hidden state registers (retained across instructions)
      val hidden_frac = Reg(UInt(16 bits)) init 0
      val hidden_s = Reg(SInt(18 bits)) init 0

      // Helper functions for sign extension
      def sign8(v: UInt): SInt = v(7 downto 0).asSInt
      def sign16(v: UInt): SInt = v(15 downto 0).asSInt
      def sign32(v: UInt): SInt = v.asSInt

      // Intermediate results
      val funct7_val = funct7

      when(execute.input(IS_CUSTOM_MIX)) {
        switch(funct7_val) {
          // funct7=0: SETF - hidden_frac = rs1[15:0]
          is(0) {
            hidden_frac := rs1(15 downto 0)
          }

          // funct7=1: LERP16 - rd = s0 + ((s1 - s0) * (hidden_frac >> 1) >> 15)
          is(1) {
            val s0 = sign16(rs1)
            val s1 = sign16(rs2)
            val diff = s1 - s0
            val frac_shifted = hidden_frac >> 1
            val prod = diff * frac_shifted.asSInt
            val shifted = prod >> 15
            val result = s0 + shifted.resize(32 bits)
            rd := result.asUInt
            hidden_s := result.resize(18 bits)
          }

           // funct7=2: MAC - g = rs2[31:8]; rd = rs1 + (hidden_s * g)
          is(2) {
            val g = rs2.asSInt >> 8
            val prod = hidden_s * g
             val result = sign32(rs1) + prod.resize(32 bits)
            rd := result.asUInt
          }

          // funct7=3: GETF - rd = zero_extend(hidden_frac)
          is(3) {
            rd := hidden_frac.resize(32 bits)
          }

          // funct7=4: ADV - t = hidden_frac + (rs2 & 0xFFFF); 
          //           hidden_frac = t & 0xFFFF; rd = rs1 + (rs2 >> 16) + (t >> 16)
          is(4) {
            val sf = rs2(15 downto 0)
            val t = hidden_frac +^ sf
            hidden_frac := t(15 downto 0)
            val carry = t >> 16
            rd := (rs1 +^ (rs2 >> 16) +^ carry).resize(32 bits)
          }

          // funct7=5: ADVB - sf = rs2 & 0xFFFF; borrow = (hidden_frac < sf);
          //           hidden_frac = (hidden_frac - sf) & 0xFFFF; rd = rs1 - (rs2 >> 16) - borrow
          is(5) {
            val sf = rs2(15 downto 0)
            val borrow = Mux(hidden_frac < sf, U(1, 32 bits), U(0, 32 bits))
            hidden_frac := (hidden_frac - sf)(15 downto 0)
            rd := (rs1 - (rs2 >> 16) - borrow).resize(32 bits)
          }

          // funct7=6: LERP8 - s0=sign8(rs1[7:0]), s1=sign8(rs2[7:0]);
          //           rd = s0*256 + (((s1-s0)*hidden_frac) >> 8)
          is(6) {
            val s0 = sign8(rs1)
            val s1 = sign8(rs2)
            val diff = s1 - s0
            val prod = diff * hidden_frac.asSInt
            val frac_part = prod >> 8
            val result = (s0 * 256) + frac_part.resize(32 bits)
            rd := result.asUInt
            hidden_s := result.resize(18 bits)
          }
        }

        when(execute.input(REGFILE_WRITE_VALID)) {
          execute.output(REGFILE_WRITE_DATA) := rd.asBits
        }
      }
    }
  }
}
