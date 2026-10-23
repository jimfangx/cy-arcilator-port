module TemporalRTL(input logic clock, reset, enable, a, b, c,
input logic [7:0] data, output logic io_success);
assign io_success=0;
assert property (@(posedge clock) disable iff (reset || !enable) a |=> b);
assert property (@(posedge clock) disable iff (reset || !enable) a |-> b);
assert property (@(posedge clock) disable iff (reset || !enable) (a ##2 b) |=> c);
assert property (@(posedge clock) disable iff (reset || !enable) (a ##[1:3] b) |-> c);
assert property (@(posedge clock) disable iff (reset || !enable) a |=> $stable(data));
assume property (@(posedge clock) disable iff (reset || !enable) a |=> (b || c));
assert property (@(negedge clock) disable iff (reset || !enable) a |=> c);
assert property (@(posedge clock) disable iff (reset || !enable) a |-> (b |=> c));
assert property (@(posedge clock) disable iff (reset || !enable) a[*2:3] |-> b);
assert property (@(posedge clock) disable iff (reset || !enable)
 ((a ##1 b) intersect (b ##1 c)) |-> !a);
assert property (@(posedge clock) disable iff (reset || !enable)
 ((a ##1 b) and (c ##2 a)) |-> !b);
assert property (@(posedge clock) disable iff (reset || !enable)
 ((a ##1 b) or (c ##2 a)) |-> !c);
endmodule
