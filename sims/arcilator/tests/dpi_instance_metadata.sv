module MetadataLeaf(input int value, output int result);
  import "DPI-C" context function int metadata_probe(input int data);
  function automatic int private_probe(input int data); return metadata_probe(data); endfunction
  always_comb result=metadata_probe(value)+private_probe(value);
endmodule
module MetadataTop(input int value, output int first,second,third);
  MetadataLeaf a(value,first);
  MetadataLeaf b(value,second);
  always_comb third=b.metadata_probe(value)+a.metadata_probe(value)+b.private_probe(value)+a.private_probe(value);
endmodule
