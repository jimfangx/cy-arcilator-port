`timescale 1ps/1fs
module ProceduralReference;
  reg clock = 0, reset = 1, enable = 0;
  reg [7:0] data = 0;
  reg [1:0][7:0] lanes = 16'habcd;
  wire [7:0] q, ticks, unused_diagnostic;
  wire [63:0] stamp, clock_stamp, held_stamp;
  arc_payload_t payload;
  wire [1:0][7:0] inverted;
  ArcRTLProcedural dut(.*);
  task automatic check(input int state, count, time_value, lane_value, clock_time, held_time);
    if (q !== 8'(state) || ticks !== 8'(count) || stamp !== 64'(time_value) ||
        payload !== 24'((state << 16) | lane_value) ||
        inverted !== 16'(lane_value ^ 'h5aa5) ||
        clock_stamp !== 64'(clock_time) || held_stamp !== 64'(held_time))
      $fatal(1, "SV procedural reference mismatch at %0t: q=%0d ticks=%0d stamp=%0d", $time, q, ticks, stamp);
  endtask
  initial begin
    #1; clock = 1;
    #0.001; check(0, 0, 0, 'habcd, 0, 0);
    #0.999; clock = 0; reset = 0; enable = 1; data = 7;
    #0.001; check(0, 1, 2, 'habcd, 0, 0);
    #0.999; clock = 1;
    #0.001; check(10, 2, 3, 'habcd, 3, 0);
    #0.999; data = 5; lanes = 'h1837;
    #0.001; check(10, 2, 3, 'h1837, 3, 0);
    #0.999; clock = 0;
    #0.001; check(10, 3, 5, 'h1837, 3, 2);
    #0.999; clock = 1;
    #0.001; check(18, 4, 6, 'h1837, 6, 2);
    #13.999; check(18, 4, 6, 'h1837, 6, 2);
    $display("PASS: SV reference for imported procedural RTL and physical time");
    $finish;
  end
endmodule
