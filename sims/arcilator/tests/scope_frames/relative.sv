module RelativeTarget;
 function automatic integer note(input integer n);
  $display("SCOPE relative=[%m] n=%0d",n);
  return n+7;
 endfunction
 task automatic show(input integer n);
  $display("SCOPE relative_task=[%m] n=%0d",n);
 endtask
endmodule
module RelativeWrapper;
 RelativeTarget own();
 initial begin:boot
  integer a,b;
  a=own.note(3);own.show(a);
  b=RelativeTop.first.own.note(9);RelativeTop.first.own.show(b);
  $display("SCOPE relative_values=%0d,%0d caller=[%m]",a,b);
 end
endmodule
module RelativeTop;
 RelativeWrapper first();
 RelativeWrapper second();
endmodule
