module NestedTarget;
 function automatic integer note(input integer n);
  $display("SCOPE nested_target=[%m] n=%0d",n);
  return n+7;
 endfunction
 task automatic show(input integer n);
  $display("SCOPE nested_task=[%m] n=%0d",n);
 endtask
endmodule
module NestedWrapper;
 NestedTarget own();
 function automatic integer inner(input integer n);
  integer value;
  value=own.note(n);
  $display("SCOPE inner=[%m] n=%0d",value);
  return value+1;
 endfunction
 function automatic integer outer(input integer n);
  integer value;
  value=inner(n);
  $display("SCOPE outer=[%m] n=%0d",value);
  return value+1;
 endfunction
 task automatic route(input integer n);
  integer value;
  value=outer(n);
  own.show(value);
  $display("SCOPE route=[%m] n=%0d",value);
 endtask
 initial begin:boot
  integer a,b;
  a=outer(3);
  b=NestedTop.first.outer(9);
  route(4);
  NestedTop.first.route(10);
  $display("SCOPE nested_values=%0d,%0d caller=[%m]",a,b);
 end
endmodule
module NestedTop;
 NestedWrapper first();
 NestedWrapper second();
endmodule
