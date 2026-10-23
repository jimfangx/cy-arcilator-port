package logpkg;
 function automatic integer note(input integer n);
  $display("SCOPE package=[%m] n=%0d",n);
  return n+7;
 endfunction
endpackage
module PackageTop;
 initial begin:boot
  integer result;
  result=logpkg::note(5);
  $display("SCOPE result=%0d caller=[%m]",result);
 end
endmodule
