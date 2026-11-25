package vexriscv.demo

import spinal.core._
import vexriscv.plugin.Plugin
import vexriscv.{Stageable, DecoderService, VexRiscv}

// Tracker mixer custom ALU plugin for music player
// opcode custom-0 = 0x0b, funct3 = 0, funct7 selects operation
// R-type: funct7|rs2|rs1|funct3|rd|opcode
// All instructions single-cycle, combinational logic

class CustomMixAluPlugin extends Plugin[VexRiscv] {
  object IS_CUSTOM_MIX extends Stageable(Bool)
  object FUNCT7 extends Stageable(Bits(7 bits))

  override def setup(pipeline: VexRiscv): Unit = {
    import pipeline.config._

    val decoderService = pipeline.service(classOf[DecoderService])
    decoderService.addDefault(IS_CUSTOM_MIX, False)
    decoderService.addDefault(FUNCT7, B(0, 7 bits))

    // 0: SETF - no write, RS1 only
    decoderService.add(M"0000000----------000-----0001011", List(
      IS_CUSTOM_MIX -> True, REGFILE_WRITE_VALID -> False,
      BYPASSABLE_EXECUTE_STAGE -> False, BYPASSABLE_MEMORY_STAGE -> True,
      RS1_USE -> True, RS2_USE -> False))
    // 1: LERP1A - no write, RS1/RS2
    decoderService.add(M"0001011----------000-----0001011", List(
      IS_CUSTOM_MIX -> True, REGFILE_WRITE_VALID -> False,
      BYPASSABLE_EXECUTE_STAGE -> False, BYPASSABLE_MEMORY_STAGE -> True,
      RS1_USE -> True, RS2_USE -> True))
    // 1b: LERP1B - no write, no sources
    decoderService.add(M"0001010----------000-----0001011", List(
      IS_CUSTOM_MIX -> True, REGFILE_WRITE_VALID -> False,
      BYPASSABLE_EXECUTE_STAGE -> False, BYPASSABLE_MEMORY_STAGE -> True,
      RS1_USE -> False, RS2_USE -> False))
    // 2: MAC - write, RS1/RS2
    decoderService.add(M"0000010----------000-----0001011", List(
      IS_CUSTOM_MIX -> True, REGFILE_WRITE_VALID -> True,
      BYPASSABLE_EXECUTE_STAGE -> False, BYPASSABLE_MEMORY_STAGE -> True,
      RS1_USE -> True, RS2_USE -> True))
    // 3: GETF - write, no sources
    decoderService.add(M"0000011----------000-----0001011", List(
      IS_CUSTOM_MIX -> True, REGFILE_WRITE_VALID -> True,
      BYPASSABLE_EXECUTE_STAGE -> False, BYPASSABLE_MEMORY_STAGE -> True,
      RS1_USE -> False, RS2_USE -> False))
    // 4: ADV - write, RS1/RS2
    decoderService.add(M"0000100----------000-----0001011", List(
      IS_CUSTOM_MIX -> True, REGFILE_WRITE_VALID -> True,
      BYPASSABLE_EXECUTE_STAGE -> False, BYPASSABLE_MEMORY_STAGE -> True,
      RS1_USE -> True, RS2_USE -> True))
    // 5: ADVB - write, RS1/RS2
    decoderService.add(M"0000101----------000-----0001011", List(
      IS_CUSTOM_MIX -> True, REGFILE_WRITE_VALID -> True,
      BYPASSABLE_EXECUTE_STAGE -> False, BYPASSABLE_MEMORY_STAGE -> True,
      RS1_USE -> True, RS2_USE -> True))
    // 6: LERP8_1 - no write, RS1/RS2
    decoderService.add(M"0000110----------000-----0001011", List(
      IS_CUSTOM_MIX -> True, REGFILE_WRITE_VALID -> False,
      BYPASSABLE_EXECUTE_STAGE -> False, BYPASSABLE_MEMORY_STAGE -> True,
      RS1_USE -> True, RS2_USE -> True))
    // 7: SETS - write, RS1 only
    decoderService.add(M"0000111----------000-----0001011", List(
      IS_CUSTOM_MIX -> True, REGFILE_WRITE_VALID -> True,
      BYPASSABLE_EXECUTE_STAGE -> False, BYPASSABLE_MEMORY_STAGE -> True,
      RS1_USE -> True, RS2_USE -> False))
    // 8: LERP2 - write, no sources (uses hidden state)
    decoderService.add(M"0001000----------000-----0001011", List(
      IS_CUSTOM_MIX -> True, REGFILE_WRITE_VALID -> True,
      BYPASSABLE_EXECUTE_STAGE -> False, BYPASSABLE_MEMORY_STAGE -> True,
      RS1_USE -> False, RS2_USE -> False))
    // 9: LERP8_2 - write, no sources (uses hidden state)
    decoderService.add(M"0001001----------000-----0001011", List(
      IS_CUSTOM_MIX -> True, REGFILE_WRITE_VALID -> True,
      BYPASSABLE_EXECUTE_STAGE -> False, BYPASSABLE_MEMORY_STAGE -> True,
      RS1_USE -> False, RS2_USE -> False))
  }

