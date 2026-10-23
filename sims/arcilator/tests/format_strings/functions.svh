  function automatic string render(input int n, input logic [64:0] wide,
                                     input string label);
    return $sformatf("n=%0d u=%0d h=%017h q=%0.2f s=[%8s]/[%-8s]",
                     n, wide, wide, real'(n)/4.0, label, label);
  endfunction
  function automatic string nested(input string inner, input logic [64:0] wide,
                                    input int n, t);
    return $sformatf("outer<%s> sgn=%0d b=%0b o=%0o c=%c p=%s",
                     inner, $signed(wide), n[7:0], n[7:0], 8'(65+t), 32'h41004243);
  endfunction
  function automatic string branch_render(input int n, input string value);
    if(n<0) return $sformatf("neg{%s}",value);
    else return $sformatf("pos{%s}",value);
  endfunction
