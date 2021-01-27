define dso_local <16 x i16> @syn_avx2_pavg_w(<16 x i16> %a, <16 x i16> %b) local_unnamed_addr #0 {
entry:
  %add = add <16 x i16> %a, %b
  %add_1 = add <16 x i16> %add, <i16 1, i16 1, i16 1, i16 1, i16 1, i16 1, i16 1, i16 1, i16 1, i16 1, i16 1, i16 1, i16 1, i16 1, i16 1, i16 1>
  %sh = lshr <16 x i16> %add_1, <i16 1, i16 1, i16 1, i16 1, i16 1, i16 1, i16 1, i16 1, i16 1, i16 1, i16 1, i16 1, i16 1, i16 1, i16 1, i16 1>
  ret <16 x i16> %sh
}
