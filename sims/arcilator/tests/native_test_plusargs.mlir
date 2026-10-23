module {
  hw.module.extern @TestPlusargs(in %clock:i1,out matched:i7,out initial_matched:i7)
  hw.module @TestPlusargsHarness(in %clock:i1,out matched:i7,out initial_matched:i7) {
   %a,%b=hw.instance "dut" @TestPlusargs(clock:%clock:i1)->(matched:i7,initial_matched:i7)
   hw.output %a,%b:i7,i7
  }
}
