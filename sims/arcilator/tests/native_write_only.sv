module WriteOnly(input logic clock, peer, enable, peer_enable,
                        input logic [64:0] data,
                        output logic [12:0] narrow,
                        output logic [64:0] wide);
  // No initializer or read inside either process. Disabled writes retain the
  // external register's previous value across activations and independent clocks.
  always @(posedge clock)
    if (enable) narrow <= data[12:0];
  always @(posedge peer)
    if (peer_enable) wide <= data;
endmodule
