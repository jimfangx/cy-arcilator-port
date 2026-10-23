module Target;
 function automatic integer note(input integer n);
  $display("SCOPE target=[%m] n=%0d",n);
  return n+7;
 endfunction
 task automatic show(input integer n);
  $display("SCOPE task=[%m] n=%0d",n);
 endtask
endmodule
module Caller;
 initial begin:boot
  integer value;
  value=SiblingTop.target.note(4);
  SiblingTop.target.show(value);
  $display("SCOPE sibling_result=%0d caller=[%m]",value);
 end
endmodule
module SiblingTop;
 Target target();
 Caller caller();
endmodule
