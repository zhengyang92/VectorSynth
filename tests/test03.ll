define dso_local <4 x i64> @test_mm_hsub_epi32(<4 x i64> %a, <4 x i64> %b, <4 x i64> %c) local_unnamed_addr #0 {
entry:
  %add.i = add <4 x i64> %a, <i64 3, i64 4, i64 8, i64 3242354325234523>
  %x = add <4 x i64> %add.i, <i64 3, i64 4, i64 8, i64 18>
  %y = add <4 x i64> %x, <i64 3, i64 4, i64 8, i64 31>
  %z = sub <4 x i64> %y, <i64 3, i64 4, i64 8, i64 31>
  ret <4 x i64> %z
}
