module HandleDPI(input logic clock, reset, enable,
                 input logic [7:0] key,
                 output logic [31:0] values, flags);
  import "DPI-C" function chandle native_handle_step(input chandle prior,
      input int unsigned key, output chandle produced, inout chandle history);
  import "DPI-C" native_handle_step = function chandle alternate(input chandle p,
      input int unsigned k, output chandle o, inout chandle h);
  import "DPI-C" pure function int unsigned native_handle_value(input chandle value);
  import "DPI-C" pure function chandle native_handle_identity(input chandle value);
  import "DPI-C" function string native_handle_label(input chandle value,
      input string prefix, output chandle echoed, output string copied);
  chandle saved, history, returned, produced, echoed;
  string label, copied, note;
  export "DPI-C" function handle_update;
  function chandle handle_update(input chandle value, output chandle previous,
                                  inout chandle selected, input string text,
                                  output string old_note);
    previous = saved;
    if (selected == null) selected = history;
    history = selected;
    saved = value;
    old_note = note;
    note = text;
    return saved;
  endfunction
  always @(posedge clock) begin
    if (reset) begin
      saved = null;
      history = null;
      note = "";
      values <= 0;
      flags <= 0;
    end else if (enable) begin
      if (key[2]) returned = alternate(native_handle_identity(saved), key, produced, history);
      else returned = native_handle_step(native_handle_identity(saved), key, produced, history);
      saved = returned;
      label = native_handle_label(returned, key[1] ? "ptr" : "", echoed, copied);
      values <= native_handle_value(returned) ^ (native_handle_value(produced) << 1) ^
                (native_handle_value(history) << 2) ^ label.len() ^ (copied.len() << 24);
      flags <= (returned == null) | ((produced != null) << 1) |
               ((history == produced) << 2) | ((saved != history) << 3) |
               ((echoed == returned) << 4);
    end
  end
endmodule
