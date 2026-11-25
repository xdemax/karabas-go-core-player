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

    // Exact funct7 patterns (no overlapping "don't care" keys).
    val common = List(
      IS_CUSTOM_MIX            -> True,
      REGFILE_WRITE_VALID      -> True,
      BYPASSABLE_EXECUTE_STAGE -> True,
      BYPASSABLE_MEMORY_STAGE  -> True,
      RS1_USE                  -> True,
      RS2_USE                  -> True
    )
    // SETF (0): hidden_frac = rs1[15:0], no write, no rs2
    decoderService.add(M"0000000----------000-----0001011", List(
      IS_CUSTOM_MIX -> True, REGFILE_WRITE_VALID -> False,
      RS1_USE -> True, RS2_USE -> False))
    decoderService.add(M"0000001----------000-----0001011", common) // LERP16
    decoderService.add(M"0000010----------000-----0001011", common) // MAC
    // GETF (3): no sources
    decoderService.add(M"0000011----------000-----0001011", List(
      IS_CUSTOM_MIX -> True, REGFILE_WRITE_VALID -> True,
      BYPASSABLE_EXECUTE_STAGE -> True, BYPASSABLE_MEMORY_STAGE -> True,
      RS1_USE -> False, RS2_USE -> False))
    decoderService.add(M"0000100----------000-----0001011", common) // ADV
    decoderService.add(M"0000101----------000-----0001011", common) // ADVB
    decoderService.add(M"0000110----------000-----0001011", common) // LERP8
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
            val s0 = sign16(rs1).resize(18)
            val s1 = sign16(rs2).resize(18)
            val dif = (s1 - s0).resize(19)
            val neg = dif < 0
            val ad = Mux(neg, (-dif).asUInt, dif.asUInt)
            val p = ad * (hidden_frac >> 1)
            val q = (p + Mux(neg, U(32767, p.getWidth bits), U(0, p.getWidth bits))) >> 15
            val shifted = Mux(neg, -(q.asSInt), q.asSInt).resize(32)
            val result = (s0.resize(32) + shifted).resize(32)
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
            val s0 = sign8(rs1).resize(10)
            val s1 = sign8(rs2).resize(10)
            val dif = (s1 - s0).resize(11)
            val neg = dif < 0
            val ad = Mux(neg, (-dif).asUInt, dif.asUInt)
            val p = ad * hidden_frac
            val q = (p + Mux(neg, U(255, p.getWidth bits), U(0, p.getWidth bits))) >> 8
            val frac_part = Mux(neg, -(q.asSInt), q.asSInt).resize(32)
            val result = ((s0.resize(32) << 8) + frac_part).resize(32)
            rd := result.asUInt
            hidden_s := result.resize(18 bits)
          }
        }

        when(execute.input(REGFILE_WRITE_VALID)) {
          execute.insert(REGFILE_WRITE_DATA) := rd.asBits
        }
      }
    }
  }
}
