module StringImports(input logic clock, reset, enable,
                     input logic [7:0] key,
                     output logic [31:0] lengths, firsts, old_length);
  import "DPI-C" function string native_string_mix(input string prefix,
      input int unsigned key, output string produced, inout string history);
  import "DPI-C" native_string_mix = function string alternate(input string p,
      input int unsigned k, output string o, inout string h);
  import "DPI-C" pure function string native_string_identity(input string text);
  import "DPI-C" pure function int native_string_size(input string text);
  import "DPI-C" function void native_string_clobber();
  string returned, produced, history, saved, prefix;
  always @(posedge clock) begin
    if (reset) begin
      history = "seed";
      saved = "";
      lengths <= 0;
      firsts <= 0;
      old_length <= 0;
    end else if (enable) begin
      old_length <= saved.len();
      case (key[1:0])
        0: prefix = "";
        1: prefix = "alpha";
        2: prefix = "Grüße";
        3: prefix = "Z";
      endcase
      if (key[2]) returned = alternate(native_string_identity(prefix), key, produced, history);
      else returned = native_string_mix(native_string_identity(prefix), key, produced, history);
      saved = returned;
      native_string_clobber();
      lengths <= native_string_size(returned) | (produced.len() << 8) | (history.len() << 16);
      firsts <= returned.getc(0) | (produced.getc(0) << 8) | (history.getc(0) << 16);
    end
  end
endmodule
