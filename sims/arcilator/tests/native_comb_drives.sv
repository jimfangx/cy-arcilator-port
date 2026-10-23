module ArcRTLCombDrives (
  input clock, reset, valid,
  input [3:0] enabled, flags,
  input [3:0][2:0] selectors,
  output reg [1:0][4:0] mask,
  output reg [1:0][1:0] sfu,
  output reg [1:0] any,
  output reg [15:0] captured
);
  for (genvar bank = 0; bank < 2; bank++) begin
    always @(*) begin
      mask[bank] = '0;
      sfu[bank] = '0;
      any[bank] = 0;
      if (valid ^ 1'(bank)) begin
        for (integer i = 0; i < 4; i++) begin
          if (enabled[i]) begin
            any[bank] = 1;
            mask[bank][selectors[i]] = 1;
            if (selectors[i] == 2) sfu[bank][flags[i]] = 1;
          end
        end
      end
    end
  end
  always @(posedge clock)
    if (reset) captured <= 16'had37;
    else captured <= {any, sfu, mask};
endmodule
