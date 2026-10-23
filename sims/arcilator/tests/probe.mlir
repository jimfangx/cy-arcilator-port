module {
  firrtl.circuit "ProbeHarness" {
    firrtl.module private @ProbeSource(in %clock: !firrtl.clock, in %reset: !firrtl.uint<1>, in %d: !firrtl.uint<8>, out %tap: !firrtl.probe<uint<8>>) {
      %zero = firrtl.constant 0 : !firrtl.uint<8>
      %value = firrtl.regreset %clock, %reset, %zero : !firrtl.clock, !firrtl.uint<1>, !firrtl.uint<8>, !firrtl.uint<8>
      firrtl.matchingconnect %value, %d : !firrtl.uint<8>
      %ref = firrtl.ref.send %value : !firrtl.uint<8>
      firrtl.ref.define %tap, %ref : !firrtl.probe<uint<8>>
    }
    firrtl.module @ProbeHarness(in %clock: !firrtl.clock, in %reset: !firrtl.uint<1>, in %drive: !firrtl.uint<8>, out %read: !firrtl.uint<8>) {
      %source_clock, %source_reset, %source_d, %source_tap = firrtl.instance source sym @source @ProbeSource(in clock: !firrtl.clock, in reset: !firrtl.uint<1>, in d: !firrtl.uint<8>, out tap: !firrtl.probe<uint<8>>)
      firrtl.matchingconnect %source_clock, %clock : !firrtl.clock
      firrtl.matchingconnect %source_reset, %reset : !firrtl.uint<1>
      firrtl.matchingconnect %source_d, %drive : !firrtl.uint<8>
      %observed = firrtl.ref.resolve %source_tap : !firrtl.probe<uint<8>>
      firrtl.matchingconnect %read, %observed : !firrtl.uint<8>
    }
  }
}
