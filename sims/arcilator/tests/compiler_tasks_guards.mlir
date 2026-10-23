module {
  llhd.coroutine private @timed(%signal: i1) {
    llhd.wait (%signal: i1), ^resume
  ^resume:
    llhd.return
  }
  llhd.coroutine private @transitive(%signal: i1) {
    llhd.call_coroutine @timed(%signal) : (i1) -> ()
    llhd.return
  }
  llhd.coroutine private @halting() {
    llhd.halt
  }
  llhd.coroutine private @recursive() {
    llhd.call_coroutine @recursive() : () -> ()
    llhd.return
  }
  llhd.coroutine private @mutual_a() {
    llhd.call_coroutine @mutual_b() : () -> ()
    llhd.return
  }
  llhd.coroutine private @mutual_b() {
    llhd.call_coroutine @mutual_a() : () -> ()
    llhd.return
  }
  llhd.coroutine @calls(%signal: i1) {
    llhd.call_coroutine @transitive(%signal) : (i1) -> ()
    llhd.call_coroutine @halting() : () -> ()
    llhd.call_coroutine @recursive() : () -> ()
    llhd.call_coroutine @mutual_a() : () -> ()
    llhd.return
  }
}
