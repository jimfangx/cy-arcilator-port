module attributes {chipyard.output_readback_safe=true,chipyard.source_time_dependence=false} {
 arc.model @Test io !hw.modty<output y:i8> storageBytes 64 {
 ^bb0(%storage: !arc.storage):
  %y=arc.root_output "y", %storage {offset=24:i32} : (!arc.storage)->!arc.state<i8>
  %one=hw.constant 1:i8
  arc.state_write %y = %one : <i8>
 }
}
