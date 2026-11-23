package vexriscv.demo

import spinal.core._
import spinal.lib._
import vexriscv.{plugin, VexRiscv, VexRiscvConfig}
import vexriscv.plugin._
import vexriscv.ip.{DataCacheConfig, InstructionCacheConfig}
import vexriscv.ip.fpu._

/** Karabas player CPU: larger caches and a single-precision FPU. */
object GenKarabas extends App {
  def cpu() = new VexRiscv(
    config = VexRiscvConfig(
      plugins = List(
        new IBusCachedPlugin(
          resetVector = 0x00000000l,
          prediction = STATIC,
          config = InstructionCacheConfig(
            cacheSize = 16384,
            bytePerLine = 32,
            wayCount = 1,
            addressWidth = 32,
            cpuDataWidth = 32,
            memDataWidth = 32,
            catchIllegalAccess = true,
            catchAccessFault = true,
            asyncTagMemory = false,
            twoCycleRam = true,
            twoCycleCache = true
          )
        ),
        new DBusCachedPlugin(
          dBusCmdMasterPipe = true,
          config = new DataCacheConfig(
            cacheSize = 8192,
            bytePerLine = 32,
            wayCount = 1,
            addressWidth = 32,
            cpuDataWidth = 32,
            memDataWidth = 32,
            catchAccessError = true,
            catchIllegal = true,
            catchUnaligned = true
          )
        ),
        new StaticMemoryTranslatorPlugin(
          ioRange = _(31 downto 28) === 0x8
        ),
        new DecoderSimplePlugin(
          catchIllegalInstruction = true
        ),
        new RegFilePlugin(
          regFileReadyKind = plugin.SYNC,
          zeroBoot = false
        ),
        new IntAluPlugin,
        new SrcPlugin(
          separatedAddSub = false,
          executeInsertion = true
        ),
        new FullBarrelShifterPlugin,
        new HazardSimplePlugin(
          bypassExecute = true,
          bypassMemory = true,
          bypassWriteBack = true,
          bypassWriteBackBuffer = true,
          pessimisticUseSrc = false,
          pessimisticWriteRegFile = false,
          pessimisticAddressMatch = false
        ),
        new MulPlugin,
        new DivPlugin,
        new CsrPlugin(CsrPluginConfig.small),
        new FpuPlugin(
          externalFpu = false,
          simHalt = false,
          p = FpuParameter(withDouble = false)
        ),
        new BranchPlugin(
          earlyBranch = false,
          catchAddressMisaligned = true
        ),
        new YamlPlugin("VexRiscv.yaml")
      )
    )
  )

  SpinalVerilog {
    val top = cpu()
    val cfg = top.config
    top.rework {
      for (plugin <- cfg.plugins) plugin match {
        case plugin: IBusCachedPlugin =>
          plugin.iBus.setAsDirectionLess()
          master(plugin.iBus.toWishbone()).setName("iBusWishbone")
        case plugin: DBusCachedPlugin =>
          plugin.dBus.setAsDirectionLess()
          master(plugin.dBus.toWishbone()).setName("dBusWishbone")
        case _ =>
      }
    }
    top
  }
}
