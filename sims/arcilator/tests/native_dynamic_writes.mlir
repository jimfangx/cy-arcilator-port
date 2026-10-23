module {
  hw.module.extern @DynamicWrites(in %active : i9, in %banks : i27, in %tag_index : i3, in %payload_index : i3, in %read_index : i3, in %tag_value : i5, in %payload_value : i13, in %write_enable : i2, out matrix : i72, out fields : i90, out read_tag : i5, out read_payload : i13, out chained_payload : i13)
  hw.module @DynamicWritesHarness(in %active : i9, in %banks : i27, in %tag_index : i3, in %payload_index : i3, in %read_index : i3, in %tag_value : i5, in %payload_value : i13, in %write_enable : i2, out matrix : i72, out fields : i90, out read_tag : i5, out read_payload : i13, out chained_payload : i13) {
    %dut.matrix, %dut.fields, %dut.read_tag, %dut.read_payload, %dut.chained_payload = hw.instance "dut" @DynamicWrites(active: %active: i9, banks: %banks: i27, tag_index: %tag_index: i3, payload_index: %payload_index: i3, read_index: %read_index: i3, tag_value: %tag_value: i5, payload_value: %payload_value: i13, write_enable: %write_enable: i2) -> (matrix: i72, fields: i90, read_tag: i5, read_payload: i13, chained_payload: i13)
    hw.output %dut.matrix, %dut.fields, %dut.read_tag, %dut.read_payload, %dut.chained_payload : i72, i90, i5, i13, i13
  }
}

