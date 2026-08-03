; The hot arm does not dominate the latch.  Moving %next into it would make
; the loop PHI invalid on the cold-arm edge.

define i32 @loop_phi_diamond(i32 %limit, i1 %choose_hot) {
entry:
  br label %loop

loop:
  %i = phi i32 [ 0, %entry ], [ %next, %latch ]
  %keep_going = icmp slt i32 %i, %limit
  br i1 %keep_going, label %dispatch, label %exit, !prof !0

dispatch:
  br i1 %choose_hot, label %hot, label %cold, !prof !1

hot:
  %hot_value = add i32 %i, 9
  br label %latch

cold:
  %cold_value = add i32 %i, 3
  br label %latch

latch:
  %selected = phi i32 [ %hot_value, %hot ], [ %cold_value, %cold ]
  %next = add i32 %i, 1
  %unused = add i32 %selected, 1
  br label %loop

exit:
  ret i32 %i
}

!0 = !{!"branch_weights", i32 1000, i32 1}
!1 = !{!"branch_weights", i32 55, i32 45}

; CHECK-LABEL: latch:
; CHECK: %selected = phi
; CHECK: %next = add i32 %i, 1
; CHECK: br label %loop
