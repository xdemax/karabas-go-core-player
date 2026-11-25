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
      BYPASSABLE_EXECUTE_STAGE -> False,
      BYPASSABLE_MEMORY_STAGE  -> True,
      RS1_USE                  -> True,
      RS2_USE                  -> True
    )
    // SETF (0): hidden_frac = rs1[15:0], no write, no rs2
    decoderService.add(M"0000000----------000-----0001011", List(
      IS_CUSTOM_MIX -> True, REGFILE_WRITE_VALID -> False,
      RS1_USE -> True, RS2_USE -> False))
    // LERP16: not bypassable (long multiply path must not feed the decode bypass)
    decoderService.add(M"0000001----------000-----0001011", List(
      IS_CUSTOM_MIX -> True, REGFILE_WRITE_VALID -> True,
      BYPASSABLE_EXECUTE_STAGE -> False, BYPASSABLE_MEMORY_STAGE -> True,
      RS1_USE -> True, RS2_USE -> True))
    decoderService.add(M"0000010----------000-----0001011", common) // MAC
    // GETF (3): no sources
    decoderService.add(M"0000011----------000-----0001011", List(
      IS_CUSTOM_MIX -> True, REGFILE_WRITE_VALID -> True,
      BYPASSABLE_EXECUTE_STAGE -> False, BYPASSABLE_MEMORY_STAGE -> True,
      RS1_USE -> False, RS2_USE -> False))
    decoderService.add(M"0000100----------000-----0001011", common) // ADV
    decoderService.add(M"0000101----------000-----0001011", common) // ADVB
    // LERP8: not bypassable either
    decoderService.add(M"0000110----------000-----0001011", List(
      IS_CUSTOM_MIX -> True, REGFILE_WRITE_VALID -> True,
      BYPASSABLE_EXECUTE_STAGE -> False, BYPASSABLE_MEMORY_STAGE -> True,
      RS1_USE -> True, RS2_USE -> True))
     // SETS (7): hidden_s = rs1, single-cycle write
     decoderService.add(M"0000111----------000-----0001011", List(
       IS_CUSTOM_MIX -> True, REGFILE_WRITE_VALID -> True,
       BYPASSABLE_EXECUTE_STAGE -> False, BYPASSABLE_MEMORY_STAGE -> True,
       RS1_USE -> True, RS2_USE -> False))
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

      // 2-cycle ALU state machine
      val busy = Reg(Bool) init False
      val saved_rs1 = Reg(UInt(32 bits))
      val saved_rs2 = Reg(UInt(32 bits))
      val saved_funct7 = Reg(UInt(7 bits))
      val saved_product_lerp16 = Reg(UInt(32 bits))
      val saved_product_lerp8 = Reg(UInt(32 bits))
      val saved_product_mac = Reg(UInt(32 bits))
      val saved_sum_adv = Reg(UInt(32 bits))
      val saved_frac = Reg(UInt(16 bits))
      val saved_carry_adv = Reg(UInt(1 bit))

      // Helper functions for sign extension
      def sign8(v: UInt): SInt = v(7 downto 0).asSInt
      def sign16(v: UInt): SInt = v(15 downto 0).asSInt
      def sign32(v: UInt): SInt = v.asSInt

      // Funct7 codes for multi-cycle ops
      val is_lerp16 = funct7 === 1
      val is_mac = funct7 === 2
      val is_lerp8 = funct7 === 6
      val is_adv = funct7 === 4
      val is_advb = funct7 === 5
      val is_multicycle = is_lerp16 || is_mac || is_lerp8 || is_adv || is_advb

      // Reset busy when instruction invalid
      when(!execute.arbitration.isValid) {
        busy := False
      }

      when(execute.input(IS_CUSTOM_MIX)) {
        when(!busy) {
          // Cycle 1: Save operands, compute product, halt
          when(is_multicycle) {
            saved_rs1 := rs1
            saved_rs2 := rs2
            saved_funct7 := funct7
            saved_frac := hidden_frac
            busy := True
            execute.arbitration.haltItself := True

            // Precompute products for LERP16 and LERP8
            when(is_lerp16) {
              val s0 = sign16(rs1).resize(18)
              val s1 = sign16(rs2).resize(18)
              val dif = (s1 - s0).resize(19)
              val neg = dif < 0
              val ad = Mux(neg, (-dif).asUInt, dif.asUInt)
              val p = ad * (hidden_frac >> 1)
              saved_product_lerp16 := p.resize(32)
            }
            when(is_lerp8) {
              val s0 = sign8(rs1).resize(10)
              val s1 = sign8(rs2).resize(10)
              val dif = (s1 - s0).resize(11)
              val neg = dif < 0
              val ad = Mux(neg, (-dif).asUInt, dif.asUInt)
              val p = ad * hidden_frac
              saved_product_lerp8 := p.resize(32)
            }
            when(is_mac) {
              val g = (rs2(18 downto 0) >> 8).asSInt.resize(11)
              saved_product_mac := (hidden_s * g).resize(32).asUInt
            }
            when(is_adv) {
              val t = hidden_frac +^ rs2(15 downto 0)
              saved_sum_adv := t.resize(32)
              saved_carry_adv := Mux(t(16), U(1, 1 bit), U(0, 1 bit))
            }
          }.otherwise {
            // Single-cycle instructions (SETF, GETF)
          switch(funct7) {
            is(0) {
              hidden_frac := rs1(15 downto 0)
            }
            is(3) {
              rd := hidden_frac.resize(32 bits)
              when(execute.input(REGFILE_WRITE_VALID)) {
                execute.insert(REGFILE_WRITE_DATA) := rd.asBits
              }
            }
            is(7) {
              hidden_s := sign16(rs1)
              rd := rs1
              when(execute.input(REGFILE_WRITE_VALID)) {
                execute.insert(REGFILE_WRITE_DATA) := rd.asBits
              }
            }
          }
          }
        }.otherwise {
          // Cycle 2: Complete computation and write
          busy := False

          switch(saved_funct7) {
            // LERP16 - complete after product is ready
            is(1) {
              val s0 = sign16(saved_rs1).resize(18)
              val s1 = sign16(saved_rs2).resize(18)
              val dif = (s1 - s0).resize(19)
              val neg = dif < 0
              val p = saved_product_lerp16
              val q = (p + Mux(neg, U(32767, p.getWidth bits), U(0, p.getWidth bits))) >> 15
              val shifted = Mux(neg, -(q.asSInt), q.asSInt).resize(32)
              val result = (s0.resize(32) + shifted).resize(32)
              rd := result.asUInt
              hidden_s := result.resize(18 bits)
            }

            // MAC - multiply already done in cycle 1 by hidden_s
            is(2) {
              val result = (sign32(saved_rs1) + saved_product_mac.asSInt).resize(32)
              rd := result.asUInt
            }

            // ADV - complete carry addition
            is(4) {
              val t = saved_frac +^ saved_rs2(15 downto 0)
              val carry_bit = Mux(t(16), U(1, 32 bits), U(0, 32 bits))
              rd := (saved_rs1 +^ (saved_rs2 >> 16) +^ carry_bit).resize(32 bits)
              hidden_frac := t(15 downto 0)
            }

            // ADVB - complete with borrow
            is(5) {
              val sf = saved_rs2(15 downto 0)
              val borrow = Mux(saved_frac < sf, U(1, 32 bits), U(0, 32 bits))
              rd := (saved_rs1 - (saved_rs2 >> 16) - borrow).resize(32 bits)
              hidden_frac := saved_frac - sf
            }

            // LERP8 - complete after product is ready
            is(6) {
              val s0 = sign8(saved_rs1).resize(10)
              val p = saved_product_lerp8
              val neg = sign8(saved_rs2) < sign8(saved_rs1)
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
}
