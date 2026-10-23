FIRRTL version 3.3.0
circuit AnalogHarness :
  extmodule AnalogDrive :
    input d : UInt<1>
    output pad : Analog<1>
    defname = AnalogDrive
  extmodule AnalogRead :
    output pad : Analog<1>
    output q : UInt<1>
    defname = AnalogRead
  module AnalogHarness :
    input drive : UInt<1>
    output read : UInt<1>
    inst driver of AnalogDrive
    inst reader of AnalogRead
    connect driver.d, drive
    attach (driver.pad, reader.pad)
    connect read, reader.q
