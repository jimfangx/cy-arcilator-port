module CaseEffects(input logic clock, input logic [7:0] value,
                   output logic [31:0] calls = 0,
                   output logic [7:0] result = 0);
  function automatic logic [7:0] item(input logic [7:0] pattern);
    calls++;
    return pattern;
  endfunction
  always @(posedge clock) begin
    case (value)
      item(8'h00): result <= 8'h00;
      item(8'h00): result <= 8'h00;
      item(8'h01): result <= 8'h01;
      default: result <= 8'hff;
    endcase
  end
endmodule
