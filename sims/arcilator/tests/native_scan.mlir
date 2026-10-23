module {
  hw.module.extern private @VXScan5<N: ui32, OP: ui32, REVERSE: ui32>(in %data_in : i5, out data_out : i5) attributes {verilogName = "VX_scan"}
  hw.module.extern private @VXScan9<N: ui32, OP: ui32, REVERSE: ui32>(in %data_in : i9, out data_out : i9) attributes {verilogName = "VX_scan"}
  hw.module.extern private @VXScan17<N: ui32, OP: ui32, REVERSE: ui32>(in %data_in : i17, out data_out : i17) attributes {verilogName = "VX_scan"}
  hw.module @NativeScanHarness(in %clock : i1, in %data : i17, out scan_5_0_0 : i5, out scan_5_0_1 : i5, out scan_5_1_0 : i5, out scan_5_1_1 : i5, out scan_5_2_0 : i5, out scan_5_2_1 : i5, out scan_9_0_0 : i9, out scan_9_0_1 : i9, out scan_9_1_0 : i9, out scan_9_1_1 : i9, out scan_9_2_0 : i9, out scan_9_2_1 : i9, out scan_17_0_0 : i17, out scan_17_0_1 : i17, out scan_17_1_0 : i17, out scan_17_1_1 : i17, out scan_17_2_0 : i17, out scan_17_2_1 : i17) {
    %data5 = comb.extract %data from 0 : (i17) -> i5
    %data9 = comb.extract %data from 0 : (i17) -> i9
    %scan_5_0_0 = hw.instance "scan_5_0_0" @VXScan5<N: ui32 = 5, OP: ui32 = 0, REVERSE: ui32 = 0>(data_in: %data5: i5) -> (data_out: i5)
    %scan_5_0_1 = hw.instance "scan_5_0_1" @VXScan5<N: ui32 = 5, OP: ui32 = 0, REVERSE: ui32 = 1>(data_in: %data5: i5) -> (data_out: i5)
    %scan_5_1_0 = hw.instance "scan_5_1_0" @VXScan5<N: ui32 = 5, OP: ui32 = 1, REVERSE: ui32 = 0>(data_in: %data5: i5) -> (data_out: i5)
    %scan_5_1_1 = hw.instance "scan_5_1_1" @VXScan5<N: ui32 = 5, OP: ui32 = 1, REVERSE: ui32 = 1>(data_in: %data5: i5) -> (data_out: i5)
    %scan_5_2_0 = hw.instance "scan_5_2_0" @VXScan5<N: ui32 = 5, OP: ui32 = 2, REVERSE: ui32 = 0>(data_in: %data5: i5) -> (data_out: i5)
    %scan_5_2_1 = hw.instance "scan_5_2_1" @VXScan5<N: ui32 = 5, OP: ui32 = 2, REVERSE: ui32 = 1>(data_in: %data5: i5) -> (data_out: i5)
    %scan_9_0_0 = hw.instance "scan_9_0_0" @VXScan9<N: ui32 = 9, OP: ui32 = 0, REVERSE: ui32 = 0>(data_in: %data9: i9) -> (data_out: i9)
    %scan_9_0_1 = hw.instance "scan_9_0_1" @VXScan9<N: ui32 = 9, OP: ui32 = 0, REVERSE: ui32 = 1>(data_in: %data9: i9) -> (data_out: i9)
    %scan_9_1_0 = hw.instance "scan_9_1_0" @VXScan9<N: ui32 = 9, OP: ui32 = 1, REVERSE: ui32 = 0>(data_in: %data9: i9) -> (data_out: i9)
    %scan_9_1_1 = hw.instance "scan_9_1_1" @VXScan9<N: ui32 = 9, OP: ui32 = 1, REVERSE: ui32 = 1>(data_in: %data9: i9) -> (data_out: i9)
    %scan_9_2_0 = hw.instance "scan_9_2_0" @VXScan9<N: ui32 = 9, OP: ui32 = 2, REVERSE: ui32 = 0>(data_in: %data9: i9) -> (data_out: i9)
    %scan_9_2_1 = hw.instance "scan_9_2_1" @VXScan9<N: ui32 = 9, OP: ui32 = 2, REVERSE: ui32 = 1>(data_in: %data9: i9) -> (data_out: i9)
    %scan_17_0_0 = hw.instance "scan_17_0_0" @VXScan17<N: ui32 = 17, OP: ui32 = 0, REVERSE: ui32 = 0>(data_in: %data: i17) -> (data_out: i17)
    %scan_17_0_1 = hw.instance "scan_17_0_1" @VXScan17<N: ui32 = 17, OP: ui32 = 0, REVERSE: ui32 = 1>(data_in: %data: i17) -> (data_out: i17)
    %scan_17_1_0 = hw.instance "scan_17_1_0" @VXScan17<N: ui32 = 17, OP: ui32 = 1, REVERSE: ui32 = 0>(data_in: %data: i17) -> (data_out: i17)
    %scan_17_1_1 = hw.instance "scan_17_1_1" @VXScan17<N: ui32 = 17, OP: ui32 = 1, REVERSE: ui32 = 1>(data_in: %data: i17) -> (data_out: i17)
    %scan_17_2_0 = hw.instance "scan_17_2_0" @VXScan17<N: ui32 = 17, OP: ui32 = 2, REVERSE: ui32 = 0>(data_in: %data: i17) -> (data_out: i17)
    %scan_17_2_1 = hw.instance "scan_17_2_1" @VXScan17<N: ui32 = 17, OP: ui32 = 2, REVERSE: ui32 = 1>(data_in: %data: i17) -> (data_out: i17)
    hw.output %scan_5_0_0, %scan_5_0_1, %scan_5_1_0, %scan_5_1_1, %scan_5_2_0, %scan_5_2_1, %scan_9_0_0, %scan_9_0_1, %scan_9_1_0, %scan_9_1_1, %scan_9_2_0, %scan_9_2_1, %scan_17_0_0, %scan_17_0_1, %scan_17_1_0, %scan_17_1_1, %scan_17_2_0, %scan_17_2_1 : i5, i5, i5, i5, i5, i5, i9, i9, i9, i9, i9, i9, i17, i17, i17, i17, i17, i17
  }
}