  override def build(pipeline: VexRiscv): Unit = {
    import pipeline._
    import pipeline.config._

    execute plug new Area {
      val rs1 = execute.input(RS1).asUInt
      val rs2 = execute.input(RS2).asUInt
      val funct7 = execute.input(INSTRUCTION)(31 downto 25).asUInt

      // Hidden state registers
      val h_frac = Reg(UInt(16 bits)) init 0
      val h_p = Reg(UInt(32 bits)) init 0
      val h_neg = Reg(Bool) init False
      val h_ad = Reg(UInt(18 bits)) init 0
      val h_s0 = Reg(SInt(18 bits)) init 0
      val h_p8 = Reg(UInt(32 bits)) init 0
      val h_neg8 = Reg(Bool) init False
      val h_s08 = Reg(SInt(10 bits)) init 0
      val h_s = Reg(SInt(18 bits)) init 0

      val rd = UInt(32 bits)
      rd := 0

      // Helper functions for sign extension
      def sign8(v: UInt): SInt = v(7 downto 0).asSInt
      def sign16(v: UInt): SInt = v(15 downto 0).asSInt
      def sign32(v: UInt): SInt = v.asSInt

      when(execute.input(IS_CUSTOM_MIX)) {
        switch(funct7) {
          // 0: SETF - h_frac = rs1[15:0]
          is(0) {
            h_frac := rs1(15 downto 0)
          }
          // 1a: LERP1A - s0=sign16(rs1), s1=sign16(rs2), d=s1-s0
          //              h_neg=(d<0), h_ad=abs(d), h_s0=s0
          is(11) {
            val s0 = sign16(rs1).resize(18)
            val s1 = sign16(rs2).resize(18)
            val d = s1 - s0
            val is_neg = d < 0
            val ad = Mux(is_neg, (-d).asUInt, d.asUInt)
            h_neg := is_neg
            h_ad := ad.resize(18)
            h_s0 := s0
          }
          // 1b: LERP1B - h_p = h_ad * (h_frac >> 1)
          is(10) {
            h_p := (h_ad * (h_frac >> 1)).resize(32)
          }
          // 2: MAC - rd = sign32(rs1) + (sign_extend(h_s) * ((rs2[18:0])>>8))
          is(2) {
            val g = (rs2(18 downto 0) >> 8).asSInt.resize(11)
            val prod = h_s * g
            rd := (sign32(rs1) + prod).resize(32).asUInt
          }
          // 3: GETF - rd = zero_extend(h_frac)
          is(3) {
            rd := h_frac.resize(32)
          }
          // 4: ADV - t = h_frac + rs2[15:0] (17 bits)
          //          h_frac = t[15:0], rd = rs1 + (rs2>>16) + t[16]
          is(4) {
            val t = h_frac +^ rs2(15 downto 0)
            h_frac := t(15 downto 0)
            val carry = Mux(t(16), U(1, 32 bits), U(0, 32 bits))
            rd := (rs1.asUInt + (rs2 >> 16) + carry).resize(32)
          }
          // 5: ADVB - sf=rs2[15:0], borrow=(h_frac<sf)
          //           h_frac=(h_frac-sf)[15:0], rd = rs1 - (rs2>>16) - borrow
          is(5) {
            val sf = rs2(15 downto 0)
            val borrow = Mux(h_frac < sf, U(1, 32 bits), U(0, 32 bits))
            h_frac := (h_frac - sf).resize(16)
            rd := (rs1.asSInt - (rs2 >> 16).asSInt - borrow.asSInt).resize(32).asUInt
          }
          // 6: LERP8_1 - s0=sign8(rs1), s1=sign8(rs2), d=s1-s0
          //              h_neg8=(d<0), h_p8=abs(d)*h_frac, h_s08=s0 (SInt10)
          is(6) {
            val s0 = sign8(rs1).resize(10)
            val s1 = sign8(rs2).resize(10)
            val d = s1 - s0
            val is_neg8 = d < 0
            val ad = Mux(is_neg8, (-d).asUInt, d.asUInt)
            h_neg8 := is_neg8
            h_p8 := ad * h_frac
            h_s08 := s0
          }
          // 7: SETS - h_s = sign16(rs1), rd = rs1
          is(7) {
            h_s := sign16(rs1).resize(18)
            rd := rs1
          }
          // 8: LERP2 - q = h_neg ? (h_p + 32767) >> 15 : h_p >> 15
          //            sh = h_neg ? -q : q
          //            s = h_s0 + sh (32-bit wrap), rd = s, h_s = s (18 bits)
          is(8) {
            val q_tmp = Mux(h_neg, h_p + U(32767, 32 bits), h_p)
            val q = q_tmp >> 15
            val sh = Mux(h_neg, -(q.asSInt), q.asSInt)
            val s = (h_s0.resize(32) + sh).resize(32)
            rd := s.asUInt
            h_s := s.resize(18)
          }
          // 9: LERP8_2 - q = h_neg8 ? (h_p8 + 255) >> 8 : h_p8 >> 8
          //              sh = h_neg8 ? -q : q
          //              s = (h_s08 << 8) + sh (32-bit wrap), rd = s, h_s = s
          is(9) {
            val q_tmp = Mux(h_neg8, h_p8 + U(255, 32 bits), h_p8)
            val q = q_tmp >> 8
            val sh = Mux(h_neg8, -(q.asSInt), q.asSInt)
            val s = ((h_s08 << 8).resize(32) + sh).resize(32)
            rd := s.asUInt
            h_s := s.resize(18)
          }
        }

        when(execute.input(REGFILE_WRITE_VALID)) {
          execute.insert(REGFILE_WRITE_DATA) := rd.asBits
        }
      }
    }
  }
}
