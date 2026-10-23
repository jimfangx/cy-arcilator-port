module FrameTarget;
 import "DPI-C" context function int scope_probe(input int tag);
 import "DPI-C" context task scope_empty();
 function automatic int call_probe(input int tag);
  return scope_probe(tag);
 endfunction
 task automatic call_task(input int tag, output int result);
  result=call_probe(tag);
 endtask
endmodule
module FrameWrapper;
 FrameTarget own();
 function automatic int route(input int tag);
  return own.call_probe(tag);
 endfunction
 task automatic route_empty;
  own.scope_empty;
 endtask
 initial begin:boot
  int a,b,c,d,e,f;
  own.scope_empty;
  route_empty;
  FrameTop.first.own.scope_empty;
  a=own.scope_probe(3);
  b=route(5);
  own.call_task(7,c);
  d=FrameTop.first.own.scope_probe(11);
  e=FrameTop.first.own.call_probe(13);
  FrameTop.first.own.call_task(17,f);
  if(a!=10||b!=12||c!=14||d!=18||e!=20||f!=24)
   $fatal(1,"context call changed values");
 end
endmodule
module FrameTop;
 FrameWrapper first();
 FrameWrapper second();
endmodule
