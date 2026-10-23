module {
  hw.module.extern private @ParameterModel<S: none, N: si32, B: i1, F: i1, R: f64>(out ok: i1)
  hw.module @Parameters(out ok: i1) {
    %ok = hw.instance "parameters" @ParameterModel<S: none = "quote\22 slash\\ line\0A \C3\A9", N: si32 = -7, B: i1 = true, F: i1 = false, R: f64 = 1.250000e+00>() -> (ok: i1)
    hw.output %ok : i1
  }
}
