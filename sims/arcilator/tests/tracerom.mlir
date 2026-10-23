module {
  hw.module.extern private @TraceROM(in %clock : !seq.clock, in %reset : i1, out stream_valid : i1, in %stream_ready : i1, out stream_bits_data : i64, out stream_bits_keep : i8, out stream_bits_last : i1, out macAddr : i48, out length : i32)
  hw.module @TraceHarness(in %clock : !seq.clock, in %reset : i1, in %stall : i1, out count : i32, out sample : i73, out checksum : i64, out valid : i1, out ready : i1, out data : i64, out keep : i8, out last : i1, out mac : i48, out length : i32) {
    %zero = hw.constant 0 : i32
    %one = hw.constant 1 : i32
    %zero64 = hw.constant 0 : i64
    %zero73 = hw.constant 0 : i73
    %true = hw.constant true
    %not_stall = comb.xor %stall, %true : i1
    %valid, %data, %keep, %last, %mac, %length = hw.instance "trace" @TraceROM(clock: %clock: !seq.clock, reset: %reset: i1, stream_ready: %ready: i1) -> (stream_valid: i1, stream_bits_data: i64, stream_bits_keep: i8, stream_bits_last: i1, macAddr: i48, length: i32)
    %ready = comb.and %valid, %not_stall : i1
    %count = seq.firreg %next_count clock %clock reset sync %reset, %zero : i32
    %increment = comb.add %count, %one : i32
    %next_count = comb.mux %ready, %increment, %count : i32
    %sample = seq.firreg %next_sample clock %clock reset sync %reset, %zero73 : i73
    %packet = comb.concat %last, %keep, %data : i1, i8, i64
    %next_sample = comb.mux %ready, %packet, %sample : i73
    %checksum = seq.firreg %next_checksum clock %clock reset sync %reset, %zero64 : i64
    %xor = comb.xor %checksum, %data : i64
    %next_checksum = comb.mux %ready, %xor, %checksum : i64
    hw.output %count, %sample, %checksum, %valid, %ready, %data, %keep, %last, %mac, %length : i32, i73, i64, i1, i1, i64, i8, i1, i48, i32
  }
}
