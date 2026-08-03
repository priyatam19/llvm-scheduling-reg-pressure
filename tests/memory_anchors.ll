; Memory/call operations must remain in source order for every scheduler.

@slot = global i32 0, align 4

define void @external_write(ptr %p) {
entry:
  store i32 7, ptr %p, align 4
  ret void
}

define void @write_slot(ptr %p) {
entry:
  store i32 41, ptr %p, align 4
  ret void
}

define i32 @call_then_load(ptr %p) {
entry:
  call void @external_write(ptr %p)
  %value = load i32, ptr %p, align 4
  %adjusted = add i32 %value, 1
  ret i32 %adjusted
}

define i32 @main() {
entry:
  call void @write_slot(ptr @slot)
  %value = load i32, ptr @slot, align 4
  %status = sub i32 %value, 41
  ret i32 %status
}

define i32 @volatile_atomic_fence(ptr %p) {
entry:
  store volatile i32 1, ptr %p, align 4
  %first = load atomic i32, ptr %p seq_cst, align 4
  fence seq_cst
  store atomic i32 2, ptr %p seq_cst, align 4
  %second = load volatile i32, ptr %p, align 4
  %sum = add i32 %first, %second
  ret i32 %sum
}

; CHECK-LABEL: define i32 @call_then_load
; CHECK: call void @external_write
; CHECK-NEXT: %value = load i32, ptr %p
; CHECK: ret i32
;
; CHECK-LABEL: define i32 @main
; CHECK: call void @write_slot
; CHECK-NEXT: %value = load i32, ptr @slot
; CHECK: ret i32
;
; CHECK-LABEL: define i32 @volatile_atomic_fence
; CHECK: store volatile i32 1
; CHECK-NEXT: %first = load atomic i32, ptr %p seq_cst
; CHECK-NEXT: fence seq_cst
; CHECK-NEXT: store atomic i32 2
; CHECK-NEXT: %second = load volatile i32, ptr %p
; CHECK: ret i32
