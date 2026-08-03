; One function contains a legal pure hoist; the other contains a division
; that must not be speculated into its dominating predecessor.

define i32 @safe_hoist(i32 %x) {
entry:
  br label %work

work:
  %hoisted = add i32 %x, 7
  ret i32 %hoisted
}

define i32 @unsafe_division(i32 %x) {
entry:
  br label %work

work:
  %quotient = sdiv i32 84, %x
  ret i32 %quotient
}

; CHECK-LABEL: define i32 @safe_hoist
; CHECK-LABEL: entry:
; CHECK-NEXT: %hoisted = add i32 %x, 7
; CHECK-NEXT: br label %work
;
; CHECK-LABEL: define i32 @unsafe_division
; CHECK-LABEL: entry:
; CHECK-NEXT: br label %work
; CHECK-LABEL: work:
; CHECK-NEXT: %quotient = sdiv i32 84, %x
