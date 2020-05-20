define i1 @eq_zero(i4 %x, i4 %y) {
; CHECK-LABEL: @eq_zero(
; CHECK-NEXT:    [[I0:%.*]] = icmp eq i4 [[X:%.*]], 0
; CHECK-NEXT:    [[I1:%.*]] = icmp eq i4 [[Y:%.*]], 0
; CHECK-NEXT:    [[R:%.*]] = xor i1 [[I0]], [[I1]]
; CHECK-NEXT:    ret i1 [[R]]
  %ia = sub i4 %x, 7
  %ib = add i4 %ia, 4
  %i0 = icmp eq i4 %ib, 0
  %i1 = icmp eq i4 %y, 0
  %r = xor i1 %i0, %i1
  ret i1 %r
}
