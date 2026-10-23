module BareTarget;
 task automatic note;
  $display("SCOPE bare_target=[%m]");
 endtask
endmodule
module BareWrapper;
 BareTarget own();
 task automatic route;
  own.note;
  $display("SCOPE bare_route=[%m]");
 endtask
 initial begin:boot
  own.note;
  route;
  BareTop.first.own.note;
  BareTop.first.route;
  $display("SCOPE bare_caller=[%m]");
 end
endmodule
module BareTop;
 BareWrapper first();
 BareWrapper second();
endmodule
