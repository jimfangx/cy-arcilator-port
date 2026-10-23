module {
  hw.module.extern private @ArcRTLFiles(in %clock: i1, in %reset: i1, in %enable: i1, in %data: i32, in %text: i129, out status: i4)
  hw.module @NativeFilesHarness(in %clock: i1, in %reset: i1, in %enable: i1, in %data: i32, in %text: i129, out status: i4) {
    %status = hw.instance "files" @ArcRTLFiles(clock: %clock: i1, reset: %reset: i1, enable: %enable: i1, data: %data: i32, text: %text: i129) -> (status: i4)
    hw.output %status : i4
  }
}
