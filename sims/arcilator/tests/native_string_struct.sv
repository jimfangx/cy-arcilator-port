module StringStructImports(input logic clock, reset, enable,
                     input logic [7:0] key,
                     output logic [31:0] lengths, firsts, old_length);
  import "DPI-C" function string native_string_mix(input string prefix,
      input int unsigned key, output string produced, inout string history);
  import "DPI-C" native_string_mix = function string alternate(input string p,
      input int unsigned k, output string o, inout string h);
  import "DPI-C" pure function string native_string_identity(input string text);
  import "DPI-C" pure function int native_string_size(input string text);
  import "DPI-C" function void native_string_clobber();
  typedef struct { string history; string saved; } text_t;
  typedef struct { bit [31:0] guard; text_t text; string returned; string produced; } entry_t;
  entry_t bank[2], saved_copy;
  string prefix;
  bit slot;
  always_comb slot=key[0];
  always @(posedge clock) begin
    if (reset) begin
      bank[0].guard=32'h8bc57231;
      bank[1].guard=32'h8bc57231;
      bank[slot].text.history = "seed";
      bank[slot].text.saved = "";
      lengths <= 0;
      firsts <= 0;
      old_length <= 0;
    end else if (enable) begin
      old_length <= bank[slot].text.saved.len();
      case (key[1:0])
        0: prefix = "";
        1: prefix = "alpha";
        2: prefix = "Grüße";
        3: prefix = "Z";
      endcase
      if (key[2]) bank[slot].returned = alternate(native_string_identity(prefix), key, bank[slot].produced, bank[slot].text.history);
      else bank[slot].returned = native_string_mix(native_string_identity(prefix), key, bank[slot].produced, bank[slot].text.history);
      bank[slot].text.saved = bank[slot].returned;
      native_string_clobber();
      lengths <= native_string_size(bank[slot].returned) | (bank[slot].produced.len() << 8) | (bank[slot].text.history.len() << 16);
      saved_copy=bank[slot];
      bank[slot]=saved_copy;
      if(bank[slot].guard!=32'h8bc57231) $fatal(1,"struct integer sibling corrupted");
      firsts <= bank[slot].returned.getc(0) | (bank[slot].produced.getc(0) << 8) | (bank[slot].text.history.getc(0) << 16);
    end
    bank[!slot]=bank[slot];
  end
endmodule
