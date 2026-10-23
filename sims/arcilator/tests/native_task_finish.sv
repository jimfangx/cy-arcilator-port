module TaskFinish(input bit clock, reset,
                  output int preflights = 0, resumed = 0);
  import "DPI-C" context task host_preflight();
  import "DPI-C" context task host_finish();
  export "DPI-C" task preflight;
  task preflight(); endtask
  export "DPI-C" task finish_now;
  task finish_now(); $finish; endtask
  always @(posedge clock) if (!reset) begin
    host_preflight();
    preflights++;
    host_finish();
    resumed++;
  end
endmodule
