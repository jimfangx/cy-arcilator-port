module UpTarget;
 function automatic integer note(input integer n);
  $display("SCOPE upward=[%m] n=%0d",n);
  return n+3;
 endfunction
endmodule
module UpCaller;
 function automatic integer call_peer(input integer n);
  integer v;
  v=target.note(n);
  $display("SCOPE upward_caller=[%m] n=%0d",v);
  return v;
 endfunction
 initial begin:boot
  integer v;
  v=call_peer(5);
  $display("SCOPE upward_result=%0d caller=[%m]",v);
 end
endmodule
module UpWrapper;
 UpTarget target();
 UpCaller caller();
endmodule
module UpTop;
 UpWrapper first();
 UpWrapper second();
endmodule
