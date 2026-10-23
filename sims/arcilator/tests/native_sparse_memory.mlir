module {
  hw.module @SparseMemoryHarness(in %clock : i1, in %peer : i1, in %we0 : i1, in %we1 : i1, in %re : i1, in %ra : i4, in %wa0 : i4, in %wa1 : i4, in %data0 : i65, in %data1 : i65, in %mask0 : i5, in %mask1 : i5, in %mode : i1, in %rw_enable : i1, out old_data : i65, out new_data : i65, out async_data : i65, out rw_data : i13) {
    %0 = seq.to_clock %clock
    %1 = seq.to_clock %peer
    %2 = comb.extract %data0 from 0 : (i65) -> i13
    %old_memory = seq.firmem 1, 1, old, port_order : <9 x 65, mask 5>
    seq.firmem.write_port %old_memory[%wa0] = %data0, clock %0 enable %we0 mask %mask0 : <9 x 65, mask 5>, i5
    seq.firmem.write_port %old_memory[%wa1] = %data1, clock %0 enable %we1 mask %mask1 : <9 x 65, mask 5>, i5
    %3 = seq.firmem.read_port %old_memory[%ra], clock %0 enable %re : <9 x 65, mask 5>
    %new_memory = seq.firmem 1, 1, new, port_order : <9 x 65, mask 5>
    seq.firmem.write_port %new_memory[%wa0] = %data0, clock %0 enable %we0 mask %mask0 : <9 x 65, mask 5>, i5
    seq.firmem.write_port %new_memory[%wa1] = %data1, clock %0 enable %we1 mask %mask1 : <9 x 65, mask 5>, i5
    %4 = seq.firmem.read_port %new_memory[%ra], clock %0 enable %re : <9 x 65, mask 5>
    %async_memory = seq.firmem 0, 1, undefined, port_order : <9 x 65, mask 5>
    seq.firmem.write_port %async_memory[%wa0] = %data0, clock %0 enable %we0 mask %mask0 : <9 x 65, mask 5>, i5
    seq.firmem.write_port %async_memory[%wa1] = %data1, clock %0 enable %we1 mask %mask1 : <9 x 65, mask 5>, i5
    %5 = seq.firmem.read_port %async_memory[%ra], clock %0 enable %re : <9 x 65, mask 5>
    %rw_memory = seq.firmem 1, 1, old, port_order : <9 x 13, mask 1>
    %6 = seq.firmem.read_write_port %rw_memory[%wa0] = %2 if %mode, clock %1 enable %rw_enable : <9 x 13, mask 1>
    hw.output %3, %4, %5, %6 : i65, i65, i65, i13
  }
}

