module {
  hw.module.extern private @Wide<FORMAT: none, DEFAULT: ui129, WIDTH: ui32>(out out : i129) attributes {verilogName = "plusarg_reader"}
  hw.module.extern private @Small<FORMAT: none, DEFAULT: ui3, WIDTH: ui32>(out out : i3) attributes {verilogName = "plusarg_reader"}
  hw.module.extern private @Signed<FORMAT: none, DEFAULT: i65, WIDTH: ui32>(out out : i65) attributes {verilogName = "plusarg_reader"}
  hw.module.extern private @Upper<FORMAT: none, DEFAULT: ui129, WIDTH: ui32>(out out : i129) attributes {verilogName = "plusarg_reader"}
  hw.module.extern private @Zero<FORMAT: none, DEFAULT: ui65, WIDTH: ui32>(out out : i65) attributes {verilogName = "plusarg_reader"}
  hw.module.extern private @Literal<FORMAT: none, DEFAULT: ui9, WIDTH: ui32>(out out : i9) attributes {verilogName = "plusarg_reader"}
  hw.module @WidePlusargHarness(in %clock : i1, in %reset : i1, out wide : i129, out small : i3, out signed : i65, out upper : i129, out zero : i65, out literal : i9) {
    %wide = hw.instance "wide" @Wide<FORMAT: none = "wide=%h", DEFAULT: ui129 = 340282366920938463463374607431768211459, WIDTH: ui32 = 129>() -> (out: i129)
    %small = hw.instance "small" @Small<FORMAT: none = "small=%o", DEFAULT: ui3 = 7, WIDTH: ui32 = 3>() -> (out: i3)
    %signed = hw.instance "signed" @Signed<FORMAT: none = "signed=%d", DEFAULT: i65 = -1, WIDTH: ui32 = 65>() -> (out: i65)
    %upper = hw.instance "upper" @Upper<FORMAT: none = "upper=%H", DEFAULT: ui129 = 0, WIDTH: ui32 = 129>() -> (out: i129)
    %zero = hw.instance "zero" @Zero<FORMAT: none = "zero=%00D", DEFAULT: ui65 = 0, WIDTH: ui32 = 65>() -> (out: i65)
    %literal = hw.instance "literal" @Literal<FORMAT: none = "lane%%=%0O", DEFAULT: ui9 = 0, WIDTH: ui32 = 9>() -> (out: i9)
    hw.output %wide, %small, %signed, %upper, %zero, %literal : i129, i3, i65, i129, i65, i9
  }
}
