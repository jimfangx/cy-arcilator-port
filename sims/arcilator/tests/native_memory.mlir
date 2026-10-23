module {
  hw.module.extern private @ArcRTLMemory<INIT: ui32>(in %clock: i1, in %reset: i1, in %enable: i1, in %op: i2, in %address: i5, in %read_address: i5, in %data: i13, in %mask: i2, out observed: i13, out before_q: i13, out after_q: i13, out stack: i260, out next_stack: i260)
  hw.module @NativeMemoryHarness(in %clock: i1, in %reset: i1, in %enable: i1, in %op: i2, in %address: i5, in %read_address: i5, in %data: i13, in %mask: i2, out a: i13, out before_a: i13, out after_a: i13, out peer_a: i13, out b: i13, out before_b: i13, out after_b: i13, out peer_b: i13, out stack_a: i260, out next_stack_a: i260, out stack_b: i260, out next_stack_b: i260) {
    %one = hw.constant 1 : i2
    %three = hw.constant 3 : i5
    %ones = hw.constant -1 : i13
    %bop = comb.xor %op, %one : i2
    %baddr = comb.add %address, %three : i5
    %bdata = comb.xor %data, %ones : i13
    %a, %before_a, %after_a, %stack_a, %next_stack_a = hw.instance "first" @ArcRTLMemory<INIT: ui32 = 410>(clock: %clock: i1, reset: %reset: i1, enable: %enable: i1, op: %op: i2, address: %address: i5, read_address: %read_address: i5, data: %data: i13, mask: %mask: i2) -> (observed: i13, before_q: i13, after_q: i13, stack: i260, next_stack: i260)
    %b, %before_b, %after_b, %stack_b, %next_stack_b = hw.instance "second" @ArcRTLMemory<INIT: ui32 = 277>(clock: %clock: i1, reset: %reset: i1, enable: %enable: i1, op: %bop: i2, address: %baddr: i5, read_address: %read_address: i5, data: %bdata: i13, mask: %mask: i2) -> (observed: i13, before_q: i13, after_q: i13, stack: i260, next_stack: i260)
    %clk = seq.to_clock %clock
    %peer_a = seq.firreg %a clock %clk : i13
    %peer_b = seq.firreg %b clock %clk : i13
    hw.output %a, %before_a, %after_a, %peer_a, %b, %before_b, %after_b, %peer_b, %stack_a, %next_stack_a, %stack_b, %next_stack_b : i13, i13, i13, i13, i13, i13, i13, i13, i260, i260, i260, i260
  }
}
