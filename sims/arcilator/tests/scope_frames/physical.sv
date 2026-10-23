module ScopedLeaf #(parameter integer BIAS=1);
 function automatic integer note(input integer n);
  $display("SCOPE physical=[%m] n=%0d",n);
  return n+BIAS;
 endfunction
endmodule
module PhysicalTop;
 ScopedLeaf first();
 ScopedLeaf second();
 ScopedLeaf #(.BIAS(7)) third();
 initial begin:boot
  integer a,b,c;
  a=first.note(2);b=second.note(3);c=third.note(4);
  $display("SCOPE values=%0d,%0d,%0d caller=[%m]",a,b,c);
 end
endmodule
