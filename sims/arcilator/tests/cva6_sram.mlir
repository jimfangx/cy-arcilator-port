module {
  hw.module.extern private @SyncSpRamBeNx64<ADDR_WIDTH: ui32, DATA_DEPTH: ui32, OUT_REGS: ui32, SIM_INIT: ui32>(in %Clk_CI: i1, in %Rst_RBI: i1, in %CSel_SI: i1, in %WrEn_SI: i1, in %BEn_SI: i8, in %WrData_DI: i64, in %Addr_DI: i8, out RdData_DO: i64)
  hw.module @CVA6SRAMHarness(in %clock: i1, in %reset: i1, in %select: i1, in %write: i1, in %address: i8, in %data: i64, in %mask: i8, out q0: i64, out q1: i64, out peer0: i64, out peer1: i64) {
    %true = hw.constant true
    %rstn = comb.xor %reset, %true : i1
    %salt = hw.constant 11961589489789340985 : i64
    %a5 = hw.constant 165 : i8
    %bdata = comb.xor %data, %salt : i64
    %baddr = comb.xor %address, %a5 : i8
    %q0 = hw.instance "direct" @SyncSpRamBeNx64<ADDR_WIDTH: ui32 = 8, DATA_DEPTH: ui32 = 256, OUT_REGS: ui32 = 0, SIM_INIT: ui32 = 1>(Clk_CI: %clock: i1, Rst_RBI: %rstn: i1, CSel_SI: %select: i1, WrEn_SI: %write: i1, BEn_SI: %mask: i8, WrData_DI: %data: i64, Addr_DI: %address: i8) -> (RdData_DO: i64)
    %q1 = hw.instance "registered" @SyncSpRamBeNx64<ADDR_WIDTH: ui32 = 8, DATA_DEPTH: ui32 = 256, OUT_REGS: ui32 = 1, SIM_INIT: ui32 = 1>(Clk_CI: %clock: i1, Rst_RBI: %rstn: i1, CSel_SI: %select: i1, WrEn_SI: %write: i1, BEn_SI: %mask: i8, WrData_DI: %bdata: i64, Addr_DI: %baddr: i8) -> (RdData_DO: i64)
    %clk = seq.to_clock %clock
    %peer0 = seq.firreg %q0 clock %clk : i64
    %peer1 = seq.firreg %q1 clock %clk : i64
    hw.output %q0, %q1, %peer0, %peer1 : i64, i64, i64, i64
  }
}
