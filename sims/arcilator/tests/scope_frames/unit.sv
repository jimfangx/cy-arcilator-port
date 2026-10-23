function automatic integer unit_note(input integer n);
 $display("SCOPE unit=[%m] n=%0d",n);
 return n+3;
endfunction
module UnitTop;
 initial begin:boot
  integer result;
  result=unit_note(8);
  $display("SCOPE result=%0d caller=[%m]",result);
 end
endmodule
